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

// Stub: the early-init task fills this file in.

#include <stdio.h>

#include "sysops.h"
#include "urunit.h"

int init_run(const struct sysops *ops, enum urunit_mode mode, int argc,
	     char **argv, const char *cmdline, int *out_argc,
	     char ***out_argv)
{
	(void)mode;
	(void)argc;
	(void)argv;
	(void)cmdline;
	(void)out_argc;
	(void)out_argv;
	ops->log("init: not implemented\n");
	return -1;
}

int init_main(enum urunit_mode mode, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "%s: early init is not implemented\n",
		mode == URUNIT_MODE_VZ_INIT ? "vz-init" : "container-init");
	return 1;
}
