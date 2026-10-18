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

// Unit tests of the exec agent pieces that need no live peer: the user
// spec, the environment defaults, the output queue, the exit code, the
// non-blocking log and the closed connection rule. cmd/urunit/agent_test.go
// drives the whole binary over the protocol.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "agent.h"
#include "agentlog.h"
#include "harness.h"

static void test_split_user(void)
{
	char empty[] = "";
	char uid[] = "501";
	char both[] = "501:1000";
	char names[] = "name:group";
	char trailing[] = "501:";
	char *u, *g;

	CHECK_EQ_INT(agent_split_user(empty, &u, &g), 0);
	CHECK_EQ_STR(u, "");
	CHECK(g == NULL);

	CHECK_EQ_INT(agent_split_user(uid, &u, &g), 0);
	CHECK_EQ_STR(u, "501");
	CHECK(g == NULL);

	CHECK_EQ_INT(agent_split_user(both, &u, &g), 1);
	CHECK_EQ_STR(u, "501");
	CHECK_EQ_STR(g, "1000");

	CHECK_EQ_INT(agent_split_user(names, &u, &g), 1);
	CHECK_EQ_STR(u, "name");
	CHECK_EQ_STR(g, "group");

	CHECK_EQ_INT(agent_split_user(trailing, &u, &g), 1);
	CHECK_EQ_STR(u, "501");
	CHECK_EQ_STR(g, "");
}

static void test_resolve_user(void)
{
	struct agent_user u;
	char err[256];

	CHECK_EQ_INT(agent_resolve_user("", &u, err, sizeof(err)), 0);
	CHECK_EQ_INT(u.resolved, 0);
	agent_user_free(&u);

	CHECK_EQ_INT(agent_resolve_user(NULL, &u, err, sizeof(err)), 0);
	CHECK_EQ_INT(u.resolved, 0);
	agent_user_free(&u);

	// A uid that is not in /etc/passwd runs as uid=gid=n.
	CHECK_EQ_INT(agent_resolve_user("3999999", &u, err, sizeof(err)), 0);
	CHECK_EQ_INT(u.resolved, 1);
	CHECK_EQ_INT(u.uid, 3999999);
	CHECK_EQ_INT(u.gid, 3999999);
	CHECK_EQ_STR(u.name, "3999999");
	CHECK_EQ_STR(u.home, "/");
	CHECK_EQ_STR(u.shell, "/bin/sh");
	CHECK_EQ_INT(u.ngroups, 0);
	agent_user_free(&u);

	CHECK_EQ_INT(agent_resolve_user("3999999:42", &u, err, sizeof(err)),
		     0);
	CHECK_EQ_INT(u.uid, 3999999);
	CHECK_EQ_INT(u.gid, 42);
	agent_user_free(&u);

	CHECK_EQ_INT(agent_resolve_user("root", &u, err, sizeof(err)), 0);
	CHECK_EQ_INT(u.resolved, 1);
	CHECK_EQ_INT(u.uid, 0);
	CHECK_EQ_STR(u.name, "root");
	agent_user_free(&u);

	CHECK_EQ_INT(agent_resolve_user("0", &u, err, sizeof(err)), 0);
	CHECK_EQ_STR(u.name, "root");
	agent_user_free(&u);

	CHECK_EQ_INT(agent_resolve_user("no-such-user-urunit", &u, err,
					sizeof(err)),
		     -1);
	CHECK_EQ_STR(err, "unknown user \"no-such-user-urunit\"");

	CHECK_EQ_INT(agent_resolve_user("0:no-such-group-urunit", &u, err,
					sizeof(err)),
		     -1);
	CHECK_EQ_STR(err, "unknown group \"no-such-group-urunit\"");

	CHECK_EQ_INT(agent_resolve_user("0:", &u, err, sizeof(err)), -1);
	CHECK_EQ_STR(err, "unknown group \"\"");
}

static void test_env(void)
{
	char *req[] = { "FOO=1", "TERM=dumb", "HOME=/x", NULL };
	char *paths[] = { "PATH_EXTRA=1", NULL };
	struct agent_user u = { 0 };
	char **v;

	CHECK(agent_env_has(req, "FOO"));
	CHECK(agent_env_has(req, "TERM"));
	CHECK(!agent_env_has(req, "FO"));
	CHECK(!agent_env_has(req, "PATH"));
	CHECK(!agent_env_has(paths, "PATH"));
	CHECK(!agent_env_has(NULL, "PATH"));

	v = agent_build_env(req, NULL);
	CHECK(v != NULL);
	if (v != NULL) {
		CHECK_EQ_STR(v[0], "FOO=1");
		CHECK_EQ_STR(v[1], "TERM=dumb");
		CHECK_EQ_STR(v[2], "HOME=/x");
		CHECK_EQ_STR(v[3], "PATH=/usr/local/sbin:/usr/local/bin:"
				   "/usr/sbin:/usr/bin:/sbin:/bin");
		CHECK(v[4] == NULL);
	}
	agent_free_strings(v);

	u.resolved = 1;
	u.name = "alice";
	u.home = "/home/alice";
	u.shell = "/bin/zsh";
	v = agent_build_env(req, &u);
	CHECK(v != NULL);
	if (v != NULL) {
		CHECK_EQ_STR(v[3], "PATH=/usr/local/sbin:/usr/local/bin:"
				   "/usr/sbin:/usr/bin:/sbin:/bin");
		CHECK_EQ_STR(v[4], "USER=alice");
		CHECK_EQ_STR(v[5], "LOGNAME=alice");
		CHECK_EQ_STR(v[6], "SHELL=/bin/zsh");
		CHECK(v[7] == NULL);
	}
	agent_free_strings(v);

	v = agent_build_env(NULL, &u);
	CHECK(v != NULL);
	if (v != NULL) {
		CHECK_EQ_STR(v[1], "TERM=xterm-256color");
		CHECK_EQ_STR(v[2], "HOME=/home/alice");
		CHECK(v[6] == NULL);
	}
	agent_free_strings(v);
}

static void test_exit_code(void)
{
	// Linux wait status: exit code in bits 8-15, signal in bits 0-6.
	CHECK_EQ_INT(agent_exit_code(0), 0);
	CHECK_EQ_INT(agent_exit_code(3 << 8), 3);
	CHECK_EQ_INT(agent_exit_code(255 << 8), 255);
	CHECK_EQ_INT(agent_exit_code(SIGKILL), 128 + SIGKILL);
	CHECK_EQ_INT(agent_exit_code(SIGTERM), 128 + SIGTERM);
	// A core dump sets bit 7 and keeps the signal.
	CHECK_EQ_INT(agent_exit_code(SIGSEGV | 0x80), 128 + SIGSEGV);
}

// Reads everything fd has now. Returns the byte count.
static size_t drain_fd(int fd, uint8_t *buf, size_t cap)
{
	size_t total = 0;
	uint8_t scratch[4096];
	ssize_t n;

	for (;;) {
		n = read(fd, scratch, sizeof(scratch));
		if (n <= 0)
			return total;
		if (buf != NULL && total + (size_t)n <= cap)
			memcpy(buf + total, scratch, (size_t)n);
		total += (size_t)n;
	}
}

static void test_outq(void)
{
	enum { N = 200000 };
	struct agent_outq q = { 0 };
	uint8_t *src = malloc(N), *dst = malloc(N);
	size_t got = 0;
	ssize_t w;
	int p[2];
	int i, rounds;

	if (src == NULL || dst == NULL || pipe(p) < 0) {
		HARNESS_FAIL("setup: %s", strerror(errno));
		free(src);
		free(dst);
		return;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	fcntl(p[1], F_SETFL, O_NONBLOCK);
	for (i = 0; i < N; i++)
		src[i] = (uint8_t)(i * 7);

	CHECK_EQ_INT(agent_outq_push(&q, src, 10, src + 10, 20, 100), 0);
	CHECK_EQ_INT(agent_outq_len(&q), 30);
	// Over the limit: nothing is appended.
	CHECK_EQ_INT(agent_outq_push(&q, src + 30, 71, NULL, 0, 100), -1);
	CHECK_EQ_INT(agent_outq_len(&q), 30);
	CHECK_EQ_INT(agent_outq_push(&q, src + 30, N - 30, NULL, 0, N), 0);
	CHECK_EQ_INT(agent_outq_len(&q), N);

	// The pipe takes less than N, so the first flush is partial.
	w = agent_outq_flush(&q, p[1]);
	CHECK(w > 0 && w < N);
	CHECK_EQ_INT(agent_outq_len(&q), N - w);
	for (rounds = 0; agent_outq_len(&q) > 0 && rounds < 100; rounds++) {
		got += drain_fd(p[0], dst + got, N - got);
		CHECK(agent_outq_flush(&q, p[1]) >= 0);
	}
	got += drain_fd(p[0], dst + got, N - got);
	CHECK_EQ_INT(agent_outq_len(&q), 0);
	CHECK_EQ_INT(got, N);
	CHECK_MEM_EQ(src, dst, N);

	// A write error is reported and the bytes stay queued.
	CHECK_EQ_INT(agent_outq_push(&q, "abc", 3, NULL, 0, 100), 0);
	close(p[0]);
	signal(SIGPIPE, SIG_IGN);
	CHECK_EQ_INT(agent_outq_flush(&q, p[1]), -1);
	CHECK_EQ_INT(errno, EPIPE);
	CHECK_EQ_INT(agent_outq_len(&q), 3);
	signal(SIGPIPE, SIG_DFL);
	agent_outq_reset(&q);
	CHECK_EQ_INT(agent_outq_len(&q), 0);
	close(p[1]);
	free(src);
	free(dst);
}

static void test_conn_frames(void)
{
	static const uint8_t want[] = { PROTO_STDOUT, 0, 0, 0, 7, 0, 0, 0, 2,
					'h', 'i' };
	struct agent_conn *c;
	uint8_t buf[64];
	ssize_t n;
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) < 0) {
		HARNESS_FAIL("socketpair: %s", strerror(errno));
		return;
	}
	c = agent_conn_new(sv[0], 0);
	CHECK(c != NULL);
	if (c == NULL)
		return;

	CHECK_EQ_INT(agent_conn_enqueue_frame(c, PROTO_STDOUT, 7, "hi", 2), 0);
	n = read(sv[1], buf, sizeof(buf));
	CHECK_EQ_INT(n, sizeof(want));
	if (n == (ssize_t)sizeof(want))
		CHECK_MEM_EQ(buf, want, sizeof(want));

	// Late frames after the peer is gone are dropped.
	agent_conn_mark_closed(c);
	CHECK_EQ_INT(c->closed, 1);
	CHECK_EQ_INT(agent_conn_enqueue_frame(c, PROTO_STDOUT, 7, "hi", 2),
		     -1);
	CHECK_EQ_INT(agent_conn_enqueue_frame(c, PROTO_EXIT, 7, "{\"code\":0}",
					      10),
		     -1);
	CHECK_EQ_INT(agent_outq_len(&c->outq), 0);
	n = read(sv[1], buf, sizeof(buf));
	CHECK_EQ_INT(n, -1);
	CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

	agent_conn_free(c);
	close(sv[0]);
	close(sv[1]);
}

static void test_conn_high_water(void)
{
	static uint8_t chunk[AGENT_READ_CHUNK];
	struct agent_conn *c;
	int sv[2];
	int i, rounds;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) < 0) {
		HARNESS_FAIL("socketpair: %s", strerror(errno));
		return;
	}
	c = agent_conn_new(sv[0], 0);
	CHECK(c != NULL);
	if (c == NULL)
		return;

	// Nobody reads sv[1], so the queue grows past the high-water mark
	// once the socket buffer is full.
	for (i = 0; i < 1000 && !c->throttled; i++)
		CHECK_EQ_INT(agent_conn_enqueue_frame(c, PROTO_STDOUT, 1, chunk,
						      sizeof(chunk)),
			     0);
	CHECK_EQ_INT(c->throttled, 1);
	CHECK(agent_outq_len(&c->outq) >= AGENT_OUTQ_HIGH);
	CHECK(agent_outq_len(&c->outq) <= AGENT_OUTQ_CAP);

	for (rounds = 0; agent_outq_len(&c->outq) > 0 && rounds < 10000;
	     rounds++) {
		drain_fd(sv[1], NULL, 0);
		CHECK_EQ_INT(agent_conn_flush(c), 0);
	}
	CHECK_EQ_INT(agent_outq_len(&c->outq), 0);
	CHECK_EQ_INT(c->throttled, 0);

	agent_conn_free(c);
	close(sv[0]);
	close(sv[1]);
}

static void test_agentlog_drops(void)
{
	char buf[4096];
	uint64_t before;
	ssize_t n;
	int saved, probe, i;
	int p[2];

	if (pipe(p) < 0) {
		HARNESS_FAIL("pipe: %s", strerror(errno));
		return;
	}
	fflush(stderr);
	saved = dup(STDERR_FILENO);
	dup2(p[1], STDERR_FILENO);
	// Without a private non-blocking description, agent_log would block
	// on the full pipe.
	probe = open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK);
	if (probe < 0) {
		dup2(saved, STDERR_FILENO);
		close(saved);
		close(p[0]);
		close(p[1]);
		fprintf(stderr, "skip agentlog drops: no /proc/self/fd\n");
		return;
	}
	close(probe);
	agentlog_init();

	before = agentlog_dropped();
	for (i = 0; i < 100000 && agentlog_dropped() == before; i++)
		agent_log("fill line %06d", i);
	agent_log("dropped too");
	agent_log("dropped as well");

	fcntl(p[0], F_SETFL, O_NONBLOCK);
	drain_fd(p[0], NULL, 0);
	agent_log("after drain");
	n = read(p[0], buf, sizeof(buf) - 1);

	dup2(saved, STDERR_FILENO);
	close(saved);
	agentlog_init();

	CHECK_EQ_INT(agentlog_dropped() - before, 3);
	CHECK(n > 0);
	if (n > 0) {
		buf[n] = '\0';
		CHECK_EQ_STR(buf, "urunit-agent: dropped 3 log lines\n"
				  "urunit-agent: after drain\n");
	}
	close(p[0]);
	close(p[1]);
}

int run_agent_tests(void)
{
	int start = harness_failures;

	test_split_user();
	test_resolve_user();
	test_env();
	test_exit_code();
	test_outq();
	test_conn_frames();
	test_conn_high_water();
	test_agentlog_drops();
	return harness_failures - start;
}
