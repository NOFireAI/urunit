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

// The data model of the exec agent in agent.c. It is declared here so that
// the unit tests in tests/test_agent.c can build a connection and drive its
// queue without the event loop.

#ifndef URUNIT_AGENT_H
#define URUNIT_AGENT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "proto.h"

// A connection stops reading session output while its output queue holds
// this many bytes or more.
#define AGENT_OUTQ_HIGH (1u << 20)

// The output queue never grows past this size. Output is throttled at
// AGENT_OUTQ_HIGH, so only one more frame can arrive above it.
#define AGENT_OUTQ_CAP (AGENT_OUTQ_HIGH + PROTO_MAX_PAYLOAD + PROTO_HDR_LEN)

// A connection stops reading frames while one of its sessions has this many
// bytes of stdin queued.
#define AGENT_STDIN_HIGH (1u << 20)
#define AGENT_STDIN_CAP (AGENT_STDIN_HIGH + PROTO_MAX_PAYLOAD)

// How long a session's exit report waits for the rest of its output.
#define AGENT_DRAIN_MS 2000

// The largest read from a session's output, and so the largest output frame.
#define AGENT_READ_CHUNK (32 * 1024)

// A bounded byte queue. The bytes waiting to be written are buf[head, tail).
struct agent_outq {
	uint8_t *buf;
	size_t head;
	size_t tail;
	size_t cap;
};

// Appends a and then b to q. Returns 0, or -1 when the queue would hold
// more than limit bytes or the allocation fails. Nothing is appended then.
int agent_outq_push(struct agent_outq *q, const void *a, size_t alen,
		    const void *b, size_t blen, size_t limit);

// Returns the number of bytes waiting in q.
size_t agent_outq_len(const struct agent_outq *q);

// Writes from q to the non-blocking fd until q is empty or fd is full.
// Returns the number of bytes written, or -1 with errno set on a write
// error other than EAGAIN or EINTR.
ssize_t agent_outq_flush(struct agent_outq *q, int fd);

// Drops the queued bytes and frees the buffer.
void agent_outq_reset(struct agent_outq *q);

// The kind of an epoll registration. The event loop dispatches on it.
enum agent_tag_kind {
	AGENT_TAG_LISTENER,
	AGENT_TAG_CONN,
	AGENT_TAG_OUT,
	AGENT_TAG_ERR,
	AGENT_TAG_STDIN,
	AGENT_TAG_SIGNAL,
};

// One descriptor in the epoll set. The epoll data points at the tag, and
// owner points at the listener, connection or session that holds it.
struct agent_tag {
	enum agent_tag_kind kind;
	int fd;
	uint32_t events;
	int registered;
	void *owner;
};

struct agent_conn;

// One running exec'd process. For a tty session, out is the pty master and
// it carries both the output and the stdin bytes. Otherwise out, err and in
// are the agent's ends of three pipes.
struct agent_session {
	uint32_t stream;
	pid_t pid;
	int tty;
	struct agent_tag out;
	struct agent_tag err;
	struct agent_tag in;
	int eio_retried;
	int exited;
	int exit_code;
	int64_t drain_deadline;
	struct agent_outq inq;
	int close_stdin_pending;
	struct agent_conn *conn;
	struct agent_session *next;
};

// One protocol peer. A socket connection owns its fd. A port connection
// (virtio-port and fd:<n>) shares the port fd with the next peer, so it
// never closes it.
struct agent_conn {
	struct agent_tag tag;
	int closed;
	int lost;
	int is_port;
	int throttled;
	struct agent_outq outq;
	uint8_t hdr[PROTO_HDR_LEN];
	size_t hdr_have;
	struct proto_hdr cur;
	uint8_t *payload;
	size_t payload_have;
	struct agent_session *sessions;
	struct agent_conn *next;
};

// Returns a new connection on fd, or NULL when out of memory. It is not
// registered with the event loop.
struct agent_conn *agent_conn_new(int fd, int is_port);

// Frees c and its parser state. It closes no descriptor and frees no
// session.
void agent_conn_free(struct agent_conn *c);

// Marks c closed after its peer is gone. Every session that has not exited
// yet gets SIGKILL, the queued output is dropped and every later frame is
// dropped too.
void agent_conn_mark_closed(struct agent_conn *c);

// Queues one frame for the peer of c and writes as much as the socket
// takes. Returns 0, or -1 when c is closed or its queue is full, and the
// frame is dropped then.
int agent_conn_enqueue_frame(struct agent_conn *c, uint8_t type,
			     uint32_t stream, const void *payload,
			     size_t len);

// Writes queued output to the peer of c. Returns 0, or -1 when the write
// failed and the peer is gone.
int agent_conn_flush(struct agent_conn *c);

// A guest identity for an exec session. resolved is 0 for an empty user
// spec, and the session then runs with the agent's own identity.
struct agent_user {
	int resolved;
	uid_t uid;
	gid_t gid;
	gid_t *groups;
	int ngroups;
	char *name;
	char *home;
	char *shell;
};

// Cuts spec in place at its first ':'. *uid_part is the text before it and
// *gid_part the text after it, or NULL without a ':'. Returns whether spec
// had a ':'.
int agent_split_user(char *spec, char **uid_part, char **gid_part);

// Resolves a user spec (name, uid or uid:gid) against the guest's passwd
// and group databases. Returns 0, or -1 with a message in err.
int agent_resolve_user(const char *spec, struct agent_user *u, char *err,
		       size_t errlen);

void agent_user_free(struct agent_user *u);

// Returns whether env, a NULL terminated array, has an entry for key.
int agent_env_has(char *const *env, const char *key);

// Returns a heap copy of env with PATH and TERM added when missing, and
// HOME, USER, LOGNAME and SHELL as well for a resolved user. Returns NULL
// when out of memory.
char **agent_build_env(char *const *env, const struct agent_user *u);

// Frees a NULL terminated array of heap strings.
void agent_free_strings(char **v);

// Returns the exit code reported for a wait status, which is the exit
// status or 128 plus the number of the signal that killed the process.
int agent_exit_code(int status);

#endif // URUNIT_AGENT_H
