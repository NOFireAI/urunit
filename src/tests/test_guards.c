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

// The fallbacks that keep a failure harmless: setup_controlling_tty() leaves
// the process alone when it cannot find the console, switch_root refuses a
// root that is not an initramfs, and rm_tree stays on its own device.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/magic.h>

#include "common.h"
#include "harness.h"
#include "sysops.h"

// tty_fallback_child: Points stdin at fd and checks that
// setup_controlling_tty() fails without changing the session or stdio.
// Returns the child's exit code: 0 when everything held.
static int tty_fallback_child(int fd)
{
	struct stat before[3], after[3];
	pid_t sid;
	int i;

	if (dup2(fd, STDIN_FILENO) < 0)
		return 10;
	sid = getsid(0);
	for (i = 0; i < 3; i++)
		if (fstat(i, &before[i]) != 0)
			return 11;
	if (setup_controlling_tty() != -1)
		return 12;
	if (getsid(0) != sid)
		return 13;
	for (i = 0; i < 3; i++) {
		if (fstat(i, &after[i]) != 0 ||
		    after[i].st_dev != before[i].st_dev ||
		    after[i].st_ino != before[i].st_ino)
			return 14;
	}
	return 0;
}

static void check_tty_fallback(const char *what, int fd)
{
	int status = 0;
	pid_t pid;

	pid = fork();
	if (pid == 0)
		_exit(tty_fallback_child(fd));
	CHECK(pid > 0);
	if (pid < 0)
		return;
	CHECK_EQ_INT(waitpid(pid, &status, 0), pid);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		HARNESS_FAIL("%s: tty fallback child status 0x%x", what, status);
}

static void test_tty_fallback(void)
{
	int fd;

	// stdin is not a terminal.
	fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	CHECK(fd >= 0);
	if (fd >= 0) {
		check_tty_fallback("/dev/null", fd);
		close(fd);
	}

	// stdin is a pty slave, which matches none of the console devices.
	fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd >= 0 && grantpt(fd) == 0 && unlockpt(fd) == 0) {
		int slave = open(ptsname(fd), O_RDWR | O_NOCTTY | O_CLOEXEC);

		CHECK(slave >= 0);
		if (slave >= 0) {
			check_tty_fallback("pty slave", slave);
			close(slave);
		}
	}
	if (fd >= 0)
		close(fd);
}

static void test_switch_root_refuses_disk_root(void)
{
	char dir[] = "/tmp/urunit-guards-XXXXXX";
	char file[sizeof(dir) + 8];
	struct statfs sfs;

	// Only meaningful, and only safe, where / is not an initramfs.
	CHECK_EQ_INT(statfs("/", &sfs), 0);
	if ((unsigned long)sfs.f_type == (unsigned long)RAMFS_MAGIC ||
	    (unsigned long)sfs.f_type == (unsigned long)TMPFS_MAGIC) {
		printf("guards: / is an initramfs, switch_root case skipped\n");
		return;
	}
	CHECK(mkdtemp(dir) != NULL);
	snprintf(file, sizeof(file), "%s/keep", dir);
	CHECK(close(open(file, O_CREAT | O_WRONLY | O_CLOEXEC, 0600)) == 0);

	errno = 0;
	CHECK_EQ_INT(sysops_switch_root_real(dir), -1);
	CHECK_EQ_INT(errno, EINVAL);
	CHECK_EQ_INT(access(file, F_OK), 0);

	unlink(file);
	rmdir(dir);

	// A newroot on another device passes the mount point check, so only the
	// initramfs check stops it. Without root the move would fail with EPERM
	// anyway, which keeps the case harmless and tells the two apart.
	if (geteuid() != 0) {
		struct stat root, shm;

		if (stat("/", &root) == 0 && stat("/dev/shm", &shm) == 0 &&
		    shm.st_dev != root.st_dev) {
			errno = 0;
			CHECK_EQ_INT(sysops_switch_root_real("/dev/shm"), -1);
			CHECK_EQ_INT(errno, EINVAL);
		}
	}
}

static void test_rm_tree_stays_on_device(void)
{
	char dir[] = "/tmp/urunit-guards-XXXXXX";
	char sub[sizeof(dir) + 8];
	char file[sizeof(dir) + 16];
	char top[sizeof(dir) + 8];
	struct stat st;
	int fd;

	CHECK(mkdtemp(dir) != NULL);
	snprintf(sub, sizeof(sub), "%s/sub", dir);
	snprintf(file, sizeof(file), "%s/sub/keep", dir);
	snprintf(top, sizeof(top), "%s/top", dir);
	CHECK_EQ_INT(mkdir(sub, 0700), 0);
	CHECK(close(open(file, O_CREAT | O_WRONLY | O_CLOEXEC, 0600)) == 0);
	CHECK(close(open(top, O_CREAT | O_WRONLY | O_CLOEXEC, 0600)) == 0);
	CHECK_EQ_INT(stat(dir, &st), 0);

	// Every entry is on another device than the one given: nothing goes.
	fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd >= 0);
	sysops_rm_tree(fd, st.st_dev + 1);
	CHECK_EQ_INT(access(top, F_OK), 0);
	CHECK_EQ_INT(access(file, F_OK), 0);

	// On its own device the whole tree goes, and dir itself stays.
	fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd >= 0);
	sysops_rm_tree(fd, st.st_dev);
	CHECK(access(top, F_OK) != 0);
	CHECK(access(sub, F_OK) != 0);
	CHECK_EQ_INT(access(dir, F_OK), 0);

	rmdir(dir);
}

int run_guard_tests(void)
{
	int start = harness_failures;

	test_tty_fallback();
	test_switch_root_refuses_disk_root();
	test_rm_tree_stays_on_device();

	return harness_failures - start;
}
