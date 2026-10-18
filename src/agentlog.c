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

// Stub: the agent-loop task fills this file in. This version writes every
// line to fd 2 and never drops one.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "agentlog.h"

void agentlog_init(void)
{
}

void agent_log(const char *fmt, ...)
{
	char line[1024];
	size_t n;
	int r;
	va_list ap;

	memcpy(line, "urunit-agent: ", 14);
	va_start(ap, fmt);
	r = vsnprintf(line + 14, sizeof(line) - 15, fmt, ap);
	va_end(ap);
	if (r < 0)
		return;
	n = 14 + ((size_t)r < sizeof(line) - 15 ? (size_t)r
						  : sizeof(line) - 16);
	line[n++] = '\n';
	if (write(STDERR_FILENO, line, n) < 0)
		return;
}

uint64_t agentlog_dropped(void)
{
	return 0;
}
