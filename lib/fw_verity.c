// SPDX-License-Identifier: GPL-2.0+
#include <fw_verity.h>
#include <stdio.h>
#include <string.h>
#ifdef USE_HOSTCC
#include <errno.h>
#include <stdlib.h>
#include <libfdt.h>
#else
#include <linux/errno.h>
#include <malloc.h>
#include <linux/libfdt.h>
#endif

static int verity_u32(const void *dtb, int node, const char *name, unsigned *value)
{
    int length;
    const fdt32_t *p = fdt_getprop(dtb, node, name, &length);
    if (!p || length != 4) return -EINVAL;
    *value = fdt32_to_cpu(*p);
    return 0;
}

static const char *verity_digest(const void *dtb, int node, const char *name)
{
    int length;
    const char *p = fdt_getprop(dtb, node, name, &length);
    if (!p || length != 65 || p[64]) return NULL;
    for (unsigned i = 0; i < 64; i++)
        if (!strchr("0123456789abcdef", p[i]) || !p[i]) return NULL;
    return p;
}

/* All parameters come from the hash-verified FDT image in kernel FIT. */
static int fw_verity_args_aligned(const void *dtb, size_t size, const char *device,
                  unsigned long long capacity, char *args, size_t args_size)
{
    unsigned version, blocks, start, tree;
    const char *root, *salt, *algorithm;
    int node, length;
    unsigned long long expected_tree = 0, level;
    if (!args || !args_size) return -EINVAL;
    *args = 0;
    if (!dtb || size < sizeof(struct fdt_header) || fdt_check_header(dtb) ||
        fdt_totalsize(dtb) > size || !device || strncmp(device, "/dev/", 5)) return -EINVAL;
    for (const char *p = device + 5; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return -EINVAL;
    if (!device[5]) return -EINVAL;
    node = fdt_path_offset(dtb, "/chosen/fw-verity");
    if (node < 0) return -ENOENT;
    algorithm = fdt_getprop(dtb, node, "algorithm", &length);
    if (!algorithm || length != 7 || memcmp(algorithm, "sha256", 7)) return -EINVAL;
    if (verity_u32(dtb, node, "version", &version) || version != 1 ||
        verity_u32(dtb, node, "data-blocks", &blocks) || !blocks ||
        verity_u32(dtb, node, "hash-start-block", &start) || start != blocks ||
        verity_u32(dtb, node, "hash-blocks", &tree) || !tree) return -EINVAL;
    /* SHA256 occupies 32 bytes in each 4096-byte hash block (128 entries). */
    level = blocks;
    while (level > 1) {
        level = (level + 127) / 128;
        expected_tree += level;
    }
    if (tree != expected_tree || ((unsigned long long)start + tree) * 4096 > capacity)
        return -EFBIG;
    root = verity_digest(dtb, node, "root-hash");
    salt = verity_digest(dtb, node, "salt");
    if (!root || !salt) return -EINVAL;
    /* PID 1 validates all rootfs bytes before creating dm-verity itself. */
    length = snprintf(args, args_size,
        "root=/dev/ram0 rdinit=/init fw.rootdev=%s fw.verity=1", device);
    if (length < 0 || (size_t)length >= args_size) { *args = 0; return -ENOSPC; }
    return 0;
}

int fw_verity_args(const void *dtb, size_t size, const char *device,
                  unsigned long long capacity, char *args, size_t args_size)
{
    if (dtb && size >= sizeof(struct fdt_header) && ((unsigned long)dtb & 7)) {
        void *aligned = malloc(size);
        if (!aligned) {
            if (args && args_size) *args = 0;
            return -ENOMEM;
        }
        memcpy(aligned, dtb, size);
        int ret = fw_verity_args_aligned(aligned, size, device, capacity, args, args_size);
        free(aligned);
        return ret;
    }
    return fw_verity_args_aligned(dtb, size, device, capacity, args, args_size);
}
