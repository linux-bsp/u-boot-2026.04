/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef U_BOOT_HWID_H
#define U_BOOT_HWID_H
#include <linux/types.h>
struct hwid_status { char text[17]; };
int hwid_read(struct hwid_status *status);
int hwid_spl_init(void);
int hwid_export_env(void);
int hwid_fdt_fixup(void *blob);
int hwid_fit_check(const void *fit, int config);
#endif
