/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef FW_VERITY_H
#define FW_VERITY_H
#include <stddef.h>
int fw_verity_args(const void *dtb, size_t size, const char *device,
                  unsigned long long capacity, char *args, size_t args_size);
#endif
