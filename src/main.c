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

// Parts of the following code are taken from
// https://github.com/krallin/tini/tree/master
// which comes with the The MIT License (MIT)
// In particular:
// The MIT License (MIT)
//
// Copyright (c) 2015 Thomas Orozco <thomas@orozco.fr>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// For more information, please check https://github.com/krallin/tini/blob/master/LICENSE

// The supervisor (PID 1) of the application. main() is in urunit.c, which
// calls supervise_main() when urunit runs under its own name.

#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>

#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "agentlog.h"
#include "common.h"
#include "urunit.h"

#define STATUS_MAX 255
#define STATUS_MIN 0

// The maximum length of an argument that spawn_app reassembles from quoted
// words of the command line, including its NUL.
#define QUOTED_ARG_MAX 1024

// The pid of the agent urunit started, 0 when there is none or it was reaped.
static pid_t agent_pid = 0;

void (*urunit_sync)(void) = sync;

void set_agent_pid(pid_t pid) {
	agent_pid = pid;
}

// The pid of the application, for forward_signal. 0 until it runs.
static volatile pid_t forward_pid = 0;

extern char **environ;

int isolate_child(void) {
	int ret = 0;
	sigset_t set;

	// First try to become a session leader with the console as the
	// controlling terminal, so that the application gets job control
	// and interactive shells stop complaining about missing ttys.
	// If that is not possible (no tty, unknown console device), fall
	// back to plain process group handling below.
	if (setup_controlling_tty() == 0) {
		return 0;
	}

	ret = sigemptyset(&set);
	if (ret) {
		perror("sigemptyset");
		return 1;
	}
	ret = sigaddset(&set, SIGTTOU);
	if (ret) {
		perror("sigaddset");
		return 1;
	}
	ret = sigaddset(&set, SIGTTIN);
	if (ret) {
		perror("sigaddset");
		return 1;
	}
	ret = sigprocmask(SIG_BLOCK, &set, NULL);
	if (ret) {
		perror("sigprocmask");
		return 1;
	}

	// Put the child into a new process group.
	if (setpgid(0, 0) < 0) {
		perror("setpgid");
		return 1;
	}

	// If there is a tty, allocate it to this new process group. We
	// can do this in the child process because we're blocking
	// SIGTTIN / SIGTTOU.
	// Doing it in the child process avoids a race condition scenario
	// if urunit is calling urunit (in which case the grandparent may make the
	// parent the foreground process group, and the actual child ends up...
	// in the background!)
	if (tcsetpgrp(STDIN_FILENO, getpgrp())) {
		if (errno != ENOTTY && errno != ENXIO) {
			perror("tcsetpgrp");
			return 1;
		}
	}

	return 0;
}

// read_exact_size: Reads exactly sz bytes from a file. It returns a
// dynamically allocated memory and the caller is responsible to free it.
//
// Arguments:
// 1. f:	The pointer to a FILE
// 2. sz:	The amount of bytes to read
//
// Return value:
// On success it returns a buffer of sz + 1 bytes: the sz bytes read from the
// file followed by a trailing NUL, so callers can treat it as a string (the
// same contract as the raw-device path). The caller frees it.
// On failure, it returns NULL.
char *read_exact_size(FILE *f, size_t sz) {
	size_t total_read = 0;
	size_t bytes_read = 0;
	char *buffer = NULL;

	// Allocate one extra byte for a trailing NUL so the buffer is a valid
	// C string; the section parsers (strtok) and the memcmp probes in
	// get_config_from_file rely on it, as read_raw_device already does.
	buffer = malloc(sz + 1);
	if (!buffer) {
		fprintf(stderr, "Failed to allocate memory for file contents\n");
		return NULL;
	}

	while (total_read < sz) {
		bytes_read = fread(buffer + total_read, 1, sz - total_read, f);
		// the retrun value of fread does not distinguish between EOF and
		// an error. Therefore, we have to use feof and ferror.
		if (bytes_read == 0) {
			if (feof(f)) {
				// No more bytes to read.
				break;
			} else if (ferror(f)) {
				fprintf(stderr, "Failed to read file data at offset %zu\n", total_read);
				goto read_exact_error;
			}
		}
		total_read += bytes_read;
	}

	// We are out of the loop so we read as much bytes the caller asked
	// or we reached the EOF. Check which of the two happened.
	if (total_read != sz) {
		fprintf(stderr, "Read %zu bytes, expected %zu bytes\n", total_read, sz);
		goto read_exact_error;
	}

	buffer[sz] = '\0';

	return buffer;

read_exact_error:
	free(buffer);
	return NULL;
}

// read_file_and_size: Opens <file>, determines whether it is a regular file or
// a block device and reads it accordingly.
//
// Arguments:
// 1. file:	The file to read
// 2. size:	The total size of the file
//
// Return value:
// On success it returns a buffer with all the contents of the file and updates
// the size argument to contain the total size of the file.
// On failure, it returns NULL.
char *read_file_and_size(char *file, size_t *size) {
	FILE *fp = NULL;
	struct stat st = { 0 };
	char *buf = NULL;

	DEBUG_PRINTF("Read configuration file %s\n", file);
	fp = fopen(file, "rb");
	if (!fp) {
		perror("Read configuration file");
		return NULL;
	}

	// Find the total size of the file in order to read the whole file
	// and have a limit to search in the buffer.
	if (fstat(fileno(fp), &st) != 0) {
		perror("Getting configuration file size");
		fclose(fp);
		return NULL;
	}

	// A raw block device has no size in st_size, so it is read by
	// the platform code (which only borrows the descriptor); a regular file
	// is read through the stdio stream. Only one of the two interfaces is
	// used per call, so the stdio buffer and the raw reads never disagree.
	if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
		DEBUG_PRINT("Configuration is a block device\n");
		buf = read_raw_device(fileno(fp), size);
	} else {
		DEBUG_PRINTF("Total size of configuration file %ld\n", (long)st.st_size);
		buf = read_exact_size(fp, st.st_size);
		if (buf)
			*size = st.st_size;
	}
	fclose(fp);
	if (!buf) {
		fprintf(stderr, "Could not read configuration %s\n", file);
	}

	return buf;
}

// The configuration consists of records. Every record is a NUL-terminated
// string. Since neither environment variables nor arguments can contain a NUL
// character, every value is stored verbatim, even if it contains new lines.
// In the legacy format the records are separated with new lines instead. The
// first record tells the two apart: it ends with a NUL in the current format
// and with a new line in the legacy one.
//
// An earlier writer began the configuration with a CONFIG_MAGIC record. It is
// skipped when present.
#define CONFIG_MAGIC "URUNIT1"

// config_has_nul_records: Returns whether the configuration in buf is in the
// NUL-terminated format, i.e. a NUL comes before the first new line. A legacy
// configuration on a raw block device has zero padding after it, but a new
// line ends its first record.
//
// Arguments:
// 1. buf:	The configuration.
// 2. size:	Its size in bytes.
//
// Return value:
// 1 for the NUL-terminated format, 0 for the legacy one.
int config_has_nul_records(const char *buf, size_t size) {
	for (size_t i = 0; i < size; i++) {
		if (buf[i] == '\0')
			return 1;
		if (buf[i] == '\n')
			return 0;
	}

	return 0;
}

// struct record_iter is declared in common.h.

// next_record: Returns the next non-empty record and moves the iterator past
// it. Empty records carry no information (e.g. the zero padding of a raw block
// device or consecutive new lines in the legacy format) and are skipped.
//
// Arguments:
// 1. iter:	The iterator over the configuration records.
//
// Return value:
// A pointer to the next record or NULL if there are no more records.
char *next_record(struct record_iter *iter) {
	while (iter->pos < iter->end) {
		char *rec = iter->pos;
		size_t len = strnlen(rec, iter->end - rec);

		// The configuration buffer always has a trailing NUL right
		// after end, hence the last record is terminated even if it
		// reaches end.
		iter->pos = rec + len + 1;
		if (len > 0)
			return rec;
	}

	return NULL;
}

// count_records: Counts the records from the current position of the iterator
// until the end_marker record or the end of the configuration. The iterator
// is passed by value and therefore stays intact.
//
// Arguments:
// 1. iter:		The iterator over the configuration records.
// 2. end_marker:	The record that ends the counting.
//
// Return value:
// The number of records found before end_marker.
size_t count_records(struct record_iter iter, const char *end_marker) {
	size_t cnt = 0;
	char *rec = NULL;

	while ((rec = next_record(&iter)) != NULL && strcmp(rec, end_marker) != 0)
		cnt++;

	return cnt;
}

// is_field: Checks whether a record is a "KEY:VALUE" field with the given key.
// The whole key, followed by ':', has to match.
//
// Arguments:
// 1. rec:	The record to check.
// 2. key:	The key of the field.
//
// Return value:
// 1 if the record is a field with the given key, otherwise 0.
int is_field(const char *rec, const char *key) {
	size_t key_len = strlen(key);

	return strncmp(rec, key, key_len) == 0 && rec[key_len] == ':';
}

// parse_envs: Parses the environment variable list. The list begins with the
// "UES" record (already consumed by the caller) and ends with the "UEE"
// record. Every record in between is an environment variable. The array of the
// environment variables points inside the configuration buffer and it is NULL
// terminated, so it can be passed as the environment variables table at
// execve and friends. The caller is responsible to free the array.
//
// Arguments:
// 1. iter:	The iterator over the configuration records. On success, it
//		moves past the "UEE" record.
// 2. envs:	Will point to the array of the environment variables or NULL
//		if the list is empty.
// 3. path_env:	Will point to the PATH environment variable, if it was found.
//
// Return value:
// On success 0 is returned. Otherwise, -1 is returned.
int parse_envs(struct record_iter *iter, char ***envs, char **path_env) {
	size_t total_envs = 0;
	char **env_vars = NULL;
	char *rec = NULL;
	size_t i = 0;

	total_envs = count_records(*iter, "UEE");
	DEBUG_PRINTF("Found %zu environment variables\n", total_envs);
	// One more pointer for the end of the table (NULL)
	env_vars = malloc((total_envs + 1) * sizeof(char *));
	if (!env_vars) {
		fprintf(stderr, "Failed to allocate memory for environment variables\n");
		return -1;
	}

	while ((rec = next_record(iter)) != NULL) {
		if (strcmp(rec, "UEE") == 0) {
			env_vars[i] = NULL;
			if (i == 0) {
				free(env_vars);
				env_vars = NULL;
			}
			*envs = env_vars;
			return 0;
		}
		DEBUG_PRINTF("Found env %s\n", rec);
		env_vars[i++] = rec;
		if (!*path_env && strncmp(rec, "PATH=", 5) == 0) {
			DEBUG_PRINTF("Found PATH env %s\n", rec);
			*path_env = rec;
		}
	}

	fprintf(stderr, "Invalid format of environment variable list. \"UEE\" was not found\n");
	free(env_vars);
	return -1;
}

// get_uint_val: Converts the value of "KEY: VALUE" string  to uint32_t
//
// Arguments:
// 1. str:	The string to convert in the form "KEY: VAL"
// 2. value:	A pointer to uint32_t where the converted value will get stored.
//
// Return value:
// On success 0 is returned and value contains the coverted value.
// On failure, -1 is returned and value stays intact.
int get_uint_val(char *str, uint32_t *value) {
	size_t str_sz = strlen(str);
	char *val_str = strchr(str, ':');
	unsigned long val = 0;
	char *end = NULL;

	if (val_str == NULL) {
		// We could not find the beginning of the value string.
		fprintf(stderr, "Failed to find ':' character in %s\n", str);
		return -1;
	}

	// strchr will return a pointer to ':', but we need to move passed
	// ':', hence +1 character.
	if (val_str + 1 >= str + str_sz) {
		// We can not go over the string. Something is wrong
		fprintf(stderr, "Failed to find value after ':' in %s\n", str);
		return -1;
	}
	val_str ++;

	// strtoul can take care of spaces.
	val = strtoul(val_str, &end, 10);
	if (errno == ERANGE || val > UINT32_MAX) {
		perror("Convert string to uint32_t");
		return -1;
	}
	if (*end != '\0') {
		fprintf(stderr, "Failed to convert %s to uint32_t. Got trailing character %c\n", val_str, *end);
		return -1;
	}

	*value = (uint32_t)val;

	return 0;
}

// get_string_val: Returns the string value of "KEY: VALUE" strings.
//
// Arguments:
// 1. str:	The whole string in the form "KEY: VALUE"
// 2. value:	A pointer which will point to the beginning of the VALUE
//
// Return value:
// On success 0 is returned and value points to the beginning of VALUE
// On failure, -1 is returned and value stays intact.
int get_string_val(char *str, char **value) {
	size_t str_sz = strlen(str);
	char *val_str = strchr(str, ':');

	if (val_str == NULL) {
		// We could not find the beginning of the value string.
		fprintf(stderr, "Failed to find ':' character in %s\n", str);
		return -1;
	}

	// strchr will return a pointer to ':', but we need to move pass this character
	// and until we find a non-space value.
	val_str++;
	while ((val_str < str + str_sz) && *val_str != '\0') {
		if (!isspace(*val_str)) {
			*value = val_str;

			return 0;
		}
		val_str++;
	}

	// We can not go over the string. Something is wrong
	fprintf(stderr, "Failed to find value after ':' in %s\n", str);

	return -1;
}

// parse_process_config: Parses the process configuration with the following
// records:
// UCS
// UID:<uid>
// GID:<gid>
// WD:<working directory>
// ARC:<number of arguments>      (optional, followed by ARC records of)
// ARV:<argument>                 (taken verbatim)
// UCE
// The "UCS" record is already consumed by the caller. The caller is
// responsible to free the returned process_config.
//
// Arguments:
// 1. iter:	The iterator over the configuration records. On success, it
//		moves past the "UCE" record.
// 2. pconf:	Will point to the parsed process configuration.
//
// Return value:
// On success 0 is returned. Otherwise, -1 is returned.
int parse_process_config(struct record_iter *iter, struct process_config **pconf) {
	struct process_config *conf = NULL;
	char *rec = NULL;
	uint32_t found_argv = 0;

	conf = malloc(sizeof(struct process_config));
	if (!conf) {
		fprintf(stderr, "Failed to allocate memory for app execution environment config\n");
		return -1;
	}
	memset(conf, 0, sizeof(struct process_config));
	conf->wdir = NULL; // Sanity
	conf->argv = NULL;
	conf->argc = 0;

	while ((rec = next_record(iter)) != NULL) {
		int ret = 0;

		if (is_field(rec, "UID")) {
			ret = get_uint_val(rec, &(conf->uid));
			if (ret != 0) {
				fprintf(stderr, "Failed to retrieve UID information from %s\n", rec);
				break;
			}
		} else if (is_field(rec, "GID")) {
			ret = get_uint_val(rec, &(conf->gid));
			if (ret != 0) {
				fprintf(stderr, "Failed to retrieve GID information from %s\n", rec);
				break;
			}
		} else if (is_field(rec, "WD")) {
			ret = get_string_val(rec, &(conf->wdir));
			if (ret != 0) {
				fprintf(stderr, "Failed to retrieve WD information from %s\n", rec);
				break;
			}
		} else if (is_field(rec, "ARC")) {
			// Number of arguments of the application command. It
			// must precede the ARV entries.
			uint32_t argc = 0;

			ret = get_uint_val(rec, &argc);
			if (ret != 0 || conf->argv != NULL) {
				fprintf(stderr, "Failed to retrieve ARC information from %s\n", rec);
				break;
			}
			// Compute the element count in size_t. argc is uint32_t, so
			// "argc + 1" alone is 32-bit unsigned and wraps to 0 for
			// argc == UINT32_MAX, handing calloc a zero-size allocation
			// that the following ARV writes would then overflow. In size_t
			// the +1 cannot wrap, and an oversized count fails calloc below.
			conf->argv = calloc((size_t)argc + 1, sizeof(char *));
			if (!conf->argv) {
				fprintf(stderr, "Failed to allocate memory for the application arguments\n");
				break;
			}
			conf->argc = argc;
			found_argv = 0;
		} else if (is_field(rec, "ARV")) {
			// One argument of the application command, taken verbatim
			// (it may contain spaces or new lines, or be empty).
			if (conf->argv == NULL || found_argv >= conf->argc) {
				fprintf(stderr, "Unexpected ARV entry %s\n", rec);
				break;
			}
			conf->argv[found_argv++] = rec + 4;
			DEBUG_PRINTF("Found argument %s\n", rec + 4);
		} else if (strcmp(rec, "UCE") == 0) {
			if (conf->argv != NULL && found_argv != conf->argc) {
				fprintf(stderr, "Expected %u arguments, found %u\n", conf->argc, found_argv);
				break;
			}
			*pconf = conf;
			return 0;
		}
	}

	if (!rec)
		fprintf(stderr, "Invalid format of application execution environment configuration. \"UCE\" was not found\n");
	free(conf->argv);
	free(conf);
	return -1;
}

// parse_net_config: Parses the network configuration with the following
// records:
// UNS
// IP:<ipv4 address>
// GW:<gateway>
// MSK:<netmask>
// UNE
// It is used by guests that can not get the network configuration from the
// kernel command line (e.g. FreeBSD). The "UNS" record is already consumed by
// the caller. The caller is responsible to free the returned net_config.
//
// Arguments:
// 1. iter:	The iterator over the configuration records. On success, it
//		moves past the "UNE" record.
// 2. nconf:	Will point to the parsed network configuration.
//
// Return value:
// On success 0 is returned. Otherwise, -1 is returned.
int parse_net_config(struct record_iter *iter, struct net_config **nconf) {
	struct net_config *conf = NULL;
	char *rec = NULL;

	conf = malloc(sizeof(struct net_config));
	if (!conf) {
		fprintf(stderr, "Failed to allocate memory for network config\n");
		return -1;
	}
	memset(conf, 0, sizeof(struct net_config));
	// just for sanity
	conf->ip = NULL;
	conf->gateway = NULL;
	conf->mask = NULL;

	while ((rec = next_record(iter)) != NULL) {
		int ret = 0;

		// An empty value (e.g. "IP:") means the field is not set.
		if (is_field(rec, "IP")) {
			ret = get_string_val(rec, &(conf->ip));
			if (ret != 0) {
				conf->ip = NULL;
				fprintf(stderr, "Failed to retrieve IP information from %s\n", rec);
			}
		} else if (is_field(rec, "GW")) {
			ret = get_string_val(rec, &(conf->gateway));
			if (ret != 0) {
				conf->gateway = NULL;
				fprintf(stderr, "Failed to retrieve GW information from %s\n", rec);
			}
		} else if (is_field(rec, "MSK")) {
			ret = get_string_val(rec, &(conf->mask));
			if (ret != 0) {
				conf->mask = NULL;
				fprintf(stderr, "Failed to retrieve MSK information from %s\n", rec);
			}
		} else if (strcmp(rec, "UNE") == 0) {
			DEBUG_PRINTF("Found network config ip=%s gw=%s mask=%s\n",
				     conf->ip ? conf->ip : "", conf->gateway ? conf->gateway : "",
				     conf->mask ? conf->mask : "");
			*nconf = conf;
			return 0;
		}
	}

	fprintf(stderr, "Invalid format of network configuration. \"UNE\" was not found\n");
	free(conf);
	return -1;
}

// free_block_config: Frees a NULL terminated array of block_config entries.
//
// Arguments:
// 1. bentries:	The array to free. It can be NULL.
void free_block_config(struct block_config **bentries) {
	if (!bentries)
		return;
	for (size_t i = 0; bentries[i] != NULL; i++)
		free(bentries[i]);
	free(bentries);
}

// parse_block_config: Parses the block mount configuration with the following
// records:
// UBS
// ID:<serial_id>
// MP:<mount_point>
// ...
// UBE
// The "UBS" record is already consumed by the caller. The caller is
// responsible to free the returned array with free_block_config.
//
// Arguments:
// 1. iter:	The iterator over the configuration records. On success, it
//		moves past the "UBE" record.
// 2. bconf:	Will point to a NULL terminated array of block_config entries,
//		or NULL if there are no block entries.
//
// Return value:
// On success 0 is returned. Otherwise, -1 is returned.
int parse_block_config(struct record_iter *iter, struct block_config ***bconf) {
	struct block_config **bentries = NULL;
	struct block_config *cur = NULL;
	char *rec = NULL;
	size_t total_entries = 0;
	size_t i = 0;

	// Every entry consists of two records (ID and MP), so the number of
	// records is more than enough. The extra pointer marks the end of the
	// array with NULL, which calloc already sets.
	total_entries = count_records(*iter, "UBE");
	bentries = calloc(total_entries + 1, sizeof(struct block_config *));
	if (!bentries) {
		fprintf(stderr, "Failed to allocate memory for block entries\n");
		return -1;
	}

	while ((rec = next_record(iter)) != NULL) {
		int ret = 0;

		if (is_field(rec, "ID")) {
			// If cur is not NULL, then we never found the MP
			// entry for the previous ID.
			if (cur) {
				fprintf(stderr, "Multiple ID entries without MP\n");
				goto parse_block_config_free;
			}
			cur = malloc(sizeof(struct block_config));
			if (!cur) {
				fprintf(stderr, "Failed to allocate memory for a block entry\n");
				goto parse_block_config_free;
			}
			cur->id = NULL;
			cur->mountpoint = NULL;

			ret = get_string_val(rec, &(cur->id));
			if (ret != 0) {
				fprintf(stderr, "Failed to retrieve block ID from %s\n", rec);
				goto parse_block_config_free;
			}
			DEBUG_PRINTF("Found block entry with ID %s\n", cur->id);
		} else if (is_field(rec, "MP")) {
			if (!cur) {
				fprintf(stderr, "Found MP entry without ID: %s\n", rec);
				goto parse_block_config_free;
			}
			ret = get_string_val(rec, &(cur->mountpoint));
			if (ret != 0) {
				fprintf(stderr, "Failed to retrieve block mountpoint from %s\n", rec);
				goto parse_block_config_free;
			}
			DEBUG_PRINTF("Found block entry with MP %s\n", cur->mountpoint);
			bentries[i++] = cur;
			cur = NULL;
		} else if (strcmp(rec, "UBE") == 0) {
			// An ID without MP is not a complete entry, so drop it.
			if (cur) {
				fprintf(stderr, "Warning: Ignoring block entry %s without MP\n", cur->id);
				free(cur);
			}
			if (i == 0) {
				free(bentries);
				bentries = NULL;
			}
			*bconf = bentries;
			return 0;
		}
	}

	fprintf(stderr, "Invalid format of block volume mounts. \"UBE\" was not found\n");
parse_block_config_free:
	free(cur);
	free_block_config(bentries);
	return -1;
}

// free_app_config: Frees a configuration returned by get_config_from_file.
// The buffer of the configuration is not part of it and is freed separately.
//
// Arguments:
// 1. config:	The configuration to free. It can be NULL.
void free_app_config(struct app_exec_config *config) {
	if (!config)
		return;
	free(config->envs);
	if (config->pr_conf)
		free(config->pr_conf->argv);
	free(config->pr_conf);
	free_block_config(config->blk_conf);
	free(config->net_conf);
	free(config);
}

// get_config_from_file: Reads the contents of <file> and parses the
// configuration of the application. The configuration consists of the
// following optional sections:
// - The environment variable list, between the "UES" and "UEE" records.
// - The process configuration, between the "UCS" and "UCE" records.
// - The block mount configuration, between the "UBS" and "UBE" records.
// - The network configuration, between the "UNS" and "UNE" records.
// A "PAD" record ends the configuration and everything after it is ignored.
// The records are NUL-terminated strings or, in the legacy format, lines.
//
// Arguments:
// 1. file:	The name of the file that contains the configuration.
// 2. sbuf:	The variable that will hold the address of the allocated memory
//		that was used to read the configuration file. The caller is
//		responsible to free it.
//
// Return value:
// On success it returns a pointer to an instance of a struct app_exec_config
// which contains all the respective information for setting up the execution
// environment of the application.
struct app_exec_config *get_config_from_file(char *file, char **sbuf) {
	char **env_vars = NULL;
	size_t size = 0;
	char *buf = NULL;
	char *path_env = NULL;
	struct app_exec_config *econf = NULL;
	struct process_config *pconf = NULL;
	struct block_config **bconf = NULL;
	struct net_config *nconf = NULL;
	struct record_iter iter = { 0 };
	char *rec = NULL;
	uint8_t found_envs = 0;
	uint8_t found_pconf = 0;
	uint8_t found_bconf = 0;
	uint8_t found_nconf = 0;

	buf = read_file_and_size(file, &size);
	if (!buf) {
		fprintf(stderr, "Could not read file %s\n", file);
		return NULL;
	}
	iter.pos = buf;
	iter.end = buf + size;

	if (config_has_nul_records(buf, size)) {
		DEBUG_PRINT("Configuration with NUL-terminated records\n");
		if (size >= sizeof(CONFIG_MAGIC) && memcmp(buf, CONFIG_MAGIC, sizeof(CONFIG_MAGIC)) == 0)
			iter.pos += sizeof(CONFIG_MAGIC);
	} else {
		DEBUG_PRINT("Configuration with new line separated records (legacy)\n");
		// Turn the new lines to NUL, so both formats get parsed the
		// same way. Values with new lines can not be stored in the
		// legacy format anyway.
		for (size_t i = 0; i < size; i++) {
			if (buf[i] == '\n')
				buf[i] = '\0';
		}
	}

	while ((rec = next_record(&iter)) != NULL) {
		int ret = 0;

		if (strcmp(rec, "UES") == 0 && !found_envs) {
			DEBUG_PRINT("Checking for environment variables list\n");
			found_envs = 1;
			ret = parse_envs(&iter, &env_vars, &path_env);
			if (ret == 0 && !env_vars)
				fprintf(stderr, "Warning: No environment variables found in the configuration\n");
		} else if (strcmp(rec, "UCS") == 0 && !found_pconf) {
			DEBUG_PRINT("Checking for execution environment configuration\n");
			found_pconf = 1;
			ret = parse_process_config(&iter, &pconf);
		} else if (strcmp(rec, "UBS") == 0 && !found_bconf) {
			DEBUG_PRINT("Checking for block volumes mount configuration\n");
			found_bconf = 1;
			ret = parse_block_config(&iter, &bconf);
			if (ret == 0 && !bconf)
				fprintf(stderr, "Warning: No configuration for block mounts\n");
		} else if (strcmp(rec, "UNS") == 0 && !found_nconf) {
			DEBUG_PRINT("Checking for network configuration\n");
			found_nconf = 1;
			ret = parse_net_config(&iter, &nconf);
		} else if (strcmp(rec, "PAD") == 0) {
			// Everything after PAD is padding (e.g. up to the sector
			// size of a raw block device), not configuration.
			DEBUG_PRINT("Found padding, end of configuration\n");
			break;
		} else {
			fprintf(stderr, "Unexpected record in the configuration: %s\n", rec);
			ret = -1;
		}
		if (ret != 0)
			goto get_env_vars_error_free;
	}

	econf = malloc(sizeof(struct app_exec_config));
	if (!econf) {
		fprintf(stderr, "Could not allocate memory for app exec config struct\n");
		goto get_env_vars_error_free;
	}

	*sbuf = buf;
	econf->envs = env_vars;
	econf->path_env = path_env;
	econf->pr_conf = pconf;
	econf->blk_conf = bconf;
	econf->net_conf = nconf;
	return econf;

get_env_vars_error_free:
	free(env_vars);
	if (pconf)
		free(pconf->argv);
	free(pconf);
	free_block_config(bconf);
	free(nconf);
	free(buf);
	return NULL;
}

// load_app_config: Reads the urunit configuration, if any. The configuration
// file is given with the URUNIT_CONFIG environment variable, or with the boot
// variable of the same name on platforms that have them (FreeBSD).
//
// Arguments:
// 1. config:		Will hold the parsed configuration, or stay untouched if
//			there is no configuration file.
// 2. config_buf:	Will hold the backing buffer of the configuration, which
//			the caller is responsible to free.
//
// Return value:
// 0 if there is no configuration or it was loaded successfully.
// Otherwise 1 is returned.
int load_app_config(struct app_exec_config **config, char **config_buf) {
	char *config_file = NULL;
	uint8_t free_boot_var = 0;
	int ret = 0;

	config_file = getenv("URUNIT_CONFIG");
	if (!config_file) {
		config_file = get_boot_var("URUNIT_CONFIG");
		free_boot_var = 1;
	}
	if (!config_file) {
		DEBUG_PRINT("No configuration file\n");
		return 0;
	}

	// We need to mount sysfs to read the data from retained initrd
	ret = mount_special_fs();
	if (ret != 0) {
		fprintf(stderr, "Failed to mount special filesystems\n");
		ret = 1;
		goto out;
	}
	*config = get_config_from_file(config_file, config_buf);
	if (!*config) {
		fprintf(stderr, "Failed to read the configuration from %s\n", config_file);
		ret = 1;
	}
out:
	if (free_boot_var) {
		free(config_file);
	}

	return ret;
}

// manual_execvpe: Tries to implement in a simple way execvpe, since execvpe is
// only supported by glibc. The rational is to combine every path in env_path
// (which is the PATH) with the file_bin (the executable) and try to execve.
// If a combination does not succeed then we move to the next path in env_path
//
// Arguments:
// 1. env_path:	A string containing the PATH environment variable with all possible
//		directories to search for the executable.
// 2. file_bin:	The basename of the executable.
// 3. argv:	The arguments for the application.
// 4. env:	The environment variables for the application.
//
// Return value:
// On success it will never return. Otherwise, a non-zero return value
// will get returned and errno will be set appropriately.
int manual_execvpe(const char *env_path, const char *file_bin, char *const argv[], char *const env[]) {
	int status = 1;
	char *path_buf = NULL;
	const char *cur_path_end = NULL;
	const char *cur_path_start = NULL;
	char *tmp_bin_path = NULL;
	size_t env_path_len = 0;
	size_t file_bin_len = 0;

	if (!env) {
		DEBUG_PRINT("No environment variables were set, just execvp and use the current ones\n");
		// No environment variables were given. So, we can just
		// use execvp.
		execvp(file_bin, argv);
		goto manual_exec_exit;
	}

	if (*file_bin == '/' || env_path == NULL) {
		DEBUG_PRINT("Binary has full path, therefore just execvp it\n");
		// The file to execute is an absolute path.
		// Or there is no custom PATH to search for.
		// Therefore, just try to execve the given file
		execve(file_bin, argv, env);
		goto manual_exec_exit;
	}

	file_bin_len = strlen(file_bin);
	env_path_len = strlen(env_path);
	if (env_path_len <= 5) {
		fprintf(stderr, "Invalid format of custom PATH environment variable");
		goto manual_exec_exit;
	}
	// Move past "PATH+" and get to its values
	env_path += 5;
	env_path_len -= 5;

	// Allocate memory for the temporary buffer where we will construct
	// all combinations. The size should be:
	// env_path_len + '/' +file_bin_len + '\0'
	path_buf = malloc((env_path_len + file_bin_len + 2) * sizeof(char));
	if (!path_buf) {
		fprintf(stderr, "Failed to allocate memory to search binary\n");
		return 1;
	}

	// Store the basename of the executable in the end of the buffer
	// and prepend the '/' character to prepare a concatenation of a
	// path from custom PATH and the basename of the executable
	// This will reduce the copies, since we only change the directory
	// that we try out each time.
	tmp_bin_path = path_buf + env_path_len;
	*(tmp_bin_path) = '/';
	memcpy(tmp_bin_path + 1, file_bin, file_bin_len);
	*(tmp_bin_path + 1 + file_bin_len) = '\0';

	// cur_path_start stores the beginning of the current path we try from custom PATH
	cur_path_start = env_path;

	do {
		char *path_attempt = NULL;
		size_t tmp_path_size = 0;

		// cur_path_end stores the end of the current path we try from custom PATH
		cur_path_end = strchr(cur_path_start, ':');
		if (!cur_path_end) {
			// We reached the last path, but strchr return NULL,
			// since the character was not found. Therefore,
			// manually set the pointer to the end of the string.
			cur_path_end = env_path + env_path_len;
		}
		tmp_path_size = cur_path_end - cur_path_start;

		// We copy right before the '/' character the current directory
		// from custom PATH
		path_attempt = (char *)memcpy(tmp_bin_path - tmp_path_size,
						cur_path_start,
						tmp_path_size);

		DEBUG_PRINTF("Trying %s\n", path_attempt);
		execve(path_attempt, argv, env);

		// Execve failed, but check the reason
		switch (errno) {
		case EACCES:
			// Permission denied and therefore, we can not execute
			// the file we found. Try the next possible path.
			//
			// TODO: However, we might want to keep this error
			// and report it if everything else fails, because the
			// error will get overwritten from the last failure.
		case ENOENT:
		case ENOTDIR:
			// The file or a directory in the path does not exist.
			// Just move to the next possible path.
			break;
		default:
			// For any other reason, just abort.
			goto manual_exec_exit_free;
		}

		// Discard the ':' character
		cur_path_start = cur_path_end + 1;

	} while (cur_path_start < (env_path + env_path_len));

	// We could not execute the binary.
manual_exec_exit_free:
	free(path_buf);
manual_exec_exit:
	// execvp/execve will only return on an error so make sure that we check
	// the errno and exit with the correct return status for the error 
	// that we encountered.
	// See: http://www.tldp.org/LDP/abs/html/exitcodes.html#EXITCODESREF
	switch (errno) {
	case ENOENT:
		status = 127;
		break;
	case EACCES:
		status = 126;
		break;
	}
	// Just a trick to print the filename in the error.
	fprintf(stderr, "exec %s ", file_bin);
	perror("failed");

	return status;
}

// setup_exec_env: Sets up the process execution environment as defined by
// the process_conf argument.
//
// Arguments:
// 1. process_conf:	The config to apply with uid/gid and CWD.
//
// Return value:
// On success 0 is returned.
// Otherwise 1 is returned.
int setup_exec_env(struct process_config *process_conf) {
	int ret = 0;

	if (!process_conf) {
		DEBUG_PRINT("Empty config, nothing to be done\n");
		return 0;
	}

	DEBUG_PRINTF("Setting gid to %d\n", process_conf->gid);
	ret = setgid(process_conf->gid);
	if (ret < 0) {
		perror("set GID");
		return 1;
	}

	DEBUG_PRINTF("Setting uid to %d\n", process_conf->uid);
	ret = setuid(process_conf->uid);
	if (ret < 0) {
		// No need for reverting gid, since we will exit.
		perror("set UID");
		return 1;
	}

	DEBUG_PRINTF("Switching to directory %s\n", process_conf->wdir);
	ret = chdir(process_conf->wdir);
	if (ret < 0) {
		// No need for reverting gid/uid, since we will exit.
		perror("set CWD");
		return 1;
	}

	return 0;
}

int child_func(int argc, char *argv[]) {
	struct app_exec_config *app_config = NULL;
	char *app_config_buf = NULL;
	// The command line was already reassembled by spawn_app. When there is
	// none, the application command comes from the configuration instead.
	char **exec_argv = argv;
	int ret = 0;

	DEBUG_PRINT("Isolating child\n");
	// Put the child in a process group and
	// make it the foreground process if there is a tty.
	if (isolate_child()) {
		return 1;
	}

	// Load the configuration (if any) and apply everything that depends on
	// it here in the child.
	ret = load_app_config(&app_config, &app_config_buf);
	if (ret != 0) {
		fprintf(stderr, "Failed to load the configuration\n");
		return 1;
	}

	// Configure the network if the configuration provides it. On Linux the
	// kernel configures it through the ip= boot parameter and this is a no-op.
	if (app_config && app_config->net_conf) {
		DEBUG_PRINT("Configuring the network\n");
		if (configure_network(app_config->net_conf) != 0) {
			fprintf(stderr, "Failed to configure the network\n");
		}
	}

	// No command line: take the application command from the
	// configuration, where every argument is already separated and the
	// array is NULL terminated.
	if (argc < 2 && app_config && app_config->pr_conf &&
	    app_config->pr_conf->argv && app_config->pr_conf->argc > 0) {
		DEBUG_PRINT("Taking the application command from the configuration\n");
		exec_argv = app_config->pr_conf->argv;
	}

	if (exec_argv[0] == NULL) {
		fprintf(stderr, "No application execute\n");
		ret = 1;
		goto child_func_free;
	}
#ifdef DEBUG
	printf("Starting app %s with the following arguments\n", exec_argv[0]);
	for (int i = 1; exec_argv[i] != NULL; i++) {
		printf("%s\n", exec_argv[i]);
	}
	printf("Environment variables\n");
	for (char **env = environ; *env != NULL; env++) {
		printf("%s\n", *env);
	}
#endif
	if (app_config) {
		ret = mount_block_vols(app_config->blk_conf);
		if (ret != 0) {
			fprintf(stderr, "Failed to mount block volumes\n");
			goto child_func_free;
		}
		ret = setup_exec_env(app_config->pr_conf);
		if (ret != 0) {
			fprintf(stderr, "Failed to set up the process execution environment\n");
			goto child_func_free;
		}
		ret = manual_execvpe(app_config->path_env, exec_argv[0], exec_argv, app_config->envs);
	} else {
		DEBUG_PRINT("No configuration, simply execvp\n");
		ret = manual_execvpe(NULL, exec_argv[0], exec_argv, NULL);
	}
	// If we returned something went wrong
child_func_free:
	// free_app_config only touches the fields of app_config when it is
	// not NULL, that is when a configuration was loaded.
	free_app_config(app_config);
	free(app_config_buf);

	return ret;
}

// append_arg: Appends str to the quoted argument in buf, which holds
// QUOTED_ARG_MAX bytes.
//
// Return value:
// 0 on success, -1 when the argument does not fit (buf stays intact).
static int append_arg(char *buf, const char *str) {
	size_t used = strlen(buf);
	size_t len = strlen(str);

	if (used + len >= QUOTED_ARG_MAX) {
		fprintf(stderr, "Quoted argument is longer than %d bytes\n", QUOTED_ARG_MAX - 1);
		return -1;
	}
	memcpy(buf + used, str, len + 1);
	return 0;
}

int spawn_app(int argc, char *argv[], pid_t *child_pid) {
	int i = 0;
	pid_t pid;
	// The arguments of the app are the same as the ones for urunit, but
	// removing the urunit argv[0]. Therefore at most argc - 1 of them and
	// the NULL at the end.
	char **new_argv = NULL;
	int new_argc = 0;

	new_argv = calloc((size_t)(argc > 0 ? argc : 1), sizeof(char *));
	if (!new_argv) {
		fprintf(stderr, "Failed to allocate memory for the application arguments\n");
		return 1;
	}

	for (i = 1; i < argc; i++) {
		char *tmp_arg = argv[i];
		char quote = tmp_arg[0];

		if (quote == '\'' || quote == '"') {
			// The below is safe since the tmp_arg has at least one char
			uint32_t last_char = strlen(tmp_arg) - 1;
			if (tmp_arg[last_char] == quote) {
				new_argv[new_argc++] = argv[i];
				continue;
			}
			// This arg (and everything until we encounter the same
			// quote) is part of the same argument
			int j = 0;
			char buffer[QUOTED_ARG_MAX] = {0};

			if (append_arg(buffer, tmp_arg + 1) != 0) { // skip the quote
				free(new_argv);
				return 1;
			}
			for (j = i + 1; j < argc; j++) {
				char *next_arg = argv[j];
				size_t arg_len = strlen(next_arg);
				uint32_t last_char = 0;
				uint8_t should_break = 0;
				if (arg_len == 0) {
					continue;
				}
				last_char = arg_len - 1;
				if (last_char == 0) {
					if (next_arg[last_char] == quote) {
						should_break = 1;
						// Remove the quote
						next_arg[last_char] = '\0';
					}
				} else {
					if (next_arg[last_char] == quote && next_arg[last_char - 1] != quote) {
						should_break = 1;
						// Remove the quote
						next_arg[last_char] = '\0';
					}
				}
				if (append_arg(buffer, " ") != 0 ||
				    append_arg(buffer, next_arg) != 0) {
					free(new_argv);
					return 1;
				}
				if (should_break) {
					break;
				}
			}
			new_argv[new_argc++] = strdup(buffer);
			break;
		} else {
			new_argv[new_argc++] = argv[i];
		}
	}
	new_argv[new_argc] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		free(new_argv);
		return 1;
	} else if (pid == 0) {
		// The child only returns from child_func when it could not
		// execute the app. Exit here, so that it never returns into the
		// caller of supervise_main as if it were the supervisor.
		exit(child_func(argc, new_argv));
	}

	*child_pid = pid;
	free(new_argv);

	return 0;
}

// spawn_agent: Forks the exec agent as a child of urunit. The agent is part
// of the same binary, so the child calls agent_main(), where urunit used to
// execute a separate agent from /run/urunc/urunit-agent. The agent
// inherits urunit's environment and standard streams (the console) and runs in
// its own process group, so that terminal signals meant for the application
// do not reach it. It is reaped by the reaping loop like every other child;
// its exit does not end the guest, and it is asked to exit (SIGTERM) once the
// application has exited.
//
// Arguments:
// No arguments.
//
// Return value:
// The pid of the agent, or -1 when fork() failed.
pid_t spawn_agent(void) {
	pid_t pid;

	pid = fork();
	if (pid == 0) {
		if (setpgid(0, 0) < 0) {
			perror("setpgid agent");
		}
#ifdef __linux__
		agentlog_init();
		_exit(agent_main());
#else
		_exit(1);
#endif
	}
	return pid;
}

// stop_agent: Asks the agent urunit started, if it is still running, to exit
// with SIGTERM. Called once the application has exited, so that the agent can
// end its sessions before the shutdown. reap() does not wait for it, and the
// reboot at the end of the shutdown ends it.
//
// Arguments:
// No arguments.
//
// Return value:
// No return value.
void stop_agent(void) {
	if (agent_pid <= 0) {
		return;
	}
	DEBUG_PRINTF("Asking agent %d to exit\n", agent_pid);
	if (kill(agent_pid, SIGTERM) < 0 && errno != ESRCH) {
		perror("kill agent");
	}
}

int reap(const pid_t child_pid, int *child_exitcode_ptr) {
	pid_t reaped_pid = 0;
	int reaped_status = 0;

	while (1) {
		reaped_pid = waitpid(-1, &reaped_status, 0);
		switch (reaped_pid) {
		case -1:
			if (errno == ECHILD) {
				break;
			}
			// A forwarded signal interrupted the wait.
			if (errno == EINTR) {
				continue;
			}
			perror("reaping");
			return 1;

		case 0:
			break;
		default:
			DEBUG_PRINTF("Reaped process %d ", reaped_pid);
			// A child was reaped. Check whether it's the app.
			// If it is, then set the exit_code,
			if (reaped_pid == child_pid) {
				if (WIFEXITED(reaped_status)) {
					DEBUG_PRINTF("with exit status %d\n", WEXITSTATUS(reaped_status));
					// The app exited normally
					*child_exitcode_ptr = WEXITSTATUS(reaped_status);
				} else if (WIFSIGNALED(reaped_status)) {
					DEBUG_PRINTF("with exit status %d\n", WTERMSIG(reaped_status));
					/* The app was terminated. Emulate what sh / bash
					 * would do, which is to return
					 * 128 + signal number.
					 */
					*child_exitcode_ptr = 128 + WTERMSIG(reaped_status);
				} else {
					DEBUG_PRINT("with unknown exit status\n");
					return 1;
				}

				// Be safe, ensure the status code is indeed between 0 and 255.
				*child_exitcode_ptr = *child_exitcode_ptr % (STATUS_MAX - STATUS_MIN + 1);

				// Flush the app's writes before the agent goes. Its
				// sessions end with it, and urunc can take that for
				// the guest going down and stop the monitor before
				// the shutdown below syncs.
				urunit_sync();

				// The app is done. The agent urunit started would
				// otherwise run forever, so ask it to exit. Nothing
				// else is signalled: any other process still around
				// is the app's responsibility.
				stop_agent();

				/* The app is what we are here to supervise, so stop
				 * waiting now that it is gone. Continuing would block
				 * in waitpid() on whatever else is still running --
				 * a sidecar started before us and inherited when we
				 * became PID 1, for instance -- and that process has
				 * no reason to ever exit. supervise_main() would then
				 * never get to see the exit code, and the shutdown
				 * after it (sync, unmount, reboot) would never run:
				 * the guest stays up forever with nothing left to do.
				 *
				 * This includes the agent: it was only sent SIGTERM
				 * and it is not waited for. The reboot (or power off)
				 * at the end of the shutdown ends it, like anything
				 * else still alive, after the shutdown has synced and
				 * unmounted.
				 */
				return 0;
			} else if (reaped_pid == agent_pid) {
				// Forget the agent, so that its pid is not signalled
				// later, when it may have been reused.
				DEBUG_PRINT("The agent exited\n");
				agent_pid = 0;
			}
			continue;
		}
		/* If we make it here, that's because we did not continue in the switch case. */
		break;
	}

	return 0;
}

// should_spawn_agent: Decides whether this boot runs the exec agent. The
// early init of a generic container boot stages a copy of the binary at
// URUNIT_STAGED_BIN in the new root, where it used to stage the separate
// agent binary. Any other boot (e.g. urunit as
// the init of a unikernel rootfs) runs no agent.
//
// URUNIT_STAGED_BIN in the environment names another file to check, so that
// the tests can run the supervisor without writing to /run/urunc. The guest
// never sets it.
//
// Return value:
// 1 if the agent should be started, otherwise 0.
static int should_spawn_agent(void) {
#ifdef __linux__
	const char *path = getenv("URUNIT_STAGED_BIN");

	return access(path != NULL && path[0] != '\0' ? path :
		      URUNIT_STAGED_BIN, X_OK) == 0;
#else
	// The agent and the early init are Linux only.
	return 0;
#endif
}

// wants_default_route: Tells whether URUNIT_DEFROUTE is set, in the
// environment or as a boot variable (FreeBSD).
int wants_default_route(void) {
	char *val = NULL;
	int set = 0;

	if (getenv("URUNIT_DEFROUTE") != NULL) {
		return 1;
	}
	val = get_boot_var("URUNIT_DEFROUTE");
	set = val != NULL;
	free(val);

	return set;
}

// forward_signal: Sends a signal urunit received to the application. As PID
// 1, urunit gets only the signals it handles, so without this a SIGTERM to
// the guest init does nothing.
static void forward_signal(int sig) {
	pid_t pid = forward_pid;

	if (pid > 0) {
		kill(pid, sig);
	}
}

// forward_signals: Forwards the signals that ask a process to stop, or that
// an application handles itself, to the application app_pid. It runs after
// the agent and the application are forked, so neither inherits the handler.
// SA_RESTART keeps the waitpid() of reap() going.
void forward_signals(pid_t app_pid) {
	static const int sigs[] = { SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2 };
	struct sigaction sa;
	size_t i;

	forward_pid = app_pid;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = forward_signal;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
		if (sigaction(sigs[i], &sa, NULL) < 0) {
			perror("sigaction");
		}
	}
}

int parse_signal_number(const char *s) {
	char *end;
	long n;

	if (s == NULL || *s == '\0') {
		return -1;
	}
	errno = 0;
	n = strtol(s, &end, 10);
	if (errno != 0 || *end != '\0' || n < 1 || n > 64) {
		return -1;
	}
	return (int)n;
}

int signal_init_main(int argc, char **argv) {
	int sig = argc == 2 ? parse_signal_number(argv[1]) : -1;

	if (sig < 0) {
		fprintf(stderr, "usage: URUNIT_MODE=signal urunit <signal number>\n");
		return 2;
	}
	if (kill(1, sig) < 0) {
		perror("kill 1");
		return 1;
	}
	return 0;
}

// The application, and whether a power button asked for the stop. The SIGIO
// handler uses both.
static volatile pid_t button_target = 0;
static volatile sig_atomic_t power_off_requested = 0;

// on_power_button: The SIGIO handler. A press of the power button asks the
// application to stop with SIGTERM, as a container engine would, and the guest
// then powers off instead of restarting. Once the application has exited,
// what it left behind gets the SIGTERM instead.
static void on_power_button(int sig) {
	int saved_errno = errno;

	(void)sig;
	if (power_button_pressed()) {
		power_off_requested = 1;
		if (button_target <= 0 || kill(button_target, SIGTERM) < 0) {
			kill(-1, SIGTERM);
		}
	}
	errno = saved_errno;
}

// watch_power_buttons: Makes a press of the VM's power button stop the
// application. As PID 1, urunit gets only the signals it handles, so the
// handler goes in before the buttons can raise SIGIO. SA_RESTART keeps the
// waitpid() of reap() going.
//
// Arguments:
// No arguments.
//
// Return value:
// No return value.
static void watch_power_buttons(void) {
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_power_button;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGIO, &sa, NULL) < 0) {
		perror("sigaction SIGIO");
		return;
	}
	if (open_power_buttons() == 0) {
		DEBUG_PRINT("No power button to watch\n");
	}
}

int supervise_main(int argc, char *argv[]) {
	pid_t app_pid;
	int ret = 0;
	int app_exitcode = -1;

	// When started by the kernel as init we may have no console yet.
	setup_console();

	// The kernel may mount the root read-only (FreeBSD does); remount it
	// read-write for the application, regardless of the configuration.
	remount_root_rw();

	if (wants_default_route()) {
		DEBUG_PRINT("URUNIT_DEFROUTE was set\n");
		ret = set_default_route();
		if (ret != 0) {
			fprintf(stderr, "Failed to set default route\n");
		}
	}

	DEBUG_PRINT("Setting subreaper\n");
	ret = set_subreaper();
	if (ret < 0) {
		perror("Become subreaper");
		return 1;
	}

	watch_power_buttons();

	// The agent is started before the app, as urunit's own child, so that
	// urunit reaps it too. A failure to start it is reported and does not
	// prevent the application from starting.
	if (should_spawn_agent()) {
		DEBUG_PRINT("Spawn the agent\n");
		agent_pid = spawn_agent();
		if (agent_pid < 0) {
			perror("fork agent");
			agent_pid = 0;
		}
	}

	DEBUG_PRINT("Spawn the app\n");
	ret = spawn_app(argc, argv, &app_pid);
	if (ret) {
		fprintf(stderr, "Could not spawn app\n");
		return ret;
	}
	forward_signals(app_pid);
	button_target = app_pid;

	DEBUG_PRINT("Starting reaping loop\n");
	while (1) {
		ret = reap(app_pid, &app_exitcode);
		if (ret) {
			fprintf(stderr, "Error while reaping %d", ret);
			break;
		}

		if (app_exitcode != -1) {
			break;
		}
	}

	DEBUG_PRINT("Exiting, will reboot in order to shutdown\n");
	sync();
	unmount_external();
	// A power button exists only where the monitor can power the guest
	// off, and Cloud Hypervisor would restart it on a reboot.
	if (power_off_requested) {
		request_poweroff();
	}
	request_reboot();

	// The reboot did not happen.
	perror("reboot");
	return 1;
}
