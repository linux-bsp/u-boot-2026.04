/* SPDX-License-Identifier: GPL-2.0+ */
#include <command.h>
#include <hwid.h>

static int do_hwid(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
    if (argc != 1) return CMD_RET_USAGE;
    return hwid_export_env() ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}
U_BOOT_CMD(hwid, 1, 0, do_hwid, "read provisioned hardware identity", "");
