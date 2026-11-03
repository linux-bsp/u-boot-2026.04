// SPDX-License-Identifier: GPL-2.0+
/* Two bounded warm-reset banks. R5 alone rotates; later stages only append. */
#include <bsp_bootlog.h>
#include <asm/cache.h>
#include <cpu_func.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/kconfig.h>
#include <bsp_bootlog_layout.h>
#if defined(CONFIG_CPU_V7R) && defined(CONFIG_XPL_BUILD)
static u8 early[2048] __aligned(64);
#endif
static struct bl_record *active __section(".data") = NULL;
static unsigned int capacity __section(".data") = 0;
static bool initialized __section(".data") = false;
static bool in_ddr __section(".data") = false;
static void start(struct bl_record *r, unsigned int stage, unsigned int bytes)
{
	memset(r, 0, sizeof(*r));
	r->magic = BL_RECORD_MAGIC;
	r->version = 2;
	r->stage = stage + 1;
	active = r;
	capacity = bytes - sizeof(*r);
}
static struct bl_bank *bank(unsigned int slot)
{
	return (void *)(BL_BASE + slot * BL_BANK_SIZE);
}
void bsp_bootlog_init(void)
{
	if (initialized)
		return;
	initialized = true;
#if defined(CONFIG_CPU_V7R) && defined(CONFIG_XPL_BUILD)
	start((void *)early, 0, sizeof(early));
#else
	int slot = bl_latest(bank(0), bank(1));
	unsigned int stage = IS_ENABLED(CONFIG_XPL_BUILD) ? 1 : 2;

	/* Never invent a bank after a broken handoff or erase its sibling. */
	if (slot < 0)
		return;
	start((void *)bank(slot) + bl_offset[stage], stage, bl_size[stage]);
	in_ddr = true;
#endif
}
void bsp_bootlog_ddr_ready(void)
{
#if defined(CONFIG_CPU_V7R) && defined(CONFIG_XPL_BUILD)
	struct bl_bank *b;
	u64 generation = 1;
	int latest, slot;

	bsp_bootlog_init();
	latest = bl_latest(bank(0), bank(1));
	slot = latest < 0 ? 0 : 1 - latest;
	if (latest >= 0)
		generation = bank(latest)->generation + 1;
	b = bank(slot);
	memset(b, 0, BL_BANK_SIZE);
	b->magic = BL_MAGIC;
	b->version = 2;
	b->generation = generation;
	b->crc = bl_crc(0, b, 32);
	active = (void *)b + bl_offset[0];
	memcpy(active, early, sizeof(early));
	capacity = bl_size[0] - sizeof(*active);
	in_ddr = true;
	flush_dcache_range((ulong)b, (ulong)b + BL_BANK_SIZE);
#endif
}
void bsp_bootlog_write(const char *data, size_t len)
{
	size_t available, used;

	bsp_bootlog_init();
	if (!active)
		return;
	available = capacity - active->length;
	used = len < available ? len : available;
	active->reserved[0] = active->length;
	active->reserved[1] = active->crc;
	memcpy(active->data + active->length, data, used);
	active->crc = bl_crc(active->crc, data, used);
	active->length += used;
	if (len - used > 0xffffffffU - active->dropped)
		active->dropped = 0xffffffffU;
	else
		active->dropped += len - used;
	if (in_ddr)
		flush_dcache_range((ulong)active & ~63UL,
			ALIGN((ulong)active + sizeof(*active) + active->length, 64));
}
