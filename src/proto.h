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

// The exec protocol of pkg/agentproto, frame for frame. A frame is a 9 byte
// header (type, then stream and payload length as big endian 32 bit values)
// followed by the payload. Control payloads are JSON and stdio payloads are
// raw bytes.

#ifndef URUNIT_PROTO_H
#define URUNIT_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define PROTO_HDR_LEN 9
#define PROTO_MAX_PAYLOAD (1u << 20)

#define PROTO_OPEN 1
#define PROTO_STDIN 2
#define PROTO_STDOUT 3
#define PROTO_STDERR 4
#define PROTO_CLOSE_STDIN 5
#define PROTO_RESIZE 6
#define PROTO_SIGNAL 7
#define PROTO_EXIT 8
#define PROTO_ERROR 9

struct proto_hdr {
	uint8_t type;
	uint32_t stream;
	uint32_t len;
};

// The fields of agentproto.OpenRequest. argv and env are NULL terminated
// heap arrays. cwd and user are NULL when the request omits them.
struct open_request {
	char **argv;
	size_t argc;
	char **env;
	size_t envc;
	char *cwd;
	char *user;
	int tty;
	uint16_t rows, cols;
};

void proto_encode_hdr(uint8_t out[PROTO_HDR_LEN], uint8_t type,
		      uint32_t stream, uint32_t len);

// Returns -1 when the payload length exceeds PROTO_MAX_PAYLOAD.
int proto_decode_hdr(const uint8_t in[PROTO_HDR_LEN], struct proto_hdr *h);

// Parses the JSON that encoding/json emits for agentproto.OpenRequest.
// Unknown keys are skipped. Returns 0, or -1 with a message in err.
int proto_parse_open(const char *json, size_t len, struct open_request *req,
		     char *err, size_t errlen);

// Frees what proto_parse_open allocated. It is safe after a failed parse.
void proto_free_open(struct open_request *req);

int proto_parse_resize(const char *json, size_t len, uint16_t *rows,
		       uint16_t *cols);
int proto_parse_signal(const char *json, size_t len, int *sig);

// Writes {"code":N} into buf. Returns its length, or -1 when it does not
// fit.
int proto_format_exit(char *buf, size_t cap, int code);

// Writes {"message":"..."} into buf with msg escaped for JSON. Returns its
// length, or -1 when it does not fit.
int proto_format_error(char *buf, size_t cap, const char *msg);

#endif // URUNIT_PROTO_H
