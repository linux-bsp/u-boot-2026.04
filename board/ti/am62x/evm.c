// SPDX-License-Identifier: GPL-2.0+
/*
 * Board specific initialization for AM62x platforms
 *
 * Copyright (C) 2020-2022 Texas Instruments Incorporated - https://www.ti.com/
 *	Suman Anna <s-anna@ti.com>
 *
 */

#include <blk.h>
#include <efi_loader.h>
#include <firmware_upgrade.h>
#include <env.h>
#include <spl.h>
#include <init.h>
#include <video.h>
#include <splash.h>
#include <cpu_func.h>
#include <k3-ddrss.h>
#include <memalign.h>
#include <mmc.h>
#include <fdt_support.h>
#include <fdt_simplefb.h>
#include <asm/io.h>
#include <asm/arch/hardware.h>
#include <dm/uclass.h>
#include <asm/arch/k3-ddr.h>

#include "../common/board_detect.h"
#include "../common/fdt_ops.h"
#include "../common/k3_32k_lfosc.h"

#define board_is_am62x_skevm()  (board_ti_k3_is("AM62-SKEVM") || \
				 board_ti_k3_is("AM62B-SKEVM"))
#define board_is_am62b_p1_skevm() board_ti_k3_is("AM62B-SKEVM-P1")
#define board_is_am62x_lp_skevm()  board_ti_k3_is("AM62-LP-SKEVM")
#define board_is_am62x_sip_skevm()  board_ti_k3_is("AM62SIP-SKEVM")

DECLARE_GLOBAL_DATA_PTR;

#if CONFIG_IS_ENABLED(FU_BOOT_SELECTOR)
struct fu_spl_store_ctx {
	struct blk_desc *blk;
};

static ulong fu_spl_metadata_offset(unsigned int copy)
{
	return copy ? CONFIG_SPL_FU_METADATA1_OFFSET :
		      CONFIG_SPL_FU_METADATA0_OFFSET;
}

static int fu_spl_metadata_read(void *ctx, unsigned int copy, void *buf,
				size_t len)
{
	ALLOC_CACHE_ALIGN_BUFFER(u8, block, MMC_MAX_BLOCK_LEN);
	struct fu_spl_store_ctx *store = ctx;
	ulong offset = fu_spl_metadata_offset(copy);

	if (copy >= FU_METADATA_COPIES || len > store->blk->blksz ||
	    store->blk->blksz > MMC_MAX_BLOCK_LEN ||
	    offset % store->blk->blksz)
		return -EINVAL;
	if (blk_dread(store->blk, offset / store->blk->blksz, 1, block) != 1)
		return -EIO;

	memcpy(buf, block, len);
	return 0;
}

static int fu_spl_metadata_write(void *ctx, unsigned int copy, const void *buf,
				 size_t len)
{
	ALLOC_CACHE_ALIGN_BUFFER(u8, block, MMC_MAX_BLOCK_LEN);
	struct fu_spl_store_ctx *store = ctx;
	ulong offset = fu_spl_metadata_offset(copy);

	if (copy >= FU_METADATA_COPIES || len > store->blk->blksz ||
	    store->blk->blksz > MMC_MAX_BLOCK_LEN ||
	    offset % store->blk->blksz)
		return -EINVAL;

	memset(block, 0xff, store->blk->blksz);
	memcpy(block, buf, len);
	if (blk_dwrite(store->blk, offset / store->blk->blksz, 1, block) != 1)
		return -EIO;

	return 0;
}

unsigned long board_spl_mmc_get_uboot_raw_sector(struct mmc *mmc,
						 unsigned long raw_sect)
{
	struct fu_spl_store_ctx ctx = { .blk = mmc_get_blk_desc(mmc) };
	struct fu_metadata_store store = {
		.read = fu_spl_metadata_read,
		.ctx = &ctx,
	};
	struct fu_metadata metadata;
	u8 selected, slot;
	int ret;

	ret = fu_metadata_load(&store, &metadata);
	if (ret) {
		printf("SPL: fu metadata unavailable (%d), using slot A\n", ret);
		return raw_sect;
	}

	if (CONFIG_IS_ENABLED(FU_BOOT_SELECTOR_COMMIT)) {
		store.write = fu_spl_metadata_write;
		ret = fu_metadata_select(&metadata, &selected);
		if (ret > 0 && fu_metadata_save(&store, &metadata)) {
			printf("SPL: cannot commit fu selection, using last-good\n");
			selected = metadata.last_good_deployment;
			if (selected > 1 ||
			    !metadata.deployment[selected].successful)
				return raw_sect;
		} else if (ret < 0) {
			printf("SPL: cannot select fu deployment (%d), using slot A\n",
			       ret);
			return raw_sect;
		}
	} else {
		ret = fu_metadata_get_selected(&metadata, &selected);
		if (ret) {
			printf("SPL: no committed fu selection (%d), using slot A\n",
			       ret);
			return raw_sect;
		}
	}

	if (selected > 1 ||
	    !metadata.deployment[selected]
		     .component[FU_COMPONENT_BOOTLOADER].valid)
		return raw_sect;
	slot = metadata.deployment[selected]
		       .component[FU_COMPONENT_BOOTLOADER].slot;
	if (slot > 1)
		return raw_sect;

	printf("SPL: fu deployment %u, bootloader slot %c\n", selected,
	       'a' + slot);
	return slot ? CONFIG_SPL_FU_RAW_SECTOR_B : raw_sect;
}
#endif

#if CONFIG_IS_ENABLED(SPLASH_SCREEN)
static struct splash_location default_splash_locations[] = {
	{
		.name = "sf",
		.storage = SPLASH_STORAGE_SF,
		.flags = SPLASH_STORAGE_RAW,
		.offset = 0x700000,
	},
	{
		.name		= "mmc",
		.storage	= SPLASH_STORAGE_MMC,
		.flags		= SPLASH_STORAGE_FS,
		.devpart	= "1:1",
	},
};

int splash_screen_prepare(void)
{
	return splash_source_load(default_splash_locations,
				ARRAY_SIZE(default_splash_locations));
}
#endif

struct efi_fw_image fw_images[] = {
	{
		.image_type_id = AM62X_SK_TIBOOT3_IMAGE_GUID,
		.fw_name = u"AM62X_SK_TIBOOT3",
		.image_index = 1,
	},
	{
		.image_type_id = AM62X_SK_SPL_IMAGE_GUID,
		.fw_name = u"AM62X_SK_SPL",
		.image_index = 2,
	},
	{
		.image_type_id = AM62X_SK_UBOOT_IMAGE_GUID,
		.fw_name = u"AM62X_SK_UBOOT",
		.image_index = 3,
	}
};

struct efi_capsule_update_info update_info = {
	.dfu_string = "sf 0:0=tiboot3.bin raw 0 80000;"
	"tispl.bin raw 80000 200000;u-boot.img raw 280000 400000",
	.num_images = ARRAY_SIZE(fw_images),
	.images = fw_images,
};

#if CONFIG_IS_ENABLED(TI_I2C_BOARD_DETECT)
int do_board_detect(void)
{
	return do_board_detect_am6();
}

int checkboard(void)
{
	struct ti_am6_eeprom *ep = TI_AM6_EEPROM_DATA;

	if (!do_board_detect())
		printf("Board: %s rev %s\n", ep->name, ep->version);

	return 0;
}

#if CONFIG_IS_ENABLED(BOARD_LATE_INIT)
static void setup_board_eeprom_env(void)
{
	char *name = "am62x_skevm";

	if (do_board_detect())
		goto invalid_eeprom;

	if (board_is_am62x_skevm())
		name = "am62x_skevm";
	else if (board_is_am62b_p1_skevm())
		name = "am62b_p1_skevm";
	else if (board_is_am62x_lp_skevm())
		name = "am62x_lp_skevm";
	else if (board_is_am62x_sip_skevm())
		name = "am62x_sip_skevm";
	else
		printf("Unidentified board claims %s in eeprom header\n",
		       board_ti_get_name());

invalid_eeprom:
	set_board_info_env_am6(name);
}
#endif
#endif

#if CONFIG_IS_ENABLED(BOARD_LATE_INIT)
int board_late_init(void)
{
#if CONFIG_IS_ENABLED(TI_I2C_BOARD_DETECT)
    setup_board_eeprom_env();
    setup_serial_am6();
#endif

	ti_set_fdt_env(NULL, NULL);
	return 0;
}
#endif

#if defined(CONFIG_XPL_BUILD)
void spl_board_init(void)
{
	if (IS_ENABLED(CONFIG_TI_K3_BOARD_LFOSC))
		enable_32k_lfosc();

	enable_caches();
	if (IS_ENABLED(CONFIG_SPL_SPLASH_SCREEN) && IS_ENABLED(CONFIG_SPL_BMP))
		splash_display();

}

void spl_perform_board_fixups(struct spl_image_info *spl_image)
{
	if (IS_ENABLED(CONFIG_K3_DDRSS)) {
		if (IS_ENABLED(CONFIG_K3_INLINE_ECC))
			fixup_ddr_driver_for_ecc(spl_image);
	} else {
		fixup_memory_node(spl_image);
	}
}
#endif

#if defined(CONFIG_OF_BOARD_SETUP)
int ft_board_setup(void *blob, struct bd_info *bd)
{
	int ret = -1;

	if (IS_ENABLED(CONFIG_FDT_SIMPLEFB))
		ret = fdt_simplefb_enable_and_mem_rsv(blob);

	/* If simplefb is not enabled and video is active, then at least reserve
	 * the framebuffer region to preserve the splash screen while OS is booting
	 */
	if (IS_ENABLED(CONFIG_VIDEO) && IS_ENABLED(CONFIG_OF_LIBFDT)) {
		if (ret && video_is_active())
			return fdt_add_fb_mem_rsv(blob);
	}

	return 0;
}
#endif
