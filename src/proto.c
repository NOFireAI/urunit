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

// Stub: the proto-json task fills this file in.

#include <string.h>

#include "proto.h"

void proto_encode_hdr(uint8_t out[PROTO_HDR_LEN], uint8_t type,
		      uint32_t stream, uint32_t len)
{
	(void)type;
	(void)stream;
	(void)len;
	memset(out, 0, PROTO_HDR_LEN);
}

int proto_decode_hdr(const uint8_t in[PROTO_HDR_LEN], struct proto_hdr *h)
{
	(void)in;
	(void)h;
	return -1;
}

int proto_parse_open(const char *json, size_t len, struct open_request *req,
		     char *err, size_t errlen)
{
	(void)json;
	(void)len;
	memset(req, 0, sizeof(*req));
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	return -1;
}

void proto_free_open(struct open_request *req)
{
	memset(req, 0, sizeof(*req));
}

int proto_parse_resize(const char *json, size_t len, uint16_t *rows,
		       uint16_t *cols)
{
	(void)json;
	(void)len;
	(void)rows;
	(void)cols;
	return -1;
}

int proto_parse_signal(const char *json, size_t len, int *sig)
{
	(void)json;
	(void)len;
	(void)sig;
	return -1;
}

int proto_format_exit(char *buf, size_t cap, int code)
{
	(void)buf;
	(void)cap;
	(void)code;
	return -1;
}

int proto_format_error(char *buf, size_t cap, const char *msg)
{
	(void)buf;
	(void)cap;
	(void)msg;
	return -1;
}
