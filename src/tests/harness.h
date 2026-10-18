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

// A minimal test harness. The CHECK macros print the failing expression
// with its file and line and let the test go on. Each suite returns the
// number of failures it added to harness_failures.

#ifndef URUNIT_TESTS_HARNESS_H
#define URUNIT_TESTS_HARNESS_H

#include <stdio.h>
#include <string.h>

extern int harness_failures;

#define HARNESS_FAIL(...)                                              \
	do {                                                           \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
		fprintf(stderr, __VA_ARGS__);                          \
		fputc('\n', stderr);                                   \
		harness_failures++;                                    \
	} while (0)

#define CHECK(cond)                                                    \
	do {                                                           \
		if (!(cond))                                           \
			HARNESS_FAIL("%s", #cond);                     \
	} while (0)

#define CHECK_EQ_INT(a, b)                                             \
	do {                                                           \
		long long harness_a_ = (long long)(a);                 \
		long long harness_b_ = (long long)(b);                 \
		if (harness_a_ != harness_b_)                          \
			HARNESS_FAIL("%s == %s: %lld != %lld", #a, #b, \
				     harness_a_, harness_b_);          \
	} while (0)

#define CHECK_EQ_STR(a, b)                                             \
	do {                                                           \
		const char *harness_a_ = (a);                          \
		const char *harness_b_ = (b);                          \
		if (harness_a_ == NULL || harness_b_ == NULL           \
			? harness_a_ != harness_b_                     \
			: strcmp(harness_a_, harness_b_) != 0)         \
			HARNESS_FAIL("%s == %s: \"%s\" != \"%s\"", #a, \
				     #b,                               \
				     harness_a_ ? harness_a_ : "(null)", \
				     harness_b_ ? harness_b_ : "(null)"); \
	} while (0)

#define CHECK_MEM_EQ(a, b, n)                                          \
	do {                                                           \
		if (memcmp((a), (b), (n)) != 0)                        \
			HARNESS_FAIL("%s == %s: %zu bytes differ", #a, \
				     #b, (size_t)(n));                 \
	} while (0)

int run_mode_tests(void);
int run_proto_tests(void);
int run_init_tests(void);
int run_config_tests(void);
int run_agent_tests(void);
int run_reap_tests(void);
int run_guard_tests(void);

#endif // URUNIT_TESTS_HARNESS_H
