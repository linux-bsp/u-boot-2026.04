// SPDX-License-Identifier: GPL-2.0+
/* AM625 log handoff v1: three LE records in a dedicated reserved DDR region.
 * Prefix-preserving buffers record dropped bytes when full. UART bytes emitted
 * before this module is initialized, ROM, TIFS and ATF output are not captured.
 */
#include <bsp_bootlog.h>
#include <asm/cache.h>
#include <cpu_func.h>
#include <linux/string.h>
#include <linux/kconfig.h>
#define BASE 0x9c900000UL
#define STRIDE 16384U
#define MAGIC 0x31474f4cU
struct record {
	u32 magic, version, stage, length, dropped, crc, reserved[2];
	u8 data[];
};
/* R5 SRAM usage is accounted for by the SPL linker BSS size limit. */
#if defined(CONFIG_CPU_V7R) && defined(CONFIG_XPL_BUILD)
static u8 early[2048] __aligned(64);
#endif
/* .data survives U-Boot relocation; do not rely on pre-relocation BSS. */
static struct record *active __section(".data") = NULL;
static unsigned int capacity __section(".data") = 0;
static bool initialized __section(".data") = false;
static bool in_ddr __section(".data") = false;
static u32 checksum(u32 crc, const u8 *p, size_t len)
{
	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ ((0U - (crc & 1U)) & 0xedb88320U);
	}
	return ~crc;
}
static inline bool valid(struct record *r, u32 stage)
{
	return r->magic == MAGIC && r->version == 1 && r->stage == stage &&
		r->length <= STRIDE - sizeof(*r) &&
		r->crc == checksum(0, r->data, r->length);
}
static void start(struct record *r, u32 stage, unsigned int bytes)
{
	memset(r, 0, sizeof(*r));
	r->magic = MAGIC;
	r->version = 1;
	r->stage = stage;
	active = r;
	capacity = bytes - sizeof(*r);
}
void bsp_bootlog_init(void)
{
	if (initialized)
		return;
	initialized = true;
#if defined(CONFIG_CPU_V7R) && defined(CONFIG_XPL_BUILD)
	start((void *)early, 1, sizeof(early));
#else
	struct record *r5 = (void *)BASE;
	struct record *a53 = (void *)(BASE + STRIDE);

	in_ddr = true;
	if (IS_ENABLED(CONFIG_XPL_BUILD)) {
		/* A prior stage-2 record means this is stale data from an older boot. */
		if (!valid(r5, 1) || a53->magic)
			memset((void *)BASE, 0, 3 * STRIDE);
		start(a53, 2, STRIDE);
	} else {
		struct record *uboot = (void *)(BASE + 2 * STRIDE);

		if (!valid(a53, 2) || uboot->magic)
			memset((void *)BASE, 0, 3 * STRIDE);
		start(uboot, 3, STRIDE);
	}
#endif
}
void bsp_bootlog_ddr_ready(void)
{
#if defined(CONFIG_CPU_V7R) && defined(CONFIG_XPL_BUILD)
	struct record *r5 = (void *)BASE;

	bsp_bootlog_init();
	memset((void *)BASE, 0, 3 * STRIDE);
	memcpy(r5, early, sizeof(early));
	active = r5;
	capacity = STRIDE - sizeof(*r5);
	in_ddr = true;
	flush_dcache_range(BASE, BASE + 3 * STRIDE);
#endif
}
void bsp_bootlog_write(const char *data, size_t len)
{
	size_t available, used;

	bsp_bootlog_init();
	available = capacity - active->length;
	used = len < available ? len : available;
	memcpy(active->data + active->length, data, used);
	active->crc = checksum(active->crc, (const u8 *)data, used);
	active->length += used;
	if (len - used > 0xffffffffU - active->dropped)
		active->dropped = 0xffffffffU;
	else
		active->dropped += len - used;
	/* Commit cache lines at each output call, including the final non-newline
	 * print. Other cores and the Linux importer must see completed records. */
	if (in_ddr)
		flush_dcache_range((ulong)active, (ulong)active + STRIDE);
}
