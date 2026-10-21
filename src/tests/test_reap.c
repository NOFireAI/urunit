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

// reap() must return once the supervised app is gone, even when another child
// (a sidecar inherited by PID 1) is still running and ignores SIGTERM.

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "harness.h"
#include "urunit.h"

int reap(const pid_t child_pid, int *child_exitcode_ptr);

// A SIGTERM to the supervisor reaches the application, which dies of it, and
// reap() reports 128 + 15. As PID 1 the supervisor would otherwise drop it.
static void test_forward_sigterm(void)
{
	static const int sigs[] = { SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2 };
	int code = -1;
	pid_t app;
	size_t i;

	app = fork();
	if (app == 0) {
		for (;;)
			pause();
	}
	CHECK(app > 0);
	if (app < 0)
		return;
	forward_signals(app);
	CHECK_EQ_INT(raise(SIGTERM), 0);
	// Wait up to 5 s for the application to die, without reaping it, so
	// that a signal that was not forwarded fails here instead of blocking
	// reap() for ever.
	for (i = 0; i < 500; i++) {
		siginfo_t si;

		memset(&si, 0, sizeof(si));
		if (waitid(P_PID, (id_t)app, &si, WEXITED | WNOHANG | WNOWAIT) == 0 &&
		    si.si_pid == app)
			break;
		usleep(10000);
	}
	if (i == 500) {
		HARNESS_FAIL("the SIGTERM did not reach the application");
		kill(app, SIGKILL);
	}
	CHECK_EQ_INT(reap(app, &code), 0);
	CHECK_EQ_INT(code, i == 500 ? 128 + SIGKILL : 128 + SIGTERM);

	forward_signals(0);
	for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		signal(sigs[i], SIG_DFL);
}

static pid_t stand_in_agent;
static int sync_calls;
static int agent_signalled_at_sync = -1;

// agent_signalled reports whether pid has a SIGTERM pending or has already
// died of it. kill(2) marks the signal pending before it returns, so either
// shows a stop_agent() that ran first.
static int agent_signalled(pid_t pid)
{
	char path[64], line[256];
	unsigned long long mask;
	int signalled = 0;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return 1;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "State:", 6) == 0 && strchr(line, 'Z'))
			signalled = 1;
		if ((sscanf(line, "SigPnd: %llx", &mask) == 1 ||
		     sscanf(line, "ShdPnd: %llx", &mask) == 1) &&
		    (mask & (1ULL << (SIGTERM - 1))))
			signalled = 1;
	}
	fclose(f);
	return signalled;
}

static void record_sync(void)
{
	sync_calls++;
	agent_signalled_at_sync = agent_signalled(stand_in_agent);
}

// The guest flushes the application's writes before it stops the agent. The
// end of the agent's sessions can make urunc stop the monitor, so a sync after
// it can come too late.
static void test_sync_before_agent_stops(void)
{
	void (*saved)(void) = urunit_sync;
	int code = -1;
	int status = 0;
	pid_t app;

	stand_in_agent = fork();
	if (stand_in_agent == 0) {
		for (;;)
			pause();
	}
	CHECK(stand_in_agent > 0);
	if (stand_in_agent < 0)
		return;
	app = fork();
	if (app == 0)
		_exit(0);
	CHECK(app > 0);

	set_agent_pid(stand_in_agent);
	urunit_sync = record_sync;
	alarm(10);
	CHECK_EQ_INT(reap(app, &code), 0);
	alarm(0);
	urunit_sync = saved;
	set_agent_pid(0);

	CHECK_EQ_INT(code, 0);
	CHECK_EQ_INT(sync_calls, 1);
	CHECK_EQ_INT(agent_signalled_at_sync, 0);
	// stop_agent() still ran after the sync.
	CHECK_EQ_INT(waitpid(stand_in_agent, &status, 0), stand_in_agent);
	CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
}

int run_reap_tests(void)
{
	int start = harness_failures;
	int code = -1;
	int status = 0;
	pid_t sidecar, app, done;

	test_forward_sigterm();
	test_sync_before_agent_stops();

	sidecar = fork();
	if (sidecar == 0) {
		signal(SIGTERM, SIG_IGN);
		for (;;)
			pause();
	}
	CHECK(sidecar > 0);
	if (sidecar < 0)
		return harness_failures - start;

	app = fork();
	if (app == 0)
		_exit(7);
	CHECK(app > 0);

	// Without the early return reap() blocks on the sidecar for ever, so
	// bound the test with an alarm (default action terminates us).
	alarm(10);
	CHECK_EQ_INT(reap(app, &code), 0);
	alarm(0);
	CHECK_EQ_INT(code, 7);

	// The sidecar must still be alive: reap() did not wait for it.
	CHECK_EQ_INT(waitpid(sidecar, &status, WNOHANG), 0);
	kill(sidecar, SIGKILL);
	done = waitpid(sidecar, &status, 0);
	CHECK_EQ_INT(done, sidecar);

	return harness_failures - start;
}
