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

// Early userspace of a generic container boot.
//
// urunc boots an unmodified OCI image with a host kernel and an initrd that
// holds this binary. The kernel runs it as PID 1, as /init or as /vz-init.
// It mounts the container root at /newroot, prepares it and switches to it.
// Then it becomes the supervisor of the application in the same process.
//
// The two modes differ in where the root comes from and in how they treat
// errors:
//
// - init: Linux hosts, and hull with a block root image. The root comes from root=,
//   rootfstype= and rootflags= on the kernel command line. Every failure
//   ends the boot. PID 1 exits and the kernel panics. With panic=-1 the
//   guest reboots and the monitor exits, so the container fails instead of
//   hanging.
// - vz-init: hull with Virtualization.framework. The root is the virtiofs
//   tag "rootfs". Failures are logged and the boot goes on. A root that
//   cannot be mounted, an empty command and a failed switch_root still end
//   it.
//
// In both modes /urunc-rootfs-lower replaces that default: the root is an
// overlay on the read-only disk it names, with the upper /urunc-rootfs-upper
// names. init then refuses a root= on the kernel command line too.
//
// Linux hosts append only /urunit.conf to the initrd. hull has no bind
// mounts to prepare the root with, so it appends the /urunc-* files too.
// A step named after a file runs only when that file is present.
//
//   step               init                      vz-init
//   /proc /sys /dev    mount                     mount
//   /urunc-epoch       set the clock             set the clock
//   /urunc-rosetta     register Rosetta          register Rosetta
//   root               root= of the cmdline      virtiofs tag rootfs
//   tmpfs overlay      with /urunc-overlay       without /urunc-rootfs-direct,
//                                                direct mount when it fails
//   layered root       the disk /urunc-rootfs-lower names (by serial,
//                      read-only) under an overlay; the upper is the one
//                      /urunc-rootfs-upper names (disk or virtiofs) or a
//                      tmpfs. Every failure is final in both modes.
//   /run/urunc         copy of this binary and an urunit-agent symlink
//   /urunit.conf       copy, URUNIT_CONFIG       ignored
//   /urunc-mounts      virtiofs mounts           virtiofs mounts
//   network files      /urunc-resolv.conf,       the same, or the resolver
//                      /urunc-hosts and          lines of /proc/net/pnp
//                      /urunc-hostname
//   default /etc/hosts when the image and hull provide none
//   /urunc-env         ignored                   exported
//   /newroot/tmp       tmpfs 1777 with urunit.conf  chmod 1777
//   /dev /proc /sys    moved into /newroot, with devpts and /dev/shm
//   cgroup2            /newroot/sys/fs/cgroup, logged when it fails
//   command            /urunc-cmd or argv, bare names looked up in /newroot
//   switch_root        /newroot                  /newroot

#define _GNU_SOURCE

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>

#include "sysops.h"
#include "urunit.h"

#define INIT_NEWROOT "/newroot"
#define INIT_LOWERROOT "/lowerroot"
#define INIT_OVERLAY_DIR "/run/urunc-overlay"
#define INIT_BINFMT_DIR "/proc/sys/fs/binfmt_misc"

// The binfmt_misc record that runs amd64 ELF binaries with Rosetta. It is
// text: the kernel decodes the \x escapes itself. The F flag opens the
// interpreter at registration, so it survives switch_root.
static const char init_rosetta_register[] =
	":rosetta:M::"
	"\\x7fELF\\x02\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00"
	"\\x02\\x00\\x3e\\x00"
	":"
	"\\xff\\xff\\xff\\xff\\xff\\xfe\\xfe\\x00\\xff\\xff\\xff\\xff\\xff\\xff"
	"\\xff\\xff\\xfe\\xff\\xff\\xff"
	":/mnt/rosetta/rosetta:OCF";

static const char *const init_path_dirs[] = {
	"/usr/local/sbin", "/usr/local/bin", "/usr/sbin",
	"/usr/bin",        "/sbin",          "/bin",
};

struct init_ctx {
	const struct sysops *ops;
	const char *prefix;
	int strict;
};

static void init_log(const struct init_ctx *c, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void init_log(const struct init_ctx *c, const char *fmt, ...)
{
	char msg[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	c->ops->log("%s%s\n", c->prefix, msg);
}

// Logs msg with errno when rc reports a failure. Returns non-zero when the
// failure ends the boot, which is every failure in init mode.
static int init_failed(const struct init_ctx *c, int rc, const char *msg)
{
	if (rc == 0)
		return 0;
	init_log(c, "%s: %s", msg, strerror(errno));
	return c->strict;
}

static char *init_strndup(const char *s, size_t n)
{
	char *d = malloc(n + 1);

	if (d == NULL)
		return NULL;
	memcpy(d, s, n);
	d[n] = '\0';
	return d;
}

// Returns the contents of path, or NULL when it is absent. The test of the
// scripts was [ -f path ].
static char *init_slurp(const struct init_ctx *c, const char *path,
			size_t *len)
{
	size_t n = 0;
	char *buf = c->ops->read_file(path, &n);

	if (len != NULL)
		*len = buf != NULL ? n : 0;
	return buf;
}

static int init_exists(const struct init_ctx *c, const char *path)
{
	char *buf = init_slurp(c, path, NULL);
	int found = buf != NULL;

	free(buf);
	return found;
}

// Returns whether path is present and not empty, the [ -s path ] test.
static int init_nonempty(const struct init_ctx *c, const char *path)
{
	size_t len;
	char *buf = init_slurp(c, path, &len);
	int found = buf != NULL && len > 0;

	free(buf);
	return found;
}

int init_parse_cmdline_root(const char *cmdline, char **root, char **fstype,
			    char **flags)
{
	static const struct {
		const char *key;
		size_t len;
	} keys[] = {
		{ "root=", 5 },
		{ "rootfstype=", 11 },
		{ "rootflags=", 10 },
	};
	char **out[3];
	const char *p = cmdline;
	size_t i;

	out[0] = root;
	out[1] = fstype;
	out[2] = flags;
	for (i = 0; i < 3; i++)
		*out[i] = NULL;
	if (p == NULL)
		return 0;

	while (*p != '\0') {
		const char *start;
		size_t n;

		while (*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		start = p;
		while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n')
			p++;
		n = (size_t)(p - start);
		if (n == 0)
			break;
		if (n == 2 && start[0] == '-' && start[1] == '-')
			break;
		for (i = 0; i < 3; i++) {
			if (n < keys[i].len ||
			    strncmp(start, keys[i].key, keys[i].len) != 0)
				continue;
			// A later parameter overrides an earlier one.
			free(*out[i]);
			*out[i] = init_strndup(start + keys[i].len,
					       n - keys[i].len);
			if (*out[i] == NULL)
				return -1;
			break;
		}
	}
	return 0;
}

char **init_read_lines(const char *buf, size_t len)
{
	size_t count = 0;
	size_t i;
	size_t start = 0;
	size_t k = 0;
	char **lines;

	for (i = 0; i < len; i++)
		if (buf[i] == '\n')
			count++;
	if (len > 0 && buf[len - 1] != '\n')
		count++;

	lines = calloc(count + 1, sizeof(*lines));
	if (lines == NULL)
		return NULL;
	for (i = 0; i <= len && k < count; i++) {
		if (i < len && buf[i] != '\n')
			continue;
		lines[k] = init_strndup(buf + start, i - start);
		if (lines[k] == NULL) {
			init_free_lines(lines);
			return NULL;
		}
		k++;
		start = i + 1;
	}
	return lines;
}

void init_free_lines(char **lines)
{
	size_t i;

	if (lines == NULL)
		return;
	for (i = 0; lines[i] != NULL; i++)
		free(lines[i]);
	free(lines);
}

// Returns the next tab separated field of *p and moves *p past it. Runs of
// tabs count as one separator, as with IFS=<tab> in the shell. The field
// is NUL terminated in place.
static char *init_next_field(char **p)
{
	char *s = *p;
	char *start;

	while (*s == '\t')
		s++;
	if (*s == '\0')
		return NULL;
	start = s;
	while (*s != '\0' && *s != '\t')
		s++;
	if (*s == '\t')
		*s++ = '\0';
	*p = s;
	return start;
}

struct init_mount *init_parse_mounts_tsv(const char *buf, size_t len)
{
	char **lines = init_read_lines(buf, len);
	struct init_mount *mounts;
	size_t i;
	size_t k = 0;

	if (lines == NULL)
		return NULL;
	for (i = 0; lines[i] != NULL; i++)
		;
	mounts = calloc(i + 1, sizeof(*mounts));
	if (mounts == NULL) {
		init_free_lines(lines);
		return NULL;
	}
	for (i = 0; lines[i] != NULL; i++) {
		char *p = lines[i];
		char *tag = init_next_field(&p);
		char *dest = init_next_field(&p);
		char *mode = init_next_field(&p);

		if (tag == NULL || dest == NULL)
			continue;
		mounts[k].tag = strdup(tag);
		mounts[k].dest = strdup(dest);
		mounts[k].ro = mode != NULL && strcmp(mode, "ro") == 0;
		if (mounts[k].tag == NULL || mounts[k].dest == NULL) {
			free(mounts[k].tag);
			free(mounts[k].dest);
			mounts[k].tag = NULL;
			init_free_mounts(mounts);
			init_free_lines(lines);
			return NULL;
		}
		k++;
	}
	init_free_lines(lines);
	return mounts;
}

void init_free_mounts(struct init_mount *mounts)
{
	size_t i;

	if (mounts == NULL)
		return;
	for (i = 0; mounts[i].tag != NULL; i++) {
		free(mounts[i].tag);
		free(mounts[i].dest);
	}
	free(mounts);
}

static int init_has_prefix(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

char *init_pnp_resolv(const char *buf, size_t len)
{
	char **lines = init_read_lines(buf, len);
	char *out;
	size_t used = 0;
	size_t i;
	int nameserver = 0;

	if (lines == NULL)
		return NULL;
	out = malloc(len + 2);
	if (out == NULL) {
		init_free_lines(lines);
		return NULL;
	}
	// Only the resolver lines are taken. pnp also carries "bootserver"
	// and a "#PROTO:" line.
	for (i = 0; lines[i] != NULL; i++) {
		size_t n = strlen(lines[i]);

		if (init_has_prefix(lines[i], "nameserver"))
			nameserver = 1;
		else if (!init_has_prefix(lines[i], "domain") &&
			 !init_has_prefix(lines[i], "search"))
			continue;
		memcpy(out + used, lines[i], n);
		used += n;
		out[used++] = '\n';
	}
	out[used] = '\0';
	init_free_lines(lines);
	if (!nameserver) {
		free(out);
		return NULL;
	}
	return out;
}

char *init_resolve_command(const struct sysops *ops, const char *newroot,
			   const char *cmd)
{
	size_t i;

	if (cmd[0] == '/' || cmd[0] == '\0')
		return strdup(cmd);
	for (i = 0; i < sizeof(init_path_dirs) / sizeof(init_path_dirs[0]);
	     i++) {
		const char *dir = init_path_dirs[i];
		size_t n = strlen(newroot) + strlen(dir) + strlen(cmd) + 2;
		char *path = malloc(n);
		int found;

		if (path == NULL)
			return NULL;
		snprintf(path, n, "%s%s/%s", newroot, dir, cmd);
		found = ops->access_x(path) == 0;
		free(path);
		if (found) {
			n = strlen(dir) + strlen(cmd) + 2;
			path = malloc(n);
			if (path != NULL)
				snprintf(path, n, "%s/%s", dir, cmd);
			return path;
		}
	}
	return strdup(cmd);
}

int init_env_export(const struct sysops *ops, const char *buf)
{
	char **lines = init_read_lines(buf, strlen(buf));
	size_t i;
	int rc = 0;

	if (lines == NULL)
		return -1;
	for (i = 0; lines[i] != NULL; i++) {
		char *eq;

		if (lines[i][0] == '\0' || lines[i][0] == '#')
			continue;
		eq = strchr(lines[i], '=');
		if (eq == NULL || eq == lines[i])
			continue;
		*eq = '\0';
		if (ops->setenv(lines[i], eq + 1) != 0)
			rc = -1;
	}
	init_free_lines(lines);
	return rc;
}

char *init_mount_opts(const char *opts, unsigned long *flags)
{
	static const struct {
		const char *name;
		unsigned long set;
		unsigned long clear;
	} known[] = {
		{ "ro", MS_RDONLY, 0 },
		{ "rw", 0, MS_RDONLY },
		{ "nosuid", MS_NOSUID, 0 },
		{ "nodev", MS_NODEV, 0 },
		{ "noexec", MS_NOEXEC, 0 },
		{ "sync", MS_SYNCHRONOUS, 0 },
		{ "dirsync", MS_DIRSYNC, 0 },
		{ "noatime", MS_NOATIME, 0 },
		{ "nodiratime", MS_NODIRATIME, 0 },
		{ "relatime", MS_RELATIME, 0 },
	};
	char *data;
	size_t used = 0;
	const char *p = opts;

	if (opts == NULL)
		return NULL;
	data = malloc(strlen(opts) + 1);
	if (data == NULL)
		return NULL;
	while (*p != '\0') {
		const char *end = strchr(p, ',');
		size_t n = end != NULL ? (size_t)(end - p) : strlen(p);
		size_t i;

		for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
			if (strlen(known[i].name) == n &&
			    strncmp(p, known[i].name, n) == 0)
				break;
		}
		if (i < sizeof(known) / sizeof(known[0])) {
			*flags |= known[i].set;
			*flags &= ~known[i].clear;
		} else if (n > 0) {
			if (used > 0)
				data[used++] = ',';
			memcpy(data + used, p, n);
			used += n;
		}
		p += n;
		if (*p == ',')
			p++;
	}
	data[used] = '\0';
	if (used == 0) {
		free(data);
		return NULL;
	}
	return data;
}

// Returns whether s is a disk serial as hull assigns them (disk<N>): not
// empty, at most the 20 bytes virtio-blk carries, and nothing that could
// break a marker field.
static int init_valid_serial(const char *s, size_t n)
{
	size_t i;

	if (n == 0 || n > 20)
		return 0;
	for (i = 0; i < n; i++) {
		char ch = s[i];

		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		      (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
		      ch == '-'))
			return 0;
	}
	return 1;
}

// Returns the length of line without trailing white space.
static size_t init_trimmed_len(const char *line)
{
	size_t n = strlen(line);

	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
			 line[n - 1] == ' ' || line[n - 1] == '\t'))
		n--;
	return n;
}

int init_parse_lower(const char *line, char **serial, char **fstype)
{
	size_t n = init_trimmed_len(line);
	const char *rest;
	const char *colon;
	size_t i;

	if (n < 6 || strncmp(line, "block:", 6) != 0)
		return -1;
	rest = line + 6;
	colon = memchr(rest, ':', n - 6);
	if (colon == NULL ||
	    !init_valid_serial(rest, (size_t)(colon - rest)) ||
	    colon + 1 == line + n)
		return -1;
	for (i = (size_t)(colon + 1 - line); i < n; i++)
		if (!((line[i] >= 'a' && line[i] <= 'z') ||
		      (line[i] >= '0' && line[i] <= '9')))
			return -1;
	*serial = init_strndup(rest, (size_t)(colon - rest));
	*fstype = init_strndup(colon + 1, (size_t)(line + n - colon - 1));
	if (*serial == NULL || *fstype == NULL) {
		free(*serial);
		free(*fstype);
		*serial = NULL;
		*fstype = NULL;
		return -1;
	}
	return 0;
}

int init_parse_upper(const char *line, enum init_upper_kind *kind,
		     char **serial)
{
	size_t n = init_trimmed_len(line);

	*serial = NULL;
	if (n == 0) {
		*kind = INIT_UPPER_TMPFS;
		return 0;
	}
	if (n == 8 && strncmp(line, "virtiofs", 8) == 0) {
		*kind = INIT_UPPER_VIRTIOFS;
		return 0;
	}
	if (n == 5 && strncmp(line, "block", 5) == 0) {
		*kind = INIT_UPPER_BLOCK;
		return 0;
	}
	if (n > 6 && strncmp(line, "block:", 6) == 0 &&
	    init_valid_serial(line + 6, n - 6)) {
		*serial = init_strndup(line + 6, n - 6);
		if (*serial == NULL)
			return -1;
		*kind = INIT_UPPER_BLOCK;
		return 0;
	}
	return -1;
}

// hull knows the serial of every disk it attaches, so nothing here depends
// on the order the guest probed them in. The kernel cuts the serial at 20
// bytes and drops the NUL padding; virtio probing is done before init runs,
// and the retries only cover a slow disk.
char *init_dev_by_serial(const struct sysops *ops, const char *serial,
			 int tries)
{
	char path[64];
	char dev[16];
	int t;
	char d;

	for (t = 0; t < tries; t++) {
		if (t > 0)
			ops->nap(100);
		for (d = 'a'; d <= 'z'; d++) {
			size_t len = 0;
			char *got;
			int match;

			snprintf(path, sizeof(path), "/sys/block/vd%c/serial", d);
			got = ops->read_file(path, &len);
			if (got == NULL)
				continue;
			while (len > 0 && (got[len - 1] == '\n' ||
					   got[len - 1] == ' '))
				got[--len] = '\0';
			match = strcmp(got, serial) == 0;
			free(got);
			snprintf(dev, sizeof(dev), "/dev/vd%c", d);
			if (match && ops->exists(dev) == 0)
				return strdup(dev);
		}
	}
	return NULL;
}

static void init_set_clock(const struct init_ctx *c)
{
	size_t len;
	char *buf = init_slurp(c, "/urunc-epoch", &len);
	time_t epoch = 0;
	size_t i;

	if (buf == NULL)
		return;
	// $(cat) dropped the trailing newlines.
	while (len > 0 && buf[len - 1] == '\n')
		len--;
	for (i = 0; i < len; i++) {
		if (buf[i] < '0' || buf[i] > '9')
			break;
		epoch = epoch * 10 + (buf[i] - '0');
	}
	free(buf);
	// 18 digits cannot overflow a 64-bit time_t.
	if (len == 0 || len > 18 || i != len) {
		init_log(c, "ignoring a malformed /urunc-epoch");
		return;
	}
	// A clock that cannot be set does not stop the boot in either mode.
	if (c->ops->set_clock(epoch) != 0)
		init_log(c, "could not set the clock: %s", strerror(errno));
}

static int init_rosetta(const struct init_ctx *c)
{
	const struct sysops *ops = c->ops;

	if (!init_exists(c, "/urunc-rosetta"))
		return 0;
	ops->mkdir_p("/mnt/rosetta", 0755);
	ops->mkdir_p(INIT_BINFMT_DIR, 0755);
	if (init_failed(c, ops->mount("rosetta", "/mnt/rosetta", "virtiofs", 0,
				      NULL),
			"could not mount the rosetta share"))
		return 1;
	if (init_failed(c, ops->mount("none", INIT_BINFMT_DIR, "binfmt_misc",
				      0, NULL),
			"could not mount binfmt_misc"))
		return 1;
	if (init_failed(c, ops->write_file(INIT_BINFMT_DIR "/register",
					   init_rosetta_register,
					   strlen(init_rosetta_register)),
			"could not register the rosetta handler"))
		return 1;
	ops->umount(INIT_BINFMT_DIR);
	return 0;
}

// Mounts the root at /newroot, under a tmpfs overlay when overlay is set.
// In vz-init mode an overlay that cannot be mounted falls back to the
// direct mount.
// The upper layer of the overlay: what to mount on /run/urunc-overlay.
struct init_upper {
	const char *src;
	const char *type;
	const char *what;
};

static const struct init_upper init_upper_tmpfs = { "tmpfs", "tmpfs",
						    "tmpfs" };

// Mounts the root at /newroot, under an overlay when overlay is set. With
// final set, as for a block lower or a persistent upper, every failure ends
// the boot: there is no virtiofs root to fall back to.
static int init_mount_root(const struct init_ctx *c, const char *root,
			   const char *fstype, const char *opts, int overlay,
			   const struct init_upper *upper, int final)
{
	const struct sysops *ops = c->ops;
	const char *lower = overlay ? INIT_LOWERROOT : INIT_NEWROOT;
	unsigned long flags = 0;
	char *data = init_mount_opts(opts, &flags);
	char msg[256];
	int rc;

	ops->mkdir_p(lower, 0755);
	rc = ops->mount(root, lower, fstype, flags, data);
	if (rc != 0) {
		snprintf(msg, sizeof(msg), "could not mount %s root %s", fstype,
			 root);
		init_failed(c, rc, msg);
		free(data);
		return 1;
	}
	if (!overlay) {
		free(data);
		return 0;
	}

	ops->mkdir_p(INIT_NEWROOT, 0755);
	ops->mkdir_p(INIT_OVERLAY_DIR, 0755);
	rc = ops->mount(upper->src, INIT_OVERLAY_DIR, upper->type, 0, NULL);
	if (rc != 0 && final) {
		snprintf(msg, sizeof(msg), "could not mount the %s upper layer",
			 upper->what);
		init_failed(c, rc, msg);
		free(data);
		return 1;
	}
	if (rc == 0) {
		ops->mkdir_p(INIT_OVERLAY_DIR "/upper", 0755);
		ops->mkdir_p(INIT_OVERLAY_DIR "/work", 0755);
		rc = ops->mount("overlay", INIT_NEWROOT, "overlay", 0,
				"lowerdir=" INIT_LOWERROOT
				",upperdir=" INIT_OVERLAY_DIR "/upper"
				",workdir=" INIT_OVERLAY_DIR "/work");
	}
	if (rc == 0) {
		free(data);
		return 0;
	}
	if (c->strict || final) {
		snprintf(msg, sizeof(msg), "could not mount the overlay over %s",
			 root);
		init_failed(c, rc, msg);
		free(data);
		return 1;
	}

	init_log(c, "overlay unavailable; using the %s root directly", fstype);
	ops->umount(INIT_LOWERROOT);
	rc = ops->mount(root, INIT_NEWROOT, fstype, flags, data);
	free(data);
	if (rc != 0) {
		snprintf(msg, sizeof(msg), "could not mount %s root %s", fstype,
			 root);
		init_failed(c, rc, msg);
		return 1;
	}
	return 0;
}

// Copies this binary and urunit.conf into /newroot/run/urunc. switch_root
// deletes the initramfs, so nothing may stay there.
static int init_stage(const struct init_ctx *c)
{
	const struct sysops *ops = c->ops;
	char bin[64];
	char conf[64];

	snprintf(bin, sizeof(bin), INIT_NEWROOT "%s", URUNIT_STAGED_BIN);
	snprintf(conf, sizeof(conf), INIT_NEWROOT "%s", URUNIT_STAGED_CONF);

	if (init_failed(c, ops->mkdir_p(INIT_NEWROOT "/run/urunc", 0755),
			"could not create " INIT_NEWROOT "/run/urunc"))
		return 1;
	// /proc/self/exe is the binary that runs now. It has to be read
	// before /proc moves into the new root.
	if (init_failed(c, ops->copy_file("/proc/self/exe", bin, 0755),
			"could not copy urunit into the new root"))
		return 1;
	if (init_failed(c, ops->symlink("urunit", INIT_NEWROOT
				       "/run/urunc/urunit-agent"),
			"could not link urunit-agent to urunit"))
		return 1;

	if (c->strict) {
		if (!init_exists(c, "/urunit.conf")) {
			init_log(c, "no urunit.conf in the initrd; urunit runs "
				    "the command as root with no environment");
			return 0;
		}
		if (init_failed(c, ops->copy_file("/urunit.conf", conf, 0600),
				"could not copy urunit.conf into the new root"))
			return 1;
		if (init_failed(c, ops->setenv("URUNIT_CONFIG",
					       URUNIT_STAGED_CONF),
				"could not set URUNIT_CONFIG"))
			return 1;
		// The urunit this replaces mounted a tmpfs on /tmp whenever it
		// had a URUNIT_CONFIG. Keep that: an image without a /tmp, or
		// with a non-sticky one, still gets a writable one in RAM.
		if (init_failed(c, ops->mkdir_p(INIT_NEWROOT "/tmp", 0755),
				"could not create " INIT_NEWROOT "/tmp"))
			return 1;
		if (init_failed(c, ops->mount("tmpfs", INIT_NEWROOT "/tmp",
					      "tmpfs", MS_NOSUID | MS_NODEV,
					      "mode=1777"),
				"could not mount a tmpfs on " INIT_NEWROOT
				"/tmp"))
			return 1;
	}
	return 0;
}

// Mounts every virtiofs export of /urunc-mounts under /newroot.
static int init_extra_mounts(const struct init_ctx *c)
{
	const struct sysops *ops = c->ops;
	size_t len;
	char *buf = init_slurp(c, "/urunc-mounts", &len);
	struct init_mount *mounts;
	size_t i;
	int rc = 0;

	if (buf == NULL)
		return 0;
	mounts = init_parse_mounts_tsv(buf, len);
	free(buf);
	if (mounts == NULL)
		return init_failed(c, -1, "could not parse /urunc-mounts");

	for (i = 0; mounts[i].tag != NULL && rc == 0; i++) {
		const char *sep = mounts[i].dest[0] == '/' ? "" : "/";
		size_t n = strlen(INIT_NEWROOT) + strlen(mounts[i].dest) + 2;
		char *dst = malloc(n);
		char msg[256];

		if (dst == NULL) {
			rc = init_failed(c, -1, "out of memory");
			break;
		}
		snprintf(dst, n, INIT_NEWROOT "%s%s", sep, mounts[i].dest);
		ops->mkdir_p(dst, 0755);
		snprintf(msg, sizeof(msg), "could not mount export %s at %s",
			 mounts[i].tag, mounts[i].dest);
		rc = init_failed(c, ops->mount(mounts[i].tag, dst, "virtiofs",
					       mounts[i].ro ? MS_RDONLY : 0,
					       NULL),
				 msg);
		free(dst);
	}
	init_free_mounts(mounts);
	return rc;
}

// Installs the network files that Linux hosts bind-mount into the root. A
// failure here never stops the boot.
static void init_network_files(const struct init_ctx *c)
{
	static const struct {
		const char *src;
		const char *dst;
	} files[] = {
		{ "/urunc-resolv.conf", INIT_NEWROOT "/etc/resolv.conf" },
		{ "/urunc-hosts", INIT_NEWROOT "/etc/hosts" },
		{ "/urunc-hostname", INIT_NEWROOT "/etc/hostname" },
	};
	const struct sysops *ops = c->ops;
	size_t n = sizeof(files) / sizeof(files[0]);
	int present[3];
	int any = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		present[i] = init_nonempty(c, files[i].src);
		any |= present[i];
	}
	if (c->strict && !any)
		return;

	ops->mkdir_p(INIT_NEWROOT "/etc", 0755);
	for (i = 0; i < n; i++) {
		if (present[i]) {
			if (ops->copy_file(files[i].src, files[i].dst, 0644))
				init_log(c, "could not copy %s: %s",
					 files[i].src, strerror(errno));
			continue;
		}
		// DHCP leaves the resolver in /proc/net/pnp. procfs reports
		// size 0 for that file, so its contents decide.
		if (i == 0 && !c->strict) {
			size_t len;
			char *pnp = init_slurp(c, "/proc/net/pnp", &len);
			char *resolv = pnp != NULL ? init_pnp_resolv(pnp, len)
						   : NULL;

			if (resolv != NULL &&
			    ops->write_file(files[i].dst, resolv,
					    strlen(resolv)) != 0)
				init_log(c, "could not write %s: %s",
					 files[i].dst, strerror(errno));
			free(resolv);
			free(pnp);
		}
	}
	// hull archives these files as root-only. A workload that drops to
	// another user, such as apt's _apt, still has to read them.
	for (i = 0; i < n; i++)
		ops->chmod(files[i].dst, 0644);
}

// Writes an /etc/hosts with the loopback names when the image has none and
// hull sent none, as hull's vz-init does. Without it nothing in the guest
// resolves localhost.
static void init_default_hosts(const struct init_ctx *c)
{
	const struct sysops *ops = c->ops;
	char buf[256];
	char *name;
	size_t len = 0;
	size_t i;
	int n;

	if (init_nonempty(c, "/urunc-hosts") ||
	    init_nonempty(c, INIT_NEWROOT "/etc/hosts"))
		return;
	name = init_slurp(c, "/proc/sys/kernel/hostname", &len);
	for (i = 0; name != NULL && i < len; i++) {
		if (name[i] == '\n' || name[i] == ' ' || name[i] == '\t') {
			name[i] = '\0';
			break;
		}
	}
	if (name != NULL && name[0] != '\0')
		n = snprintf(buf, sizeof(buf),
			     "127.0.0.1\tlocalhost\n"
			     "::1\tlocalhost ip6-localhost ip6-loopback\n"
			     "127.0.1.1\t%.64s\n", name);
	else
		n = snprintf(buf, sizeof(buf),
			     "127.0.0.1\tlocalhost\n"
			     "::1\tlocalhost ip6-localhost ip6-loopback\n");
	free(name);
	ops->mkdir_p(INIT_NEWROOT "/etc", 0755);
	if (ops->write_file(INIT_NEWROOT "/etc/hosts", buf, (size_t)n) != 0) {
		init_log(c, "could not write a default /etc/hosts: %s",
			 strerror(errno));
		return;
	}
	ops->chmod(INIT_NEWROOT "/etc/hosts", 0644);
}

static int init_move_mounts(const struct init_ctx *c)
{
	const struct sysops *ops = c->ops;
	static const char *const moved[] = { "/dev", "/proc", "/sys" };
	size_t i;

	for (i = 0; i < sizeof(moved) / sizeof(moved[0]); i++) {
		char dst[64];
		char msg[96];

		snprintf(dst, sizeof(dst), INIT_NEWROOT "%s", moved[i]);
		snprintf(msg, sizeof(msg), "could not move %s into the new root",
			 moved[i]);
		ops->mkdir_p(dst, 0755);
		if (init_failed(c, ops->mount(moved[i], dst, NULL, MS_MOVE,
					      NULL),
				msg))
			return 1;
		if (i != 0)
			continue;
		// The agent's tty sessions need /dev/pts.
		ops->mkdir_p(INIT_NEWROOT "/dev/pts", 0755);
		ops->mkdir_p(INIT_NEWROOT "/dev/shm", 0755);
		if (init_failed(c, ops->mount("devpts", INIT_NEWROOT "/dev/pts",
					      "devpts", MS_NOSUID | MS_NOEXEC,
					      "gid=5,mode=0620,ptmxmode=0666"),
				"could not mount devpts"))
			return 1;
		if (init_failed(c, ops->mount("shm", INIT_NEWROOT "/dev/shm",
					      "tmpfs",
					      MS_NOSUID | MS_NODEV,
					      "mode=1777"),
				"could not mount /dev/shm"))
			return 1;
	}
	// Workloads expect the unified cgroup hierarchy, as hull's vz-init
	// mounts it. A kernel without cgroup2 boots on without it.
	ops->mkdir_p(INIT_NEWROOT "/sys/fs/cgroup", 0755);
	if (ops->mount("cgroup2", INIT_NEWROOT "/sys/fs/cgroup", "cgroup2",
		       MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0)
		init_log(c, "could not mount cgroup2: %s", strerror(errno));
	return 0;
}

// Builds the argv for supervise_main: "urunit", the command and its
// arguments. /urunc-cmd, one argument per line, replaces argv[1..], since
// the kernel command line splits on whitespace. With neither, a urunit.conf
// in the initrd carries the command (ARC/ARV), and the supervisor, started
// with no arguments, takes it from there.
static char **init_command(const struct init_ctx *c, int argc, char **argv,
			   int *out_argc)
{
	size_t len;
	char *buf = init_slurp(c, "/urunc-cmd", &len);
	char **cmd = NULL;
	char **out;
	size_t n = 0;
	size_t i;

	if (buf != NULL) {
		cmd = init_read_lines(buf, len);
		free(buf);
		if (cmd == NULL)
			return NULL;
		while (cmd[n] != NULL)
			n++;
	} else if (argc > 1) {
		n = (size_t)argc - 1;
	} else if (init_exists(c, "/urunit.conf")) {
		out = calloc(2, sizeof(*out));
		if (out == NULL || (out[0] = strdup("urunit")) == NULL) {
			init_log(c, "out of memory");
			free(out);
			return NULL;
		}
		*out_argc = 1;
		return out;
	}
	if (n == 0 || (cmd != NULL ? cmd[0][0] : argv[1][0]) == '\0') {
		init_log(c, "no command to run");
		init_free_lines(cmd);
		return NULL;
	}

	out = calloc(n + 2, sizeof(*out));
	if (out == NULL) {
		init_free_lines(cmd);
		return NULL;
	}
	out[0] = strdup("urunit");
	// switch_root is followed by an exec of the command, so a bare name
	// such as "bash" is looked up in the new root first.
	out[1] = init_resolve_command(c->ops, INIT_NEWROOT,
				      cmd != NULL ? cmd[0] : argv[1]);
	for (i = 1; i < n; i++)
		out[i + 1] = strdup(cmd != NULL ? cmd[i] : argv[i + 1]);
	init_free_lines(cmd);
	for (i = 0; i < n + 1; i++) {
		if (out[i] == NULL)
			break;
	}
	if (i < n + 1) {
		init_log(c, "out of memory");
		for (i = 0; i < n + 1; i++)
			free(out[i]);
		free(out);
		return NULL;
	}
	*out_argc = (int)n + 1;
	return out;
}

// Sets the layers of a root the /urunc-rootfs-* markers describe. The lower
// is the disk /urunc-rootfs-lower names, mounted read-only, or the virtiofs
// tag rootfs without it (vz-init only). With an overlay, /urunc-rootfs-upper
// names the upper: a disk, the virtiofs tag rootfs-upper, or a tmpfs without
// it. A named disk that is missing ends the boot, and so does any marker that
// does not parse.
static int init_layers(const struct init_ctx *c, int overlay, char **root,
			  char **fstype, char **flags,
			  struct init_upper *upper, char **upper_dev,
			  int *final)
{
	char *lower = init_slurp(c, "/urunc-rootfs-lower", NULL);
	char *line = NULL;
	char *serial = NULL;
	enum init_upper_kind kind;
	int rc = 1;

	if (lower == NULL) {
		*root = strdup("rootfs");
		*fstype = strdup("virtiofs");
		if (*root == NULL || *fstype == NULL) {
			init_log(c, "out of memory");
			goto out;
		}
	} else {
		if (!overlay) {
			init_log(c, "/urunc-rootfs-direct and "
				    "/urunc-rootfs-lower are both present");
			goto out;
		}
		if (init_parse_lower(lower, &serial, fstype) != 0) {
			init_log(c, "cannot parse /urunc-rootfs-lower: '%.*s'",
				 (int)init_trimmed_len(lower), lower);
			goto out;
		}
		*root = init_dev_by_serial(c->ops, serial, 20);
		if (*root == NULL) {
			init_log(c, "no virtio-blk disk with serial %s for the "
				    "lower layer", serial);
			goto out;
		}
		*flags = strdup("ro");
		*final = 1;
		free(serial);
		serial = NULL;
	}
	if (!overlay) {
		rc = 0;
		goto out;
	}

	line = init_slurp(c, "/urunc-rootfs-upper", NULL);
	if (line == NULL) {
		rc = 0;
		goto out;
	}
	if (init_parse_upper(line, &kind, &serial) != 0) {
		init_log(c, "cannot parse /urunc-rootfs-upper: '%.*s'",
			 (int)init_trimmed_len(line), line);
		goto out;
	}
	switch (kind) {
	case INIT_UPPER_TMPFS:
		break;
	case INIT_UPPER_VIRTIOFS:
		upper->src = "rootfs-upper";
		upper->type = "virtiofs";
		upper->what = "virtiofs";
		*final = 1;
		break;
	case INIT_UPPER_BLOCK:
		*upper_dev = serial != NULL
				     ? init_dev_by_serial(c->ops, serial, 20)
				     : strdup("/dev/vda");
		if (*upper_dev == NULL) {
			init_log(c, "no virtio-blk disk with serial %s for the "
				    "upper layer", serial);
			goto out;
		}
		if (lower != NULL && strcmp(*upper_dev, *root) == 0) {
			init_log(c, "the upper layer %s is also the lower layer",
				 *upper_dev);
			goto out;
		}
		upper->src = *upper_dev;
		upper->type = "ext4";
		upper->what = "block";
		*final = 1;
		break;
	}
	rc = 0;
out:
	free(lower);
	free(line);
	free(serial);
	return rc;
}

int init_run(const struct sysops *ops, enum urunit_mode mode, int argc,
	     char **argv, const char *cmdline, int *out_argc,
	     char ***out_argv)
{
	struct init_ctx c;
	char *cmdline_buf = NULL;
	char *root = NULL;
	char *fstype = NULL;
	char *flags = NULL;
	char *upper_dev = NULL;
	struct init_upper upper = init_upper_tmpfs;
	char **out;
	int overlay;
	int layered;
	int final = 0;
	int rc = 1;

	c.ops = ops;
	c.strict = mode == URUNIT_MODE_INIT;
	c.prefix = c.strict ? "container-init: " : "vz-init: ";

	if (init_failed(&c, ops->mount("proc", "/proc", "proc",
				       MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL),
			"could not mount /proc"))
		return 1;
	if (init_failed(&c, ops->mount("sysfs", "/sys", "sysfs",
				       MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL),
			"could not mount /sys"))
		return 1;
	if (init_failed(&c, ops->mount("dev", "/dev", "devtmpfs", MS_NOSUID,
				       NULL),
			"could not mount /dev"))
		return 1;

	// The clock comes first, before anything checks a certificate.
	init_set_clock(&c);
	if (init_rosetta(&c))
		return 1;

	// The markers decide the root in both modes. Without them, init takes
	// it from the kernel command line and vz-init from the virtiofs tag
	// rootfs.
	layered = !c.strict || init_exists(&c, "/urunc-rootfs-lower");
	if (c.strict) {
		if (cmdline == NULL) {
			cmdline_buf = ops->read_file("/proc/cmdline", NULL);
			cmdline = cmdline_buf;
		}
		if (init_parse_cmdline_root(cmdline, &root, &fstype, &flags)) {
			init_log(&c, "out of memory");
			goto out;
		}
	}
	if (c.strict && layered &&
	    (root != NULL || fstype != NULL || flags != NULL)) {
		init_log(&c, "/urunc-rootfs-lower and a root on the kernel "
			     "command line are both present");
		goto out;
	}
	if (!layered) {
		if (init_exists(&c, "/urunc-rootfs-upper")) {
			init_log(&c, "/urunc-rootfs-upper without "
				     "/urunc-rootfs-lower");
			goto out;
		}
		if (root == NULL || root[0] == '\0' || fstype == NULL ||
		    fstype[0] == '\0') {
			init_log(&c, "no root= and rootfstype= on the kernel "
				     "command line");
			goto out;
		}
		overlay = init_exists(&c, "/urunc-overlay");
	} else {
		overlay = !init_exists(&c, "/urunc-rootfs-direct");
		if (init_layers(&c, overlay, &root, &fstype, &flags, &upper,
				&upper_dev, &final))
			goto out;
	}

	if (init_mount_root(&c, root, fstype, flags, overlay, &upper, final))
		goto out;
	// The two scripts differ in order and the order matters when an
	// export covers /tmp, /etc or /run. container-init stages the binary,
	// mounts the exports, then writes the network files. vz-init fixes
	// /tmp and writes the network files into the rootfs first, then mounts
	// the exports and stages the binary last, so the exports neither
	// receive the chmod and the files nor hide the staged binary.
	if (c.strict) {
		if (init_stage(&c))
			goto out;
		if (init_extra_mounts(&c))
			goto out;
		init_network_files(&c);
		init_default_hosts(&c);
	} else {
		size_t len;
		char *env = init_slurp(&c, "/urunc-env", &len);

		if (env != NULL && init_env_export(ops, env) != 0)
			init_log(&c, "could not export every line of "
				     "/urunc-env");
		free(env);
		// An OCI image unpacked on macOS often has a 0755 /tmp. The
		// container runtime contract is a writable 01777 /tmp.
		ops->mkdir_p(INIT_NEWROOT "/tmp", 0755);
		ops->chmod(INIT_NEWROOT "/tmp", 01777);
		init_network_files(&c);
		init_default_hosts(&c);
		if (init_extra_mounts(&c))
			goto out;
		if (init_stage(&c))
			goto out;
	}

	if (init_move_mounts(&c))
		goto out;

	out = init_command(&c, argc, argv, out_argc);
	if (out == NULL)
		goto out;
	if (ops->switch_root(INIT_NEWROOT) != 0) {
		// There is no root to go on with in either mode.
		init_log(&c, "could not switch to " INIT_NEWROOT ": %s",
			 strerror(errno));
		init_free_lines(out);
		goto out;
	}
	*out_argv = out;
	rc = 0;
out:
	free(cmdline_buf);
	free(upper_dev);
	free(root);
	free(fstype);
	free(flags);
	return rc;
}

int init_main(enum urunit_mode mode, int argc, char **argv)
{
	int app_argc = 0;
	char **app_argv = NULL;

	if (init_run(&sysops_real, mode, argc, argv, NULL, &app_argc,
		     &app_argv) != 0)
		return 1;
	return supervise_main(app_argc, app_argv);
}
