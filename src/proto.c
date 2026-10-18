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

// The frames of pkg/agentproto and the JSON its control payloads need.
// The reader handles one flat object and follows encoding/json:
//
//   - keys match their field case-insensitively,
//   - the last duplicate key wins,
//   - null leaves a field unchanged,
//   - unknown keys are skipped, whatever their value,
//   - only whitespace may follow the object,
//   - integer fields take integers only.
//
// Two things differ from Go. Rows and cols are clamped to 65535 instead of
// failing. A string we keep may not hold a NUL byte, since it ends up as a
// C string.

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto.h"

// encoding/json stops at 10000 levels. A Go client sends no nested values,
// so a much lower bound keeps the recursion of the skipper small.
#define JR_MAX_DEPTH 512

void proto_encode_hdr(uint8_t out[PROTO_HDR_LEN], uint8_t type,
		      uint32_t stream, uint32_t len)
{
	out[0] = type;
	out[1] = (uint8_t)(stream >> 24);
	out[2] = (uint8_t)(stream >> 16);
	out[3] = (uint8_t)(stream >> 8);
	out[4] = (uint8_t)stream;
	out[5] = (uint8_t)(len >> 24);
	out[6] = (uint8_t)(len >> 16);
	out[7] = (uint8_t)(len >> 8);
	out[8] = (uint8_t)len;
}

static uint32_t proto_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int proto_decode_hdr(const uint8_t in[PROTO_HDR_LEN], struct proto_hdr *h)
{
	// The fields are filled in even for a frame we reject, so the
	// caller can log what it got.
	h->type = in[0];
	h->stream = proto_be32(in + 1);
	h->len = proto_be32(in + 5);
	if (h->len > PROTO_MAX_PAYLOAD)
		return -1;
	return 0;
}

// The JSON reader.

struct jr {
	const char *start;
	const char *p;
	const char *end;
	char *err;
	size_t errlen;
	int depth;
};

struct jr_num {
	int neg;
	int integral;	// no fraction and no exponent
	int overflow;	// the magnitude does not fit in 64 bits
	uint64_t mag;
};

static void jr_init(struct jr *r, const char *json, size_t len, char *err,
		    size_t errlen)
{
	if (json == NULL) {
		json = "";
		len = 0;
	}
	r->start = json;
	r->p = json;
	r->end = json + len;
	r->err = err;
	r->errlen = errlen;
	r->depth = 0;
}

typedef int (*jr_field_fn)(struct jr *r, const char *key, void *ctx);

static int jr_fail(struct jr *r, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (r->err == NULL || r->errlen == 0)
		return -1;
	// Only the first error counts. Callers up the stack just return.
	if (r->err[0] != '\0')
		return -1;
	va_start(ap, fmt);
	n = vsnprintf(r->err, r->errlen, fmt, ap);
	va_end(ap);
	if (n >= 0 && (size_t)n < r->errlen)
		snprintf(r->err + n, r->errlen - (size_t)n, " at offset %zu",
			 (size_t)(r->p - r->start));
	return -1;
}

static void jr_ws(struct jr *r)
{
	while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' ||
				 *r->p == '\n' || *r->p == '\r'))
		r->p++;
}

static int jr_peek(struct jr *r)
{
	jr_ws(r);
	if (r->p >= r->end)
		return -1;
	return (unsigned char)*r->p;
}

static int jr_expect(struct jr *r, char c)
{
	int got = jr_peek(r);

	if (got == -1)
		return jr_fail(r, "unexpected end of JSON, want '%c'", c);
	if (got != (unsigned char)c)
		return jr_fail(r, "invalid character 0x%02x, want '%c'", got,
			       c);
	r->p++;
	return 0;
}

// Consumes the literal word (true, false or null) at the cursor.
static int jr_word(struct jr *r, const char *word)
{
	size_t n = strlen(word);

	if ((size_t)(r->end - r->p) < n || memcmp(r->p, word, n) != 0)
		return jr_fail(r, "invalid literal, want %s", word);
	r->p += n;
	return 0;
}

// Returns 1 and consumes null when the next value is null, 0 otherwise.
static int jr_null(struct jr *r)
{
	if (jr_peek(r) != 'n')
		return 0;
	return jr_word(r, "null") == 0 ? 1 : -1;
}

static int jr_hex4(const char *p, const char *end, uint32_t *out)
{
	uint32_t v = 0;
	int i;

	if (end - p < 4)
		return -1;
	for (i = 0; i < 4; i++) {
		char c = p[i];

		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (uint32_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (uint32_t)(c - 'A' + 10);
		else
			return -1;
	}
	*out = v;
	return 0;
}

static size_t jr_utf8(uint32_t cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

// Reads a string. With out set, the decoded text is returned as a fresh
// NUL terminated buffer, otherwise the string is only checked and
// skipped. A decoded string is never longer than its quoted form, so the
// buffer is sized from the raw span.
static int jr_string(struct jr *r, char **out)
{
	const char *s, *q;
	char *buf = NULL;
	size_t w = 0;

	if (jr_expect(r, '"') != 0)
		return -1;
	s = r->p;
	for (q = s; q < r->end && *q != '"'; q++) {
		if ((unsigned char)*q < 0x20) {
			r->p = q;
			return jr_fail(r, "control character in string");
		}
		if (*q == '\\' && q + 1 < r->end)
			q++;
	}
	if (q >= r->end) {
		r->p = q;
		return jr_fail(r, "unexpected end of JSON in string");
	}
	if (out != NULL) {
		buf = malloc((size_t)(q - s) + 1);
		if (buf == NULL)
			return jr_fail(r, "out of memory");
	}
	while (r->p < q) {
		char c = *r->p;
		char tmp[4];
		size_t n = 1;
		uint32_t cp, lo;

		if (c != '\\') {
			tmp[0] = c;
			r->p++;
			goto emit;
		}
		switch (r->p[1]) {
		case '"': tmp[0] = '"'; break;
		case '\\': tmp[0] = '\\'; break;
		case '/': tmp[0] = '/'; break;
		case 'b': tmp[0] = '\b'; break;
		case 'f': tmp[0] = '\f'; break;
		case 'n': tmp[0] = '\n'; break;
		case 'r': tmp[0] = '\r'; break;
		case 't': tmp[0] = '\t'; break;
		case 'u':
			if (jr_hex4(r->p + 2, q, &cp) != 0)
				goto bad_escape;
			r->p += 6;
			if (cp >= 0xd800 && cp < 0xe000) {
				// A high surrogate followed by a low one is a
				// pair. Anything else becomes U+FFFD and the
				// next escape is read on its own, as Go does.
				if (cp < 0xdc00 && q - r->p >= 6 &&
				    r->p[0] == '\\' && r->p[1] == 'u' &&
				    jr_hex4(r->p + 2, q, &lo) == 0 &&
				    lo >= 0xdc00 && lo < 0xe000) {
					cp = 0x10000 + ((cp - 0xd800) << 10) +
					     (lo - 0xdc00);
					r->p += 6;
				} else {
					cp = 0xfffd;
				}
			}
			n = jr_utf8(cp, tmp);
			goto emit;
		default:
			goto bad_escape;
		}
		r->p += 2;
emit:
		if (buf != NULL) {
			if (memchr(tmp, '\0', n) != NULL) {
				free(buf);
				return jr_fail(r, "string holds a NUL byte");
			}
			memcpy(buf + w, tmp, n);
			w += n;
		}
	}
	r->p = q + 1;
	if (buf != NULL) {
		buf[w] = '\0';
		*out = buf;
	}
	return 0;

bad_escape:
	free(buf);
	return jr_fail(r, "invalid escape in string");
}

static int jr_digits(struct jr *r, struct jr_num *num, int keep)
{
	const char *s = r->p;

	while (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
		if (keep) {
			uint64_t d = (uint64_t)(*r->p - '0');

			if (num->mag > (UINT64_MAX - d) / 10)
				num->overflow = 1;
			else
				num->mag = num->mag * 10 + d;
		}
		r->p++;
	}
	if (r->p == s)
		return jr_fail(r, "invalid number, want a digit");
	return 0;
}

// Reads a number with the JSON grammar: -?(0|[1-9][0-9]*)(.[0-9]+)?
// ([eE][+-]?[0-9]+)?. The magnitude is only meaningful when integral.
static int jr_number(struct jr *r, struct jr_num *num)
{
	memset(num, 0, sizeof(*num));
	num->integral = 1;
	jr_ws(r);
	if (r->p < r->end && *r->p == '-') {
		num->neg = 1;
		r->p++;
	}
	if (r->p < r->end && *r->p == '0') {
		r->p++;
	} else if (jr_digits(r, num, 1) != 0) {
		return -1;
	}
	if (r->p < r->end && *r->p == '.') {
		num->integral = 0;
		r->p++;
		if (jr_digits(r, num, 0) != 0)
			return -1;
	}
	if (r->p < r->end && (*r->p == 'e' || *r->p == 'E')) {
		num->integral = 0;
		r->p++;
		if (r->p < r->end && (*r->p == '+' || *r->p == '-'))
			r->p++;
		if (jr_digits(r, num, 0) != 0)
			return -1;
	}
	return 0;
}

static int jr_skip_value(struct jr *r);

static int jr_skip_array(struct jr *r)
{
	if (jr_expect(r, '[') != 0)
		return -1;
	if (jr_peek(r) == ']') {
		r->p++;
		return 0;
	}
	for (;;) {
		if (jr_skip_value(r) != 0)
			return -1;
		if (jr_peek(r) == ',') {
			r->p++;
			continue;
		}
		return jr_expect(r, ']');
	}
}

static int jr_object(struct jr *r, jr_field_fn fn, void *ctx);

static int jr_skip_field(struct jr *r, const char *key, void *ctx)
{
	(void)key;
	(void)ctx;
	return jr_skip_value(r);
}

static int jr_skip_value(struct jr *r)
{
	struct jr_num num;
	int c = jr_peek(r);
	int rc;

	switch (c) {
	case -1:
		return jr_fail(r, "unexpected end of JSON, want a value");
	case '"':
		return jr_string(r, NULL);
	case 't':
		return jr_word(r, "true");
	case 'f':
		return jr_word(r, "false");
	case 'n':
		return jr_word(r, "null");
	case '[':
	case '{':
		if (r->depth >= JR_MAX_DEPTH)
			return jr_fail(r, "JSON nested too deeply");
		r->depth++;
		if (c == '[')
			rc = jr_skip_array(r);
		else
			rc = jr_object(r, jr_skip_field, NULL);
		r->depth--;
		return rc;
	default:
		if (c == '-' || (c >= '0' && c <= '9'))
			return jr_number(r, &num);
		return jr_fail(r, "invalid character 0x%02x, want a value",
			       c);
	}
}

// Reads an object and hands each key to fn, which must consume the value.
static int jr_object(struct jr *r, jr_field_fn fn, void *ctx)
{
	if (jr_expect(r, '{') != 0)
		return -1;
	if (jr_peek(r) == '}') {
		r->p++;
		return 0;
	}
	for (;;) {
		char *key = NULL;
		int rc;

		if (jr_peek(r) != '"')
			return jr_fail(r, "want a string key");
		if (jr_string(r, &key) != 0)
			return -1;
		rc = jr_expect(r, ':');
		if (rc == 0)
			rc = fn(r, key, ctx);
		free(key);
		if (rc != 0)
			return -1;
		if (jr_peek(r) == ',') {
			r->p++;
			continue;
		}
		return jr_expect(r, '}');
	}
}

// Parses the whole payload, which is one object (or null, which Go accepts
// and treats as no fields at all) with nothing but whitespace after it.
static int jr_parse(struct jr *r, jr_field_fn fn, void *ctx)
{
	r->depth = 1;
	if (r->err != NULL && r->errlen > 0)
		r->err[0] = '\0';
	switch (jr_null(r)) {
	case 1:
		break;
	case 0:
		if (jr_object(r, fn, ctx) != 0)
			return -1;
		break;
	default:
		return -1;
	}
	jr_ws(r);
	if (r->p != r->end)
		return jr_fail(r, "invalid character after top-level value");
	return 0;
}

static int jr_key_is(const char *key, const char *field)
{
	// encoding/json falls back to a case-insensitive match. ASCII is
	// enough for our field names.
	for (; *key != '\0' && *field != '\0'; key++, field++) {
		char a = *key, b = *field;

		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (a != b)
			return 0;
	}
	return *key == *field;
}

static int jr_bool(struct jr *r, const char *key, int *out)
{
	switch (jr_peek(r)) {
	case 't':
		if (jr_word(r, "true") != 0)
			return -1;
		*out = 1;
		return 0;
	case 'f':
		if (jr_word(r, "false") != 0)
			return -1;
		*out = 0;
		return 0;
	case 'n':
		return jr_word(r, "null");
	default:
		return jr_fail(r, "%s: want a boolean", key);
	}
}

static int jr_uint16(struct jr *r, const char *key, uint16_t *out)
{
	struct jr_num num;
	int c;

	switch (jr_null(r)) {
	case 1:
		return 0;
	case 0:
		break;
	default:
		return -1;
	}
	c = jr_peek(r);
	if (c != '-' && (c < '0' || c > '9'))
		return jr_fail(r, "%s: want a number", key);
	if (jr_number(r, &num) != 0)
		return -1;
	if (!num.integral)
		return jr_fail(r, "%s: want an integer", key);
	if (num.neg)
		return jr_fail(r, "%s: want an unsigned integer", key);
	*out = (num.overflow || num.mag > UINT16_MAX) ? UINT16_MAX :
		(uint16_t)num.mag;
	return 0;
}

static int jr_int(struct jr *r, const char *key, int *out)
{
	struct jr_num num;
	int c;

	switch (jr_null(r)) {
	case 1:
		return 0;
	case 0:
		break;
	default:
		return -1;
	}
	c = jr_peek(r);
	if (c != '-' && (c < '0' || c > '9'))
		return jr_fail(r, "%s: want a number", key);
	if (jr_number(r, &num) != 0)
		return -1;
	if (!num.integral)
		return jr_fail(r, "%s: want an integer", key);
	if (num.overflow ||
	    num.mag > (num.neg ? (uint64_t)INT_MAX + 1 : (uint64_t)INT_MAX))
		return jr_fail(r, "%s: integer out of range", key);
	if (num.neg)
		*out = num.mag == (uint64_t)INT_MAX + 1 ?
			INT_MIN : -(int)num.mag;
	else
		*out = (int)num.mag;
	return 0;
}

static int jr_str(struct jr *r, const char *key, char **out)
{
	char *s = NULL;

	switch (jr_null(r)) {
	case 1:
		return 0;
	case 0:
		break;
	default:
		return -1;
	}
	if (jr_peek(r) != '"')
		return jr_fail(r, "%s: want a string", key);
	if (jr_string(r, &s) != 0)
		return -1;
	free(*out);
	*out = s;
	return 0;
}

static void proto_free_strings(char **v, size_t n)
{
	size_t i;

	if (v == NULL)
		return;
	for (i = 0; i < n; i++)
		free(v[i]);
	free(v);
}

// Reads an array of strings into a NULL terminated heap array. null gives
// an empty array, as a nil slice does in Go, and a null element gives an
// empty string. The result replaces *out only on success.
static int jr_strings(struct jr *r, const char *key, char ***out,
		      size_t *out_len)
{
	char **v;
	size_t n = 0, cap = 4;

	v = calloc(cap + 1, sizeof(*v));
	if (v == NULL)
		return jr_fail(r, "out of memory");
	switch (jr_null(r)) {
	case 1:
		goto done;
	case 0:
		break;
	default:
		goto fail;
	}
	if (jr_peek(r) != '[') {
		jr_fail(r, "%s: want an array of strings", key);
		goto fail;
	}
	r->p++;
	if (jr_peek(r) == ']') {
		r->p++;
		goto done;
	}
	for (;;) {
		char *s = NULL;

		if (n == cap) {
			char **nv;

			if (cap > (SIZE_MAX / sizeof(*v) - 1) / 2) {
				jr_fail(r, "out of memory");
				goto fail;
			}
			nv = realloc(v, (cap * 2 + 1) * sizeof(*v));
			if (nv == NULL) {
				jr_fail(r, "out of memory");
				goto fail;
			}
			v = nv;
			cap *= 2;
		}
		switch (jr_null(r)) {
		case 1:
			s = strdup("");
			if (s == NULL) {
				jr_fail(r, "out of memory");
				goto fail;
			}
			break;
		case 0:
			if (jr_peek(r) != '"') {
				jr_fail(r, "%s: want an array of strings", key);
				goto fail;
			}
			if (jr_string(r, &s) != 0)
				goto fail;
			break;
		default:
			goto fail;
		}
		v[n++] = s;
		v[n] = NULL;
		if (jr_peek(r) == ',') {
			r->p++;
			continue;
		}
		if (jr_expect(r, ']') != 0)
			goto fail;
		break;
	}
done:
	v[n] = NULL;
	proto_free_strings(*out, *out_len);
	*out = v;
	*out_len = n;
	return 0;
fail:
	proto_free_strings(v, n);
	return -1;
}

static int proto_open_field(struct jr *r, const char *key, void *ctx)
{
	struct open_request *req = ctx;

	if (jr_key_is(key, "argv"))
		return jr_strings(r, "argv", &req->argv, &req->argc);
	if (jr_key_is(key, "env"))
		return jr_strings(r, "env", &req->env, &req->envc);
	if (jr_key_is(key, "cwd"))
		return jr_str(r, "cwd", &req->cwd);
	if (jr_key_is(key, "user"))
		return jr_str(r, "user", &req->user);
	if (jr_key_is(key, "tty"))
		return jr_bool(r, "tty", &req->tty);
	if (jr_key_is(key, "rows"))
		return jr_uint16(r, "rows", &req->rows);
	if (jr_key_is(key, "cols"))
		return jr_uint16(r, "cols", &req->cols);
	return jr_skip_value(r);
}

int proto_parse_open(const char *json, size_t len, struct open_request *req,
		     char *err, size_t errlen)
{
	struct jr r;

	memset(req, 0, sizeof(*req));
	jr_init(&r, json, len, err, errlen);
	if (jr_parse(&r, proto_open_field, req) != 0)
		goto fail;
	// argv and env are always arrays after a successful parse, empty
	// when the request left them out or sent null.
	if (req->argv == NULL)
		req->argv = calloc(1, sizeof(*req->argv));
	if (req->env == NULL)
		req->env = calloc(1, sizeof(*req->env));
	if (req->argv == NULL || req->env == NULL) {
		jr_fail(&r, "out of memory");
		goto fail;
	}
	return 0;
fail:
	if (err != NULL && errlen > 0 && err[0] == '\0')
		snprintf(err, errlen, "invalid open request");
	proto_free_open(req);
	return -1;
}

void proto_free_open(struct open_request *req)
{
	if (req == NULL)
		return;
	proto_free_strings(req->argv, req->argc);
	proto_free_strings(req->env, req->envc);
	free(req->cwd);
	free(req->user);
	memset(req, 0, sizeof(*req));
}

struct proto_resize {
	uint16_t rows, cols;
};

static int proto_resize_field(struct jr *r, const char *key, void *ctx)
{
	struct proto_resize *rs = ctx;

	if (jr_key_is(key, "rows"))
		return jr_uint16(r, "rows", &rs->rows);
	if (jr_key_is(key, "cols"))
		return jr_uint16(r, "cols", &rs->cols);
	return jr_skip_value(r);
}

int proto_parse_resize(const char *json, size_t len, uint16_t *rows,
		       uint16_t *cols)
{
	struct jr r;
	struct proto_resize rs = { 0, 0 };

	jr_init(&r, json, len, NULL, 0);
	if (jr_parse(&r, proto_resize_field, &rs) != 0)
		return -1;
	*rows = rs.rows;
	*cols = rs.cols;
	return 0;
}

static int proto_signal_field(struct jr *r, const char *key, void *ctx)
{
	if (jr_key_is(key, "signal"))
		return jr_int(r, "signal", ctx);
	return jr_skip_value(r);
}

int proto_parse_signal(const char *json, size_t len, int *sig)
{
	struct jr r;
	int s = 0;

	jr_init(&r, json, len, NULL, 0);
	if (jr_parse(&r, proto_signal_field, &s) != 0)
		return -1;
	*sig = s;
	return 0;
}

int proto_format_exit(char *buf, size_t cap, int code)
{
	int n = snprintf(buf, cap, "{\"code\":%d}", code);

	if (n < 0 || (size_t)n >= cap)
		return -1;
	return n;
}

int proto_format_error(char *buf, size_t cap, const char *msg)
{
	static const char head[] = "{\"message\":\"";
	static const char hex[] = "0123456789abcdef";
	size_t w = 0;
	const unsigned char *s;

	if (msg == NULL)
		msg = "";
	if (cap < sizeof(head))
		return -1;
	memcpy(buf, head, sizeof(head) - 1);
	w = sizeof(head) - 1;
	for (s = (const unsigned char *)msg; *s != '\0'; s++) {
		char esc[6];
		size_t n = 2;

		esc[0] = '\\';
		switch (*s) {
		case '"': esc[1] = '"'; break;
		case '\\': esc[1] = '\\'; break;
		case '\n': esc[1] = 'n'; break;
		case '\r': esc[1] = 'r'; break;
		case '\t': esc[1] = 't'; break;
		default:
			if (*s < 0x20) {
				esc[1] = 'u';
				esc[2] = '0';
				esc[3] = '0';
				esc[4] = hex[*s >> 4];
				esc[5] = hex[*s & 0xf];
				n = 6;
			} else {
				esc[0] = (char)*s;
				n = 1;
			}
		}
		if (cap - w <= n)
			return -1;
		memcpy(buf + w, esc, n);
		w += n;
	}
	// The closing quote and brace, and the NUL.
	if (cap - w < 3)
		return -1;
	buf[w++] = '"';
	buf[w++] = '}';
	buf[w] = '\0';
	return (int)w;
}
