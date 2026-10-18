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

// urunit is the only guest binary of a generic container boot. It runs as
// the early init of the initrd, as the supervisor (PID 1) of the container
// application and as the exec agent. The name it runs under selects the
// role: /init and /vz-init are symlinks to /urunit in the initrd, and the
// supervisor starts the agent with fork() and no exec.
//
// Symbol names. Every non-static symbol of the new files carries the prefix
// of its file: init_, sysops_, agent_, proto_, agentlog_ or urunit_. The
// supervisor and the platform code (main.c, common.c, linux.c, freebsd.c)
// keep their original names, such as read_file_and_size, parse_envs or
// mkdir_all.

#ifndef URUNIT_H
#define URUNIT_H

#include <sys/types.h>

enum urunit_mode {
	URUNIT_MODE_SUPERVISE,
	URUNIT_MODE_INIT,
	URUNIT_MODE_VZ_INIT,
	URUNIT_MODE_AGENT,
};

// The directory of the new root where init places a copy of the binary and
// the urunit.conf it received.
static const char URUNIT_RUN_DIR[] = "/run/urunc";
static const char URUNIT_STAGED_BIN[] = "/run/urunc/urunit";
static const char URUNIT_STAGED_CONF[] = "/run/urunc/urunit.conf";

// Returns the mode for the program name argv0 and the value of URUNIT_MODE.
// A known env_mode (init, vz-init, agent, supervise) wins. Otherwise the
// base name of argv0 decides: init, vz-init and urunit-agent select their
// mode and any other name selects URUNIT_MODE_SUPERVISE.
enum urunit_mode urunit_mode_from(const char *argv0, const char *env_mode);

// Runs the early init for mode and then the supervisor in the same process.
// It returns only on failure, with a non-zero value. As PID 1, main exits
// with it and the kernel panics.
int init_main(enum urunit_mode mode, int argc, char **argv);

// Runs argv[1..] as the container application and supervises it. It
// returns only on error. Otherwise it reboots or powers off the guest.
int supervise_main(int argc, char **argv);

// Serves exec sessions until the process is killed. It returns non-zero
// only when no transport can ever be set up.
int agent_main(void);

// Forks the exec agent. The child calls agent_main() in a new process group
// and exits with its result. Returns the child's pid, or -1 on failure.
// Defined in main.c.
pid_t spawn_agent(void);

#endif // URUNIT_H
