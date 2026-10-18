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

// The agent log goes to the console without ever blocking the agent. A
// slow console must not stall the exec sessions, so a line that does not
// fit is dropped and counted.

#ifndef URUNIT_AGENTLOG_H
#define URUNIT_AGENTLOG_H

#include <stdint.h>

// Opens a non-blocking file description of the console through
// /proc/self/fd/2. It falls back to fd 2 when that fails.
void agentlog_init(void);

// Writes one line prefixed with "urunit-agent: " in a single write(2). When
// lines were dropped before it, the line is preceded by
// "urunit-agent: dropped N log lines" in the same write.
void agent_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Returns the number of lines dropped so far.
uint64_t agentlog_dropped(void);

#endif // URUNIT_AGENTLOG_H
