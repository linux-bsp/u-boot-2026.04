// SPDX-License-Identifier: GPL-2.0+
#include <command.h>
#include <env.h>
#include <test/cmd.h>
#include <test/ut.h>

static int cmd_test_fw_metadata(struct unit_test_state *uts)
{
	ut_assertok(run_command("fw init a", 0));
	ut_assert_nextline("Initialized metadata with slot a active");
	ut_assert_console_end();

	ut_assertok(run_command("fw select", 0));
	ut_assert_nextline("Selected deployment 0: boot=a kernel=a rootfs=a");
	ut_assert_console_end();
	ut_asserteq_str("0", env_get("fw_deployment"));
	ut_asserteq_str("a", env_get("fw_kernel_slot"));
	ut_asserteq_str("a", env_get("fw_rootfs_slot"));

	ut_assertok(run_command("fw rollback", 0));
	ut_assert_console_end();

	return 0;
}

CMD_TEST(cmd_test_fw_metadata, UTF_CONSOLE);
