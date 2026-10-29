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

// Tests of the early init. A recording struct sysops writes every call
// that changes the system as one line and serves files from a table, so
// each scenario checks the whole sequence of calls.

#define _GNU_SOURCE

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>

#include <linux/reboot.h>

#include "common.h"
#include "harness.h"
#include "sysops.h"
#include "urunit.h"

// The record of container-init line 68, as `printf '%s'` writes it.
static const char rosetta_expected[] =
	":rosetta:M::\\x7fELF\\x02\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00"
	"\\x00\\x00\\x00\\x02\\x00\\x3e\\x00:\\xff\\xff\\xff\\xff\\xff\\xfe"
	"\\xfe\\x00\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xfe\\xff\\xff"
	"\\xff:/mnt/rosetta/rosetta:OCF";

#define FAKE_MAX 32

struct fake_file {
	const char *path;
	const char *data;
	int exec;
};

struct fake_write {
	char *path;
	char *data;
	size_t len;
};

static char fake_calls[16384];
static char fake_console[4096];
static struct fake_file fake_fs[FAKE_MAX];
static size_t fake_nfs;
static struct fake_write fake_writes[FAKE_MAX];
static size_t fake_nwrites;
static const char *fake_fail[4];

static void fake_reset(void)
{
	size_t i;

	fake_calls[0] = '\0';
	fake_console[0] = '\0';
	fake_nfs = 0;
	for (i = 0; i < fake_nwrites; i++) {
		free(fake_writes[i].path);
		free(fake_writes[i].data);
	}
	fake_nwrites = 0;
	memset(fake_fail, 0, sizeof(fake_fail));
}

static void fake_add(const char *path, const char *data, int exec)
{
	fake_fs[fake_nfs].path = path;
	fake_fs[fake_nfs].data = data;
	fake_fs[fake_nfs].exec = exec;
	fake_nfs++;
}

// Appends one call to the record. Returns -1 with errno set when the line
// starts with one of the fake_fail prefixes.
static int fake_record(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static int fake_record(const char *fmt, ...)
{
	size_t used = strlen(fake_calls);
	char *line = fake_calls + used;
	va_list ap;
	size_t i;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(fake_calls) - used - 1, fmt, ap);
	va_end(ap);
	strcat(fake_calls, "\n");
	for (i = 0; i < sizeof(fake_fail) / sizeof(fake_fail[0]); i++) {
		if (fake_fail[i] != NULL &&
		    strncmp(line, fake_fail[i], strlen(fake_fail[i])) == 0) {
			errno = EIO;
			return -1;
		}
	}
	return 0;
}

static const char *fake_flags(unsigned long flags, char *buf, size_t cap)
{
	static const struct {
		unsigned long flag;
		const char *name;
	} names[] = {
		{ MS_RDONLY, "ro" },     { MS_NOSUID, "nosuid" },
		{ MS_NODEV, "nodev" },   { MS_NOEXEC, "noexec" },
		{ MS_NOATIME, "noatime" }, { MS_MOVE, "move" },
	};
	size_t i;

	buf[0] = '\0';
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!(flags & names[i].flag))
			continue;
		if (buf[0] != '\0')
			strncat(buf, ",", cap - strlen(buf) - 1);
		strncat(buf, names[i].name, cap - strlen(buf) - 1);
		flags &= ~names[i].flag;
	}
	if (flags != 0)
		snprintf(buf + strlen(buf), cap - strlen(buf), "+%#lx", flags);
	if (buf[0] == '\0')
		snprintf(buf, cap, "0");
	return buf;
}

#define NUL_DASH(s) ((s) != NULL ? (s) : "-")

static int fake_mount(const char *src, const char *dst, const char *type,
		      unsigned long flags, const char *data)
{
	char fl[64];

	return fake_record("mount %s %s %s %s %s", NUL_DASH(src),
			   NUL_DASH(dst), NUL_DASH(type),
			   fake_flags(flags, fl, sizeof(fl)), NUL_DASH(data));
}

static int fake_umount(const char *dst)
{
	return fake_record("umount %s", dst);
}

static int fake_mkdir_p(const char *path, mode_t mode)
{
	return fake_record("mkdir_p %s %o", path, (unsigned)mode);
}

static int fake_chmod(const char *path, mode_t mode)
{
	return fake_record("chmod %s %o", path, (unsigned)mode);
}

static int fake_copy_file(const char *src, const char *dst, mode_t mode)
{
	return fake_record("copy %s %s %o", src, dst, (unsigned)mode);
}

static int fake_write_file(const char *path, const void *buf, size_t len)
{
	size_t i;

	if (fake_record("write %s %zu", path, len) != 0)
		return -1;
	for (i = 0; i < fake_nwrites; i++)
		if (strcmp(fake_writes[i].path, path) == 0)
			break;
	if (i == fake_nwrites) {
		fake_writes[i].path = strdup(path);
		fake_nwrites++;
	} else {
		free(fake_writes[i].data);
	}
	fake_writes[i].data = malloc(len + 1);
	memcpy(fake_writes[i].data, buf, len);
	fake_writes[i].data[len] = '\0';
	fake_writes[i].len = len;
	return 0;
}

static int fake_symlink(const char *target, const char *path)
{
	return fake_record("symlink %s %s", target, path);
}

static int fake_set_clock(time_t epoch)
{
	return fake_record("set_clock %lld", (long long)epoch);
}

static int fake_setenv(const char *key, const char *val)
{
	return fake_record("setenv %s %s", key, val);
}

static int fake_switch_root(const char *path)
{
	return fake_record("switch_root %s", path);
}

static const struct fake_file *fake_lookup(const char *path)
{
	size_t i;

	for (i = 0; i < fake_nfs; i++)
		if (strcmp(fake_fs[i].path, path) == 0)
			return &fake_fs[i];
	return NULL;
}

static int fake_access_x(const char *path)
{
	const struct fake_file *f = fake_lookup(path);

	if (f == NULL || !f->exec) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

static char *fake_read_file(const char *path, size_t *len)
{
	const struct fake_file *f = fake_lookup(path);

	if (len != NULL)
		*len = 0;
	if (f == NULL || f->exec) {
		errno = ENOENT;
		return NULL;
	}
	if (len != NULL)
		*len = strlen(f->data);
	return strdup(f->data);
}

static void fake_log(const char *fmt, ...)
{
	size_t used = strlen(fake_console);
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(fake_console + used, sizeof(fake_console) - used, fmt, ap);
	va_end(ap);
}

static int fake_exists(const char *path)
{
	return fake_lookup(path) != NULL ? 0 : -1;
}

static void fake_nap(unsigned int ms)
{
	fake_record("nap %u", ms);
}

static const struct sysops fake_ops = {
	.mount = fake_mount,
	.umount = fake_umount,
	.mkdir_p = fake_mkdir_p,
	.chmod = fake_chmod,
	.copy_file = fake_copy_file,
	.write_file = fake_write_file,
	.symlink = fake_symlink,
	.set_clock = fake_set_clock,
	.setenv = fake_setenv,
	.access_x = fake_access_x,
	.exists = fake_exists,
	.nap = fake_nap,
	.read_file = fake_read_file,
	.switch_root = fake_switch_root,
	.log = fake_log,
};

static const struct fake_write *fake_written(const char *path)
{
	size_t i;

	for (i = 0; i < fake_nwrites; i++)
		if (strcmp(fake_writes[i].path, path) == 0)
			return &fake_writes[i];
	return NULL;
}

// Compares the recorded calls with expected, a NULL terminated list, and
// reports the first line that differs.
static void check_calls(const char *const *expected, int line)
{
	const char *p = fake_calls;
	size_t i;

	for (i = 0; expected[i] != NULL; i++) {
		const char *nl = strchr(p, '\n');
		size_t n = nl != NULL ? (size_t)(nl - p) : strlen(p);

		if (nl == NULL || strlen(expected[i]) != n ||
		    strncmp(p, expected[i], n) != 0) {
			HARNESS_FAIL("scenario at line %d, call %zu: want "
				     "\"%s\", got \"%.*s\"\nrecorded:\n%s",
				     line, i, expected[i], (int)n, p,
				     fake_calls);
			return;
		}
		p = nl + 1;
	}
	if (*p != '\0')
		HARNESS_FAIL("scenario at line %d: unexpected calls:\n%s", line,
			     p);
}

#define CHECK_CALLS(...)                                               \
	do {                                                           \
		const char *const want_[] = { __VA_ARGS__, NULL };     \
		check_calls(want_, __LINE__);                          \
	} while (0)

static void check_argv(char **got, int got_argc, const char *const *want)
{
	int i;

	for (i = 0; want[i] != NULL; i++) {
		if (i >= got_argc) {
			HARNESS_FAIL("argv has %d entries, want more", got_argc);
			return;
		}
		CHECK_EQ_STR(got[i], want[i]);
	}
	CHECK_EQ_INT(got_argc, i);
	CHECK(got[got_argc] == NULL);
}

#define CHECK_ARGV(got, got_argc, ...)                                 \
	do {                                                           \
		const char *const want_[] = { __VA_ARGS__, NULL };     \
		check_argv(got, got_argc, want_);                      \
	} while (0)

static void free_argv(char **argv)
{
	size_t i;

	if (argv == NULL)
		return;
	for (i = 0; argv[i] != NULL; i++)
		free(argv[i]);
	free(argv);
}

#define EARLY_MOUNTS                                                   \
	"mount proc /proc proc nosuid,nodev,noexec -",                 \
	"mount sysfs /sys sysfs nosuid,nodev,noexec -",                \
	"mount dev /dev devtmpfs nosuid -"

#define STAGE_BIN                                                      \
	"mkdir_p /newroot/run/urunc 755",                              \
	"copy /proc/self/exe /newroot/run/urunc/urunit 755",           \
	"symlink urunit /newroot/run/urunc/urunit-agent"

#define STAGE_CONF                                                     \
	"copy /urunit.conf /newroot/run/urunc/urunit.conf 600",        \
	"setenv URUNIT_CONFIG /run/urunc/urunit.conf",                 \
	"mkdir_p /newroot/tmp 755",                                    \
	"mount tmpfs /newroot/tmp tmpfs nosuid,nodev mode=1777"

#define CHMOD_NET                                                      \
	"chmod /newroot/etc/resolv.conf 644",                          \
	"chmod /newroot/etc/hosts 644",                                \
	"chmod /newroot/etc/hostname 644"

#define MOVE_MOUNTS                                                    \
	"mkdir_p /newroot/dev 755",                                    \
	"mount /dev /newroot/dev - move -",                            \
	"mkdir_p /newroot/dev/pts 755",                                \
	"mkdir_p /newroot/dev/shm 755",                                \
	"mount devpts /newroot/dev/pts devpts nosuid,noexec "          \
	"gid=5,mode=0620,ptmxmode=0666",                               \
	"mount shm /newroot/dev/shm tmpfs nosuid,nodev mode=1777",     \
	"mkdir_p /newroot/proc 755",                                   \
	"mount /proc /newroot/proc - move -",                          \
	"mkdir_p /newroot/sys 755",                                    \
	"mount /sys /newroot/sys - move -",                            \
	"mkdir_p /newroot/sys/fs/cgroup 755",                          \
	"mount cgroup2 /newroot/sys/fs/cgroup cgroup2 nosuid,nodev,noexec -"

// The /etc/hosts written when neither the image nor hull has one, without a
// hostname line since the fake has no /proc/sys/kernel/hostname.
#define DEFAULT_HOSTS                                                  \
	"mkdir_p /newroot/etc 755",                                    \
	"write /newroot/etc/hosts 61",                                 \
	"chmod /newroot/etc/hosts 644"

#define OVERLAY_CHAIN                                                  \
	"mkdir_p /newroot 755",                                        \
	"mkdir_p /run/urunc-overlay 755",                              \
	"mount tmpfs /run/urunc-overlay tmpfs 0 -",                    \
	"mkdir_p /run/urunc-overlay/upper 755",                        \
	"mkdir_p /run/urunc-overlay/work 755",                         \
	"mount overlay /newroot overlay 0 lowerdir=/lowerroot,"        \
	"upperdir=/run/urunc-overlay/upper,"                           \
	"workdir=/run/urunc-overlay/work"

// Scenario 1: a Linux host with a virtiofs root and urunit.conf.
static void test_init_linux_virtiofs(void)
{
	char *argv[] = { "/init", "/bin/sh", "-c", "x", NULL };
	char **out = NULL;
	int out_argc = 0;
	int rc;

	fake_reset();
	fake_add("/urunit.conf", "UES:\nPATH=/usr/bin\nUEE:\n", 0);
	rc = init_run(&fake_ops, URUNIT_MODE_INIT, 4, argv,
		      "panic=-1 console=ttyS0 root=fs0 rw rootfstype=virtiofs "
		      "ip=10.0.0.2::10.0.0.1:255.255.255.0:urunc:eth0:off "
		      "rdinit=/init -- /bin/sh -c x",
		      &out_argc, &out);
	CHECK_EQ_INT(rc, 0);
	CHECK_CALLS(EARLY_MOUNTS,
		    "mkdir_p /newroot 755",
		    "mount fs0 /newroot virtiofs 0 -",
		    STAGE_BIN,
		    STAGE_CONF,
		    DEFAULT_HOSTS,
		    MOVE_MOUNTS,
		    "switch_root /newroot");
	if (rc == 0)
		CHECK_ARGV(out, out_argc, "urunit", "/bin/sh", "-c", "x");
	CHECK_EQ_STR(fake_console, "");
	free_argv(out);
}

// Scenario 1b: urunc hands the command over in urunit.conf, not on the
// kernel command line, so the supervisor starts with no arguments. Without
// urunit.conf there is no command at all.
static void test_init_command_from_config(void)
{
	char *argv[] = { "/init", NULL };
	char **out = NULL;
	int out_argc = 0;
	int rc;

	fake_reset();
	fake_add("/urunit.conf", "UCS\0ARC:1\0ARV:/bin/true\0UCE\0", 0);
	rc = init_run(&fake_ops, URUNIT_MODE_INIT, 1, argv,
		      "panic=-1 console=ttyS0 root=fs0 rw rootfstype=virtiofs "
		      "rdinit=/init", &out_argc, &out);
	CHECK_EQ_INT(rc, 0);
	if (rc == 0)
		CHECK_ARGV(out, out_argc, "urunit");
	CHECK_EQ_STR(fake_console, "");
	free_argv(out);

	fake_reset();
	out = NULL;
	rc = init_run(&fake_ops, URUNIT_MODE_INIT, 1, argv,
		      "panic=-1 console=ttyS0 root=fs0 rw rootfstype=virtiofs "
		      "rdinit=/init", &out_argc, &out);
	CHECK(rc != 0);
	CHECK(out == NULL);
	CHECK(strstr(fake_console, "no command to run") != NULL);
}

// Scenario 2: hull with hvi, a block root and every /urunc-* file.
static void test_init_hull_block(void)
{
	char *argv[] = { "/init", NULL };
	const struct fake_write *reg;
	char **out = NULL;
	int out_argc = 0;
	int rc;

	fake_reset();
	fake_add("/dev/vda", "", 0);
	fake_add("/urunit.conf", "UES:\nUEE:\n", 0);
	fake_add("/urunc-overlay", "", 0);
	fake_add("/urunc-mounts", "data\t/data\tro\n"
				  "\t\n"
				  "cache\t\t/var/cache\trw\n", 0);
	fake_add("/urunc-resolv.conf", "nameserver 1.1.1.1\n", 0);
	fake_add("/urunc-hosts", "", 0);
	fake_add("/urunc-hostname", "box\n", 0);
	fake_add("/urunc-cmd", "bash\n-c\necho hi\n", 0);
	fake_add("/urunc-epoch", "1760000000\n", 0);
	fake_add("/urunc-rosetta", "", 0);
	fake_add("/newroot/usr/bin/bash", "", 1);
	fake_add("/newroot/bin/bash", "", 1);
	rc = init_run(&fake_ops, URUNIT_MODE_INIT, 1, argv,
		      "console=ttyAMA0 root=/dev/vda rw rootfstype=ext4 "
		      "rootflags=noatime,data=ordered",
		      &out_argc, &out);
	CHECK_EQ_INT(rc, 0);
	CHECK_CALLS(EARLY_MOUNTS,
		    "set_clock 1760000000",
		    "mkdir_p /mnt/rosetta 755",
		    "mkdir_p /proc/sys/fs/binfmt_misc 755",
		    "mount rosetta /mnt/rosetta virtiofs 0 -",
		    "mount none /proc/sys/fs/binfmt_misc binfmt_misc 0 -",
		    "write /proc/sys/fs/binfmt_misc/register 189",
		    "umount /proc/sys/fs/binfmt_misc",
		    "mkdir_p /lowerroot 755",
		    "mount /dev/vda /lowerroot ext4 noatime data=ordered",
		    OVERLAY_CHAIN,
		    STAGE_BIN,
		    STAGE_CONF,
		    "mkdir_p /newroot/data 755",
		    "mount data /newroot/data virtiofs ro -",
		    "mkdir_p /newroot/var/cache 755",
		    "mount cache /newroot/var/cache virtiofs 0 -",
		    "mkdir_p /newroot/etc 755",
		    "copy /urunc-resolv.conf /newroot/etc/resolv.conf 644",
		    "copy /urunc-hostname /newroot/etc/hostname 644",
		    CHMOD_NET,
		    DEFAULT_HOSTS,
		    MOVE_MOUNTS,
		    "setenv URUNIT_POWEROFF 1",
		    "switch_root /newroot");
	if (rc == 0)
		CHECK_ARGV(out, out_argc, "urunit", "/usr/bin/bash", "-c",
			   "echo hi");

	CHECK_EQ_INT(strlen(rosetta_expected), 189);
	CHECK_EQ_INT(sizeof(rosetta_expected), 190);
	reg = fake_written("/proc/sys/fs/binfmt_misc/register");
	CHECK(reg != NULL);
	if (reg != NULL) {
		CHECK_EQ_INT(reg->len, 189);
		CHECK(memchr(reg->data, '\0', reg->len) == NULL);
		if (reg->len == 189)
			CHECK_MEM_EQ(reg->data, rosetta_expected, 189);
	}
	free_argv(out);
}

// Scenario 3: no rootfstype= stops init mode before any root mount.
static void test_init_missing_fstype(void)
{
	char *argv[] = { "/init", "/bin/true", NULL };
	char **out = NULL;
	int out_argc = 0;
	int rc;

	fake_reset();
	fake_add("/urunit.conf", "", 0);
	rc = init_run(&fake_ops, URUNIT_MODE_INIT, 2, argv,
		      "console=ttyS0 root=fs0 -- rootfstype=virtiofs",
		      &out_argc, &out);
	CHECK_EQ_INT(rc, 1);
	CHECK_CALLS(EARLY_MOUNTS);
	CHECK(strstr(fake_console, "container-init: no root= and "
				   "rootfstype=") != NULL);
	CHECK(out == NULL);
}

// Scenario 3b: hull before the single binary boots its block rootfs with
// rdinit=/init and no root=. With its /urunc-* files and a /dev/vda, init
// takes /dev/vda as an ext4 root, as the script init did, and exports
// /urunc-env. A Linux host, which appends no /urunc-* files, still has to
// name its root, and so does hull when there is no disk.
static void test_init_hull_block_no_root(void)
{
	char *argv[] = { "/init", NULL };
	char **out = NULL;
	int out_argc = 0;
	int rc;

	fake_reset();
	fake_add("/dev/vda", "", 0);
	fake_add("/urunc-cmd", "/bin/echo\nhello", 0);
	fake_add("/urunc-env", "PATH=/usr/bin:/bin\nFOO=a=b", 0);
	rc = init_run(&fake_ops, URUNIT_MODE_INIT, 1, argv,
		      "rdinit=/init console=ttyAMA0", &out_argc, &out);
	CHECK_EQ_INT(rc, 0);
	CHECK_CALLS(EARLY_MOUNTS,
		    "mkdir_p /newroot 755",
		    "mount /dev/vda /newroot ext4 0 -",
		    "setenv PATH /usr/bin:/bin",
		    "setenv FOO a=b",
		    STAGE_BIN,
		    DEFAULT_HOSTS,
		    MOVE_MOUNTS,
		    "setenv URUNIT_POWEROFF 1",
		    "switch_root /newroot");
	if (rc == 0)
		CHECK_ARGV(out, out_argc, "urunit", "/bin/echo", "hello");
	CHECK(strstr(fake_console, "taking hull's block rootfs /dev/vda") != NULL);
	free_argv(out);

	// A Linux host: a disk, but none of hull's files.
	fake_reset();
	out = NULL;
	fake_add("/dev/vda", "", 0);
	fake_add("/urunit.conf", "UCS\0ARC:1\0ARV:/bin/true\0UCE\0", 0);
	rc = init_run(&fake_ops, URUNIT_MODE_INIT, 1, argv,
		      "rdinit=/init console=ttyS0", &out_argc, &out);
	CHECK_EQ_INT(rc, 1);
	CHECK(out == NULL);
	CHECK(strstr(fake_console, "no root= and rootfstype=") != NULL);

	// hull's files, but no disk: it waits for one, then refuses.
	fake_reset();
	out = NULL;
	fake_add("/urunc-cmd", "/bin/true", 0);
	rc = init_run(&fake_ops, URUNIT_MODE_INIT, 1, argv,
		      "rdinit=/init console=ttyAMA0", &out_argc, &out);
	CHECK_EQ_INT(rc, 1);
	CHECK(out == NULL);
	CHECK(strstr(fake_calls, "nap 100") != NULL);
	CHECK(strstr(fake_console, "no root= and rootfstype=") != NULL);
}

// Scenario 4: Vz with a direct root, /urunc-env and DHCP resolver data.
static void test_vz_direct(void)
{
	char *argv[] = { "/vz-init", NULL };
	const char *resolv = "domain example.com\nnameserver 192.168.64.1\n";
	const struct fake_write *w;
	char **out = NULL;
	int out_argc = 0;
	int rc;

	fake_reset();
	fake_add("/urunc-rootfs-direct", "", 0);
	fake_add("/urunc-env", "PATH=/usr/bin:/bin\n\n# a comment\n"
			       "HOME=/root\nFOO=a=b", 0);
	fake_add("/proc/net/pnp", "#PROTO: DHCP\ndomain example.com\n"
				  "nameserver 192.168.64.1\n"
				  "bootserver 192.168.64.1\n", 0);
	fake_add("/urunc-cmd", "/bin/echo\nhello", 0);
	rc = init_run(&fake_ops, URUNIT_MODE_VZ_INIT, 1, argv, NULL,
		      &out_argc, &out);
	CHECK_EQ_INT(rc, 0);
	CHECK_CALLS(EARLY_MOUNTS,
		    "mkdir_p /newroot 755",
		    "mount rootfs /newroot virtiofs 0 -",
		    "setenv PATH /usr/bin:/bin",
		    "setenv HOME /root",
		    "setenv FOO a=b",
		    "mkdir_p /newroot/tmp 755",
		    "chmod /newroot/tmp 1777",
		    "mkdir_p /newroot/etc 755",
		    "write /newroot/etc/resolv.conf 43",
		    CHMOD_NET,
		    DEFAULT_HOSTS,
		    STAGE_BIN,
		    MOVE_MOUNTS,
		    "setenv URUNIT_POWEROFF 1",
		    "switch_root /newroot");
	w = fake_written("/newroot/etc/resolv.conf");
	CHECK(w != NULL);
	if (w != NULL)
		CHECK_EQ_STR(w->data, resolv);
	if (rc == 0)
		CHECK_ARGV(out, out_argc, "urunit", "/bin/echo", "hello");
	free_argv(out);
}

// Scenario 4b: Vz with every /urunc-* file hull writes and exports that
// cover /tmp, /etc and /run. The rootfs /tmp and the network files are
// handled before the exports are mounted, so the host shares receive
// neither the chmod nor the files, and the binary is staged after the
// exports so that an export at /run cannot hide it. A provided
// /urunc-resolv.conf wins over /proc/net/pnp.
static void test_vz_full(void)
{
	char *argv[] = { "/vz-init", NULL };
	const struct fake_write *reg;
	char **out = NULL;
	int out_argc = 0;
	int rc;

	fake_reset();
	fake_add("/urunc-rootfs-direct", "", 0);
	fake_add("/urunc-epoch", "1760000000\n", 0);
	fake_add("/urunc-rosetta", "", 0);
	fake_add("/urunc-env", "A=b\n", 0);
	fake_add("/urunc-mounts", "work\t/tmp\trw\n"
				  "conf\t/etc\tro\n"
				  "state\t/run\trw\n", 0);
	fake_add("/urunc-resolv.conf", "nameserver 1.1.1.1\n", 0);
	fake_add("/proc/net/pnp", "nameserver 192.168.64.1\n", 0);
	fake_add("/urunc-hosts", "127.0.0.1 localhost\n", 0);
	fake_add("/urunc-hostname", "box\n", 0);
	fake_add("/urunc-cmd", "bash\n-c\necho hi\n", 0);
	fake_add("/newroot/usr/bin/bash", "", 1);
	rc = init_run(&fake_ops, URUNIT_MODE_VZ_INIT, 1, argv, NULL,
		      &out_argc, &out);
	CHECK_EQ_INT(rc, 0);
	CHECK_CALLS(EARLY_MOUNTS,
		    "set_clock 1760000000",
		    "mkdir_p /mnt/rosetta 755",
		    "mkdir_p /proc/sys/fs/binfmt_misc 755",
		    "mount rosetta /mnt/rosetta virtiofs 0 -",
		    "mount none /proc/sys/fs/binfmt_misc binfmt_misc 0 -",
		    "write /proc/sys/fs/binfmt_misc/register 189",
		    "umount /proc/sys/fs/binfmt_misc",
		    "mkdir_p /newroot 755",
		    "mount rootfs /newroot virtiofs 0 -",
		    "setenv A b",
		    "mkdir_p /newroot/tmp 755",
		    "chmod /newroot/tmp 1777",
		    "mkdir_p /newroot/etc 755",
		    "copy /urunc-resolv.conf /newroot/etc/resolv.conf 644",
		    "copy /urunc-hosts /newroot/etc/hosts 644",
		    "copy /urunc-hostname /newroot/etc/hostname 644",
		    CHMOD_NET,
		    "mkdir_p /newroot/tmp 755",
		    "mount work /newroot/tmp virtiofs 0 -",
		    "mkdir_p /newroot/etc 755",
		    "mount conf /newroot/etc virtiofs ro -",
		    "mkdir_p /newroot/run 755",
		    "mount state /newroot/run virtiofs 0 -",
		    STAGE_BIN,
		    MOVE_MOUNTS,
		    "setenv URUNIT_POWEROFF 1",
		    "switch_root /newroot");
	if (rc == 0)
		CHECK_ARGV(out, out_argc, "urunit", "/usr/bin/bash", "-c",
			   "echo hi");
	reg = fake_written("/proc/sys/fs/binfmt_misc/register");
	CHECK(reg != NULL);
	if (reg != NULL && reg->len == 189)
		CHECK_MEM_EQ(reg->data, rosetta_expected, 189);
	CHECK(fake_written("/newroot/etc/resolv.conf") == NULL);
	free_argv(out);
}

// Scenario 5: Vz with an overlay that cannot be mounted falls back to the
// direct root.
static void test_vz_overlay_fallback(void)
{
	char *argv[] = { "/vz-init", "/bin/true", NULL };
	char **out = NULL;
	int out_argc = 0;
	int rc;

	fake_reset();
	fake_fail[0] = "mount overlay ";
	rc = init_run(&fake_ops, URUNIT_MODE_VZ_INIT, 2, argv, NULL,
		      &out_argc, &out);
	CHECK_EQ_INT(rc, 0);
	CHECK_CALLS(EARLY_MOUNTS,
		    "mkdir_p /lowerroot 755",
		    "mount rootfs /lowerroot virtiofs 0 -",
		    OVERLAY_CHAIN,
		    "umount /lowerroot",
		    "mount rootfs /newroot virtiofs 0 -",
		    "mkdir_p /newroot/tmp 755",
		    "chmod /newroot/tmp 1777",
		    "mkdir_p /newroot/etc 755",
		    CHMOD_NET,
		    DEFAULT_HOSTS,
		    STAGE_BIN,
		    MOVE_MOUNTS,
		    "switch_root /newroot");
	CHECK(strstr(fake_console, "vz-init: overlay unavailable") != NULL);
	if (rc == 0)
		CHECK_ARGV(out, out_argc, "urunit", "/bin/true");
	free_argv(out);
}

// Scenario 6: in Vz mode a failed /proc mount is logged and ignored, but
// a root that cannot be mounted ends the boot.
static void test_vz_rootfs_fails(void)
{
	char *argv[] = { "/vz-init", "/bin/true", NULL };
	char **out = NULL;
	int out_argc = 0;
	int rc;

	fake_reset();
	fake_fail[0] = "mount proc ";
	fake_fail[1] = "mount rootfs ";
	rc = init_run(&fake_ops, URUNIT_MODE_VZ_INIT, 2, argv, NULL,
		      &out_argc, &out);
	CHECK_EQ_INT(rc, 1);
	CHECK_CALLS(EARLY_MOUNTS,
		    "mkdir_p /lowerroot 755",
		    "mount rootfs /lowerroot virtiofs 0 -");
	CHECK(strstr(fake_console, "vz-init: could not mount /proc") != NULL);
	CHECK(strstr(fake_console, "vz-init: could not mount virtiofs root "
				   "rootfs") != NULL);
	CHECK(out == NULL);
}

// Scenario 7: the root parameters after "--" belong to the application.
static void test_init_cmdline_root(void)
{
	char *root;
	char *fstype;
	char *flags;
	unsigned long mflags = 0;
	char *data;

	CHECK_EQ_INT(init_parse_cmdline_root(
			     "root=fs0 rootfstype=virtiofs rootflags=a,b "
			     "-- root=evil rootfstype=ext4",
			     &root, &fstype, &flags),
		     0);
	CHECK_EQ_STR(root, "fs0");
	CHECK_EQ_STR(fstype, "virtiofs");
	CHECK_EQ_STR(flags, "a,b");
	free(root);
	free(fstype);
	free(flags);

	CHECK_EQ_INT(init_parse_cmdline_root("console=ttyS0 -- root=evil",
					     &root, &fstype, &flags),
		     0);
	CHECK(root == NULL);
	CHECK(fstype == NULL);
	CHECK(flags == NULL);

	// The 9p root of urunc keeps every option for the filesystem.
	data = init_mount_opts("trans=virtio,version=9p2000.L,ro,posixacl",
			       &mflags);
	CHECK_EQ_STR(data, "trans=virtio,version=9p2000.L,posixacl");
	CHECK_EQ_INT(mflags, MS_RDONLY);
	free(data);
	mflags = 0;
	CHECK(init_mount_opts("nosuid,nodev", &mflags) == NULL);
	CHECK_EQ_INT(mflags, MS_NOSUID | MS_NODEV);
}

// Scenario 8: /proc/net/pnp without a nameserver gives no resolv.conf.
static void test_init_pnp(void)
{
	const char *no_ns = "#PROTO: DHCP\ndomain example.com\n"
			    "bootserver 10.0.0.1\n";
	const char *ns = "#PROTO: DHCP\nnameserver 10.0.0.1\nsearch lan\n";
	char *r;

	CHECK(init_pnp_resolv(no_ns, strlen(no_ns)) == NULL);
	CHECK(init_pnp_resolv("", 0) == NULL);
	r = init_pnp_resolv(ns, strlen(ns));
	CHECK_EQ_STR(r, "nameserver 10.0.0.1\nsearch lan\n");
	free(r);
}

// Scenario 9: init mode exports /urunc-env too. hull appends it for a
// container boot in both modes, and it is the only place the image's
// environment travels; a Linux host never appends it (its environment is in
// urunit.conf).
static void test_init_exports_env(void)
{
	char *argv[] = { "/init", "/bin/true", NULL };
	char **out = NULL;
	int out_argc = 0;
	int rc;

	fake_reset();
	fake_add("/urunc-env", "FOO=bar\n", 0);
	rc = init_run(&fake_ops, URUNIT_MODE_INIT, 2, argv,
		      "root=fs0 rootfstype=virtiofs", &out_argc, &out);
	CHECK_EQ_INT(rc, 0);
	CHECK_CALLS(EARLY_MOUNTS,
		    "mkdir_p /newroot 755",
		    "mount fs0 /newroot virtiofs 0 -",
		    "setenv FOO bar",
		    STAGE_BIN,
		    DEFAULT_HOSTS,
		    MOVE_MOUNTS,
		    "setenv URUNIT_POWEROFF 1",
		    "switch_root /newroot");
	CHECK(strstr(fake_console, "container-init: no urunit.conf") != NULL);
	free_argv(out);
}

// Scenario 10: URUNIT_POWEROFF and URUNIT_DEFROUTE keep their meaning. The
// early init runs in the process that goes on as the supervisor, so it must
// leave the environment alone and the supervisor must still see both.
static void test_init_env_switches(void)
{
	char *argv[] = { "/init", "/bin/true", NULL };
	char **out = NULL;
	int out_argc = 0;
	int rc;

	unsetenv("URUNIT_POWEROFF");
	unsetenv("URUNIT_DEFROUTE");
	CHECK_EQ_INT(reboot_command(), LINUX_REBOOT_CMD_RESTART);
	CHECK_EQ_INT(wants_default_route(), 0);

	setenv("URUNIT_POWEROFF", "1", 1);
	setenv("URUNIT_DEFROUTE", "1", 1);
	fake_reset();
	rc = init_run(&fake_ops, URUNIT_MODE_INIT, 2, argv,
		      "root=fs0 rootfstype=virtiofs", &out_argc, &out);
	CHECK_EQ_INT(rc, 0);
	CHECK_EQ_INT(reboot_command(), LINUX_REBOOT_CMD_POWER_OFF);
	CHECK_EQ_INT(wants_default_route(), 1);
	free_argv(out);
	out = NULL;

	fake_reset();
	argv[0] = "/vz-init";
	rc = init_run(&fake_ops, URUNIT_MODE_VZ_INIT, 2, argv, NULL,
		      &out_argc, &out);
	CHECK_EQ_INT(rc, 0);
	CHECK_EQ_INT(reboot_command(), LINUX_REBOOT_CMD_POWER_OFF);
	CHECK_EQ_INT(wants_default_route(), 1);
	free_argv(out);

	unsetenv("URUNIT_POWEROFF");
	unsetenv("URUNIT_DEFROUTE");
}

// The default /etc/hosts: the hostname line comes from the kernel's
// hostname, cut at the first blank, and nothing is written when hull sent
// /urunc-hosts or the image has its own. A failed cgroup2 mount is logged
// and the boot goes on.
static void test_default_hosts(void)
{
	char *argv[] = { "/vz-init", "/bin/true", NULL };
	const struct fake_write *w;
	char **out = NULL;
	int out_argc = 0;

	fake_reset();
	fake_add("/urunc-rootfs-direct", "", 0);
	fake_add("/proc/sys/kernel/hostname", "sandbox\n", 0);
	fake_fail[0] = "mount cgroup2 ";
	CHECK_EQ_INT(init_run(&fake_ops, URUNIT_MODE_VZ_INIT, 2, argv, NULL,
			      &out_argc, &out), 0);
	w = fake_written("/newroot/etc/hosts");
	CHECK(w != NULL);
	if (w != NULL)
		CHECK_EQ_STR(w->data,
			     "127.0.0.1\tlocalhost\n"
			     "::1\tlocalhost ip6-localhost ip6-loopback\n"
			     "127.0.1.1\tsandbox\n");
	CHECK(strstr(fake_console, "could not mount cgroup2") != NULL);
	free_argv(out);

	fake_reset();
	fake_add("/urunc-rootfs-direct", "", 0);
	fake_add("/urunc-hosts", "10.0.0.2 box\n", 0);
	CHECK_EQ_INT(init_run(&fake_ops, URUNIT_MODE_VZ_INIT, 2, argv, NULL,
			      &out_argc, &out), 0);
	CHECK(fake_written("/newroot/etc/hosts") == NULL);
	CHECK(strstr(fake_calls, "copy /urunc-hosts /newroot/etc/hosts") != NULL);
	free_argv(out);

	fake_reset();
	fake_add("/urunc-rootfs-direct", "", 0);
	fake_add("/newroot/etc/hosts", "127.0.0.1 localhost\n", 0);
	CHECK_EQ_INT(init_run(&fake_ops, URUNIT_MODE_VZ_INIT, 2, argv, NULL,
			      &out_argc, &out), 0);
	CHECK(fake_written("/newroot/etc/hosts") == NULL);
	free_argv(out);
}

// The markers hull stages for an overlay-block root.
static void test_overlay_markers(void)
{
	static const struct {
		const char *line;
		const char *serial;
		const char *fstype;
	} lower[] = {
		{ "block:disk0:ext4", "disk0", "ext4" },
		{ "block:disk0:ext4\n", "disk0", "ext4" },
		{ "block:a.b_c-1:erofs", "a.b_c-1", "erofs" },
		{ "block:disk0:", NULL, NULL },
		{ "block::ext4", NULL, NULL },
		{ "block:disk0", NULL, NULL },
		{ "virtiofs", NULL, NULL },
		{ "block:disk 0:ext4", NULL, NULL },
		{ "block:disk0:Ext4", NULL, NULL },
		{ "block:disk0:ext4:x", NULL, NULL },
		{ "block:abcdefghijklmnopqrstu:ext4", NULL, NULL },
	};
	static const struct {
		const char *line;
		int rc;
		enum init_upper_kind kind;
		const char *serial;
	} upper[] = {
		{ "", 0, INIT_UPPER_TMPFS, NULL },
		{ "\n", 0, INIT_UPPER_TMPFS, NULL },
		{ "virtiofs\n", 0, INIT_UPPER_VIRTIOFS, NULL },
		{ "block", 0, INIT_UPPER_BLOCK, NULL },
		{ "block:disk1\n", 0, INIT_UPPER_BLOCK, "disk1" },
		{ "block:", -1, INIT_UPPER_TMPFS, NULL },
		{ "block:disk/1", -1, INIT_UPPER_TMPFS, NULL },
		{ "tmpfs", -1, INIT_UPPER_TMPFS, NULL },
	};
	size_t i;

	for (i = 0; i < sizeof(lower) / sizeof(lower[0]); i++) {
		char *serial = NULL;
		char *fstype = NULL;
		int rc = init_parse_lower(lower[i].line, &serial, &fstype);

		if (lower[i].serial == NULL) {
			if (rc != -1)
				HARNESS_FAIL("lower '%s' parsed", lower[i].line);
			continue;
		}
		CHECK_EQ_INT(rc, 0);
		if (rc == 0) {
			CHECK_EQ_STR(serial, lower[i].serial);
			CHECK_EQ_STR(fstype, lower[i].fstype);
		}
		free(serial);
		free(fstype);
	}
	for (i = 0; i < sizeof(upper) / sizeof(upper[0]); i++) {
		enum init_upper_kind kind = INIT_UPPER_TMPFS;
		char *serial = NULL;
		int rc = init_parse_upper(upper[i].line, &kind, &serial);

		if (rc != upper[i].rc) {
			HARNESS_FAIL("upper '%s': rc %d, want %d", upper[i].line,
				     rc, upper[i].rc);
		} else if (rc == 0) {
			CHECK_EQ_INT(kind, upper[i].kind);
			if (upper[i].serial == NULL)
				CHECK(serial == NULL);
			else if (serial != NULL)
				CHECK_EQ_STR(serial, upper[i].serial);
			else
				HARNESS_FAIL("upper '%s': no serial",
					     upper[i].line);
		}
		free(serial);
	}
}

// Disks are found by serial, not by probe order, and only when the device
// node exists. A missing one is looked for again, 100 ms apart.
static void test_dev_by_serial(void)
{
	char *dev;

	fake_reset();
	fake_add("/sys/block/vda/serial", "disk1", 0);
	fake_add("/sys/block/vdb/serial", "disk0", 0);
	fake_add("/sys/block/vdc/serial", "disk2", 0);
	fake_add("/dev/vda", "", 0);
	fake_add("/dev/vdb", "", 0);
	dev = init_dev_by_serial(&fake_ops, "disk0", 3);
	CHECK(dev != NULL);
	if (dev != NULL)
		CHECK_EQ_STR(dev, "/dev/vdb");
	free(dev);
	CHECK(init_dev_by_serial(&fake_ops, "disk2", 3) == NULL);
	CHECK_EQ_STR(fake_calls, "nap 100\nnap 100\n");
	CHECK(init_dev_by_serial(&fake_ops, "disk", 1) == NULL);
}

// Scenario: hull's overlay-block root on Vz or hvi. The lower is a
// read-only ext4 disk, the upper a writable one, both found by serial.
static void test_vz_overlay_block(void)
{
	char *argv[] = { "/vz-init", "/bin/true", NULL };
	char **out = NULL;
	int out_argc = 0;
	int rc;

	fake_reset();
	fake_add("/urunc-rootfs-lower", "block:disk0:ext4\n", 0);
	fake_add("/urunc-rootfs-upper", "block:disk1\n", 0);
	fake_add("/sys/block/vda/serial", "disk0", 0);
	fake_add("/sys/block/vdb/serial", "disk1", 0);
	fake_add("/dev/vda", "", 0);
	fake_add("/dev/vdb", "", 0);
	rc = init_run(&fake_ops, URUNIT_MODE_VZ_INIT, 2, argv, NULL,
		      &out_argc, &out);
	CHECK_EQ_INT(rc, 0);
	CHECK_CALLS(EARLY_MOUNTS,
		    "mkdir_p /lowerroot 755",
		    "mount /dev/vda /lowerroot ext4 ro -",
		    "mkdir_p /newroot 755",
		    "mkdir_p /run/urunc-overlay 755",
		    "mount /dev/vdb /run/urunc-overlay ext4 0 -",
		    "mkdir_p /run/urunc-overlay/upper 755",
		    "mkdir_p /run/urunc-overlay/work 755",
		    "mount overlay /newroot overlay 0 lowerdir=/lowerroot,"
		    "upperdir=/run/urunc-overlay/upper,"
		    "workdir=/run/urunc-overlay/work",
		    "mkdir_p /newroot/tmp 755",
		    "chmod /newroot/tmp 1777",
		    "mkdir_p /newroot/etc 755",
		    CHMOD_NET,
		    DEFAULT_HOSTS,
		    STAGE_BIN,
		    MOVE_MOUNTS,
		    "switch_root /newroot");
	free_argv(out);

	// The older upper words, with the lower still on virtiofs.
	fake_reset();
	fake_add("/urunc-rootfs-upper", "virtiofs\n", 0);
	CHECK_EQ_INT(init_run(&fake_ops, URUNIT_MODE_VZ_INIT, 2, argv, NULL,
			      &out_argc, &out), 0);
	CHECK(strstr(fake_calls, "mount rootfs /lowerroot virtiofs 0 -\n") !=
	      NULL);
	CHECK(strstr(fake_calls, "mount rootfs-upper /run/urunc-overlay "
				 "virtiofs 0 -\n") != NULL);
	free_argv(out);

	fake_reset();
	fake_add("/urunc-rootfs-upper", "block", 0);
	CHECK_EQ_INT(init_run(&fake_ops, URUNIT_MODE_VZ_INIT, 2, argv, NULL,
			      &out_argc, &out), 0);
	CHECK(strstr(fake_calls, "mount /dev/vda /run/urunc-overlay ext4 0 -\n")
	      != NULL);
	free_argv(out);
}

// Every failure of a block lower or a persistent upper ends the boot, with
// no fallback to a direct virtiofs root.
static void test_vz_overlay_block_fails(void)
{
	static const struct {
		const char *lower;
		const char *upper;
		int direct;
		const char *fail;
		const char *msg;
	} cases[] = {
		{ "block:disk9:ext4", NULL, 0, NULL,
		  "no virtio-blk disk with serial disk9 for the lower layer" },
		{ "block:disk0:ext4", NULL, 1, NULL,
		  "/urunc-rootfs-direct and /urunc-rootfs-lower are both present" },
		{ "bogus", NULL, 0, NULL,
		  "cannot parse /urunc-rootfs-lower: 'bogus'" },
		{ NULL, "tmpfs", 0, NULL,
		  "cannot parse /urunc-rootfs-upper: 'tmpfs'" },
		{ "block:disk0:ext4", "block:disk0", 0, NULL,
		  "the upper layer /dev/vda is also the lower layer" },
		{ "block:disk0:ext4", "block:disk8", 0, NULL,
		  "no virtio-blk disk with serial disk8 for the upper layer" },
		{ "block:disk0:ext4", NULL, 0, "mount /dev/vda /lowerroot",
		  "could not mount ext4 root /dev/vda" },
		{ "block:disk0:ext4", "block:disk1", 0,
		  "mount /dev/vdb /run/urunc-overlay",
		  "could not mount the block upper layer" },
		{ "block:disk0:ext4", NULL, 0, "mount overlay ",
		  "could not mount the overlay over /dev/vda" },
		{ NULL, "virtiofs", 0, "mount overlay ",
		  "could not mount the overlay over rootfs" },
	};
	char *argv[] = { "/vz-init", "/bin/true", NULL };
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char **out = NULL;
		int out_argc = 0;
		int rc;

		fake_reset();
		if (cases[i].lower != NULL)
			fake_add("/urunc-rootfs-lower", cases[i].lower, 0);
		if (cases[i].upper != NULL)
			fake_add("/urunc-rootfs-upper", cases[i].upper, 0);
		if (cases[i].direct)
			fake_add("/urunc-rootfs-direct", "", 0);
		fake_add("/sys/block/vda/serial", "disk0", 0);
		fake_add("/sys/block/vdb/serial", "disk1", 0);
		fake_add("/dev/vda", "", 0);
		fake_add("/dev/vdb", "", 0);
		fake_fail[0] = cases[i].fail;
		rc = init_run(&fake_ops, URUNIT_MODE_VZ_INIT, 2, argv, NULL,
			      &out_argc, &out);
		if (rc == 0)
			HARNESS_FAIL("case %zu booted", i);
		if (strstr(fake_console, cases[i].msg) == NULL)
			HARNESS_FAIL("case %zu: want '%s' on the console, got '%s'",
				     i, cases[i].msg, fake_console);
		if (strstr(fake_calls, "mount rootfs /newroot") != NULL)
			HARNESS_FAIL("case %zu fell back to a direct root", i);
		if (strstr(fake_calls, "switch_root") != NULL)
			HARNESS_FAIL("case %zu switched root", i);
		free_argv(out);
	}
}

// Scenario: urunc's overlay-block root on a Linux host. init takes the
// layers from the markers, as vz-init does, and keeps its own order: the
// staged binary and urunit.conf, then the exports.
static void test_init_overlay_block(void)
{
	char *argv[] = { "/init", "/bin/true", NULL };
	char **out = NULL;
	int out_argc = 0;
	int rc;

	fake_reset();
	fake_add("/urunit.conf", "UES:\nUEE:\n", 0);
	fake_add("/urunc-rootfs-lower", "block:disk0:ext4\n", 0);
	fake_add("/urunc-rootfs-upper", "block:disk1\n", 0);
	fake_add("/urunc-mounts", "share0\t/work/proj\trw\n", 0);
	fake_add("/sys/block/vda/serial", "disk1", 0);
	fake_add("/sys/block/vdb/serial", "disk0", 0);
	fake_add("/dev/vda", "", 0);
	fake_add("/dev/vdb", "", 0);
	rc = init_run(&fake_ops, URUNIT_MODE_INIT, 2, argv,
		      "console=ttyS0 panic=-1 rdinit=/init -- /bin/true",
		      &out_argc, &out);
	CHECK_EQ_INT(rc, 0);
	CHECK_CALLS(EARLY_MOUNTS,
		    "mkdir_p /lowerroot 755",
		    "mount /dev/vdb /lowerroot ext4 ro -",
		    "mkdir_p /newroot 755",
		    "mkdir_p /run/urunc-overlay 755",
		    "mount /dev/vda /run/urunc-overlay ext4 0 -",
		    "mkdir_p /run/urunc-overlay/upper 755",
		    "mkdir_p /run/urunc-overlay/work 755",
		    "mount overlay /newroot overlay 0 lowerdir=/lowerroot,"
		    "upperdir=/run/urunc-overlay/upper,"
		    "workdir=/run/urunc-overlay/work",
		    STAGE_BIN,
		    STAGE_CONF,
		    "mkdir_p /newroot/work/proj 755",
		    "mount share0 /newroot/work/proj virtiofs 0 -",
		    DEFAULT_HOSTS,
		    MOVE_MOUNTS,
		    "switch_root /newroot");
	if (rc == 0)
		CHECK_ARGV(out, out_argc, "urunit", "/bin/true");
	CHECK_EQ_STR(fake_console, "");
	free_argv(out);
}

// init refuses what it cannot tell apart: markers next to a root on the
// command line, and an upper with no lower. A missing disk or a failed
// mount ends the boot as in vz-init.
static void test_init_overlay_block_fails(void)
{
	static const struct {
		const char *lower;
		const char *upper;
		const char *cmdline;
		const char *fail;
		const char *msg;
	} cases[] = {
		{ "block:disk0:ext4", "block:disk1",
		  "root=/dev/vda rootfstype=ext4",  NULL,
		  "/urunc-rootfs-lower and a root on the kernel command line" },
		{ "block:disk0:ext4", NULL, "rootfstype=virtiofs", NULL,
		  "/urunc-rootfs-lower and a root on the kernel command line" },
		{ NULL, "block:disk1", "root=fs0 rootfstype=virtiofs", NULL,
		  "/urunc-rootfs-upper without /urunc-rootfs-lower" },
		{ "block:disk9:ext4", "block:disk1", "", NULL,
		  "no virtio-blk disk with serial disk9 for the lower layer" },
		{ "block:disk0:ext4", "block:disk8", "", NULL,
		  "no virtio-blk disk with serial disk8 for the upper layer" },
		{ "block:disk0:ext4", "block:disk1", "",
		  "mount /dev/vda /lowerroot",
		  "could not mount ext4 root /dev/vda" },
		{ "block:disk0:ext4", "block:disk1", "",
		  "mount /dev/vdb /run/urunc-overlay",
		  "could not mount the block upper layer" },
		{ "block:disk0:ext4", NULL, "", "mount overlay ",
		  "could not mount the overlay over /dev/vda" },
	};
	char *argv[] = { "/init", "/bin/true", NULL };
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char **out = NULL;
		int out_argc = 0;
		int rc;

		fake_reset();
		fake_add("/urunit.conf", "UES:\nUEE:\n", 0);
		if (cases[i].lower != NULL)
			fake_add("/urunc-rootfs-lower", cases[i].lower, 0);
		if (cases[i].upper != NULL)
			fake_add("/urunc-rootfs-upper", cases[i].upper, 0);
		fake_add("/sys/block/vda/serial", "disk0", 0);
		fake_add("/sys/block/vdb/serial", "disk1", 0);
		fake_add("/dev/vda", "", 0);
		fake_add("/dev/vdb", "", 0);
		fake_fail[0] = cases[i].fail;
		rc = init_run(&fake_ops, URUNIT_MODE_INIT, 2, argv,
			      cases[i].cmdline, &out_argc, &out);
		if (rc == 0)
			HARNESS_FAIL("init case %zu booted", i);
		if (strstr(fake_console, cases[i].msg) == NULL)
			HARNESS_FAIL("init case %zu: want '%s' on the console, "
				     "got '%s'", i, cases[i].msg, fake_console);
		if (strstr(fake_calls, " /newroot ext4 ") != NULL ||
		    strstr(fake_calls, " /newroot virtiofs ") != NULL)
			HARNESS_FAIL("init case %zu mounted a direct root", i);
		if (strstr(fake_calls, "switch_root") != NULL)
			HARNESS_FAIL("init case %zu switched root", i);
		free_argv(out);
	}
}

static void test_init_parsers(void)
{
	const char *tsv = "a\t/x\tro\nb\t/y\n\tc\n\nd\t/z\trw";
	struct init_mount *m;
	char **lines;

	lines = init_read_lines("one\n\nthree", 11);
	CHECK(lines != NULL);
	if (lines != NULL) {
		CHECK_EQ_STR(lines[0], "one");
		CHECK_EQ_STR(lines[1], "");
		CHECK_EQ_STR(lines[2], "three");
		CHECK(lines[3] == NULL);
	}
	init_free_lines(lines);
	lines = init_read_lines("x\n", 2);
	CHECK(lines != NULL && lines[1] == NULL);
	init_free_lines(lines);

	// "\tc" has a tag and no dest once the leading tab is skipped.
	m = init_parse_mounts_tsv(tsv, strlen(tsv));
	CHECK(m != NULL);
	if (m != NULL) {
		CHECK_EQ_STR(m[0].tag, "a");
		CHECK_EQ_STR(m[0].dest, "/x");
		CHECK_EQ_INT(m[0].ro, 1);
		CHECK_EQ_STR(m[1].tag, "b");
		CHECK_EQ_INT(m[1].ro, 0);
		CHECK_EQ_STR(m[2].tag, "d");
		CHECK_EQ_STR(m[2].dest, "/z");
		CHECK_EQ_INT(m[2].ro, 0);
		CHECK(m[3].tag == NULL);
	}
	init_free_mounts(m);
}

int run_init_tests(void)
{
	int start = harness_failures;

	test_init_linux_virtiofs();
	test_init_command_from_config();
	test_init_hull_block();
	test_init_missing_fstype();
	test_init_hull_block_no_root();
	test_vz_direct();
	test_default_hosts();
	test_overlay_markers();
	test_dev_by_serial();
	test_vz_overlay_block();
	test_vz_overlay_block_fails();
	test_init_overlay_block();
	test_init_overlay_block_fails();
	test_vz_full();
	test_vz_overlay_fallback();
	test_vz_rootfs_fails();
	test_init_cmdline_root();
	test_init_pnp();
	test_init_exports_env();
	test_init_env_switches();
	test_init_parsers();
	fake_reset();

	return harness_failures - start;
}
