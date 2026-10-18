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

// The system calls behind struct sysops. The file depends on nothing else
// in urunit, so the early init can be tested and built on its own.

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#include "sysops.h"

static int sysops_mount(const char *src, const char *dst, const char *type,
			unsigned long flags, const char *data)
{
	return mount(src, dst, type, flags, data);
}

static int sysops_umount(const char *dst)
{
	return umount2(dst, 0);
}

static int sysops_mkdir_p(const char *path, mode_t mode)
{
	char buf[PATH_MAX];
	size_t len = strlen(path);
	size_t i;

	if (len == 0 || len >= sizeof(buf)) {
		errno = len == 0 ? ENOENT : ENAMETOOLONG;
		return -1;
	}
	memcpy(buf, path, len + 1);
	for (i = 1; i <= len; i++) {
		if (buf[i] != '/' && buf[i] != '\0')
			continue;
		if (buf[i - 1] == '/')
			continue;
		buf[i] = '\0';
		if (mkdir(buf, mode) != 0 && errno != EEXIST)
			return -1;
		if (i < len)
			buf[i] = '/';
	}
	return 0;
}

static int sysops_chmod(const char *path, mode_t mode)
{
	return chmod(path, mode);
}

static int sysops_write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len > 0) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int sysops_close(int fd, int rc)
{
	int saved = errno;

	if (close(fd) != 0 && rc == 0)
		return -1;
	errno = saved;
	return rc;
}

static int sysops_copy_file(const char *src, const char *dst, mode_t mode)
{
	char buf[65536];
	int in;
	int out;
	int rc = 0;

	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return -1;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (out < 0)
		return sysops_close(in, -1);
	for (;;) {
		ssize_t n = read(in, buf, sizeof(buf));

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			rc = n < 0 ? -1 : 0;
			break;
		}
		if (sysops_write_all(out, buf, (size_t)n) != 0) {
			rc = -1;
			break;
		}
	}
	// open() applies the umask and leaves an existing file's mode alone.
	if (rc == 0)
		rc = fchmod(out, mode);
	rc = sysops_close(out, rc);
	return sysops_close(in, rc);
}

// Writes buf with one write() where the kernel takes it, as the
// binfmt_misc register file needs.
static int sysops_write_file(const char *path, const void *buf, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	if (fd < 0)
		return -1;
	return sysops_close(fd, sysops_write_all(fd, buf, len));
}

// Replaces an existing entry, which a persistent root keeps from an
// earlier boot.
static int sysops_symlink(const char *target, const char *path)
{
	if (symlink(target, path) == 0)
		return 0;
	if (errno != EEXIST || unlink(path) != 0)
		return -1;
	return symlink(target, path);
}

static int sysops_set_clock(time_t epoch)
{
	struct timespec ts;

	ts.tv_sec = epoch;
	ts.tv_nsec = 0;
	return clock_settime(CLOCK_REALTIME, &ts);
}

static int sysops_setenv(const char *key, const char *val)
{
	return setenv(key, val, 1);
}

static int sysops_access_x(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return -1;
	if (S_ISDIR(st.st_mode)) {
		errno = EISDIR;
		return -1;
	}
	return access(path, X_OK);
}

// Reads until EOF. procfs files such as /proc/cmdline and /proc/net/pnp
// report st_size 0, so the size is only the first guess.
static char *sysops_read_file(const char *path, size_t *len)
{
	struct stat st;
	size_t cap = 4096;
	size_t used = 0;
	char *buf;
	int fd;

	if (len != NULL)
		*len = 0;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) == 0 && st.st_size > 0)
		cap = (size_t)st.st_size + 1;
	buf = malloc(cap);
	if (buf == NULL) {
		sysops_close(fd, -1);
		return NULL;
	}
	for (;;) {
		ssize_t n;

		if (used + 1 >= cap) {
			char *grown = realloc(buf, cap * 2);

			if (grown == NULL)
				break;
			buf = grown;
			cap *= 2;
		}
		n = read(fd, buf + used, cap - used - 1);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			free(buf);
			sysops_close(fd, -1);
			return NULL;
		}
		if (n == 0)
			break;
		used += (size_t)n;
	}
	buf[used] = '\0';
	sysops_close(fd, 0);
	if (len != NULL)
		*len = used;
	return buf;
}

static void sysops_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

// Deletes everything below the directory fd, which it takes over. Entries
// on another device are mount points, such as the lower root of an overlay
// or the Rosetta share, and are left alone.
void sysops_rm_tree(int fd, dev_t rootdev)
{
	DIR *dir = fdopendir(fd);
	struct dirent *d;

	if (dir == NULL) {
		close(fd);
		return;
	}
	while ((d = readdir(dir)) != NULL) {
		struct stat st;
		int dfd = dirfd(dir);

		if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
			continue;
		if (fstatat(dfd, d->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			continue;
		if (st.st_dev != rootdev)
			continue;
		if (S_ISDIR(st.st_mode)) {
			int sub = openat(dfd, d->d_name,
					 O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
						 O_CLOEXEC);

			if (sub >= 0) {
				struct stat sst;

				// Check again on the open directory, in case a
				// mount appeared since fstatat.
				if (fstat(sub, &sst) != 0 ||
				    sst.st_dev != rootdev) {
					close(sub);
					continue;
				}
				sysops_rm_tree(sub, rootdev);
			}
			unlinkat(dfd, d->d_name, AT_REMOVEDIR);
		} else {
			unlinkat(dfd, d->d_name, 0);
		}
	}
	closedir(dir);
}

int sysops_switch_root_real(const char *newroot)
{
	struct statfs sfs;
	struct stat root_st;
	struct stat new_st;
	int oldroot;
	int fd;

	// Deleting the old root is only right for an initramfs.
	if (statfs("/", &sfs) != 0)
		return -1;
	if ((unsigned long)sfs.f_type != (unsigned long)RAMFS_MAGIC &&
	    (unsigned long)sfs.f_type != (unsigned long)TMPFS_MAGIC) {
		errno = EINVAL;
		return -1;
	}
	if (stat("/", &root_st) != 0 || stat(newroot, &new_st) != 0)
		return -1;
	if (new_st.st_dev == root_st.st_dev) {
		// newroot is a plain directory of the initramfs, so the
		// deletion below would take it too.
		errno = EINVAL;
		return -1;
	}

	oldroot = open("/", O_DIRECTORY | O_RDONLY | O_CLOEXEC);
	if (oldroot < 0)
		return -1;
	if (chdir(newroot) != 0 || mount(".", "/", NULL, MS_MOVE, NULL) != 0 ||
	    chroot(".") != 0 || chdir("/") != 0)
		return sysops_close(oldroot, -1);

	fd = dup(oldroot);
	if (fd >= 0)
		sysops_rm_tree(fd, root_st.st_dev);
	close(oldroot);
	return 0;
}

const struct sysops sysops_real = {
	.mount = sysops_mount,
	.umount = sysops_umount,
	.mkdir_p = sysops_mkdir_p,
	.chmod = sysops_chmod,
	.copy_file = sysops_copy_file,
	.write_file = sysops_write_file,
	.symlink = sysops_symlink,
	.set_clock = sysops_set_clock,
	.setenv = sysops_setenv,
	.access_x = sysops_access_x,
	.read_file = sysops_read_file,
	.switch_root = sysops_switch_root_real,
	.log = sysops_log,
};
