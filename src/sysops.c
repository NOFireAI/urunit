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

// Stub: the early-init task fills this file in. Only log does real work.

#include <stdarg.h>
#include <stdio.h>

#include "sysops.h"

static int sysops_stub_mount(const char *src, const char *dst,
			     const char *type, unsigned long flags,
			     const char *data)
{
	(void)src;
	(void)dst;
	(void)type;
	(void)flags;
	(void)data;
	return -1;
}

static int sysops_stub_umount(const char *dst)
{
	(void)dst;
	return -1;
}

static int sysops_stub_path_mode(const char *path, mode_t mode)
{
	(void)path;
	(void)mode;
	return -1;
}

static int sysops_stub_copy_file(const char *src, const char *dst,
				 mode_t mode)
{
	(void)src;
	(void)dst;
	(void)mode;
	return -1;
}

static int sysops_stub_write_file(const char *path, const void *buf,
				  size_t len)
{
	(void)path;
	(void)buf;
	(void)len;
	return -1;
}

static int sysops_stub_symlink(const char *target, const char *path)
{
	(void)target;
	(void)path;
	return -1;
}

static int sysops_stub_set_clock(time_t epoch)
{
	(void)epoch;
	return -1;
}

static int sysops_stub_setenv(const char *key, const char *val)
{
	(void)key;
	(void)val;
	return -1;
}

static int sysops_stub_access_x(const char *path)
{
	(void)path;
	return -1;
}

static char *sysops_stub_read_file(const char *path, size_t *len)
{
	(void)path;
	if (len != NULL)
		*len = 0;
	return NULL;
}

static void sysops_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

int sysops_switch_root_real(const char *newroot)
{
	(void)newroot;
	return -1;
}

const struct sysops sysops_real = {
	.mount = sysops_stub_mount,
	.umount = sysops_stub_umount,
	.mkdir_p = sysops_stub_path_mode,
	.chmod = sysops_stub_path_mode,
	.copy_file = sysops_stub_copy_file,
	.write_file = sysops_stub_write_file,
	.symlink = sysops_stub_symlink,
	.set_clock = sysops_stub_set_clock,
	.setenv = sysops_stub_setenv,
	.access_x = sysops_stub_access_x,
	.read_file = sysops_stub_read_file,
	.switch_root = sysops_switch_root_real,
	.log = sysops_log,
};
