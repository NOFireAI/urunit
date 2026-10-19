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

#include <stdio.h>

#include "harness.h"
#include "urunit.h"

int harness_failures;

int run_mode_tests(void)
{
	int start = harness_failures;

	CHECK_EQ_INT(urunit_mode_from("/init", NULL), URUNIT_MODE_INIT);
	CHECK_EQ_INT(urunit_mode_from("/vz-init", NULL), URUNIT_MODE_VZ_INIT);
	CHECK_EQ_INT(urunit_mode_from("/run/urunc/urunit-agent", NULL),
		     URUNIT_MODE_AGENT);
	CHECK_EQ_INT(urunit_mode_from("/run/urunc/urunit", NULL),
		     URUNIT_MODE_SUPERVISE);
	CHECK_EQ_INT(urunit_mode_from("init", NULL), URUNIT_MODE_INIT);
	CHECK_EQ_INT(urunit_mode_from("/sbin/init.bak", NULL),
		     URUNIT_MODE_SUPERVISE);
	CHECK_EQ_INT(urunit_mode_from(NULL, NULL), URUNIT_MODE_SUPERVISE);

	CHECK_EQ_INT(urunit_mode_from("/init", "agent"), URUNIT_MODE_AGENT);
	CHECK_EQ_INT(urunit_mode_from("/urunit", "init"), URUNIT_MODE_INIT);
	CHECK_EQ_INT(urunit_mode_from("/urunit", "vz-init"),
		     URUNIT_MODE_VZ_INIT);
	CHECK_EQ_INT(urunit_mode_from("/init", "supervise"),
		     URUNIT_MODE_SUPERVISE);
	// An unknown URUNIT_MODE falls back to the program name.
	CHECK_EQ_INT(urunit_mode_from("/vz-init", "bogus"),
		     URUNIT_MODE_VZ_INIT);
	CHECK_EQ_INT(urunit_mode_from("/vz-init", ""), URUNIT_MODE_VZ_INIT);
	// The signal role has no name of its own: urunc asks for it through
	// URUNIT_MODE on the staged /run/urunc/urunit.
	CHECK_EQ_INT(urunit_mode_from("/run/urunc/urunit", "signal"),
		     URUNIT_MODE_SIGNAL);

	CHECK_EQ_INT(parse_signal_number("15"), 15);
	CHECK_EQ_INT(parse_signal_number("1"), 1);
	CHECK_EQ_INT(parse_signal_number("64"), 64);
	CHECK_EQ_INT(parse_signal_number("0"), -1);
	CHECK_EQ_INT(parse_signal_number("65"), -1);
	CHECK_EQ_INT(parse_signal_number("-15"), -1);
	CHECK_EQ_INT(parse_signal_number("TERM"), -1);
	CHECK_EQ_INT(parse_signal_number("15x"), -1);
	CHECK_EQ_INT(parse_signal_number(""), -1);
	CHECK_EQ_INT(parse_signal_number(NULL), -1);
	{
		char *one[] = { "urunit", NULL };
		char *bad[] = { "urunit", "TERM", NULL };

		CHECK_EQ_INT(signal_init_main(1, one), 2);
		CHECK_EQ_INT(signal_init_main(2, bad), 2);
	}

	return harness_failures - start;
}

int main(void)
{
	struct {
		const char *name;
		int (*run)(void);
	} suites[] = {
		{ "mode", run_mode_tests },
		{ "proto", run_proto_tests },
		{ "init", run_init_tests },
		{ "config", run_config_tests },
		{ "agent", run_agent_tests },
		{ "reap", run_reap_tests },
		{ "guards", run_guard_tests },
	};
	size_t i;

	for (i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
		int failed = suites[i].run();

		printf("%-8s %s (%d failures)\n", suites[i].name,
		       failed == 0 ? "ok" : "FAIL", failed);
	}
	if (harness_failures != 0) {
		printf("FAIL: %d checks failed\n", harness_failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
