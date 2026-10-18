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
	// Returns 0 when path exists.
	int (*exists)(const char *path);
	// Sleeps for ms milliseconds.
	void (*nap)(unsigned int ms);
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
// It refuses to run when / is not a ramfs or tmpfs, or when newroot is not
// a mount point. The deletion never crosses into another filesystem.
int sysops_switch_root_real(const char *newroot);
// Deletes the tree under the directory fd, skipping every entry on a device
// other than rootdev. It takes ownership of fd.
void sysops_rm_tree(int fd, dev_t rootdev);

// The helpers below are part of init_run. They are exported for the unit
// tests.

// One line of /urunc-mounts: a virtiofs tag, its absolute path in the guest
// and whether it is mounted read-only.
struct init_mount {
	char *tag;
	char *dest;
	int ro;
};

// Finds root=, rootfstype= and rootflags= in a kernel command line. The
// parameters after "--" belong to the application and are not looked at.
// Each output is a heap string, or NULL when the parameter is absent.
// Returns 0, or -1 when memory runs out.
int init_parse_cmdline_root(const char *cmdline, char **root, char **fstype,
			    char **flags);

// Splits buf into lines. A trailing newline adds no empty line. Returns a
// NULL terminated, heap allocated vector, or NULL when memory runs out.
char **init_read_lines(const char *buf, size_t len);

// Frees a vector from init_read_lines.
void init_free_lines(char **lines);

// Parses /urunc-mounts, one "<tag>\t<dest>\t<ro|rw>" per line. Lines without
// a tag or a dest are skipped. Returns an array that ends with an entry
// whose tag is NULL, or NULL when memory runs out.
struct init_mount *init_parse_mounts_tsv(const char *buf, size_t len);

// Frees an array from init_parse_mounts_tsv.
void init_free_mounts(struct init_mount *mounts);

// Returns the nameserver, domain and search lines of /proc/net/pnp as a
// heap string, or NULL when there is no nameserver line.
char *init_pnp_resolv(const char *buf, size_t len);

// Returns the absolute path of a bare command name, found in the standard
// directories under newroot. An absolute command, or one that is not found,
// comes back unchanged. The result is a heap string, or NULL when memory
// runs out.
char *init_resolve_command(const struct sysops *ops, const char *newroot,
			   const char *cmd);

// Exports every KEY=VALUE line of buf with ops->setenv. Blank lines and
// lines that start with '#' are skipped. Returns 0, or -1 when a setenv
// failed.
int init_env_export(const struct sysops *ops, const char *buf);

// The upper layer of the overlay that /urunc-rootfs-upper names.
enum init_upper_kind {
	INIT_UPPER_TMPFS,
	INIT_UPPER_VIRTIOFS,
	INIT_UPPER_BLOCK,
};

// Parses /urunc-rootfs-lower, block:<serial>:<fstype>, into heap copies of
// the serial and the filesystem type. Returns -1, and sets neither, for
// anything else.
int init_parse_lower(const char *line, char **serial, char **fstype);

// Parses /urunc-rootfs-upper: block:<serial>, the older words block
// (/dev/vda) and virtiofs (the tag rootfs-upper), or nothing for a tmpfs.
// *serial is a heap copy for block:<serial> and NULL otherwise. Returns -1
// for any other word.
int init_parse_upper(const char *line, enum init_upper_kind *kind,
		     char **serial);

// Returns "/dev/<name>" for the virtio-blk disk whose serial is serial, in a
// heap buffer, or NULL when none answers after tries looks 100 ms apart.
char *init_dev_by_serial(const struct sysops *ops, const char *serial,
			 int tries);

// Splits mount options into MS_* flags, added to *flags, and the options
// left for the filesystem. Returns those as a heap string, or NULL when
// there are none.
char *init_mount_opts(const char *opts, unsigned long *flags);

#endif // URUNIT_SYSOPS_H
