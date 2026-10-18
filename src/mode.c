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

// The mode lookup lives apart from urunit.c, which holds main(), so that the
// unit tests, which link everything but urunit.c, can call it.

#include <string.h>

#include "urunit.h"

enum urunit_mode urunit_mode_from(const char *argv0, const char *env_mode)
{
	const char *name;

	if (env_mode != NULL) {
		if (strcmp(env_mode, "init") == 0)
			return URUNIT_MODE_INIT;
		if (strcmp(env_mode, "vz-init") == 0)
			return URUNIT_MODE_VZ_INIT;
		if (strcmp(env_mode, "agent") == 0)
			return URUNIT_MODE_AGENT;
		if (strcmp(env_mode, "supervise") == 0)
			return URUNIT_MODE_SUPERVISE;
	}
	if (argv0 == NULL)
		return URUNIT_MODE_SUPERVISE;

	name = strrchr(argv0, '/');
	name = name != NULL ? name + 1 : argv0;
	if (strcmp(name, "init") == 0)
		return URUNIT_MODE_INIT;
	if (strcmp(name, "vz-init") == 0)
		return URUNIT_MODE_VZ_INIT;
	if (strcmp(name, "urunit-agent") == 0)
		return URUNIT_MODE_AGENT;
	return URUNIT_MODE_SUPERVISE;
}
