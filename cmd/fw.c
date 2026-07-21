// SPDX-License-Identifier: GPL-2.0+
#include <blk.h>
#include <command.h>
#include <dm.h>
#include <dm/ofnode.h>
#include <env.h>
#include <firmware_manager.h>
#include <hash.h>
#include <hexdump.h>
#include <image.h>
#include <mapmem.h>
#include <malloc.h>
#include <part.h>
#include <spi_flash.h>
#include <asm/cache.h>
#include <linux/ctype.h>
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <linux/mtd/spi-nor.h>

#define FW_COMPATIBLE "u-boot,firmware-manager"
#define FW_VERIFY_CHUNK (64 * 1024)
#define FW_SHA256_SIZE 32
#define FW_RELEASE_PATH_SIZE 128
#define FW_RELEASE_MAX_COMPONENTS FW_COMPONENT_COUNT

enum fw_storage_type {
	FW_STORAGE_MMC,
	FW_STORAGE_SPI_NOR,
};

enum fw_region_id {
	FW_REGION_META0,
	FW_REGION_META1,
	FW_REGION_TIBOOT3_A,
	FW_REGION_TIBOOT3_B,
	FW_REGION_TISPL_A,
	FW_REGION_TISPL_B,
	FW_REGION_UBOOT_A,
	FW_REGION_UBOOT_B,
	FW_REGION_KERNEL_A,
	FW_REGION_KERNEL_B,
	FW_REGION_ROOTFS_A,
	FW_REGION_ROOTFS_B,
	FW_REGION_COUNT,
};

struct fw_region {
	const char *name;
	u64 offset;
	u64 size;
};

struct fw_layout {
	enum fw_storage_type type;
	u32 device;
	u32 bus;
	u32 cs;
	u8 attempts;
	u8 default_slot;
	bool auto_init;
	bool allow_bootloader_update;
	bool spl_selects;
	const char *product;
	const char *layout_id;
	struct fw_region region[FW_REGION_COUNT];
};

struct fw_storage {
	enum fw_storage_type type;
	struct blk_desc *blk;
	struct udevice *sf;
	u64 capacity;
	u32 write_size;
	u32 erase_size;
};

struct fw_package {
	enum fw_component component;
	int conf_noffset;
	u32 version;
	u32 rollback_index;
};

struct fw_release_component {
	enum fw_component component;
	char file[FW_RELEASE_PATH_SIZE];
	u8 sha256[FW_SHA256_SIZE];
};

struct fw_release {
	u32 version;
	u32 rollback_index;
	bool allow_bootloader;
	int count;
	struct fw_release_component component[FW_RELEASE_MAX_COMPONENTS];
};

static const char *const fw_region_names[FW_REGION_COUNT] = {
	"meta0",   "meta1",   "tiboot3_a", "tiboot3_b", "tispl_a",  "tispl_b",
	"uboot_a", "uboot_b", "kernel_a",  "kernel_b",	"rootfs_a", "rootfs_b",
};

static int fw_read_layout(struct fw_layout *layout)
{
	const char *storage;
	ofnode node, child;
	u32 attempts;
	int i;

	node = ofnode_by_compatible(ofnode_null(), FW_COMPATIBLE);
	if (!ofnode_valid(node))
		return -ENOENT;
	if (!ofnode_is_enabled(node))
		return -ENODEV;

	memset(layout, 0, sizeof(*layout));
	storage = ofnode_read_string(node, "storage");
	if (!storage)
		return -EINVAL;
	if (!strcmp(storage, "mmc")) {
		layout->type = FW_STORAGE_MMC;
		layout->device = ofnode_read_u32_default(node, "device", 0);
	} else if (!strcmp(storage, "spi-nor")) {
		layout->type = FW_STORAGE_SPI_NOR;
		layout->bus = ofnode_read_u32_default(node, "bus", 0);
		layout->cs = ofnode_read_u32_default(node, "cs", 0);
	} else {
		return -EINVAL;
	}

	layout->product = ofnode_read_string(node, "product");
	layout->layout_id = ofnode_read_string(node, "layout-id");
	if (!layout->product || !layout->layout_id)
		return -EINVAL;
	attempts = ofnode_read_u32_default(node, "boot-attempts",
					   FW_DEFAULT_ATTEMPTS);
	if (!attempts || attempts > U8_MAX)
		return -EINVAL;
	layout->attempts = attempts;
	layout->default_slot = ofnode_read_u32_default(node, "default-slot", 0);
	if (layout->default_slot > 1)
		return -EINVAL;
	layout->auto_init = ofnode_read_bool(node, "auto-init");
	layout->allow_bootloader_update =
		ofnode_read_bool(node, "allow-bootloader-update");
	layout->spl_selects = ofnode_read_bool(node, "spl-selects");

	for (i = 0; i < FW_REGION_COUNT; i++) {
		child = ofnode_find_subnode(node, fw_region_names[i]);
		if (!ofnode_valid(child))
			return -ENOENT;
		layout->region[i].name = fw_region_names[i];
		if (ofnode_read_u64(child, "offset",
				    &layout->region[i].offset) ||
		    ofnode_read_u64(child, "size", &layout->region[i].size) ||
		    !layout->region[i].size)
			return -EINVAL;
	}

	return 0;
}

static int fw_storage_open(const struct fw_layout *layout,
			   struct fw_storage *storage)
{
	char dev[12];
	struct spi_nor *nor;
	int ret;

	memset(storage, 0, sizeof(*storage));
	storage->type = layout->type;
	if (layout->type == FW_STORAGE_MMC) {
		snprintf(dev, sizeof(dev), "%u", layout->device);
		ret = blk_get_device_by_str("mmc", dev, &storage->blk);
		if (ret < 0)
			return ret;
		storage->write_size = storage->blk->blksz;
		storage->capacity = storage->blk->lba * storage->blk->blksz;
		return 0;
	}

	ret = spi_flash_probe_bus_cs(layout->bus, layout->cs, &storage->sf);
	if (ret)
		return ret;
	nor = dev_get_uclass_priv(storage->sf);
	if (!nor)
		return -ENODEV;
	storage->write_size = 1;
	storage->erase_size = nor->mtd.erasesize;
	storage->capacity = nor->mtd.size;

	return 0;
}

static int fw_layout_validate(const struct fw_layout *layout,
			      const struct fw_storage *storage)
{
	const struct fw_region *a, *b;
	int i, j;

	for (i = 0; i < FW_REGION_COUNT; i++) {
		a = &layout->region[i];
		if (a->offset > storage->capacity ||
		    a->size > storage->capacity - a->offset)
			return -EFBIG;
		if (a->offset % storage->write_size)
			return -EINVAL;
		if (storage->type == FW_STORAGE_SPI_NOR &&
		    (a->offset > UINT_MAX ||
		     a->size > UINT_MAX - a->offset + 1ULL))
			return -EFBIG;
		if (storage->type == FW_STORAGE_SPI_NOR &&
		    (a->offset % storage->erase_size ||
		     a->size % storage->erase_size))
			return -EINVAL;

		for (j = i + 1; j < FW_REGION_COUNT; j++) {
			b = &layout->region[j];
			if (a->offset < b->offset + b->size &&
			    b->offset < a->offset + a->size)
				return -EINVAL;
		}
	}

	if (layout->region[FW_REGION_META0].size < FW_METADATA_SIZE ||
	    layout->region[FW_REGION_META1].size < FW_METADATA_SIZE)
		return -ENOSPC;

	return 0;
}

static int fw_storage_read(const struct fw_storage *storage, u64 offset,
			   void *buf, size_t len)
{
	size_t block_size, blocks, chunk_blocks, tail, copied = 0;
	u8 *bounce;
	lbaint_t lba;
	ulong done;

	if (!len)
		return 0;
	if (offset > storage->capacity || len > storage->capacity - offset)
		return -EFBIG;

	if (storage->type == FW_STORAGE_SPI_NOR) {
		if (offset > UINT_MAX || len > UINT_MAX - offset + 1ULL)
			return -EFBIG;
		return spi_flash_read_dm(storage->sf, offset, len, buf);
	}

	block_size = storage->write_size;
	if (offset % block_size)
		return -EINVAL;
	blocks = len / block_size;
	tail = len % block_size;
	chunk_blocks = max_t(size_t, 1, FW_VERIFY_CHUNK / block_size);
	bounce = memalign(ARCH_DMA_MINALIGN, chunk_blocks * block_size);
	if (!bounce)
		return -ENOMEM;

	lba = offset / block_size;
	while (blocks) {
		size_t count = min(blocks, chunk_blocks);

		done = blk_dread(storage->blk, lba, count, bounce);
		if (done != count)
			goto io_error;
		memcpy((u8 *)buf + copied, bounce, count * block_size);
		copied += count * block_size;
		lba += count;
		blocks -= count;
	}
	if (tail) {
		done = blk_dread(storage->blk, lba, 1, bounce);
		if (done != 1)
			goto io_error;
		memcpy((u8 *)buf + copied, bounce, tail);
	}
	free(bounce);

	return 0;

io_error:
	free(bounce);
	return -EIO;
}

static int fw_storage_write(const struct fw_storage *storage, u64 offset,
			    const void *buf, size_t len)
{
	size_t block_size, blocks, chunk_blocks, tail, copied = 0;
	u8 *bounce;
	lbaint_t lba;
	ulong done;

	if (!len)
		return 0;
	if (offset > storage->capacity || len > storage->capacity - offset)
		return -EFBIG;

	if (storage->type == FW_STORAGE_SPI_NOR) {
		if (offset > UINT_MAX || len > UINT_MAX - offset + 1ULL)
			return -EFBIG;
		return spi_flash_write_dm(storage->sf, offset, len, buf);
	}

	block_size = storage->write_size;
	if (offset % block_size)
		return -EINVAL;
	blocks = len / block_size;
	tail = len % block_size;
	chunk_blocks = max_t(size_t, 1, FW_VERIFY_CHUNK / block_size);
	bounce = memalign(ARCH_DMA_MINALIGN, chunk_blocks * block_size);
	if (!bounce)
		return -ENOMEM;

	lba = offset / block_size;
	while (blocks) {
		size_t count = min(blocks, chunk_blocks);

		memcpy(bounce, (const u8 *)buf + copied, count * block_size);
		done = blk_dwrite(storage->blk, lba, count, bounce);
		if (done != count)
			goto io_error;
		copied += count * block_size;
		lba += count;
		blocks -= count;
	}
	if (tail) {
		memset(bounce, 0xff, block_size);
		memcpy(bounce, (const u8 *)buf + copied, tail);
		done = blk_dwrite(storage->blk, lba, 1, bounce);
		if (done != 1)
			goto io_error;
	}
	free(bounce);

	return 0;

io_error:
	free(bounce);
	return -EIO;
}

static int fw_write_region(const struct fw_storage *storage,
			   const struct fw_region *region, const void *data,
			   size_t size)
{
	u8 *verify;
	size_t offset, chunk;
	int ret;

	if (size > region->size)
		return -EFBIG;

	if (storage->type == FW_STORAGE_SPI_NOR) {
		ret = spi_flash_erase_dm(storage->sf, region->offset,
					 region->size);
		if (ret)
			return ret;
	}

	ret = fw_storage_write(storage, region->offset, data, size);
	if (ret)
		return ret;

	verify = memalign(ARCH_DMA_MINALIGN, FW_VERIFY_CHUNK);
	if (!verify)
		return -ENOMEM;
	for (offset = 0; offset < size; offset += chunk) {
		chunk = min_t(size_t, FW_VERIFY_CHUNK, size - offset);
		ret = fw_storage_read(storage, region->offset + offset, verify,
				      chunk);
		if (ret || memcmp(verify, (const u8 *)data + offset, chunk)) {
			ret = ret ?: -EIO;
			break;
		}
	}
	free(verify);

	return ret;
}

struct fw_store_ctx {
	const struct fw_layout *layout;
	const struct fw_storage *storage;
};

static int fw_store_read(void *ctx, unsigned int copy, void *buf, size_t len)
{
	struct fw_store_ctx *store = ctx;
	const struct fw_region *region;

	if (copy >= FW_METADATA_COPIES)
		return -EINVAL;
	region = &store->layout->region[FW_REGION_META0 + copy];

	return fw_storage_read(store->storage, region->offset, buf, len);
}

static int fw_store_write(void *ctx, unsigned int copy, const void *buf,
			  size_t len)
{
	struct fw_store_ctx *store = ctx;
	const struct fw_region *region;
	int ret;

	if (copy >= FW_METADATA_COPIES)
		return -EINVAL;
	region = &store->layout->region[FW_REGION_META0 + copy];
	if (store->storage->type == FW_STORAGE_SPI_NOR) {
		ret = spi_flash_erase_dm(store->storage->sf, region->offset,
					 region->size);
		if (ret)
			return ret;
	}

	return fw_storage_write(store->storage, region->offset, buf, len);
}

static int fw_open(struct fw_layout *layout, struct fw_storage *storage,
		   struct fw_metadata_store *store, struct fw_store_ctx *ctx)
{
	int ret;

	ret = fw_read_layout(layout);
	if (ret)
		return ret;
	ret = fw_storage_open(layout, storage);
	if (ret)
		return ret;
	ret = fw_layout_validate(layout, storage);
	if (ret)
		return ret;

	ctx->layout = layout;
	ctx->storage = storage;
	store->read = fw_store_read;
	store->write = fw_store_write;
	store->ctx = ctx;

	return 0;
}

static int fw_fit_u32(const void *fit, int node, const char *name, u32 *value)
{
	const fdt32_t *prop;
	int len;

	prop = fdt_getprop(fit, node, name, &len);
	if (!prop)
		return len;
	if (len != sizeof(*prop))
		return -EINVAL;
	*value = fdt32_to_cpu(*prop);

	return 0;
}

static int fw_verify_prop(const void *fit, int conf, const char *prop)
{
	int count, i, node;

	count = fit_conf_get_prop_node_count(fit, conf, prop);
	if (count < 0)
		return count;
	for (i = 0; i < count; i++) {
		node = fit_conf_get_prop_node_index(fit, conf, prop, i);
		if (node < 0)
			return node;
		if (!fit_image_verify(fit, node))
			return -EBADMSG;
	}

	return 0;
}

static int fw_verify_package(const struct fw_layout *layout, const void *fit,
			     size_t size, struct fw_package *package)
{
	const char *type, *product, *layout_id;
	int conf, ret;

	ret = fit_check_format(fit, size);
	if (ret)
		return ret;
	if (fit_get_size(fit) > size)
		return -EFBIG;

	conf = fit_conf_get_node(fit, NULL);
	if (conf < 0)
		return conf;
	ret = fit_config_verify(fit, conf);
	if (ret)
		return -EACCES;

	type = fdt_getprop(fit, conf, "fw,type", NULL);
	product = fdt_getprop(fit, conf, "fw,product", NULL);
	layout_id = fdt_getprop(fit, conf, "fw,layout-id", NULL);
	if (!type || !product || !layout_id)
		return -EINVAL;
	if (strcmp(product, layout->product) ||
	    strcmp(layout_id, layout->layout_id))
		return -EPERM;

	if (!strcmp(type, "bootloader"))
		package->component = FW_COMPONENT_BOOTLOADER;
	else if (!strcmp(type, "kernel"))
		package->component = FW_COMPONENT_KERNEL;
	else if (!strcmp(type, "rootfs"))
		package->component = FW_COMPONENT_ROOTFS;
	else
		return -EOPNOTSUPP;

	ret = fw_fit_u32(fit, conf, "fw,version", &package->version);
	if (ret)
		return ret;
	ret = fw_fit_u32(fit, conf, "fw,rollback-index",
			 &package->rollback_index);
	if (ret)
		return ret;

	if (package->component == FW_COMPONENT_KERNEL) {
		ret = fw_verify_prop(fit, conf, FIT_KERNEL_PROP);
		if (!ret)
			ret = fw_verify_prop(fit, conf, FIT_FDT_PROP);
	} else if (package->component == FW_COMPONENT_ROOTFS) {
		ret = fw_verify_prop(fit, conf, FIT_FIRMWARE_PROP);
	} else {
		ret = fw_verify_prop(fit, conf, FIT_FIRMWARE_PROP);
		if (!ret)
			ret = fw_verify_prop(fit, conf, FIT_LOADABLE_PROP);
	}
	if (ret)
		return ret;

	package->conf_noffset = conf;
	return 0;
}

static int fw_component_from_name(const char *name,
				  enum fw_component *component)
{
	int i;

	for (i = 0; i < FW_COMPONENT_COUNT; i++) {
		if (!strcmp(name, fw_component_name(i))) {
			*component = i;
			return 0;
		}
	}

	return -EINVAL;
}

static int fw_copy_string_prop(const void *fit, int node, const char *name,
			       char *dest, size_t dest_size)
{
	const char *value;
	int len;

	value = fdt_getprop(fit, node, name, &len);
	if (!value)
		return len;
	if (len < 2 || value[len - 1] || strnlen(value, len) != len - 1)
		return -EINVAL;
	if (len > dest_size)
		return -ENAMETOOLONG;
	memcpy(dest, value, len);

	return 0;
}

static int fw_verify_release(const struct fw_layout *layout, const void *fit,
			     size_t size, struct fw_release *release)
{
	const char *type, *product, *layout_id, *name, *sha256;
	unsigned int seen = 0;
	char prop[48];
	int conf, count, i, len, ret;

	ret = fit_check_format(fit, size);
	if (ret)
		return ret;
	if (fit_get_size(fit) > size)
		return -EFBIG;

	conf = fit_conf_get_node(fit, NULL);
	if (conf < 0)
		return conf;
	ret = fit_config_verify(fit, conf);
	if (ret)
		return -EACCES;

	type = fdt_getprop(fit, conf, "fw,type", NULL);
	product = fdt_getprop(fit, conf, "fw,product", NULL);
	layout_id = fdt_getprop(fit, conf, "fw,layout-id", NULL);
	if (!type || strcmp(type, "release") || !product || !layout_id)
		return -EINVAL;
	if (strcmp(product, layout->product) ||
	    strcmp(layout_id, layout->layout_id))
		return -EPERM;

	ret = fw_fit_u32(fit, conf, "fw,version", &release->version);
	if (ret)
		return ret;
	ret = fw_fit_u32(fit, conf, "fw,rollback-index",
			 &release->rollback_index);
	if (ret)
		return ret;
	ret = fw_verify_prop(fit, conf, FIT_FIRMWARE_PROP);
	if (ret)
		return ret;

	memset(release->component, 0, sizeof(release->component));
	release->allow_bootloader =
		!!fdt_getprop(fit, conf, "fw,allow-bootloader", NULL);
	count = fdt_stringlist_count(fit, conf, "fw,components");
	if (count <= 0 || count > FW_RELEASE_MAX_COMPONENTS)
		return count < 0 ? count : -E2BIG;

	for (i = 0; i < count; i++) {
		struct fw_release_component *item = &release->component[i];

		name = fdt_stringlist_get(fit, conf, "fw,components", i, &len);
		if (!name)
			return len;
		ret = fw_component_from_name(name, &item->component);
		if (ret)
			return ret;
		if (seen & (1U << item->component))
			return -EEXIST;
		seen |= 1U << item->component;

		snprintf(prop, sizeof(prop), "fw,%s-file", name);
		ret = fw_copy_string_prop(fit, conf, prop, item->file,
					  sizeof(item->file));
		if (ret)
			return ret;
		snprintf(prop, sizeof(prop), "fw,%s-sha256", name);
		sha256 = fdt_getprop(fit, conf, prop, &len);
		if (!sha256)
			return len;
		if (len != FW_SHA256_SIZE * 2 + 1 ||
		    sha256[FW_SHA256_SIZE * 2] ||
		    hex2bin(item->sha256, sha256, FW_SHA256_SIZE))
			return -EINVAL;
	}
	release->count = count;

	return 0;
}

static const struct fw_region *
fw_component_region(const struct fw_layout *layout, enum fw_component component,
		    u8 slot)
{
	enum fw_region_id id;

	if (slot > 1)
		return NULL;
	if (component == FW_COMPONENT_KERNEL)
		id = FW_REGION_KERNEL_A + slot;
	else if (component == FW_COMPONENT_ROOTFS)
		id = FW_REGION_ROOTFS_A + slot;
	else
		return NULL;

	return &layout->region[id];
}

static int fw_fit_data(const void *fit, int conf, const char *prop, int index,
		       const void **data, size_t *size, const char **name)
{
	int node;

	node = fit_conf_get_prop_node_index(fit, conf, prop, index);
	if (node < 0)
		return node;
	if (fit_image_get_data(fit, node, data, size))
		return -ENOENT;
	if (name)
		*name = fit_get_name(fit, node, NULL);

	return 0;
}

static int fw_fit_named_data(const void *fit, int conf, const char *target,
			     const void **data, size_t *size)
{
	static const char *const props[] = {
		FIT_FIRMWARE_PROP,
		FIT_LOADABLE_PROP,
	};
	const char *name;
	int count, i, j, ret;

	for (i = 0; i < ARRAY_SIZE(props); i++) {
		count = fit_conf_get_prop_node_count(fit, conf, props[i]);
		if (count < 0)
			continue;
		for (j = 0; j < count; j++) {
			ret = fw_fit_data(fit, conf, props[i], j, data, size,
					  &name);
			if (ret)
				return ret;
			if (!strcmp(name, target))
				return 0;
		}
	}

	return -ENOENT;
}

static int fw_write_bootloader(const struct fw_layout *layout,
			       const struct fw_storage *storage,
			       const void *fit, int conf, u8 slot)
{
	static const struct {
		const char *name;
		enum fw_region_id region_a;
	} images[] = {
		/* Write the later boot stages first and tiboot3 last. */
		{ "uboot",   FW_REGION_UBOOT_A },
		{ "tispl",   FW_REGION_TISPL_A },
		{ "tiboot3", FW_REGION_TIBOOT3_A },
	};
	const void *data;
	size_t size;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(images); i++) {
		ret = fw_fit_named_data(fit, conf, images[i].name, &data, &size);
		if (ret)
			return ret;
		ret = fw_write_region(storage,
				      &layout->region[images[i].region_a + slot],
				      data, size);
		if (ret)
			return ret;
	}

	return 0;
}

static int fw_write_package(const struct fw_layout *layout,
			    const struct fw_storage *storage,
			    struct fw_metadata *metadata,
			    const struct fw_package *package, const void *fit,
			    size_t size, int release_deployment,
			    u8 *deployment_out)
{
	struct fw_component_slot *current;
	const struct fw_region *region;
	const void *data = fit;
	size_t data_size = size;
	u8 source, target, deployment;
	int ret;

	if (package->component == FW_COMPONENT_BOOTLOADER &&
	    !layout->allow_bootloader_update)
		return -EPERM;
	if (package->component == FW_COMPONENT_KERNEL)
		data_size = fit_get_size(fit);

	if (release_deployment >= 0) {
		deployment = release_deployment;
		current = &metadata->deployment[deployment]
				   .component[package->component];
	} else {
		source = metadata->active_deployment;
		if (!metadata->deployment[source].successful)
			source = metadata->last_good_deployment;
		current = &metadata->deployment[source]
				   .component[package->component];
	}
	target = 1 - current->slot;
	if (metadata->deployment[metadata->last_good_deployment]
			    .component[package->component]
			    .slot == target &&
	    release_deployment < 0 && metadata->last_good_deployment != source)
		return -ENOSPC;
	if (package->rollback_index < current->rollback_index)
		return -EPERM;

	if (package->component == FW_COMPONENT_ROOTFS) {
		ret = fw_fit_data(fit, package->conf_noffset, FIT_FIRMWARE_PROP,
				  0, &data, &data_size, NULL);
		if (ret)
			return ret;
	}
	if (package->component == FW_COMPONENT_BOOTLOADER) {
		ret = fw_write_bootloader(layout, storage, fit,
					  package->conf_noffset, target);
	} else {
		region = fw_component_region(layout, package->component, target);
		ret = fw_write_region(storage, region, data, data_size);
	}
	if (ret)
		return ret;

	if (release_deployment >= 0) {
		ret = fw_metadata_stage_component(metadata, deployment,
						  package->component, target,
						  package->version,
						  package->rollback_index);
	} else {
		ret = fw_metadata_prepare_update(metadata, package->component,
						 target, package->version,
						 package->rollback_index,
						 layout->attempts, &deployment);
	}
	if (ret)
		return ret;
	if (deployment_out)
		*deployment_out = deployment;

	return 0;
}

static int fw_update_fit(const struct fw_layout *layout,
			 const struct fw_storage *storage,
			 const struct fw_metadata_store *store, const void *fit,
			 size_t size, int expected_component)
{
	struct fw_package package;
	struct fw_metadata metadata;
	u8 deployment;
	int ret;

	ret = fw_verify_package(layout, fit, size, &package);
	if (ret)
		return ret;
	if (expected_component >= 0 && package.component != expected_component)
		return -EINVAL;
	ret = fw_metadata_load(store, &metadata);
	if (ret)
		return ret;
	ret = fw_write_package(layout, storage, &metadata, &package, fit, size,
			       -1, &deployment);
	if (ret)
		return ret;
	ret = fw_metadata_save(store, &metadata);
	if (ret)
		return ret;

	printf("Updated %s slot %c; deployment %u is pending (%u tries)\n",
	       fw_component_name(package.component),
	       'a' + metadata.deployment[deployment]
			     .component[package.component].slot,
	       deployment,
	       layout->attempts);
	return 0;
}

static void fw_print_metadata(const struct fw_metadata *metadata)
{
	int i, j;

	printf("sequence: %u\n", metadata->sequence);
	printf("active: %u  pending: ", metadata->active_deployment);
	if (metadata->pending_deployment == FW_SLOT_NONE)
		printf("none");
	else
		printf("%u", metadata->pending_deployment);
	printf("  last-good: %u  selected: %u\n",
	       metadata->last_good_deployment, metadata->selected_deployment);

	for (i = 0; i < 2; i++) {
		const struct fw_deployment *deployment =
			&metadata->deployment[i];

		printf("deployment %d: %s successful=%u tries=%u\n", i,
		       fw_state_name(deployment->state), deployment->successful,
		       deployment->tries_remaining);
		for (j = 0; j < FW_COMPONENT_COUNT; j++) {
			const struct fw_component_slot *component =
				&deployment->component[j];

			printf("  %-10s slot=%c version=%u rollback=%u valid=%u\n",
			       fw_component_name(j), 'a' + component->slot,
			       component->version, component->rollback_index,
			       component->valid);
		}
	}
}

static int fw_export_selection(const struct fw_layout *layout,
			       const struct fw_metadata *metadata, u8 selected)
{
	const struct fw_deployment *deployment =
		&metadata->deployment[selected];
	char value[24];
	int i;

	snprintf(value, sizeof(value), "%u", selected);
	env_set("fw_deployment", value);
	env_set("fw_storage",
		layout->type == FW_STORAGE_MMC ? "mmc" : "spi-nor");

	for (i = 0; i < FW_COMPONENT_COUNT; i++) {
		const struct fw_component_slot *component =
			&deployment->component[i];
		const struct fw_region *region;

		snprintf(value, sizeof(value), "%c", 'a' + component->slot);
		if (i == FW_COMPONENT_BOOTLOADER)
			env_set("fw_bootloader_slot", value);
		else if (i == FW_COMPONENT_KERNEL)
			env_set("fw_kernel_slot", value);
		else
			env_set("fw_rootfs_slot", value);

		region = fw_component_region(layout, i, component->slot);
		if (!region)
			continue;
		snprintf(value, sizeof(value), "%llx",
			 (unsigned long long)region->offset);
		env_set(i == FW_COMPONENT_KERNEL ? "fw_kernel_offset" :
						   "fw_rootfs_offset",
			value);
		snprintf(value, sizeof(value), "%llx",
			 (unsigned long long)region->size);
		env_set(i == FW_COMPONENT_KERNEL ? "fw_kernel_size" :
						   "fw_rootfs_size",
			value);
	}

	return 0;
}

static bool fw_file_name_valid(const char *file)
{
	const char *p;

	if (!file || !*file || strstr(file, ".."))
		return false;
	for (p = file; *p; p++) {
		if (!isalnum(*p) && !strchr("._/-", *p))
			return false;
	}

	return true;
}

static int fw_tftp_image(const char *file, const void **fit, size_t *size)
{
	ulong addr;

	if (!fw_file_name_valid(file))
		return -EINVAL;
	addr = env_get_hex("loadaddr", image_load_addr);
	if (run_commandf("tftpboot %lx %s", addr, file))
		return -EIO;
	*size = env_get_hex("filesize", 0);
	if (!*size)
		return -EINVAL;
	*fit = map_sysmem(addr, *size);

	return 0;
}

static int fw_get_image(int argc, char *const argv[], const void **fit,
			size_t *size)
{
	ulong addr;

	if (argc < 1)
		return -EINVAL;
	if (!strcmp(argv[0], "tftp")) {
		const char *file;

		if (argc != 2)
			return -EINVAL;
		file = argv[1];
		return fw_tftp_image(file, fit, size);
	} else if (!strcmp(argv[0], "addr")) {
		if (argc != 3)
			return -EINVAL;
		addr = hextoul(argv[1], NULL);
		*size = hextoul(argv[2], NULL);
	} else {
		return -EINVAL;
	}
	if (!*size)
		return -EINVAL;

	*fit = map_sysmem(addr, *size);
	return 0;
}

static int fw_metadata_store_is_blank(const struct fw_metadata_store *store)
{
	u8 buf[FW_METADATA_SIZE];
	int copy, i, ret;

	for (copy = 0; copy < FW_METADATA_COPIES; copy++) {
		ret = store->read(store->ctx, copy, buf, sizeof(buf));
		if (ret)
			return ret;
		for (i = 0; i < sizeof(buf); i++) {
			if (buf[i] != 0x00 && buf[i] != 0xff)
				return -EBADMSG;
		}
	}

	return 0;
}

static int fw_update_release(const struct fw_layout *layout,
			     const struct fw_storage *storage,
			     const struct fw_metadata_store *store,
			     const void *fit, size_t size)
{
	struct fw_release_component *item;
	struct fw_package package;
	struct fw_metadata metadata;
	struct fw_release release;
	const void *component_fit;
	u8 digest[FW_SHA256_SIZE];
	size_t component_size;
	u8 deployment;
	bool replacing;
	int digest_size, i, ret;

	memset(&release, 0, sizeof(release));
	ret = fw_verify_release(layout, fit, size, &release);
	if (ret)
		return ret;
	ret = fw_metadata_load(store, &metadata);
	if (ret)
		return ret;
	replacing = metadata.pending_deployment != FW_SLOT_NONE;
	ret = fw_metadata_begin_release(&metadata, release.version,
					&deployment);
	if (ret)
		return ret;
	ret = fw_metadata_save(store, &metadata);
	if (ret)
		return ret;
	if (replacing)
		printf("Replacing pending deployment %u with release %u\n",
		       deployment, release.version);

	for (i = 0; i < release.count; i++) {
		item = &release.component[i];
		if (item->component == FW_COMPONENT_BOOTLOADER &&
		    !release.allow_bootloader)
			return -EPERM;

		ret = fw_tftp_image(item->file, &component_fit, &component_size);
		if (ret)
			return ret;
		if (component_size > UINT_MAX) {
			ret = -EFBIG;
			goto unmap_component;
		}
		digest_size = sizeof(digest);
		ret = hash_block("sha256", component_fit, component_size, digest,
				 &digest_size);
		if (ret)
			goto unmap_component;
		if (digest_size != FW_SHA256_SIZE ||
		    memcmp(digest, item->sha256, FW_SHA256_SIZE)) {
			ret = -EBADMSG;
			goto unmap_component;
		}

		ret = fw_verify_package(layout, component_fit, component_size,
					&package);
		if (ret)
			goto unmap_component;
		if (package.component != item->component ||
		    package.rollback_index < release.rollback_index) {
			ret = -EPERM;
			goto unmap_component;
		}
		ret = fw_write_package(layout, storage, &metadata, &package,
				       component_fit, component_size, deployment,
				       NULL);

unmap_component:
		unmap_sysmem(component_fit);
		if (ret)
			return ret;
	}

	ret = fw_metadata_commit_release(&metadata, deployment,
					 layout->attempts);
	if (ret)
		return ret;
	ret = fw_metadata_save(store, &metadata);
	if (ret)
		return ret;

	printf("Updated release %u; deployment %u is pending (%u tries)\n",
	       release.version, deployment, layout->attempts);
	return 0;
}

static int do_fw(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct fw_metadata_store store;
	struct fw_store_ctx store_ctx;
	struct fw_metadata metadata;
	struct fw_storage storage;
	struct fw_layout layout;
	const void *fit;
	size_t fit_size;
	u8 selected, slot;
	int deployment, ret;

	if (argc < 2)
		return CMD_RET_USAGE;

	ret = fw_open(&layout, &storage, &store, &store_ctx);
	if (ret) {
		printf("fw: invalid or unavailable layout (%d)\n", ret);
		return CMD_RET_FAILURE;
	}

	if (!strcmp(argv[1], "list")) {
		printf("product: %s  layout: %s  storage: %s\n", layout.product,
		       layout.layout_id,
		       layout.type == FW_STORAGE_MMC ? "mmc" : "spi-nor");
		for (int i = 0; i < FW_REGION_COUNT; i++)
			printf("%-12s offset=%#llx size=%#llx\n",
			       layout.region[i].name,
			       (unsigned long long)layout.region[i].offset,
			       (unsigned long long)layout.region[i].size);
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "init")) {
		if (argc != 3 || (strcmp(argv[2], "a") && strcmp(argv[2], "b")))
			return CMD_RET_USAGE;
		slot = argv[2][0] - 'a';
		fw_metadata_init(&metadata, slot);
		ret = fw_metadata_save(&store, &metadata);
		if (!ret)
			ret = fw_metadata_save(&store, &metadata);
		if (!ret)
			printf("Initialized metadata with slot %c active\n",
			       'a' + slot);
		goto out;
	}

	ret = fw_metadata_load(&store, &metadata);
	if (ret == -ENODATA && !strcmp(argv[1], "select") &&
	    layout.auto_init && !fw_metadata_store_is_blank(&store)) {
		fw_metadata_init(&metadata, layout.default_slot);
		ret = fw_metadata_save(&store, &metadata);
		if (!ret)
			ret = fw_metadata_save(&store, &metadata);
		if (!ret)
			printf("Initialized blank metadata with slot %c active\n",
			       'a' + layout.default_slot);
	}
	if (ret) {
		printf("fw: metadata unavailable (%d); run 'fw init a|b'\n",
		       ret);
		return CMD_RET_FAILURE;
	}

	if (!strcmp(argv[1], "status")) {
		fw_print_metadata(&metadata);
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "select")) {
		if (layout.spl_selects)
			ret = fw_metadata_get_selected(&metadata, &selected);
		else
			ret = fw_metadata_select(&metadata, &selected);
		if (ret < 0)
			goto out;
		if (!layout.spl_selects && ret > 0) {
			ret = fw_metadata_save(&store, &metadata);
			if (ret)
				goto out;
		}
		fw_export_selection(&layout, &metadata, selected);
		printf("Selected deployment %u: boot=%c kernel=%c rootfs=%c\n",
		       selected,
		       'a' + metadata.deployment[selected]
				       .component[FW_COMPONENT_BOOTLOADER]
				       .slot,
		       'a' + metadata.deployment[selected]
				       .component[FW_COMPONENT_KERNEL]
				       .slot,
		       'a' + metadata.deployment[selected]
				       .component[FW_COMPONENT_ROOTFS]
				       .slot);
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "mark-good")) {
		deployment = argc == 3 ? dectoul(argv[2], NULL) : -1;
		ret = fw_metadata_mark_good(&metadata, deployment);
		if (!ret)
			ret = fw_metadata_save(&store, &metadata);
		goto out;
	}

	if (!strcmp(argv[1], "mark-bad")) {
		if (argc != 3)
			return CMD_RET_USAGE;
		deployment = dectoul(argv[2], NULL);
		ret = fw_metadata_mark_bad(&metadata, deployment);
		if (!ret)
			ret = fw_metadata_save(&store, &metadata);
		goto out;
	}

	if (!strcmp(argv[1], "rollback")) {
		ret = fw_metadata_rollback(&metadata);
		if (!ret)
			ret = fw_metadata_save(&store, &metadata);
		goto out;
	}

	if (!strcmp(argv[1], "boot")) {
		if (argc < 3 || argc > 4)
			return CMD_RET_USAGE;
		deployment = dectoul(argv[2], NULL);
		if (deployment > 1 ||
		    metadata.deployment[deployment].state == FW_STATE_EMPTY ||
		    metadata.deployment[deployment].state == FW_STATE_FAILED)
			return CMD_RET_FAILURE;
		if (argc == 4 && !strcmp(argv[3], "once")) {
			metadata.boot_once_deployment = deployment;
		} else {
			metadata.pending_deployment = deployment;
			if (!metadata.deployment[deployment].tries_remaining)
				metadata.deployment[deployment].tries_remaining =
					layout.attempts;
		}
		ret = fw_metadata_save(&store, &metadata);
		goto out;
	}

	if (!strcmp(argv[1], "update") && argc == 5 &&
	    !strcmp(argv[4], "all")) {
		if (strcmp(argv[2], "tftp"))
			return CMD_RET_USAGE;
		ret = fw_get_image(2, argv + 2, &fit, &fit_size);
		if (ret)
			goto out;
		ret = fw_update_release(&layout, &storage, &store, fit, fit_size);
		unmap_sysmem(fit);
		goto out;
	}

	if (!strcmp(argv[1], "verify") || !strcmp(argv[1], "update") ||
	    !strcmp(argv[1], "restore")) {
		enum fw_component expected;
		int image_arg = 2;
		int expected_component = -1;

		if (!strcmp(argv[1], "restore")) {
			if (argc < 4 || fw_component_from_name(argv[2], &expected))
				return CMD_RET_USAGE;
			expected_component = expected;
			image_arg = 3;
		}
		ret = fw_get_image(argc - image_arg, argv + image_arg, &fit,
				   &fit_size);
		if (ret)
			goto out;
		if (!strcmp(argv[1], "verify")) {
			struct fw_package package;
			struct fw_release release;

			ret = fw_verify_package(&layout, fit, fit_size,
						&package);
			if (!ret) {
				printf("Valid %s package, version %u, rollback %u\n",
				       fw_component_name(package.component),
				       package.version, package.rollback_index);
			} else if (ret == -EOPNOTSUPP) {
				memset(&release, 0, sizeof(release));
				ret = fw_verify_release(&layout, fit, fit_size, &release);
				if (!ret)
					printf("Valid release package, version %u, rollback %u\n",
					       release.version,
					       release.rollback_index);
			}
		} else {
			ret = fw_update_fit(&layout, &storage, &store, fit,
					    fit_size, expected_component);
		}
		unmap_sysmem(fit);
		goto out;
	}

	return CMD_RET_USAGE;

out:
	if (ret)
		printf("fw: operation failed (%d)\n", ret);
	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_LONGHELP(fw, "list\n"
		    "fw status\n"
		    "fw init <a|b>\n"
		    "fw verify <tftp file|addr address size>\n"
		    "fw update <tftp file|addr address size>\n"
		    "fw update tftp <release-file> all\n"
		    "fw restore <component> <tftp file|addr address size>\n"
		    "fw select\n"
		    "fw boot <deployment> [once]\n"
		    "fw mark-good [deployment]\n"
		    "fw mark-bad <deployment>\n"
		    "fw rollback");

U_BOOT_CMD(fw, 7, 1, do_fw, "RAW A/B firmware manager", fw_help_text);
