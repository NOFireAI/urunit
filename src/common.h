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

#ifndef COMMON_H
#define COMMON_H

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SERIAL_MAX_SZ 10

#ifdef DEBUG
#define SHOW_DEBUG 1
#else
#define SHOW_DEBUG 0
#endif

#define DEBUG_PRINTF(fmt, ...) \
	do { if (SHOW_DEBUG) fprintf(stderr, "[DEBUG] " fmt, __VA_ARGS__); } while (0)

#define DEBUG_PRINT(fmt) \
	do { if (SHOW_DEBUG) fprintf(stderr, "[DEBUG] " fmt); } while (0)

struct block_config {
	char *id;
	char *mountpoint;
};

struct net_config {
	char *ip;
	char *gateway;
	char *mask;
};

int ensure_dir(const char *path);
int mkdir_all(const char *path, mode_t mode, char *first_dir);
int rm_empty_dirs(const char *dir, const char *top_dir);
char *skip_n_words(const char *str, size_t n);
int is_block_fs(const char *fs_type);
int is_network_fs(const char *fs_type);
int is_cloud_storage_fs(const char *fs_type);

// Platform specific functions
char *read_raw_device(int fd, size_t *size);
int setup_console(void);
int remount_root_rw(void);
char *get_boot_var(const char *name);
int read_block_dev_serial(const char *device_name, char *serial, const size_t size);
// The size of the path vblock_device_name writes, "/dev/vdX" and its NUL.
#define VBLOCK_NAME_SZ 9
void vblock_device_name(const uint32_t n, char *device_path);
int is_order_block_id(const char *id);
int find_vblock_device_by_order(const uint32_t n, char *device_path);
int find_vblock_device_by_serial(const char *target_serial, char *device_path);
int mount_special_fs(void);
int mount_block_vols(struct block_config **vols);
int configure_network(struct net_config *net);
int set_default_route(void);
void unmount_external(void);
int set_subreaper(void);
int setup_controlling_tty(void);
void request_reboot(void);

// configuration
//
// The urunit configuration (urunit.conf) and its parser, defined in
// main.c. The parsed values point inside the buffer that holds the
// configuration, so that buffer has to outlive them.

struct process_config {
	uint32_t uid;
	uint32_t gid;
	char     *wdir;
	uint32_t argc;
	char     **argv;
};

struct app_exec_config {
	char	 **envs;
	char	 *path_env;
	struct process_config *pr_conf;
	struct block_config **blk_conf;
	struct net_config *net_conf;
};

// record_iter: Iterates over the NUL-terminated records of the configuration,
// starting at pos and stopping at end.
struct record_iter {
	char *pos;
	char *end;
};

char *read_exact_size(FILE *f, size_t sz);
char *read_file_and_size(char *file, size_t *size);
char *next_record(struct record_iter *iter);
size_t count_records(struct record_iter iter, const char *end_marker);
int is_field(const char *rec, const char *key);
int get_uint_val(char *str, uint32_t *value);
int get_string_val(char *str, char **value);
int parse_envs(struct record_iter *iter, char ***envs, char **path_env);
int parse_process_config(struct record_iter *iter, struct process_config **pconf);
int parse_net_config(struct record_iter *iter, struct net_config **nconf);
int parse_block_config(struct record_iter *iter, struct block_config ***bconf);
void free_block_config(struct block_config **bentries);
struct app_exec_config *get_config_from_file(char *file, char **sbuf);
void free_app_config(struct app_exec_config *config);

#endif // COMMON_H
