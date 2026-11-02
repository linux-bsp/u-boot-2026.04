/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef BSP_BOOTLOG_H
#define BSP_BOOTLOG_H
#include <linux/types.h>
#ifdef CONFIG_BSP_BOOTLOG
void bsp_bootlog_init(void);
void bsp_bootlog_ddr_ready(void);
void bsp_bootlog_write(const char *data, size_t len);
#else
static inline void bsp_bootlog_init(void) {}
static inline void bsp_bootlog_ddr_ready(void) {}
static inline void bsp_bootlog_write(const char *data, size_t len) {}
#endif
#endif
