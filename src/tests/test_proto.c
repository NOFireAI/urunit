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

// Tests for the agentproto frames and JSON in proto.c.
//
// The JSON literals marked "from Go" are what encoding/json emits for the
// agentproto types, printed with a throwaway program inside the module:
//
//	b, _ := json.Marshal(agentproto.OpenRequest{...})
//	fmt.Println(string(b))
//
// run with `go run` (go1.26, darwin/arm64).

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "proto.h"

#define S(lit) (lit), (sizeof(lit) - 1)

static int parse_open(const char *json, size_t len, struct open_request *req,
		      char *err, size_t errlen)
{
	return proto_parse_open(json, len, req, err, errlen);
}

static void test_hdr_encode(void)
{
	static const uint8_t want[PROTO_HDR_LEN] = {
		3, 0x01, 0x02, 0x03, 0x04, 0x0a, 0x0b, 0x0c, 0x0d,
	};
	uint8_t out[PROTO_HDR_LEN];

	proto_encode_hdr(out, PROTO_STDOUT, 0x01020304u, 0x0a0b0c0du);
	CHECK_MEM_EQ(out, want, PROTO_HDR_LEN);
}

static void test_hdr_round_trip(void)
{
	uint8_t out[PROTO_HDR_LEN];
	struct proto_hdr h;

	proto_encode_hdr(out, PROTO_EXIT, 0xfffffffeu, 12);
	CHECK_EQ_INT(proto_decode_hdr(out, &h), 0);
	CHECK_EQ_INT(h.type, PROTO_EXIT);
	CHECK_EQ_INT(h.stream, 0xfffffffeu);
	CHECK_EQ_INT(h.len, 12);

	// MaxPayload itself is fine, one byte more is not.
	proto_encode_hdr(out, PROTO_STDIN, 1, PROTO_MAX_PAYLOAD);
	CHECK_EQ_INT(proto_decode_hdr(out, &h), 0);
	CHECK_EQ_INT(h.len, 1u << 20);
	proto_encode_hdr(out, PROTO_STDIN, 1, PROTO_MAX_PAYLOAD + 1);
	CHECK_EQ_INT(proto_decode_hdr(out, &h), -1);
	CHECK_EQ_INT(h.len, PROTO_MAX_PAYLOAD + 1);
	proto_encode_hdr(out, PROTO_STDIN, 1, 0xffffffffu);
	CHECK_EQ_INT(proto_decode_hdr(out, &h), -1);
}

static void test_open_full(void)
{
	// From Go: OpenRequest{Argv: []string{"/bin/sh", "-c", "echo \"hi\""},
	// Env: []string{"A=1"}, Cwd: "/tmp", User: "501:1000", TTY: true,
	// Rows: 24, Cols: 80}
	static const char json[] =
		"{\"argv\":[\"/bin/sh\",\"-c\",\"echo \\\"hi\\\"\"],"
		"\"env\":[\"A=1\"],\"cwd\":\"/tmp\",\"user\":\"501:1000\","
		"\"tty\":true,\"rows\":24,\"cols\":80}";
	struct open_request req;
	char err[128];

	CHECK_EQ_INT(parse_open(S(json), &req, err, sizeof(err)), 0);
	CHECK_EQ_INT(req.argc, 3);
	if (req.argc == 3) {
		CHECK_EQ_STR(req.argv[0], "/bin/sh");
		CHECK_EQ_STR(req.argv[1], "-c");
		CHECK_EQ_STR(req.argv[2], "echo \"hi\"");
		CHECK(req.argv[3] == NULL);
	}
	CHECK_EQ_INT(req.envc, 1);
	if (req.envc == 1) {
		CHECK_EQ_STR(req.env[0], "A=1");
		CHECK(req.env[1] == NULL);
	}
	CHECK_EQ_STR(req.cwd, "/tmp");
	CHECK_EQ_STR(req.user, "501:1000");
	CHECK_EQ_INT(req.tty, 1);
	CHECK_EQ_INT(req.rows, 24);
	CHECK_EQ_INT(req.cols, 80);
	proto_free_open(&req);
	CHECK(req.argv == NULL && req.cwd == NULL);
	// A second free is harmless.
	proto_free_open(&req);
}

static void test_open_omitempty(void)
{
	// From Go: OpenRequest{Argv: []string{"ls"}}
	static const char json[] = "{\"argv\":[\"ls\"],\"tty\":false}";
	// From Go: OpenRequest{}
	static const char empty[] = "{\"argv\":null,\"tty\":false}";
	struct open_request req;
	char err[128];

	CHECK_EQ_INT(parse_open(S(json), &req, err, sizeof(err)), 0);
	CHECK_EQ_INT(req.argc, 1);
	CHECK_EQ_STR(req.argv[0], "ls");
	CHECK(req.argv[1] == NULL);
	CHECK_EQ_INT(req.envc, 0);
	CHECK(req.env != NULL && req.env[0] == NULL);
	CHECK(req.cwd == NULL);
	CHECK(req.user == NULL);
	CHECK_EQ_INT(req.tty, 0);
	CHECK_EQ_INT(req.rows, 0);
	CHECK_EQ_INT(req.cols, 0);
	proto_free_open(&req);

	CHECK_EQ_INT(parse_open(S(empty), &req, err, sizeof(err)), 0);
	CHECK_EQ_INT(req.argc, 0);
	CHECK(req.argv != NULL && req.argv[0] == NULL);
	proto_free_open(&req);

	// Go accepts a bare null and an empty object, and so do we.
	CHECK_EQ_INT(parse_open(S(" null "), &req, err, sizeof(err)), 0);
	CHECK_EQ_INT(req.argc, 0);
	proto_free_open(&req);
	CHECK_EQ_INT(parse_open(S("{}"), &req, err, sizeof(err)), 0);
	CHECK(req.argv != NULL && req.env != NULL);
	proto_free_open(&req);
}

static void test_open_skips_unknown(void)
{
	static const char json[] =
		" {\n\t\"x\": {\"a\": [1, {\"b\": null}, []],"
		"\"c\": \"\\\"}\"}, \"argv\": [\"a\"], \"y\": -1.5e+3,"
		"\"z\": [true, false, null],"
		"\"w\": {}, \"v\": 0, \"u\": \"\\u00e9\", \"tty\": true } \r\n";
	struct open_request req;
	char err[128];

	CHECK_EQ_INT(parse_open(S(json), &req, err, sizeof(err)), 0);
	CHECK_EQ_INT(req.argc, 1);
	if (req.argc == 1)
		CHECK_EQ_STR(req.argv[0], "a");
	CHECK_EQ_INT(req.tty, 1);
	proto_free_open(&req);
}

static void test_open_escapes(void)
{
	// From Go: OpenRequest{Argv: []string{"<a&b>", "é\u2028\x01\t\\"}}
	static const char go[] =
		"{\"argv\":[\"\\u003ca\\u0026b\\u003e\","
		"\"\xc3\xa9\\u2028\\u0001\\t\\\\\"],\"tty\":false}";
	static const char more[] =
		"{\"argv\":[\"\\ud83d\\ude00\",\"\\ud800x\",\"\\udc00\","
		"\"\\ud800\\u0041\",\"\\/\\b\\f\\n\\r\\\"\",\"\\u00E9\","
		"\"\xff\xfe\"]}";
	struct open_request req;
	char err[128];

	CHECK_EQ_INT(parse_open(S(go), &req, err, sizeof(err)), 0);
	CHECK_EQ_INT(req.argc, 2);
	if (req.argc == 2) {
		CHECK_EQ_STR(req.argv[0], "<a&b>");
		CHECK_EQ_STR(req.argv[1], "\xc3\xa9\xe2\x80\xa8\x01\t\\");
	}
	proto_free_open(&req);

	CHECK_EQ_INT(parse_open(S(more), &req, err, sizeof(err)), 0);
	CHECK_EQ_INT(req.argc, 7);
	if (req.argc == 7) {
		// A surrogate pair is one code point.
		CHECK_EQ_STR(req.argv[0], "\xf0\x9f\x98\x80");
		// Lone surrogates turn into U+FFFD, as in Go, and the
		// escape after one is read on its own.
		CHECK_EQ_STR(req.argv[1], "\xef\xbf\xbd" "x");
		CHECK_EQ_STR(req.argv[2], "\xef\xbf\xbd");
		CHECK_EQ_STR(req.argv[3], "\xef\xbf\xbd" "A");
		CHECK_EQ_STR(req.argv[4], "/\b\f\n\r\"");
		CHECK_EQ_STR(req.argv[5], "\xc3\xa9");
		// Raw high bytes pass through untouched.
		CHECK_EQ_STR(req.argv[6], "\xff\xfe");
	}
	proto_free_open(&req);
}

static void test_open_keys(void)
{
	struct open_request req;
	char err[128];

	// encoding/json matches keys case-insensitively and the last
	// duplicate wins, null leaves a field as it was.
	CHECK_EQ_INT(parse_open(S("{\"ARGV\":[\"x\"],\"Tty\":true,"
				  "\"argv\":[\"a\"],\"argv\":[\"b\",\"c\"],"
				  "\"cwd\":\"/a\",\"cwd\":null,"
				  "\"\\u0075ser\":\"root\"}"),
				&req, err, sizeof(err)), 0);
	CHECK_EQ_INT(req.argc, 2);
	if (req.argc == 2) {
		CHECK_EQ_STR(req.argv[0], "b");
		CHECK_EQ_STR(req.argv[1], "c");
	}
	CHECK_EQ_INT(req.tty, 1);
	CHECK_EQ_STR(req.cwd, "/a");
	CHECK_EQ_STR(req.user, "root");
	proto_free_open(&req);

	// A null element in argv is an empty string, as in a Go []string.
	CHECK_EQ_INT(parse_open(S("{\"argv\":[\"a\",null]}"), &req, err,
				sizeof(err)), 0);
	CHECK_EQ_INT(req.argc, 2);
	if (req.argc == 2)
		CHECK_EQ_STR(req.argv[1], "");
	proto_free_open(&req);

	// Many arguments grow the array past its first allocation.
	CHECK_EQ_INT(parse_open(S("{\"argv\":[\"0\",\"1\",\"2\",\"3\",\"4\","
				  "\"5\",\"6\",\"7\",\"8\",\"9\",\"10\"]}"),
				&req, err, sizeof(err)), 0);
	CHECK_EQ_INT(req.argc, 11);
	if (req.argc == 11) {
		CHECK_EQ_STR(req.argv[10], "10");
		CHECK(req.argv[11] == NULL);
	}
	proto_free_open(&req);
}

static void test_open_numbers(void)
{
	struct open_request req;
	char err[128];

	// rows and cols clamp to a uint16 instead of failing.
	CHECK_EQ_INT(parse_open(S("{\"rows\":70000,\"cols\":65535}"), &req,
				err, sizeof(err)), 0);
	CHECK_EQ_INT(req.rows, 65535);
	CHECK_EQ_INT(req.cols, 65535);
	proto_free_open(&req);
	CHECK_EQ_INT(parse_open(S("{\"rows\":99999999999999999999999,"
				  "\"cols\":0}"),
				&req, err, sizeof(err)), 0);
	CHECK_EQ_INT(req.rows, 65535);
	CHECK_EQ_INT(req.cols, 0);
	proto_free_open(&req);
}

static void test_open_malformed(void)
{
	static const struct {
		const char *json;
		const char *why;
	} cases[] = {
		{ "", "empty" },
		{ "   ", "only whitespace" },
		{ "{", "unterminated object" },
		{ "{\"argv\":[\"a\"", "unterminated array" },
		{ "{\"argv\":[\"a", "unterminated string" },
		{ "{\"argv\":[\"a\",]}", "trailing comma in array" },
		{ "{\"argv\":[\"ls\"],}", "trailing comma in object" },
		{ "{\"argv\":[\"ls\"]} x", "data after the object" },
		{ "{\"argv\":[\"ls\"]}{}", "two objects" },
		{ "[1]", "not an object" },
		{ "{'a':1}", "single quotes" },
		{ "{\"a\" 1}", "missing colon" },
		{ "{\"a\":1 \"b\":2}", "missing comma" },
		{ "{\"a\":01}", "leading zero" },
		{ "{\"a\":-}", "lone minus" },
		{ "{\"a\":1.}", "no fraction digits" },
		{ "{\"a\":1e}", "no exponent digits" },
		{ "{\"a\":tru}", "short literal" },
		{ "{\"a\":nul}", "short null" },
		{ "{\"argv\":nul}", "short null in a field" },
		{ "{\"tty\":1}", "tty not a boolean" },
		{ "{\"tty\":\"true\"}", "tty a string" },
		{ "{\"argv\":\"ls\"}", "argv not an array" },
		{ "{\"argv\":[1]}", "argv of numbers" },
		{ "{\"cwd\":[]}", "cwd not a string" },
		{ "{\"argv\":[\"a\\x\"]}", "bad escape" },
		{ "{\"argv\":[\"a\\u12\"]}", "short \\u escape" },
		{ "{\"argv\":[\"a\\u12g4\"]}", "bad \\u digit" },
		{ "{\"argv\":[\"a\nb\"]}", "raw newline in a string" },
		{ "{\"argv\":[\"a\\u0000\"]}", "NUL in a kept string" },
		{ "{\"rows\":-1}", "negative rows" },
		{ "{\"rows\":1.5}", "fractional rows" },
		{ "{\"cols\":1e2}", "exponent in cols" },
		{ "{\"rows\":\"24\"}", "rows a string" },
		{ "{\"argv\":[\"a\",\"b\"],\"env\":[\"c\"],\"cwd\":\"/x\","
		  "\"user\":\"u\",\"tty\":nope}", "fails after allocating" },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct open_request req;
		char err[128];

		memset(&req, 0xa5, sizeof(req));
		if (parse_open(cases[i].json, strlen(cases[i].json), &req, err,
			       sizeof(err)) != -1) {
			HARNESS_FAIL("%s: %s parsed", cases[i].why,
				     cases[i].json);
			proto_free_open(&req);
			continue;
		}
		if (err[0] == '\0')
			HARNESS_FAIL("%s: no error message", cases[i].why);
		// A failed parse leaves nothing behind, and freeing it
		// again is safe.
		CHECK(req.argv == NULL && req.env == NULL);
		CHECK(req.cwd == NULL && req.user == NULL);
		CHECK_EQ_INT(req.argc, 0);
		proto_free_open(&req);
	}
}

static void test_open_limits(void)
{
	struct open_request req;
	char err[128], small[8];
	char *deep;
	size_t i, n = 2000;

	// Nesting beyond the bound fails cleanly instead of recursing on.
	deep = malloc(n * 2 + 16);
	CHECK(deep != NULL);
	if (deep != NULL) {
		size_t w = 0;

		memcpy(deep, "{\"x\":", 5);
		w = 5;
		for (i = 0; i < n; i++)
			deep[w++] = '[';
		for (i = 0; i < n; i++)
			deep[w++] = ']';
		deep[w++] = '}';
		CHECK_EQ_INT(parse_open(deep, w, &req, err, sizeof(err)), -1);
		CHECK(strstr(err, "nested") != NULL);
		proto_free_open(&req);
		free(deep);
	}

	// The input need not be NUL terminated: only len bytes are read.
	CHECK_EQ_INT(parse_open("{\"argv\":[\"ls\"]}garbage", 15, &req, err,
				sizeof(err)), 0);
	CHECK_EQ_INT(req.argc, 1);
	proto_free_open(&req);

	// A short error buffer truncates the message but still fails.
	CHECK_EQ_INT(parse_open(S("{\"tty\":1}"), &req, small, sizeof(small)),
		     -1);
	CHECK(strlen(small) == sizeof(small) - 1);
	CHECK_EQ_INT(parse_open(S("{"), &req, NULL, 0), -1);
	CHECK_EQ_INT(parse_open(NULL, 0, &req, err, sizeof(err)), -1);
}

static void test_resize(void)
{
	uint16_t rows = 1, cols = 1;

	// From Go: Resize{Rows: 50, Cols: 132}
	CHECK_EQ_INT(proto_parse_resize(S("{\"rows\":50,\"cols\":132}"), &rows,
					&cols), 0);
	CHECK_EQ_INT(rows, 50);
	CHECK_EQ_INT(cols, 132);
	CHECK_EQ_INT(proto_parse_resize(S("{\"cols\":7,\"extra\":[1]}"), &rows,
					&cols), 0);
	CHECK_EQ_INT(rows, 0);
	CHECK_EQ_INT(cols, 7);
	rows = cols = 9;
	CHECK_EQ_INT(proto_parse_resize(S("{\"rows\":-1}"), &rows, &cols), -1);
	CHECK_EQ_INT(proto_parse_resize(S("{\"rows\":1"), &rows, &cols), -1);
	CHECK_EQ_INT(rows, 9);
	CHECK_EQ_INT(cols, 9);
}

static void test_signal(void)
{
	int sig = 1;

	// From Go: Signal{Signal: 15}
	CHECK_EQ_INT(proto_parse_signal(S("{\"signal\":15}"), &sig), 0);
	CHECK_EQ_INT(sig, 15);
	CHECK_EQ_INT(proto_parse_signal(S("{\"signal\":-9}"), &sig), 0);
	CHECK_EQ_INT(sig, -9);
	CHECK_EQ_INT(proto_parse_signal(S("{\"signal\":-2147483648}"), &sig),
		     0);
	CHECK_EQ_INT(sig, -2147483647 - 1);
	CHECK_EQ_INT(proto_parse_signal(S("{}"), &sig), 0);
	CHECK_EQ_INT(sig, 0);
	sig = 3;
	CHECK_EQ_INT(proto_parse_signal(S("{\"signal\":1.5}"), &sig), -1);
	CHECK_EQ_INT(proto_parse_signal(S("{\"signal\":2147483648}"), &sig),
		     -1);
	CHECK_EQ_INT(proto_parse_signal(S("{\"signal\":99999999999999999999}"),
					&sig), -1);
	CHECK_EQ_INT(proto_parse_signal(S("{\"signal\":\"TERM\"}"), &sig), -1);
	CHECK_EQ_INT(sig, 3);
}

static void test_format_exit(void)
{
	char buf[32];

	CHECK_EQ_INT(proto_format_exit(buf, sizeof(buf), 143), 12);
	CHECK_EQ_STR(buf, "{\"code\":143}");
	CHECK_EQ_INT(proto_format_exit(buf, sizeof(buf), -1), 11);
	CHECK_EQ_STR(buf, "{\"code\":-1}");
	CHECK_EQ_INT(proto_format_exit(buf, sizeof(buf), 0), 10);
	CHECK_EQ_STR(buf, "{\"code\":0}");
	// The NUL needs room too.
	CHECK_EQ_INT(proto_format_exit(buf, 12, 143), -1);
	CHECK_EQ_INT(proto_format_exit(buf, 13, 143), 12);
}

static void test_format_error(void)
{
	static const char want[] =
		"{\"message\":\"exec \\\"x\\\": no such file\\n\\ttab"
		"\\u0001\\u001f\\r\\\\ \xc3\xa9\"}";
	char buf[128];
	int n, sig = 7;

	n = proto_format_error(buf, sizeof(buf),
			       "exec \"x\": no such file\n\ttab\x01\x1f\r\\ "
			       "\xc3\xa9");
	CHECK_EQ_INT(n, (int)(sizeof(want) - 1));
	CHECK_EQ_STR(buf, want);
	// What we write is valid JSON: the reader skips it as an unknown
	// field without complaint.
	CHECK_EQ_INT(proto_parse_signal(buf, (size_t)n, &sig), 0);
	CHECK_EQ_INT(sig, 0);

	CHECK_EQ_INT(proto_format_error(buf, sizeof(buf), ""), 14);
	CHECK_EQ_STR(buf, "{\"message\":\"\"}");
	CHECK_EQ_INT(proto_format_error(buf, sizeof(buf), NULL), 14);
	// Every size short of the whole message and its NUL fails.
	CHECK_EQ_INT(proto_format_error(buf, 14, ""), -1);
	CHECK_EQ_INT(proto_format_error(buf, 15, ""), 14);
	CHECK_EQ_INT(proto_format_error(buf, 16, "\""), -1);
	CHECK_EQ_INT(proto_format_error(buf, 17, "\""), 16);
	CHECK_EQ_INT(proto_format_error(buf, 19, "\x01"), -1);
	CHECK_EQ_INT(proto_format_error(buf, 21, "\x01"), 20);
	CHECK_EQ_INT(proto_format_error(buf, 5, "x"), -1);
}

int run_proto_tests(void)
{
	int start = harness_failures;

	test_hdr_encode();
	test_hdr_round_trip();
	test_open_full();
	test_open_omitempty();
	test_open_skips_unknown();
	test_open_escapes();
	test_open_keys();
	test_open_numbers();
	test_open_malformed();
	test_open_limits();
	test_resize();
	test_signal();
	test_format_exit();
	test_format_error();

	return harness_failures - start;
}
