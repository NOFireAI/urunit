// Copyright (c) 2023-2026, Nubificus LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// The exec agent spawns additional processes (exec sessions) in the guest on
// behalf of the host. It speaks the framed protocol of urunc's pkg/agentproto.
//
// Transports. By default the agent listens on vsock port 1024. QEMU/KVM, Vz
// and hvi all reach the guest over vsock, and every host connection is an
// independent peer. When vsock fails, the agent probes the virtio-serial
// port io.urunc.agent.0. QEMU on macOS has no vhost-vsock and uses the port
// instead. The port is one byte stream to one host peer, so all sessions
// multiplex on it. A read of 0 bytes means the host peer is gone. The agent
// kills that peer's sessions, waits 100 ms and serves the next peer on the
// same descriptor with a fresh frame parser. URUNIT_AGENT_TRANSPORT pins
// one transport: vsock, virtio-port, unix:<path> or fd:<n>. The last two
// are for tests. fd:<n> serves an inherited descriptor like the port.
// Setup failures are retried every 200 ms, since the agent starts early in
// the boot and the devices may not be ready yet.
//
// One loop. Everything the agent waits on is a descriptor: the listener,
// the connections, the pty masters and pipes of the sessions, and a
// signalfd for SIGCHLD. One epoll loop serves them all and needs no locks.
// Writes never block the loop. Each connection has a bounded output queue.
// While the queue is above AGENT_OUTQ_HIGH, the agent stops reading the
// output of that connection's sessions, and the processes block on their
// own writes instead.
//
// EIO retry. Once the slave side of a pty has closed, a read of the master
// can return EIO while the child's last bytes are still queued. The next
// read returns those bytes. So on a pty the first EIO is retried and the
// second one ends the output. A pipe ends on any error.
//
// Drain bound. The peer ends a session on its Exit frame and drops any
// output after it, so the Exit frame waits for the session's outputs to
// close. A background process can keep them open, so the wait ends
// AGENT_DRAIN_MS after the exit.

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <linux/vm_sockets.h>

#include "agent.h"
#include "agentlog.h"
#include "proto.h"
#include "urunit.h"

#define AGENT_VSOCK_PORT 1024
#define AGENT_PORT_NAME "io.urunc.agent.0"
#define AGENT_RETRY_MS 200
#define AGENT_PORT_RETRY_MS 100
#define AGENT_CONN_READ (64 * 1024)
#define AGENT_MAX_EVENTS 64
#define AGENT_DEFAULT_PATH \
	"/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

// The state of the event loop. There is one agent per process.
static struct {
	int epoll_fd;
	struct agent_tag sig;
	struct agent_tag listener;
	const char *listener_name;

	// The transport plan from URUNIT_AGENT_TRANSPORT.
	int plan_vsock;
	int plan_port;
	int plan_fd;
	const char *plan_unix;

	// Whether a listener or a port is set up. retry_at is when the next
	// setup attempt runs while none is.
	int active;
	int64_t retry_at;

	// The port transport (virtio-port or fd:<n>). port_conn is the
	// current peer. It is NULL during the pause after a peer is gone.
	int port_fd;
	int port_owned;
	char port_name[PATH_MAX];
	struct agent_conn *port_conn;
	int64_t port_resume_at;

	struct agent_conn *conns;
} agent_st = {
	.epoll_fd = -1,
	.sig = { .kind = AGENT_TAG_SIGNAL, .fd = -1 },
	.listener = { .kind = AGENT_TAG_LISTENER, .fd = -1 },
	.plan_fd = -1,
	.port_fd = -1,
};

static int64_t agent_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Returns a heap string formatted like printf, or NULL when out of memory.
static char *agent_format(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static char *agent_format(const char *fmt, ...)
{
	va_list ap;
	char *s;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		return NULL;
	s = malloc((size_t)n + 1);
	if (s == NULL)
		return NULL;
	va_start(ap, fmt);
	vsnprintf(s, (size_t)n + 1, fmt, ap);
	va_end(ap);
	return s;
}

static void agent_close_fd(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

// ---------------------------------------------------------------- queue

size_t agent_outq_len(const struct agent_outq *q)
{
	return q->tail - q->head;
}

int agent_outq_push(struct agent_outq *q, const void *a, size_t alen,
		    const void *b, size_t blen, size_t limit)
{
	size_t len = agent_outq_len(q);
	size_t need = alen + blen;

	if (need < alen || need > limit || len > limit - need)
		return -1;
	if (q->tail + need > q->cap && q->head > 0) {
		memmove(q->buf, q->buf + q->head, len);
		q->head = 0;
		q->tail = len;
	}
	if (q->tail + need > q->cap) {
		size_t ncap = q->cap ? q->cap : 4096;
		uint8_t *nb;

		while (ncap < len + need)
			ncap *= 2;
		nb = realloc(q->buf, ncap);
		if (nb == NULL)
			return -1;
		q->buf = nb;
		q->cap = ncap;
	}
	if (alen > 0)
		memcpy(q->buf + q->tail, a, alen);
	q->tail += alen;
	if (blen > 0)
		memcpy(q->buf + q->tail, b, blen);
	q->tail += blen;
	return 0;
}

ssize_t agent_outq_flush(struct agent_outq *q, int fd)
{
	size_t total = 0;
	ssize_t w;

	while (q->head < q->tail) {
		w = write(fd, q->buf + q->head, q->tail - q->head);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return -1;
		}
		q->head += (size_t)w;
		total += (size_t)w;
	}
	if (q->head == q->tail)
		q->head = q->tail = 0;
	return (ssize_t)total;
}

void agent_outq_reset(struct agent_outq *q)
{
	free(q->buf);
	q->buf = NULL;
	q->head = q->tail = q->cap = 0;
}

// ---------------------------------------------------------------- epoll

// Sets the events tag is registered for. Zero events removes the tag from
// the epoll set, since EPOLLHUP would otherwise be reported anyway.
static void agent_tag_set(struct agent_tag *t, uint32_t events)
{
	struct epoll_event ev;
	int op;

	if (t->fd < 0 || agent_st.epoll_fd < 0) {
		t->events = events;
		return;
	}
	if (events == 0) {
		if (t->registered)
			epoll_ctl(agent_st.epoll_fd, EPOLL_CTL_DEL, t->fd, NULL);
		t->registered = 0;
		t->events = 0;
		return;
	}
	if (t->registered && t->events == events)
		return;
	memset(&ev, 0, sizeof(ev));
	ev.events = events;
	ev.data.ptr = t;
	op = t->registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
	if (epoll_ctl(agent_st.epoll_fd, op, t->fd, &ev) < 0) {
		agent_log("epoll_ctl fd %d: %s", t->fd, strerror(errno));
		return;
	}
	t->registered = 1;
	t->events = events;
}

static void agent_tag_close(struct agent_tag *t)
{
	agent_tag_set(t, 0);
	agent_close_fd(&t->fd);
}

// ---------------------------------------------------------------- conn

static void agent_session_update(struct agent_session *s);

struct agent_conn *agent_conn_new(int fd, int is_port)
{
	struct agent_conn *c = calloc(1, sizeof(*c));

	if (c == NULL)
		return NULL;
	c->tag.kind = AGENT_TAG_CONN;
	c->tag.fd = fd;
	c->tag.owner = c;
	c->is_port = is_port;
	return c;
}

void agent_conn_free(struct agent_conn *c)
{
	if (c == NULL)
		return;
	agent_outq_reset(&c->outq);
	free(c->payload);
	free(c);
}

static void agent_conn_set_throttle(struct agent_conn *c, int on)
{
	struct agent_session *s;

	if (c->throttled == on)
		return;
	c->throttled = on;
	for (s = c->sessions; s != NULL; s = s->next)
		agent_session_update(s);
}

void agent_conn_mark_closed(struct agent_conn *c)
{
	struct agent_session *s;

	if (c->closed)
		return;
	c->closed = 1;
	for (s = c->sessions; s != NULL; s = s->next) {
		if (!s->exited && s->pid > 0)
			kill(s->pid, SIGKILL);
	}
	agent_outq_reset(&c->outq);
	free(c->payload);
	c->payload = NULL;
	c->hdr_have = 0;
	c->payload_have = 0;
	// The sessions' output now goes nowhere. Reading it lets the outputs
	// reach EOF.
	agent_conn_set_throttle(c, 0);
}

// Returns whether a session of c has so much stdin queued that c stops
// reading frames.
static int agent_conn_stdin_blocked(const struct agent_conn *c)
{
	const struct agent_session *s;

	for (s = c->sessions; s != NULL; s = s->next) {
		if (agent_outq_len(&s->inq) >= AGENT_STDIN_HIGH)
			return 1;
	}
	return 0;
}

static void agent_conn_update(struct agent_conn *c)
{
	uint32_t ev = 0;

	if (c->lost)
		return;
	if (!c->closed && !agent_conn_stdin_blocked(c))
		ev |= EPOLLIN;
	if (agent_outq_len(&c->outq) > 0)
		ev |= EPOLLOUT;
	agent_tag_set(&c->tag, ev);
}

// Ends the peer of c. A socket is closed. The port stays open and the next
// peer gets a new connection after AGENT_PORT_RETRY_MS. c is freed once its
// sessions are gone.
static void agent_conn_lost(struct agent_conn *c)
{
	if (c->lost)
		return;
	agent_conn_mark_closed(c);
	c->lost = 1;
	agent_tag_set(&c->tag, 0);
	if (!c->is_port) {
		agent_close_fd(&c->tag.fd);
		return;
	}
	c->tag.fd = -1;
	if (agent_st.port_conn == c) {
		agent_st.port_conn = NULL;
		agent_st.port_resume_at = agent_now() + AGENT_PORT_RETRY_MS;
	}
}

int agent_conn_flush(struct agent_conn *c)
{
	if (c->closed || c->tag.fd < 0)
		return -1;
	if (agent_outq_flush(&c->outq, c->tag.fd) < 0) {
		agent_conn_lost(c);
		return -1;
	}
	if (c->throttled && agent_outq_len(&c->outq) < AGENT_OUTQ_HIGH)
		agent_conn_set_throttle(c, 0);
	agent_conn_update(c);
	return 0;
}

int agent_conn_enqueue_frame(struct agent_conn *c, uint8_t type,
			     uint32_t stream, const void *payload,
			     size_t len)
{
	uint8_t hdr[PROTO_HDR_LEN];
	int was_empty;

	if (c->closed || len > PROTO_MAX_PAYLOAD)
		return -1;
	proto_encode_hdr(hdr, type, stream, (uint32_t)len);
	was_empty = agent_outq_len(&c->outq) == 0;
	if (agent_outq_push(&c->outq, hdr, sizeof(hdr), payload, len,
			    AGENT_OUTQ_CAP) < 0) {
		agent_log("stream %u: output queue full, frame dropped",
			  stream);
		return -1;
	}
	if (was_empty && agent_conn_flush(c) < 0)
		return -1;
	if (!c->throttled && agent_outq_len(&c->outq) >= AGENT_OUTQ_HIGH)
		agent_conn_set_throttle(c, 1);
	return 0;
}

static void agent_send_error(struct agent_conn *c, uint32_t stream,
			     const char *msg)
{
	size_t cap = strlen(msg) * 6 + 32;
	char *buf = malloc(cap);
	int n;

	if (buf == NULL)
		return;
	n = proto_format_error(buf, cap, msg);
	if (n >= 0)
		agent_conn_enqueue_frame(c, PROTO_ERROR, stream, buf,
					 (size_t)n);
	free(buf);
}

static struct agent_session *agent_conn_session(struct agent_conn *c,
						uint32_t stream)
{
	struct agent_session *s;

	for (s = c->sessions; s != NULL; s = s->next) {
		if (s->stream == stream)
			return s;
	}
	return NULL;
}

// ---------------------------------------------------------------- user

static int agent_parse_u32(const char *s, uint32_t *out)
{
	uint64_t v = 0;

	if (s == NULL || *s == '\0')
		return -1;
	for (; *s != '\0'; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		v = v * 10 + (uint64_t)(*s - '0');
		if (v > UINT32_MAX)
			return -1;
	}
	*out = (uint32_t)v;
	return 0;
}

int agent_split_user(char *spec, char **uid_part, char **gid_part)
{
	char *colon = strchr(spec, ':');

	*uid_part = spec;
	*gid_part = NULL;
	if (colon == NULL)
		return 0;
	*colon = '\0';
	*gid_part = colon + 1;
	return 1;
}

static size_t agent_pw_bufsize(int name)
{
	long n = sysconf(name);

	return n > 0 ? (size_t)n : 16384;
}

// Looks up a passwd entry by name, or by uid when name is NULL. The strings
// of pw point into *buf. Returns 0, or -1 when there is no such entry.
static int agent_getpw(const char *name, uid_t uid, struct passwd *pw,
		       char **buf)
{
	size_t len = agent_pw_bufsize(_SC_GETPW_R_SIZE_MAX);
	struct passwd *res = NULL;
	char *b;
	int r;

	for (;;) {
		b = realloc(*buf, len);
		if (b == NULL)
			return -1;
		*buf = b;
		if (name != NULL)
			r = getpwnam_r(name, pw, b, len, &res);
		else
			r = getpwuid_r(uid, pw, b, len, &res);
		if (r == ERANGE && len < (1u << 20)) {
			len *= 2;
			continue;
		}
		return r == 0 && res != NULL ? 0 : -1;
	}
}

static int agent_getgr(const char *name, gid_t *gid)
{
	size_t len = agent_pw_bufsize(_SC_GETGR_R_SIZE_MAX);
	struct group gr, *res = NULL;
	char *buf = NULL, *b;
	int r;

	for (;;) {
		b = realloc(buf, len);
		if (b == NULL) {
			free(buf);
			return -1;
		}
		buf = b;
		r = getgrnam_r(name, &gr, buf, len, &res);
		if (r == ERANGE && len < (1u << 20)) {
			len *= 2;
			continue;
		}
		break;
	}
	if (r == 0 && res != NULL)
		*gid = gr.gr_gid;
	free(buf);
	return r == 0 && res != NULL ? 0 : -1;
}

// Fills in u->groups with the groups of a passwd user, its primary group
// included. A failure leaves the list empty.
static void agent_user_groups(struct agent_user *u, const char *name,
			      gid_t gid)
{
	int n = 32;
	int cnt;
	gid_t *g = NULL, *ng;

	for (;;) {
		ng = realloc(g, (size_t)n * sizeof(*g));
		if (ng == NULL)
			break;
		g = ng;
		cnt = n;
		if (getgrouplist(name, gid, g, &cnt) >= 0) {
			u->groups = g;
			u->ngroups = cnt;
			return;
		}
		if (n >= 65536)
			break;
		n = cnt > n ? cnt : n * 2;
	}
	free(g);
}

void agent_user_free(struct agent_user *u)
{
	free(u->groups);
	free(u->name);
	free(u->home);
	free(u->shell);
	memset(u, 0, sizeof(*u));
}

int agent_resolve_user(const char *spec, struct agent_user *u, char *err,
		       size_t errlen)
{
	struct passwd pw;
	char *copy, *uid_part, *gid_part;
	char *buf = NULL;
	uint32_t n = 0, g;
	int numeric, found, has_gid;

	memset(u, 0, sizeof(*u));
	if (spec == NULL || *spec == '\0')
		return 0;
	copy = strdup(spec);
	if (copy == NULL) {
		snprintf(err, errlen, "%s", strerror(ENOMEM));
		return -1;
	}
	has_gid = agent_split_user(copy, &uid_part, &gid_part);
	numeric = agent_parse_u32(uid_part, &n) == 0;
	found = agent_getpw(numeric ? NULL : uid_part, (uid_t)n, &pw,
			    &buf) == 0;

	if (found) {
		u->uid = pw.pw_uid;
		u->gid = pw.pw_gid;
		u->name = strdup(pw.pw_name);
		u->home = strdup(pw.pw_dir && *pw.pw_dir ? pw.pw_dir : "/");
		u->shell = strdup(pw.pw_shell && *pw.pw_shell ? pw.pw_shell
							       : "/bin/sh");
		agent_user_groups(u, pw.pw_name, pw.pw_gid);
	} else if (numeric) {
		// Not in /etc/passwd: accept a raw numeric uid.
		u->uid = (uid_t)n;
		u->gid = (gid_t)n;
		u->name = strdup(uid_part);
		u->home = strdup("/");
		u->shell = strdup("/bin/sh");
	} else {
		snprintf(err, errlen, "unknown user \"%s\"", uid_part);
		goto fail;
	}
	free(buf);
	buf = NULL;
	if (u->name == NULL || u->home == NULL || u->shell == NULL) {
		snprintf(err, errlen, "%s", strerror(ENOMEM));
		goto fail;
	}

	if (has_gid) {
		if (agent_parse_u32(gid_part, &g) == 0) {
			u->gid = (gid_t)g;
		} else if (agent_getgr(gid_part, &u->gid) < 0) {
			snprintf(err, errlen, "unknown group \"%s\"", gid_part);
			goto fail;
		}
	}
	u->resolved = 1;
	free(copy);
	return 0;

fail:
	free(buf);
	free(copy);
	agent_user_free(u);
	return -1;
}

// ---------------------------------------------------------------- env

int agent_env_has(char *const *env, const char *key)
{
	size_t k = strlen(key);

	for (; env != NULL && *env != NULL; env++) {
		if (strncmp(*env, key, k) == 0 && (*env)[k] == '=')
			return 1;
	}
	return 0;
}

void agent_free_strings(char **v)
{
	char **p;

	if (v == NULL)
		return;
	for (p = v; *p != NULL; p++)
		free(*p);
	free(v);
}

// Appends key=val to v unless env already has key. Returns 0, or -1 when
// out of memory.
static int agent_env_default(char **v, size_t *n, char *const *env,
			     const char *key, const char *val)
{
	if (agent_env_has(env, key))
		return 0;
	v[*n] = agent_format("%s=%s", key, val);
	if (v[*n] == NULL)
		return -1;
	(*n)++;
	return 0;
}

char **agent_build_env(char *const *env, const struct agent_user *u)
{
	size_t count = 0, n = 0;
	char **v;
	int r = 0;

	while (env != NULL && env[count] != NULL)
		count++;
	// Six defaults at most, and the NULL.
	v = calloc(count + 7, sizeof(*v));
	if (v == NULL)
		return NULL;
	for (n = 0; n < count; n++) {
		v[n] = strdup(env[n]);
		if (v[n] == NULL) {
			agent_free_strings(v);
			return NULL;
		}
	}
	r |= agent_env_default(v, &n, env, "PATH", AGENT_DEFAULT_PATH);
	r |= agent_env_default(v, &n, env, "TERM", "xterm-256color");
	if (u != NULL && u->resolved) {
		r |= agent_env_default(v, &n, env, "HOME", u->home);
		r |= agent_env_default(v, &n, env, "USER", u->name);
		r |= agent_env_default(v, &n, env, "LOGNAME", u->name);
		r |= agent_env_default(v, &n, env, "SHELL", u->shell);
	}
	if (r != 0) {
		agent_free_strings(v);
		return NULL;
	}
	return v;
}

int agent_exit_code(int status)
{
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 126;
}

// ---------------------------------------------------------------- session

static void agent_session_update(struct agent_session *s)
{
	uint32_t rd = s->conn->throttled ? 0 : EPOLLIN;
	uint32_t wr = agent_outq_len(&s->inq) > 0 ? EPOLLOUT : 0;

	if (s->tty) {
		agent_tag_set(&s->out, rd | wr);
		return;
	}
	agent_tag_set(&s->out, rd);
	agent_tag_set(&s->err, rd);
	agent_tag_set(&s->in, wr);
}

static void agent_session_close_stdin(struct agent_session *s)
{
	agent_outq_reset(&s->inq);
	s->close_stdin_pending = 0;
	if (!s->tty)
		agent_tag_close(&s->in);
}

static void agent_session_flush_stdin(struct agent_session *s)
{
	int fd = s->tty ? s->out.fd : s->in.fd;

	if (fd < 0) {
		agent_outq_reset(&s->inq);
	} else if (agent_outq_flush(&s->inq, fd) < 0) {
		if (s->tty)
			agent_outq_reset(&s->inq);
		else
			agent_session_close_stdin(s);
	}
	if (!s->tty && s->close_stdin_pending &&
	    agent_outq_len(&s->inq) == 0)
		agent_session_close_stdin(s);
	agent_session_update(s);
	agent_conn_update(s->conn);
}

static void agent_session_stdin(struct agent_session *s, const void *data,
				size_t len)
{
	int fd = s->tty ? s->out.fd : s->in.fd;

	if (fd < 0 || s->close_stdin_pending || len == 0)
		return;
	if (agent_outq_push(&s->inq, data, len, NULL, 0, AGENT_STDIN_CAP) <
	    0) {
		agent_log("stream %u: stdin queue full, %zu bytes dropped",
			  s->stream, len);
		return;
	}
	agent_session_flush_stdin(s);
}

static void agent_session_close_output(struct agent_session *s,
				       struct agent_tag *t)
{
	agent_tag_close(t);
	// The pty master also carries stdin. With the slave closed nobody
	// reads it.
	if (s->tty) {
		agent_outq_reset(&s->inq);
		agent_conn_update(s->conn);
	}
}

static void agent_session_read(struct agent_session *s, struct agent_tag *t)
{
	static uint8_t buf[AGENT_READ_CHUNK];
	uint8_t type = t == &s->err ? PROTO_STDERR : PROTO_STDOUT;
	ssize_t n;

	for (;;) {
		n = read(t->fd, buf, sizeof(buf));
		if (n > 0) {
			if (agent_conn_enqueue_frame(s->conn, type, s->stream,
						     buf, (size_t)n) < 0)
				agent_session_close_output(s, t);
			return;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n < 0 && errno == EIO && s->tty && !s->eio_retried) {
			s->eio_retried = 1;
			continue;
		}
		agent_session_close_output(s, t);
		return;
	}
}

// Closes the session's descriptors, unlinks it from its connection and
// reports its exit code to the peer.
static void agent_session_finish(struct agent_session *s)
{
	struct agent_conn *c = s->conn;
	struct agent_session **pp;
	char buf[32];
	int n;

	agent_tag_close(&s->out);
	agent_tag_close(&s->err);
	agent_tag_close(&s->in);
	agent_outq_reset(&s->inq);
	for (pp = &c->sessions; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == s) {
			*pp = s->next;
			break;
		}
	}
	n = proto_format_exit(buf, sizeof(buf), s->exit_code);
	if (n >= 0)
		agent_conn_enqueue_frame(c, PROTO_EXIT, s->stream, buf,
					 (size_t)n);
	free(s);
	agent_conn_update(c);
}

// Marks every descriptor from lowfd up close-on-exec in a session child.
// The agent's own descriptors already are. This catches anything the
// agent inherited.
static void agent_cloexec_from(int lowfd)
{
	struct dirent *de;
	DIR *d;
	int fd;

#ifdef SYS_close_range
	if (syscall(SYS_close_range, (unsigned int)lowfd, ~0U,
		    CLOSE_RANGE_CLOEXEC) == 0)
		return;
#endif
	d = opendir("/proc/self/fd");
	if (d != NULL) {
		while ((de = readdir(d)) != NULL) {
			fd = atoi(de->d_name);
			if (fd >= lowfd && fd != dirfd(d))
				fcntl(fd, F_SETFD, FD_CLOEXEC);
		}
		closedir(d);
		return;
	}
	for (fd = lowfd; fd < 4096; fd++)
		fcntl(fd, F_SETFD, FD_CLOEXEC);
}

// The child side of agent_open_session. It writes errno to status and
// exits with 127 when anything before the exec fails.
static void agent_child(const struct open_request *req,
			const struct agent_user *u, char **envp,
			const char *cwd, int slave, const int in[2],
			const int out[2], const int err[2], int status)
	__attribute__((noreturn));

static void agent_child(const struct open_request *req,
			const struct agent_user *u, char **envp,
			const char *cwd, int slave, const int in[2],
			const int out[2], const int err[2], int status)
{
	sigset_t none;
	int e;

	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	signal(SIGPIPE, SIG_DFL);

	// The agent keeps 0, 1 and 2 open, so every source descriptor here is
	// above 2 and dup2 clears its close-on-exec flag.
	if (req->tty) {
		if (setsid() < 0)
			goto fail;
		if (dup2(slave, 0) < 0 || dup2(slave, 1) < 0 ||
		    dup2(slave, 2) < 0)
			goto fail;
		if (ioctl(0, TIOCSCTTY, 0) < 0)
			goto fail;
	} else {
		if (dup2(in[0], 0) < 0 || dup2(out[1], 1) < 0 ||
		    dup2(err[1], 2) < 0)
			goto fail;
	}
	agent_cloexec_from(3);
	if (u->resolved) {
		if (setgroups((size_t)u->ngroups, u->groups) < 0 ||
		    setgid(u->gid) < 0 || setuid(u->uid) < 0)
			goto fail;
	}
	if (chdir(cwd) < 0)
		goto fail;
	// execvpe searches the agent's own PATH, like exec.Command in the Go
	// agent, and the session gets envp.
	execvpe(req->argv[0], req->argv, envp);
fail:
	e = errno;
	if (write(status, &e, sizeof(e)) < 0)
		_exit(127);
	_exit(127);
}

// Opens a pty pair. name receives the path of the slave. Returns 0, or -1
// with errno set.
static int agent_open_pty(int *master, int *slave, char *name, size_t len)
{
	int m, r, e;

	m = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (m < 0)
		return -1;
	if (grantpt(m) < 0 || unlockpt(m) < 0)
		goto fail;
	r = ptsname_r(m, name, len);
	if (r != 0) {
		errno = r;
		goto fail;
	}
	*slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (*slave < 0)
		goto fail;
	*master = m;
	return 0;
fail:
	e = errno;
	close(m);
	errno = e;
	return -1;
}

static void agent_set_nonblock(int fd)
{
	int fl;

	if (fd < 0)
		return;
	fl = fcntl(fd, F_GETFL);
	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// Starts req on stream of c. Returns 0, or -1 with the message for the
// Error frame in *errmsg (NULL when out of memory).
static int agent_open_session(struct agent_conn *c, uint32_t stream,
			      const struct open_request *req, char **errmsg)
{
	const char *start = req->tty ? "start (tty)" : "start";
	struct agent_session *s = NULL;
	struct agent_user u;
	struct winsize ws;
	char uerr[512];
	char pts[128];
	char **envp = NULL;
	const char *cwd;
	int master = -1, slave = -1;
	int in[2] = { -1, -1 }, out[2] = { -1, -1 }, err[2] = { -1, -1 };
	int status[2] = { -1, -1 };
	int child_errno, rc = -1, i;
	ssize_t n;
	pid_t pid;

	if (agent_resolve_user(req->user, &u, uerr, sizeof(uerr)) < 0) {
		*errmsg = strdup(uerr);
		return -1;
	}
	envp = agent_build_env(req->env, &u);
	s = calloc(1, sizeof(*s));
	if (envp == NULL || s == NULL) {
		*errmsg = agent_format("%s: %s", start, strerror(ENOMEM));
		goto out;
	}
	if (req->cwd != NULL && *req->cwd != '\0')
		cwd = req->cwd;
	else if (u.resolved)
		cwd = u.home;
	else
		cwd = "/";

	if (req->tty) {
		if (agent_open_pty(&master, &slave, pts, sizeof(pts)) < 0) {
			*errmsg = agent_format("open pty: %s", strerror(errno));
			goto out;
		}
		// Programs that reopen their own /dev/pts/N need to own it,
		// as login(1) arranges.
		if (u.resolved && chown(pts, u.uid, u.gid) < 0)
			agent_log("chown %s: %s", pts, strerror(errno));
		memset(&ws, 0, sizeof(ws));
		ws.ws_row = req->rows;
		ws.ws_col = req->cols;
		if (ws.ws_row == 0 || ws.ws_col == 0) {
			ws.ws_row = 24;
			ws.ws_col = 80;
		}
		ioctl(master, TIOCSWINSZ, &ws);
	} else if (pipe2(in, O_CLOEXEC) < 0 || pipe2(out, O_CLOEXEC) < 0 ||
		   pipe2(err, O_CLOEXEC) < 0) {
		*errmsg = agent_format("%s: %s", start, strerror(errno));
		goto out;
	}
	if (pipe2(status, O_CLOEXEC) < 0) {
		*errmsg = agent_format("%s: %s", start, strerror(errno));
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		*errmsg = agent_format("%s: %s", start, strerror(errno));
		goto out;
	}
	if (pid == 0)
		agent_child(req, &u, envp, cwd, slave, in, out, err,
			    status[1]);

	// The status pipe reaches EOF when the exec succeeds. Otherwise it
	// carries the errno of the step that failed.
	agent_close_fd(&status[1]);
	do {
		n = read(status[0], &child_errno, sizeof(child_errno));
	} while (n < 0 && errno == EINTR);
	if (n == (ssize_t)sizeof(child_errno)) {
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
			;
		*errmsg = agent_format("%s: %s", start, strerror(child_errno));
		goto out;
	}

	s->stream = stream;
	s->pid = pid;
	s->tty = req->tty;
	s->conn = c;
	s->out.kind = AGENT_TAG_OUT;
	s->err.kind = AGENT_TAG_ERR;
	s->in.kind = AGENT_TAG_STDIN;
	s->out.owner = s->err.owner = s->in.owner = s;
	s->out.fd = s->err.fd = s->in.fd = -1;
	if (req->tty) {
		s->out.fd = master;
		master = -1;
	} else {
		s->in.fd = in[1];
		s->out.fd = out[0];
		s->err.fd = err[0];
		in[1] = out[0] = err[0] = -1;
	}
	agent_set_nonblock(s->out.fd);
	agent_set_nonblock(s->err.fd);
	agent_set_nonblock(s->in.fd);
	s->next = c->sessions;
	c->sessions = s;
	agent_session_update(s);
	s = NULL;
	rc = 0;

out:
	agent_close_fd(&master);
	agent_close_fd(&slave);
	for (i = 0; i < 2; i++) {
		agent_close_fd(&in[i]);
		agent_close_fd(&out[i]);
		agent_close_fd(&err[i]);
		agent_close_fd(&status[i]);
	}
	free(s);
	agent_free_strings(envp);
	agent_user_free(&u);
	return rc;
}

// ---------------------------------------------------------------- frames

static void agent_conn_open(struct agent_conn *c, uint32_t stream,
			    const uint8_t *payload, size_t len)
{
	struct open_request req;
	char perr[256];
	char *msg = NULL;

	if (proto_parse_open(payload ? (const char *)payload : "", len, &req,
			     perr, sizeof(perr)) < 0) {
		msg = agent_format("bad open request: %s", perr);
		if (msg != NULL)
			agent_send_error(c, stream, msg);
		free(msg);
		return;
	}
	if (req.argc == 0) {
		agent_send_error(c, stream, "empty argv");
	} else if (agent_conn_session(c, stream) != NULL) {
		msg = agent_format("stream %u already in use", stream);
		if (msg != NULL)
			agent_send_error(c, stream, msg);
	} else if (agent_open_session(c, stream, &req, &msg) < 0) {
		agent_send_error(c, stream,
				 msg != NULL ? msg : "start: out of memory");
	}
	free(msg);
	proto_free_open(&req);
}

static void agent_conn_dispatch(struct agent_conn *c, const uint8_t *p,
				size_t len)
{
	struct agent_session *s = agent_conn_session(c, c->cur.stream);
	const char *json = p ? (const char *)p : "";
	struct winsize ws;
	uint16_t rows, cols;
	int sig;

	switch (c->cur.type) {
	case PROTO_OPEN:
		agent_conn_open(c, c->cur.stream, p, len);
		break;
	case PROTO_STDIN:
		if (s != NULL)
			agent_session_stdin(s, p, len);
		break;
	case PROTO_CLOSE_STDIN:
		// A tty session has no separate stdin to close.
		if (s != NULL && !s->tty && s->in.fd >= 0) {
			s->close_stdin_pending = 1;
			agent_session_flush_stdin(s);
		}
		break;
	case PROTO_RESIZE:
		if (proto_parse_resize(json, len, &rows, &cols) == 0 &&
		    s != NULL && s->tty && s->out.fd >= 0) {
			memset(&ws, 0, sizeof(ws));
			ws.ws_row = rows;
			ws.ws_col = cols;
			ioctl(s->out.fd, TIOCSWINSZ, &ws);
		}
		break;
	case PROTO_SIGNAL:
		if (proto_parse_signal(json, len, &sig) == 0 && s != NULL &&
		    !s->exited)
			kill(s->pid, sig);
		break;
	default:
		agent_log("unknown frame type %d", c->cur.type);
		break;
	}
}

// Feeds bytes from the peer to the frame parser of c. Returns 0, or -1 on
// a protocol error that ends the peer.
static int agent_conn_feed(struct agent_conn *c, const uint8_t *p, size_t n)
{
	uint8_t *payload;
	size_t take;

	while (n > 0 && !c->closed) {
		if (c->hdr_have < PROTO_HDR_LEN) {
			take = PROTO_HDR_LEN - c->hdr_have;
			if (take > n)
				take = n;
			memcpy(c->hdr + c->hdr_have, p, take);
			c->hdr_have += take;
			p += take;
			n -= take;
			if (c->hdr_have < PROTO_HDR_LEN)
				break;
			if (proto_decode_hdr(c->hdr, &c->cur) < 0) {
				agent_log("read: frame payload too large: %u",
					  c->cur.len);
				return -1;
			}
			if (c->cur.len == 0) {
				c->hdr_have = 0;
				agent_conn_dispatch(c, NULL, 0);
				continue;
			}
			c->payload = malloc(c->cur.len);
			if (c->payload == NULL) {
				agent_log("read: %s", strerror(ENOMEM));
				return -1;
			}
			c->payload_have = 0;
			continue;
		}
		take = c->cur.len - c->payload_have;
		if (take > n)
			take = n;
		memcpy(c->payload + c->payload_have, p, take);
		c->payload_have += take;
		p += take;
		n -= take;
		if (c->payload_have < c->cur.len)
			break;
		payload = c->payload;
		c->payload = NULL;
		c->payload_have = 0;
		c->hdr_have = 0;
		agent_conn_dispatch(c, payload, c->cur.len);
		free(payload);
	}
	return 0;
}

// ---------------------------------------------------------------- transports

static void agent_link_conn(struct agent_conn *c)
{
	c->next = agent_st.conns;
	agent_st.conns = c;
	agent_conn_update(c);
}

static void agent_listener_start(int fd, const char *name)
{
	agent_st.listener.fd = fd;
	agent_st.listener_name = name;
	agent_st.active = 1;
	agent_tag_set(&agent_st.listener, EPOLLIN);
}

static int agent_listen_vsock(void)
{
	struct sockaddr_vm sa;
	const char *step = "socket";
	int fd, e;

	fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		goto fail;
	memset(&sa, 0, sizeof(sa));
	sa.svm_family = AF_VSOCK;
	sa.svm_cid = VMADDR_CID_ANY;
	sa.svm_port = AGENT_VSOCK_PORT;
	step = "bind";
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		goto fail;
	step = "listen";
	if (listen(fd, 8) < 0)
		goto fail;
	agent_log("listening on vsock port %d", AGENT_VSOCK_PORT);
	agent_listener_start(fd, "vsock");
	return 0;
fail:
	e = errno;
	if (fd >= 0)
		close(fd);
	agent_log("vsock transport unavailable: vsock %s: %s; retrying", step,
		  strerror(e));
	return -1;
}

static int agent_listen_unix(const char *path)
{
	struct sockaddr_un sa;
	const char *step = "socket";
	int fd, e;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		goto fail;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	memcpy(sa.sun_path, path, strlen(path));
	if (unlink(path) < 0 && errno != ENOENT)
		agent_log("unlink %s: %s", path, strerror(errno));
	step = "bind";
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		goto fail;
	step = "listen";
	if (listen(fd, 64) < 0)
		goto fail;
	agent_log("listening on unix socket %s", path);
	agent_listener_start(fd, "unix");
	return 0;
fail:
	e = errno;
	if (fd >= 0)
		close(fd);
	agent_log("unix transport unavailable: unix %s %s: %s; retrying", step,
		  path, strerror(e));
	return -1;
}

static void agent_accept(void)
{
	struct agent_conn *c;
	int fd;

	for (;;) {
		fd = accept4(agent_st.listener.fd, NULL, NULL,
			     SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (fd < 0) {
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			agent_log("%s transport unavailable: %s accept: %s; "
				  "retrying",
				  agent_st.listener_name,
				  agent_st.listener_name, strerror(errno));
			agent_tag_close(&agent_st.listener);
			agent_st.active = 0;
			agent_st.retry_at = agent_now() + AGENT_RETRY_MS;
			return;
		}
		c = agent_conn_new(fd, 0);
		if (c == NULL) {
			close(fd);
			continue;
		}
		agent_link_conn(c);
	}
}

static void agent_port_start(int fd, int owned, const char *name)
{
	agent_st.port_fd = fd;
	agent_st.port_owned = owned;
	snprintf(agent_st.port_name, sizeof(agent_st.port_name), "%s", name);
	agent_st.port_conn = NULL;
	agent_st.port_resume_at = agent_now();
	agent_st.active = 1;
}

// Starts a connection for the next peer on the port.
static void agent_port_resume(void)
{
	struct agent_conn *c = agent_conn_new(agent_st.port_fd, 1);

	if (c == NULL) {
		agent_st.port_resume_at = agent_now() + AGENT_PORT_RETRY_MS;
		return;
	}
	agent_st.port_conn = c;
	agent_link_conn(c);
}

// Ends the port transport after a read error. A virtio port is closed and
// probed again. fd:<n> stays open and is served again.
static void agent_port_fail(int e)
{
	if (agent_st.port_owned) {
		agent_log("virtio port transport unavailable: virtio port %s: "
			  "%s; retrying",
			  agent_st.port_name, strerror(e));
		close(agent_st.port_fd);
	} else {
		agent_log("fd transport unavailable: %s: %s; retrying",
			  agent_st.port_name, strerror(e));
	}
	agent_st.port_fd = -1;
	agent_st.port_conn = NULL;
	agent_st.active = 0;
	agent_st.retry_at = agent_now() + AGENT_RETRY_MS;
}

// Reads the first line of path into buf, without surrounding white space.
static int agent_read_name(const char *path, char *buf, size_t len)
{
	ssize_t n;
	size_t a = 0, b;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	b = (size_t)n;
	while (b > 0 && (buf[b - 1] == '\n' || buf[b - 1] == ' ' ||
			 buf[b - 1] == '\t' || buf[b - 1] == '\r'))
		b--;
	while (a < b && (buf[a] == ' ' || buf[a] == '\t'))
		a++;
	memmove(buf, buf + a, b - a);
	buf[b - a] = '\0';
	return 0;
}

// Finds the device of the agent's virtio-serial port. Minimal guests run
// no udev, so /dev/virtio-ports/<name> may be missing and the name is
// resolved through /sys/class/virtio-ports/vportNpM/name -> /dev/vportNpM.
static int agent_probe_port(char *dev, size_t len)
{
	struct dirent *de;
	struct stat st;
	char path[PATH_MAX];
	char name[256];
	DIR *d;

	// The agent may start before init mounts sysfs.
	if (stat("/sys/class", &st) < 0 &&
	    mount("sysfs", "/sys", "sysfs", 0, "") < 0)
		agent_log("mount /sys: %s", strerror(errno));

	snprintf(path, sizeof(path), "/dev/virtio-ports/%s", AGENT_PORT_NAME);
	if (stat(path, &st) == 0) {
		snprintf(dev, len, "%s", path);
		return 0;
	}
	d = opendir("/sys/class/virtio-ports");
	if (d == NULL)
		return -1;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/virtio-ports/%s/name",
			 de->d_name);
		if (agent_read_name(path, name, sizeof(name)) < 0 ||
		    strcmp(name, AGENT_PORT_NAME) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/%s", de->d_name);
		if (stat(path, &st) == 0) {
			snprintf(dev, len, "%s", path);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

static int agent_serve_port_dev(void)
{
	char dev[PATH_MAX];
	int fd;

	if (agent_probe_port(dev, sizeof(dev)) < 0)
		return -1;
	fd = open(dev, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		agent_log("virtio port transport unavailable: open %s: %s; "
			  "retrying",
			  dev, strerror(errno));
		return -1;
	}
	agent_log("serving on virtio port %s", dev);
	agent_port_start(fd, 1, dev);
	return 0;
}

static int agent_serve_fd(int fd)
{
	char name[32];
	int fl;

	snprintf(name, sizeof(name), "fd %d", fd);
	fl = fcntl(fd, F_GETFL);
	if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ||
	    fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		agent_log("fd transport unavailable: %s: %s; retrying", name,
			  strerror(errno));
		return -1;
	}
	agent_log("serving on %s", name);
	agent_port_start(fd, 0, name);
	return 0;
}

static void agent_transport_setup(void)
{
	if (agent_st.plan_unix != NULL) {
		if (agent_listen_unix(agent_st.plan_unix) == 0)
			return;
	} else if (agent_st.plan_fd >= 0) {
		if (agent_serve_fd(agent_st.plan_fd) == 0)
			return;
	} else {
		if (agent_st.plan_vsock && agent_listen_vsock() == 0)
			return;
		if (agent_st.plan_port && agent_serve_port_dev() == 0)
			return;
	}
	agent_st.retry_at = agent_now() + AGENT_RETRY_MS;
}

// Sets the transport plan from the value of URUNIT_AGENT_TRANSPORT.
// Returns -1 when the pinned transport can never be set up.
static int agent_plan(const char *env)
{
	struct sockaddr_un sa;
	uint32_t n;

	agent_st.plan_vsock = 1;
	agent_st.plan_port = 1;
	agent_st.plan_fd = -1;
	agent_st.plan_unix = NULL;
	if (env == NULL || *env == '\0')
		return 0;
	if (strcmp(env, "vsock") == 0) {
		agent_st.plan_port = 0;
		return 0;
	}
	if (strcmp(env, "virtio-port") == 0) {
		agent_st.plan_vsock = 0;
		return 0;
	}
	if (strncmp(env, "unix:", 5) == 0 && env[5] != '\0') {
		if (strlen(env + 5) >= sizeof(sa.sun_path)) {
			agent_log("unix socket path too long: %s", env + 5);
			return -1;
		}
		agent_st.plan_unix = env + 5;
		return 0;
	}
	if (strncmp(env, "fd:", 3) == 0 && agent_parse_u32(env + 3, &n) == 0 &&
	    n <= INT_MAX) {
		if (fcntl((int)n, F_GETFD) < 0) {
			agent_log("fd %u: %s", n, strerror(errno));
			return -1;
		}
		agent_st.plan_fd = (int)n;
		return 0;
	}
	agent_log("ignoring unknown URUNIT_AGENT_TRANSPORT=\"%s\"", env);
	return 0;
}

// ---------------------------------------------------------------- loop

static struct agent_session *agent_find_pid(pid_t pid)
{
	struct agent_conn *c;
	struct agent_session *s;

	for (c = agent_st.conns; c != NULL; c = c->next) {
		for (s = c->sessions; s != NULL; s = s->next) {
			if (s->pid == pid && !s->exited)
				return s;
		}
	}
	return NULL;
}

static void agent_reap(void)
{
	struct signalfd_siginfo si;
	struct agent_session *s;
	pid_t pid;
	int status;

	while (read(agent_st.sig.fd, &si, sizeof(si)) == (ssize_t)sizeof(si))
		;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		s = agent_find_pid(pid);
		if (s == NULL)
			continue;
		s->exited = 1;
		s->exit_code = agent_exit_code(status);
		s->drain_deadline = agent_now() + AGENT_DRAIN_MS;
	}
}

// Finishes the sessions that exited and whose output is done or ran past
// the drain bound, and frees the connections that are gone for good.
static void agent_sweep(int64_t now)
{
	struct agent_conn **cp = &agent_st.conns;
	struct agent_conn *c;
	struct agent_session *s, *next;
	int open;

	while ((c = *cp) != NULL) {
		for (s = c->sessions; s != NULL; s = next) {
			next = s->next;
			if (!s->exited)
				continue;
			open = s->out.fd >= 0 || s->err.fd >= 0;
			if (open && now < s->drain_deadline)
				continue;
			if (open)
				agent_log("stream %u: output still open %ds "
					  "after exit",
					  s->stream, AGENT_DRAIN_MS / 1000);
			agent_session_finish(s);
		}
		if (c->lost && c->sessions == NULL) {
			*cp = c->next;
			agent_conn_free(c);
			continue;
		}
		cp = &c->next;
	}
}

// Returns the epoll_wait timeout in milliseconds until the next deadline,
// or -1 when there is none.
static int agent_timeout(int64_t now)
{
	int64_t next = INT64_MAX;
	struct agent_conn *c;
	struct agent_session *s;

	if (!agent_st.active && agent_st.retry_at < next)
		next = agent_st.retry_at;
	if (agent_st.port_fd >= 0 && agent_st.port_conn == NULL &&
	    agent_st.port_resume_at < next)
		next = agent_st.port_resume_at;
	for (c = agent_st.conns; c != NULL; c = c->next) {
		for (s = c->sessions; s != NULL; s = s->next) {
			if (s->exited && s->drain_deadline < next)
				next = s->drain_deadline;
		}
	}
	if (next == INT64_MAX)
		return -1;
	if (next <= now)
		return 0;
	return next - now > INT_MAX ? INT_MAX : (int)(next - now);
}

static void agent_conn_read(struct agent_conn *c)
{
	static uint8_t buf[AGENT_CONN_READ];
	ssize_t n;
	int e;

	do {
		n = read(c->tag.fd, buf, sizeof(buf));
	} while (n < 0 && errno == EINTR);
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return;
	if (n < 0) {
		e = errno;
		agent_log("read: %s", strerror(e));
		agent_conn_lost(c);
		if (c->is_port && agent_st.port_fd >= 0)
			agent_port_fail(e);
		return;
	}
	if (n == 0) {
		if ((c->hdr_have > 0 && c->hdr_have < PROTO_HDR_LEN) ||
		    c->payload_have > 0)
			agent_log("read: unexpected EOF");
		agent_conn_lost(c);
		return;
	}
	if (agent_conn_feed(c, buf, (size_t)n) < 0)
		agent_conn_lost(c);
}

static void agent_dispatch(struct agent_tag *t, uint32_t ev)
{
	struct agent_conn *c;
	struct agent_session *s;

	switch (t->kind) {
	case AGENT_TAG_LISTENER:
		if (t->fd >= 0)
			agent_accept();
		break;
	case AGENT_TAG_SIGNAL:
		agent_reap();
		break;
	case AGENT_TAG_CONN:
		c = t->owner;
		if (c->lost)
			break;
		if (ev & EPOLLOUT)
			agent_conn_flush(c);
		if (!c->lost && (ev & (EPOLLIN | EPOLLHUP | EPOLLERR)))
			agent_conn_read(c);
		break;
	case AGENT_TAG_OUT:
	case AGENT_TAG_ERR:
		s = t->owner;
		if (s->tty && t->fd >= 0 && (ev & EPOLLOUT))
			agent_session_flush_stdin(s);
		if (t->fd < 0 || !(ev & (EPOLLIN | EPOLLHUP | EPOLLERR)))
			break;
		if (!s->conn->throttled) {
			agent_session_read(s, t);
		} else if (s->tty) {
			// Registered for stdin only, and the slave is gone.
			agent_outq_reset(&s->inq);
			agent_session_update(s);
			agent_conn_update(s->conn);
		}
		break;
	case AGENT_TAG_STDIN:
		if (t->fd >= 0)
			agent_session_flush_stdin(t->owner);
		break;
	}
}

// Opens /dev/null on any of 0, 1 and 2 that is closed, so that no session
// descriptor lands on a standard one.
static void agent_std_fds(void)
{
	int fd;

	for (fd = 0; fd < 3; fd++) {
		if (fcntl(fd, F_GETFD) < 0 && errno == EBADF)
			open("/dev/null", O_RDWR);
	}
}

int agent_main(void)
{
	struct epoll_event evs[AGENT_MAX_EVENTS];
	const char *path = getenv("PATH");
	sigset_t mask;
	int64_t now;
	int i, n;

	if (agent_plan(getenv("URUNIT_AGENT_TRANSPORT")) < 0)
		return 1;
	agent_std_fds();

	// The agent starts early in the boot with a near-empty environment.
	// execvpe resolves bare command names against the agent's own PATH.
	if (path == NULL || *path == '\0')
		setenv("PATH", AGENT_DEFAULT_PATH, 1);

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigprocmask(SIG_SETMASK, &mask, NULL);

	agent_st.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (agent_st.epoll_fd < 0) {
		agent_log("epoll_create1: %s", strerror(errno));
		return 1;
	}
	agent_st.sig.fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	if (agent_st.sig.fd < 0) {
		agent_log("signalfd: %s", strerror(errno));
		return 1;
	}
	agent_tag_set(&agent_st.sig, EPOLLIN);
	agent_st.retry_at = agent_now();

	for (;;) {
		now = agent_now();
		if (!agent_st.active && now >= agent_st.retry_at)
			agent_transport_setup();
		if (agent_st.port_fd >= 0 && agent_st.port_conn == NULL &&
		    now >= agent_st.port_resume_at)
			agent_port_resume();
		agent_sweep(now);

		n = epoll_wait(agent_st.epoll_fd, evs, AGENT_MAX_EVENTS,
			       agent_timeout(agent_now()));
		if (n < 0) {
			if (errno != EINTR) {
				agent_log("epoll_wait: %s", strerror(errno));
				usleep(AGENT_RETRY_MS * 1000);
			}
			continue;
		}
		for (i = 0; i < n; i++)
			agent_dispatch(evs[i].data.ptr, evs[i].events);
	}
}
