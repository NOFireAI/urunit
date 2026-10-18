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

// The agent writes its log straight to the console from the event loop.
// O_NONBLOCK on fd 2 itself would also change the console of the supervisor
// and the application, since they share the file description. So the agent
// opens /proc/self/fd/2, which gives it a description of its own.

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "agentlog.h"

#define AGENTLOG_PREFIX "urunit-agent: "
#define AGENTLOG_LINE_MAX 1024

static int agentlog_fd = STDERR_FILENO;
static int agentlog_owned;
static uint64_t agentlog_total;
// The lines dropped since the last line that got out.
static int agentlog_pending;

void agentlog_init(void)
{
	int fd;

	if (agentlog_owned) {
		close(agentlog_fd);
		agentlog_owned = 0;
	}
	agentlog_fd = STDERR_FILENO;
	fd = open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		agentlog_fd = fd;
		agentlog_owned = 1;
	}
}

void agent_log(const char *fmt, ...)
{
	char line[AGENTLOG_LINE_MAX + 64];
	size_t n = 0;
	size_t room;
	ssize_t w;
	int saved_errno = errno;
	int r;
	va_list ap;

	if (agentlog_pending > 0) {
		r = snprintf(line, sizeof(line),
			     AGENTLOG_PREFIX "dropped %d log lines\n",
			     agentlog_pending);
		if (r > 0)
			n = (size_t)r;
	}
	memcpy(line + n, AGENTLOG_PREFIX, strlen(AGENTLOG_PREFIX));
	n += strlen(AGENTLOG_PREFIX);

	// Keep one byte for the new line.
	room = sizeof(line) - n - 1;
	va_start(ap, fmt);
	r = vsnprintf(line + n, room, fmt, ap);
	va_end(ap);
	if (r < 0) {
		errno = saved_errno;
		return;
	}
	n += (size_t)r < room ? (size_t)r : room - 1;
	line[n++] = '\n';

	do {
		w = write(agentlog_fd, line, n);
	} while (w < 0 && errno == EINTR);
	if (w == (ssize_t)n) {
		agentlog_pending = 0;
	} else {
		agentlog_pending++;
		agentlog_total++;
	}
	errno = saved_errno;
}

uint64_t agentlog_dropped(void)
{
	return agentlog_total;
}
