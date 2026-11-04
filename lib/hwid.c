/* SPDX-License-Identifier: GPL-2.0+ */
#include <dm-hwid-value.h>
#include <hwid.h>
#include <env.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/libfdt.h>

int hwid_read(struct hwid_status *status)
{
    const char *value = env_get("hwid"); unsigned char raw[8];
    memset(status, 0, sizeof(*status));
    if (!value) return -ENODATA;
    if (strlen(value) != 16 || !dm_hwid_parse(value, raw)) return -EINVAL;
    dm_hwid_format(raw, status->text);
    return 0;
}

int hwid_spl_init(void)
{
    struct hwid_status status;
    /* Called after the boot storage controller is usable. Environment drivers
     * select and restore eMMC hardware partitions and handle NOR redundancy. */
    int ret = env_init();
    if (!ret || ret == -ENOENT) ret = env_load();
    if (!ret) ret = hwid_read(&status);
    if (ret) printf("[hwid] [ERROR] stage=SPL environment identity unavailable (%d)\n", ret);
    else printf("[hwid] [INFO] stage=SPL hwid=%s source=uboot-env\n", status.text);
    return ret;
}

#ifndef CONFIG_XPL_BUILD
int hwid_export_env(void)
{
    struct hwid_status status;
    int ret = hwid_read(&status);
    if (ret) printf("[hwid] [ERROR] environment identity unavailable (%d)\n", ret);
    else printf("[hwid] [INFO] hwid=%s source=uboot-env\n", status.text);
    return ret;
}

int hwid_fdt_fixup(void *blob)
{
    struct hwid_status status;
    int node = fdt_path_offset(blob, "/chosen");
    if (node < 0) node = fdt_add_subnode(blob, 0, "chosen");
    if (node < 0) return node;
    fdt_delprop(blob, node, "dm,hwid");
    if (hwid_read(&status)) return 0;
    return fdt_setprop_string(blob, node, "dm,hwid", status.text);
}

int hwid_fit_check(const void *fit, int config)
{
    struct hwid_status status;
    int ret = hwid_read(&status);
    if (ret) return ret;
    int count = fdt_stringlist_count(fit, config, "fw,hwids");
    if (count < 1 || count > 256) return -EPERM;
    for (int i = 0; i < count; i++) {
        int length; unsigned char value[8];
        const char *text = fdt_stringlist_get(fit, config, "fw,hwids", i, &length);
        if (!text || length != 16 || !dm_hwid_parse(text, value)) return -EINVAL;
    }
    return fdt_stringlist_search(fit, config, "fw,hwids", status.text) >= 0 ? 0 : -EPERM;
}
#endif
