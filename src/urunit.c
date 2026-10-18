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

#include <stdlib.h>

#include "agentlog.h"
#include "urunit.h"

int main(int argc, char **argv)
{
#ifdef __linux__
	enum urunit_mode mode;

	mode = urunit_mode_from(argc > 0 ? argv[0] : NULL,
				getenv("URUNIT_MODE"));
	switch (mode) {
	case URUNIT_MODE_INIT:
	case URUNIT_MODE_VZ_INIT:
		return init_main(mode, argc, argv);
	case URUNIT_MODE_AGENT:
		agentlog_init();
		return agent_main();
	case URUNIT_MODE_SUPERVISE:
	default:
		return supervise_main(argc, argv);
	}
#else
	// The early init and the agent are Linux only: elsewhere urunit is
	// the supervisor, whatever its name.
	return supervise_main(argc, argv);
#endif
}
