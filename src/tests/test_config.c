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

// Tests of the urunit.conf parser. The configurations are the ones urunc's
// BuildUrunitConfig (pkg/unikontainers/unikernels) writes, so a change on
// either side shows up here.

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "harness.h"

// write_config: Writes len bytes of buf to a new temporary file and returns
// its path in path, which holds cap bytes. Returns 0 on success.
static int write_config(const void *buf, size_t len, char *path, size_t cap)
{
	const char *dir = getenv("TMPDIR");
	ssize_t n;
	int fd;

	if (!dir || !*dir)
		dir = "/tmp";
	if (snprintf(path, cap, "%s/urunit-conf-XXXXXX", dir) >= (int)cap)
		return -1;
	fd = mkstemp(path);
	if (fd < 0)
		return -1;
	n = write(fd, buf, len);
	close(fd);
	if (n < 0 || (size_t)n != len) {
		unlink(path);
		return -1;
	}
	return 0;
}

// parse_config: Parses the len bytes of buf as a urunit.conf. On success the
// caller frees the result with free_app_config and *sbuf with free.
static struct app_exec_config *parse_config(const void *buf, size_t len,
					    char **sbuf)
{
	struct app_exec_config *conf;
	char path[512];

	*sbuf = NULL;
	if (write_config(buf, len, path, sizeof(path)) != 0) {
		HARNESS_FAIL("could not write a temporary configuration");
		return NULL;
	}
	conf = get_config_from_file(path, sbuf);
	unlink(path);
	return conf;
}

// The newline separated configuration BuildUrunitConfig writes for a
// container with two environment variables, a user, a working directory and
// one volume besides the rootfs (which BuildUrunitConfig skips).
static void test_build_urunit_config(void)
{
	static const char conf_text[] =
		"UES\n"
		"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n"
		"HOSTNAME=brig\n"
		"UEE\n"
		"UCS\n"
		"UID:1000\n"
		"GID:1000\n"
		"WD:/work\n"
		"UCE\n"
		"UBS\n"
		"ID:vol0\n"
		"MP:/data\n"
		"UBE\n";
	struct app_exec_config *conf;
	char *buf = NULL;

	conf = parse_config(conf_text, sizeof(conf_text) - 1, &buf);
	CHECK(conf != NULL);
	if (!conf)
		return;

	CHECK(conf->envs != NULL);
	if (conf->envs) {
		CHECK_EQ_STR(conf->envs[0],
			     "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
		CHECK_EQ_STR(conf->envs[1], "HOSTNAME=brig");
		CHECK(conf->envs[2] == NULL);
	}
	CHECK_EQ_STR(conf->path_env,
		     "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");

	CHECK(conf->pr_conf != NULL);
	if (conf->pr_conf) {
		CHECK_EQ_INT(conf->pr_conf->uid, 1000);
		CHECK_EQ_INT(conf->pr_conf->gid, 1000);
		CHECK_EQ_STR(conf->pr_conf->wdir, "/work");
		CHECK(conf->pr_conf->argv == NULL);
		CHECK_EQ_INT(conf->pr_conf->argc, 0);
	}

	CHECK(conf->blk_conf != NULL);
	if (conf->blk_conf) {
		CHECK(conf->blk_conf[0] != NULL);
		if (conf->blk_conf[0]) {
			CHECK_EQ_STR(conf->blk_conf[0]->id, "vol0");
			CHECK_EQ_STR(conf->blk_conf[0]->mountpoint, "/data");
			CHECK(!is_order_block_id(conf->blk_conf[0]->id));
		}
		CHECK(conf->blk_conf[1] == NULL);
	}
	CHECK(conf->net_conf == NULL);

	free_app_config(conf);
	free(buf);
}

// The records of a configuration in the NUL-terminated format, as urunc writes
// it. Its values are taken verbatim, so they can hold new lines and text that
// looks like a marker, and the command comes as ARC/ARV records. Everything
// after PAD is ignored.
static const char nul_records[] =
	"UES\0"
	"PATH=/usr/bin:/bin\0"
	"MULTI=line one\nUEE\nline three\0"
	"UEE\0"
	"UCS\0"
	"UID:1000\0"
	"GID:1000\0"
	"WD:/work\0"
	"ARC:3\0"
	"ARV:/bin/sh\0"
	"ARV:-c\0"
	"ARV:echo hello world\0"
	"UCE\0"
	"UBS\0"
	"ID:vol0\0"
	"MP:/data\0"
	"UBE\0"
	"PAD\0";

// check_nul_records parses the len bytes of text and checks that it holds
// nul_records.
static void check_nul_records(const char *text, size_t len)
{
	struct app_exec_config *conf;
	char *buf = NULL;

	conf = parse_config(text, len, &buf);
	CHECK(conf != NULL);
	if (!conf)
		return;

	CHECK(conf->envs != NULL);
	if (conf->envs) {
		CHECK_EQ_STR(conf->envs[0], "PATH=/usr/bin:/bin");
		CHECK_EQ_STR(conf->envs[1], "MULTI=line one\nUEE\nline three");
		CHECK(conf->envs[2] == NULL);
	}
	CHECK_EQ_STR(conf->path_env, "PATH=/usr/bin:/bin");

	CHECK(conf->pr_conf != NULL);
	if (conf->pr_conf) {
		CHECK_EQ_INT(conf->pr_conf->uid, 1000);
		CHECK_EQ_INT(conf->pr_conf->gid, 1000);
		CHECK_EQ_STR(conf->pr_conf->wdir, "/work");
		CHECK_EQ_INT(conf->pr_conf->argc, 3);
		CHECK(conf->pr_conf->argv != NULL);
		if (conf->pr_conf->argv) {
			CHECK_EQ_STR(conf->pr_conf->argv[0], "/bin/sh");
			CHECK_EQ_STR(conf->pr_conf->argv[1], "-c");
			CHECK_EQ_STR(conf->pr_conf->argv[2], "echo hello world");
			CHECK(conf->pr_conf->argv[3] == NULL);
		}
	}

	CHECK(conf->blk_conf != NULL);
	if (conf->blk_conf) {
		CHECK(conf->blk_conf[0] != NULL);
		if (conf->blk_conf[0]) {
			CHECK_EQ_STR(conf->blk_conf[0]->id, "vol0");
			CHECK_EQ_STR(conf->blk_conf[0]->mountpoint, "/data");
		}
		CHECK(conf->blk_conf[1] == NULL);
	}

	free_app_config(conf);
	free(buf);
}

// urunc writes the NUL-terminated records with nothing before them
// (urunc-dev/urunc#1111). An earlier writer put a URUNIT1 record first, and
// a raw block device pads the configuration, with new lines (FreeBSD) or
// zeros, after PAD. All of them read the same.
static void test_nul_records(void)
{
	char text[sizeof(nul_records) + 64];
	size_t len = sizeof(nul_records) - 1;

	check_nul_records(nul_records, len);

	memcpy(text, "URUNIT1", sizeof("URUNIT1"));
	memcpy(text + sizeof("URUNIT1"), nul_records, len);
	check_nul_records(text, sizeof("URUNIT1") + len);

	memcpy(text, nul_records, len);
	memset(text + len, '\n', 32);
	check_nul_records(text, len + 32);

	memcpy(text, nul_records, len);
	memset(text + len, 0, 16);
	memcpy(text + len + 16, "UES\0garbage\0", 12);
	check_nul_records(text, len + 28);
}

// A legacy configuration on a raw block device is followed by zeros. Its first
// record ends with a new line, so it is still read as lines.
static void test_legacy_with_zero_padding(void)
{
	static const char conf_text[] =
		"UES\nA=1\nB=2\nUEE\nUCS\nUID:7\nGID:8\nWD:/w\nUCE\n"
		"\0\0\0\0\0\0\0\0";
	struct app_exec_config *conf;
	char *buf = NULL;

	CHECK(!config_has_nul_records(conf_text, sizeof(conf_text) - 1));
	CHECK(config_has_nul_records(nul_records, sizeof(nul_records) - 1));
	conf = parse_config(conf_text, sizeof(conf_text) - 1, &buf);
	CHECK(conf != NULL);
	if (!conf)
		return;
	CHECK(conf->envs != NULL);
	if (conf->envs) {
		CHECK_EQ_STR(conf->envs[0], "A=1");
		CHECK_EQ_STR(conf->envs[1], "B=2");
		CHECK(conf->envs[2] == NULL);
	}
	CHECK(conf->pr_conf != NULL);
	if (conf->pr_conf) {
		CHECK_EQ_INT(conf->pr_conf->uid, 7);
		CHECK_EQ_INT(conf->pr_conf->gid, 8);
		CHECK_EQ_STR(conf->pr_conf->wdir, "/w");
	}
	free_app_config(conf);
	free(buf);
}

// A container with no environment and no volumes: empty UES/UEE and UBS/UBE
// sections. A section without its end marker is rejected.
static void test_empty_sections(void)
{
	static const char conf_text[] =
		"UES\nUEE\nUCS\nUID:0\nGID:0\nWD:/\nUCE\nUBS\nUBE\n";
	static const char truncated[] = "UES\nPATH=/bin\nUCS\nUID:0\n";
	struct app_exec_config *conf;
	char *buf = NULL;

	conf = parse_config(conf_text, sizeof(conf_text) - 1, &buf);
	CHECK(conf != NULL);
	if (conf) {
		CHECK(conf->envs == NULL);
		CHECK(conf->path_env == NULL);
		CHECK(conf->blk_conf == NULL);
		CHECK(conf->net_conf == NULL);
		CHECK(conf->pr_conf != NULL);
		if (conf->pr_conf) {
			CHECK_EQ_INT(conf->pr_conf->uid, 0);
			CHECK_EQ_INT(conf->pr_conf->gid, 0);
			CHECK_EQ_STR(conf->pr_conf->wdir, "/");
		}
		free_app_config(conf);
		free(buf);
	}

	buf = NULL;
	conf = parse_config(truncated, sizeof(truncated) - 1, &buf);
	CHECK(conf == NULL);
	CHECK(buf == NULL);
	free_app_config(conf);
}

// BuildUrunitConfig prefixes the block IDs with "FC" for Firecracker, which
// has no serial IDs. Those volumes are found by their order instead, the nth
// volume (the rootfs being vda) at /dev/vd<'a' + n>.
static void test_fc_block_ids(void)
{
	static const char conf_text[] =
		"UES\nPATH=/bin\nUEE\nUCS\nUID:0\nGID:0\nWD:/\nUCE\n"
		"UBS\nID:FCvol0\nMP:/data\nID:FCvol1\nMP:/logs\nUBE\n";
	struct app_exec_config *conf;
	char name[VBLOCK_NAME_SZ];
	char path[VBLOCK_NAME_SZ] = "unset";
	char *buf = NULL;

	vblock_device_name(0, name);
	CHECK_EQ_STR(name, "/dev/vda");
	vblock_device_name(1, name);
	CHECK_EQ_STR(name, "/dev/vdb");
	vblock_device_name(25, name);
	CHECK_EQ_STR(name, "/dev/vdz");
	// No more than 26 devices: the names wrap around.
	vblock_device_name(26, name);
	CHECK_EQ_STR(name, "/dev/vda");

	CHECK(is_order_block_id("FCvol0"));
	CHECK(is_order_block_id("FC1"));
	CHECK(!is_order_block_id("FC"));
	CHECK(!is_order_block_id("fc-vol0"));
	CHECK(!is_order_block_id("vol0"));
	CHECK(!is_order_block_id(""));

	// A device that does not exist is not found and path stays intact.
	vblock_device_name(25, name);
	if (access(name, F_OK) != 0) {
		CHECK_EQ_INT(find_vblock_device_by_order(25, path), -1);
		CHECK_EQ_STR(path, "unset");
	}

	conf = parse_config(conf_text, sizeof(conf_text) - 1, &buf);
	CHECK(conf != NULL);
	if (!conf)
		return;
	CHECK(conf->blk_conf != NULL);
	if (conf->blk_conf && conf->blk_conf[0] && conf->blk_conf[1]) {
		CHECK_EQ_STR(conf->blk_conf[0]->id, "FCvol0");
		CHECK_EQ_STR(conf->blk_conf[0]->mountpoint, "/data");
		CHECK(is_order_block_id(conf->blk_conf[0]->id));
		CHECK_EQ_STR(conf->blk_conf[1]->id, "FCvol1");
		CHECK_EQ_STR(conf->blk_conf[1]->mountpoint, "/logs");
		CHECK(conf->blk_conf[2] == NULL);
	} else {
		HARNESS_FAIL("expected two block entries");
	}
	free_app_config(conf);
	free(buf);
}

int run_config_tests(void)
{
	int start = harness_failures;

	test_build_urunit_config();
	test_nul_records();
	test_legacy_with_zero_padding();
	test_empty_sections();
	test_fc_block_ids();

	return harness_failures - start;
}
