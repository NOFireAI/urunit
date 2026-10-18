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

// The early init reaches the system only through struct sysops. Production
// uses sysops_real. The unit tests pass a fake that records every call and
// serves files from a table.

#ifndef URUNIT_SYSOPS_H
#define URUNIT_SYSOPS_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "urunit.h"

struct sysops {
	int (*mount)(const char *src, const char *dst, const char *type,
		     unsigned long flags, const char *data);
	int (*umount)(const char *dst);
	int (*mkdir_p)(const char *path, mode_t mode);
	int (*chmod)(const char *path, mode_t mode);
	int (*copy_file)(const char *src, const char *dst, mode_t mode);
	int (*write_file)(const char *path, const void *buf, size_t len);
	int (*symlink)(const char *target, const char *path);
	int (*set_clock)(time_t epoch);
	int (*setenv)(const char *key, const char *val);
	// Returns 0 when path exists and is executable.
	int (*access_x)(const char *path);
	// Returns the contents of path in a heap buffer with a terminating NUL
	// that len does not count, or NULL when the file cannot be read.
	char *(*read_file)(const char *path, size_t *len);
	int (*switch_root)(const char *newroot);
	void (*log)(const char *fmt, ...)
		__attribute__((format(printf, 1, 2)));
};

extern const struct sysops sysops_real;

// Runs the early init sequence of mode through ops. cmdline is the text of
// /proc/cmdline. When it is NULL, init_run reads the file through
// ops->read_file. On success it returns 0 and sets *out_argv to a heap
// allocated, NULL terminated argv for supervise_main: "urunit", then the
// resolved command and its arguments. On failure it logs the reason and
// returns non-zero.
int init_run(const struct sysops *ops, enum urunit_mode mode, int argc,
	     char **argv, const char *cmdline, int *out_argc,
	     char ***out_argv);

// Moves newroot onto / and deletes the old initramfs, like switch_root(8).
// It refuses to run when / is not a ramfs or tmpfs.
int sysops_switch_root_real(const char *newroot);

#endif // URUNIT_SYSOPS_H
