// SPDX-License-Identifier: GPL-2.0+
#include <blk.h>
#include <command.h>
#include <dm.h>
#include <dm/ofnode.h>
#include <env.h>
#include <firmware_upgrade.h>
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

#define FU_COMPATIBLE "u-boot,firmware-upgrade"
#define FU_VERIFY_CHUNK (64 * 1024)
#define FU_SHA256_SIZE 32
#define FU_RELEASE_PATH_SIZE 128
#define FU_RELEASE_MAX_COMPONENTS FU_COMPONENT_COUNT

enum fu_storage_type {
	FU_STORAGE_MMC,
	FU_STORAGE_SPI_NOR,
};

enum fu_region_id {
	FU_REGION_META0,
	FU_REGION_META1,
	FU_REGION_TIBOOT3_A,
	FU_REGION_TIBOOT3_B,
	FU_REGION_TISPL_A,
	FU_REGION_TISPL_B,
	FU_REGION_UBOOT_A,
	FU_REGION_UBOOT_B,
	FU_REGION_KERNEL_A,
	FU_REGION_KERNEL_B,
	FU_REGION_ROOTFS_A,
	FU_REGION_ROOTFS_B,
	FU_REGION_COUNT,
};

struct fu_region {
	const char *name;
	u64 offset;
	u64 size;
};

struct fu_layout {
	enum fu_storage_type type;
	u32 device;
	u32 bus;
	u32 cs;
	u8 attempts;
	u8 default_slot;
	bool auto_init;
	bool allow_bootloader_update;
	const char *product;
	const char *layout_id;
	struct fu_region region[FU_REGION_COUNT];
};

struct fu_storage {
	enum fu_storage_type type;
	struct blk_desc *blk;
	struct udevice *sf;
	u64 capacity;
	u32 write_size;
	u32 erase_size;
};

struct fu_package {
	enum fu_component component;
	int conf_noffset;
	u32 version;
	u32 rollback_index;
};

struct fu_release_component {
	enum fu_component component;
	char file[FU_RELEASE_PATH_SIZE];
	u8 sha256[FU_SHA256_SIZE];
};

struct fu_release {
	u32 version;
	u32 rollback_index;
	bool allow_bootloader;
	int count;
	struct fu_release_component component[FU_RELEASE_MAX_COMPONENTS];
};

static const char *const fu_region_names[FU_REGION_COUNT] = {
	"meta0",   "meta1",   "tiboot3_a", "tiboot3_b", "tispl_a",  "tispl_b",
	"uboot_a", "uboot_b", "kernel_a",  "kernel_b",	"rootfs_a", "rootfs_b",
};

static int fu_read_layout(struct fu_layout *layout)
{
	const char *storage;
	ofnode node, child;
	u32 attempts;
	int i;

	node = ofnode_by_compatible(ofnode_null(), FU_COMPATIBLE);
	if (!ofnode_valid(node))
		return -ENOENT;
	if (!ofnode_is_enabled(node))
		return -ENODEV;

	memset(layout, 0, sizeof(*layout));
	storage = ofnode_read_string(node, "storage");
	if (!storage)
		return -EINVAL;
	if (!strcmp(storage, "mmc")) {
		layout->type = FU_STORAGE_MMC;
		layout->device = ofnode_read_u32_default(node, "device", 0);
	} else if (!strcmp(storage, "spi-nor")) {
		layout->type = FU_STORAGE_SPI_NOR;
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
					   FU_DEFAULT_ATTEMPTS);
	if (!attempts || attempts > U8_MAX)
		return -EINVAL;
	layout->attempts = attempts;
	layout->default_slot = ofnode_read_u32_default(node, "default-slot", 0);
	if (layout->default_slot > 1)
		return -EINVAL;
	layout->auto_init = ofnode_read_bool(node, "auto-init");
	layout->allow_bootloader_update =
		ofnode_read_bool(node, "allow-bootloader-update");

	for (i = 0; i < FU_REGION_COUNT; i++) {
		child = ofnode_find_subnode(node, fu_region_names[i]);
		if (!ofnode_valid(child))
			return -ENOENT;
		layout->region[i].name = fu_region_names[i];
		if (ofnode_read_u64(child, "offset",
				    &layout->region[i].offset) ||
		    ofnode_read_u64(child, "size", &layout->region[i].size) ||
		    !layout->region[i].size)
			return -EINVAL;
	}

	return 0;
}

static int fu_storage_open(const struct fu_layout *layout,
			   struct fu_storage *storage)
{
	char dev[12];
	struct spi_nor *nor;
	int ret;

	memset(storage, 0, sizeof(*storage));
	storage->type = layout->type;
	if (layout->type == FU_STORAGE_MMC) {
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

static int fu_layout_validate(const struct fu_layout *layout,
			      const struct fu_storage *storage)
{
	const struct fu_region *a, *b;
	int i, j;

	for (i = 0; i < FU_REGION_COUNT; i++) {
		a = &layout->region[i];
		if (a->offset > storage->capacity ||
		    a->size > storage->capacity - a->offset)
			return -EFBIG;
		if (a->offset % storage->write_size)
			return -EINVAL;
		if (storage->type == FU_STORAGE_SPI_NOR &&
		    (a->offset > UINT_MAX ||
		     a->size > UINT_MAX - a->offset + 1ULL))
			return -EFBIG;
		if (storage->type == FU_STORAGE_SPI_NOR &&
		    (a->offset % storage->erase_size ||
		     a->size % storage->erase_size))
			return -EINVAL;

		for (j = i + 1; j < FU_REGION_COUNT; j++) {
			b = &layout->region[j];
			if (a->offset < b->offset + b->size &&
			    b->offset < a->offset + a->size)
				return -EINVAL;
		}
	}

	if (layout->region[FU_REGION_META0].size < FU_METADATA_SIZE ||
	    layout->region[FU_REGION_META1].size < FU_METADATA_SIZE)
		return -ENOSPC;

	return 0;
}

static int fu_storage_read(const struct fu_storage *storage, u64 offset,
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

	if (storage->type == FU_STORAGE_SPI_NOR) {
		if (offset > UINT_MAX || len > UINT_MAX - offset + 1ULL)
			return -EFBIG;
		return spi_flash_read_dm(storage->sf, offset, len, buf);
	}

	block_size = storage->write_size;
	if (offset % block_size)
		return -EINVAL;
	blocks = len / block_size;
	tail = len % block_size;
	chunk_blocks = max_t(size_t, 1, FU_VERIFY_CHUNK / block_size);
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

static int fu_storage_write(const struct fu_storage *storage, u64 offset,
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

	if (storage->type == FU_STORAGE_SPI_NOR) {
		if (offset > UINT_MAX || len > UINT_MAX - offset + 1ULL)
			return -EFBIG;
		return spi_flash_write_dm(storage->sf, offset, len, buf);
	}

	block_size = storage->write_size;
	if (offset % block_size)
		return -EINVAL;
	blocks = len / block_size;
	tail = len % block_size;
	chunk_blocks = max_t(size_t, 1, FU_VERIFY_CHUNK / block_size);
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

static int fu_write_region(const struct fu_storage *storage,
			   const struct fu_region *region, const void *data,
			   size_t size)
{
	u8 *verify;
	size_t offset, chunk;
	int ret;

	if (size > region->size)
		return -EFBIG;

	if (storage->type == FU_STORAGE_SPI_NOR) {
		ret = spi_flash_erase_dm(storage->sf, region->offset,
					 region->size);
		if (ret)
			return ret;
	}

	ret = fu_storage_write(storage, region->offset, data, size);
	if (ret)
		return ret;

	verify = memalign(ARCH_DMA_MINALIGN, FU_VERIFY_CHUNK);
	if (!verify)
		return -ENOMEM;
	for (offset = 0; offset < size; offset += chunk) {
		chunk = min_t(size_t, FU_VERIFY_CHUNK, size - offset);
		ret = fu_storage_read(storage, region->offset + offset, verify,
				      chunk);
		if (ret || memcmp(verify, (const u8 *)data + offset, chunk)) {
			ret = ret ?: -EIO;
			break;
		}
	}
	free(verify);

	return ret;
}

struct fu_store_ctx {
	const struct fu_layout *layout;
	const struct fu_storage *storage;
};

static int fu_store_read(void *ctx, unsigned int copy, void *buf, size_t len)
{
	struct fu_store_ctx *store = ctx;
	const struct fu_region *region;

	if (copy >= FU_METADATA_COPIES)
		return -EINVAL;
	region = &store->layout->region[FU_REGION_META0 + copy];

	return fu_storage_read(store->storage, region->offset, buf, len);
}

static int fu_store_write(void *ctx, unsigned int copy, const void *buf,
			  size_t len)
{
	struct fu_store_ctx *store = ctx;
	const struct fu_region *region;
	int ret;

	if (copy >= FU_METADATA_COPIES)
		return -EINVAL;
	region = &store->layout->region[FU_REGION_META0 + copy];
	if (store->storage->type == FU_STORAGE_SPI_NOR) {
		ret = spi_flash_erase_dm(store->storage->sf, region->offset,
					 region->size);
		if (ret)
			return ret;
	}

	return fu_storage_write(store->storage, region->offset, buf, len);
}

static int fu_open(struct fu_layout *layout, struct fu_storage *storage,
		   struct fu_metadata_store *store, struct fu_store_ctx *ctx)
{
	int ret;

	ret = fu_read_layout(layout);
	if (ret)
		return ret;
	ret = fu_storage_open(layout, storage);
	if (ret)
		return ret;
	ret = fu_layout_validate(layout, storage);
	if (ret)
		return ret;

	ctx->layout = layout;
	ctx->storage = storage;
	store->read = fu_store_read;
	store->write = fu_store_write;
	store->ctx = ctx;

	return 0;
}

static int fu_fit_u32(const void *fit, int node, const char *name, u32 *value)
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

static int fu_verify_prop(const void *fit, int conf, const char *prop)
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

static int fu_verify_package(const struct fu_layout *layout, const void *fit,
			     size_t size, struct fu_package *package)
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

	type = fdt_getprop(fit, conf, "fu,type", NULL);
	product = fdt_getprop(fit, conf, "fu,product", NULL);
	layout_id = fdt_getprop(fit, conf, "fu,layout-id", NULL);
	if (!type || !product || !layout_id)
		return -EINVAL;
	if (strcmp(product, layout->product) ||
	    strcmp(layout_id, layout->layout_id))
		return -EPERM;

	if (!strcmp(type, "bootloader"))
		package->component = FU_COMPONENT_BOOTLOADER;
	else if (!strcmp(type, "kernel"))
		package->component = FU_COMPONENT_KERNEL;
	else if (!strcmp(type, "rootfs"))
		package->component = FU_COMPONENT_ROOTFS;
	else
		return -EOPNOTSUPP;

	ret = fu_fit_u32(fit, conf, "fu,version", &package->version);
	if (ret)
		return ret;
	ret = fu_fit_u32(fit, conf, "fu,rollback-index",
			 &package->rollback_index);
	if (ret)
		return ret;

	if (package->component == FU_COMPONENT_KERNEL) {
		ret = fu_verify_prop(fit, conf, FIT_KERNEL_PROP);
		if (!ret)
			ret = fu_verify_prop(fit, conf, FIT_FDT_PROP);
	} else if (package->component == FU_COMPONENT_ROOTFS) {
		ret = fu_verify_prop(fit, conf, FIT_FIRMWARE_PROP);
	} else {
		ret = fu_verify_prop(fit, conf, FIT_FIRMWARE_PROP);
		if (!ret)
			ret = fu_verify_prop(fit, conf, FIT_LOADABLE_PROP);
	}
	if (ret)
		return ret;

	package->conf_noffset = conf;
	return 0;
}

static int fu_component_from_name(const char *name,
				  enum fu_component *component)
{
	int i;

	for (i = 0; i < FU_COMPONENT_COUNT; i++) {
		if (!strcmp(name, fu_component_name(i))) {
			*component = i;
			return 0;
		}
	}

	return -EINVAL;
}

static int fu_copy_string_prop(const void *fit, int node, const char *name,
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

static int fu_verify_release(const struct fu_layout *layout, const void *fit,
			     size_t size, struct fu_release *release)
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

	type = fdt_getprop(fit, conf, "fu,type", NULL);
	product = fdt_getprop(fit, conf, "fu,product", NULL);
	layout_id = fdt_getprop(fit, conf, "fu,layout-id", NULL);
	if (!type || strcmp(type, "release") || !product || !layout_id)
		return -EINVAL;
	if (strcmp(product, layout->product) ||
	    strcmp(layout_id, layout->layout_id))
		return -EPERM;

	ret = fu_fit_u32(fit, conf, "fu,version", &release->version);
	if (ret)
		return ret;
	ret = fu_fit_u32(fit, conf, "fu,rollback-index",
			 &release->rollback_index);
	if (ret)
		return ret;
	ret = fu_verify_prop(fit, conf, FIT_FIRMWARE_PROP);
	if (ret)
		return ret;

	memset(release->component, 0, sizeof(release->component));
	release->allow_bootloader =
		!!fdt_getprop(fit, conf, "fu,allow-bootloader", NULL);
	count = fdt_stringlist_count(fit, conf, "fu,components");
	if (count <= 0 || count > FU_RELEASE_MAX_COMPONENTS)
		return count < 0 ? count : -E2BIG;

	for (i = 0; i < count; i++) {
		struct fu_release_component *item = &release->component[i];

		name = fdt_stringlist_get(fit, conf, "fu,components", i, &len);
		if (!name)
			return len;
		ret = fu_component_from_name(name, &item->component);
		if (ret)
			return ret;
		if (seen & (1U << item->component))
			return -EEXIST;
		seen |= 1U << item->component;

		snprintf(prop, sizeof(prop), "fu,%s-file", name);
		ret = fu_copy_string_prop(fit, conf, prop, item->file,
					  sizeof(item->file));
		if (ret)
			return ret;
		snprintf(prop, sizeof(prop), "fu,%s-sha256", name);
		sha256 = fdt_getprop(fit, conf, prop, &len);
		if (!sha256)
			return len;
		if (len != FU_SHA256_SIZE * 2 + 1 ||
		    sha256[FU_SHA256_SIZE * 2] ||
		    hex2bin(item->sha256, sha256, FU_SHA256_SIZE))
			return -EINVAL;
	}
	release->count = count;

	return 0;
}

static const struct fu_region *
fu_component_region(const struct fu_layout *layout, enum fu_component component,
		    u8 slot)
{
	enum fu_region_id id;

	if (slot > 1)
		return NULL;
	if (component == FU_COMPONENT_KERNEL)
		id = FU_REGION_KERNEL_A + slot;
	else if (component == FU_COMPONENT_ROOTFS)
		id = FU_REGION_ROOTFS_A + slot;
	else
		return NULL;

	return &layout->region[id];
}

static int fu_fit_data(const void *fit, int conf, const char *prop, int index,
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

static int fu_fit_named_data(const void *fit, int conf, const char *target,
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
			ret = fu_fit_data(fit, conf, props[i], j, data, size,
					  &name);
			if (ret)
				return ret;
			if (!strcmp(name, target))
				return 0;
		}
	}

	return -ENOENT;
}

static int fu_write_bootloader(const struct fu_layout *layout,
			       const struct fu_storage *storage,
			       const void *fit, int conf, u8 slot)
{
	static const char *const order[] = { "uboot", "tispl", "tiboot3" };
	const void *data;
	size_t size;
	int i, ret;
	enum fu_region_id id;

	for (i = 0; i < ARRAY_SIZE(order); i++) {
		ret = fu_fit_named_data(fit, conf, order[i], &data, &size);
		if (ret)
			return ret;
		if (!strcmp(order[i], "tiboot3"))
			id = FU_REGION_TIBOOT3_A + slot;
		else if (!strcmp(order[i], "tispl"))
			id = FU_REGION_TISPL_A + slot;
		else
			id = FU_REGION_UBOOT_A + slot;
		ret = fu_write_region(storage, &layout->region[id], data, size);
		if (ret)
			return ret;
	}

	return 0;
}

static int fu_write_package(const struct fu_layout *layout,
			    const struct fu_storage *storage,
			    struct fu_metadata *metadata,
			    const struct fu_package *package, const void *fit,
			    size_t size, int release_deployment,
			    u8 *deployment_out)
{
	struct fu_component_slot *current;
	const struct fu_region *region;
	const void *data = fit;
	size_t data_size = size;
	u8 source, target, deployment;
	int ret;

	if (package->component == FU_COMPONENT_BOOTLOADER &&
	    !layout->allow_bootloader_update)
		return -EPERM;
	if (package->component == FU_COMPONENT_KERNEL)
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

	if (package->component == FU_COMPONENT_ROOTFS) {
		ret = fu_fit_data(fit, package->conf_noffset, FIT_FIRMWARE_PROP,
				  0, &data, &data_size, NULL);
		if (ret)
			return ret;
	}
	if (package->component == FU_COMPONENT_BOOTLOADER) {
		ret = fu_write_bootloader(layout, storage, fit,
					  package->conf_noffset, target);
	} else {
		region = fu_component_region(layout, package->component, target);
		ret = fu_write_region(storage, region, data, data_size);
	}
	if (ret)
		return ret;

	if (release_deployment >= 0) {
		ret = fu_metadata_stage_component(metadata, deployment,
						  package->component, target,
						  package->version,
						  package->rollback_index);
	} else {
		ret = fu_metadata_prepare_update(metadata, package->component,
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

static int fu_update_fit(const struct fu_layout *layout,
			 const struct fu_storage *storage,
			 const struct fu_metadata_store *store, const void *fit,
			 size_t size, int expected_component)
{
	struct fu_package package;
	struct fu_metadata metadata;
	u8 deployment;
	int ret;

	ret = fu_verify_package(layout, fit, size, &package);
	if (ret)
		return ret;
	if (expected_component >= 0 && package.component != expected_component)
		return -EINVAL;
	ret = fu_metadata_load(store, &metadata);
	if (ret)
		return ret;
	ret = fu_write_package(layout, storage, &metadata, &package, fit, size,
			       -1, &deployment);
	if (ret)
		return ret;
	ret = fu_metadata_save(store, &metadata);
	if (ret)
		return ret;

	printf("Updated %s slot %c; deployment %u is pending (%u tries)\n",
	       fu_component_name(package.component),
	       'a' + metadata.deployment[deployment]
			     .component[package.component].slot,
	       deployment,
	       layout->attempts);
	return 0;
}

static void fu_print_metadata(const struct fu_metadata *metadata)
{
	int i, j;

	printf("sequence: %u\n", metadata->sequence);
	printf("active: %u  pending: ", metadata->active_deployment);
	if (metadata->pending_deployment == FU_SLOT_NONE)
		printf("none");
	else
		printf("%u", metadata->pending_deployment);
	printf("  last-good: %u\n", metadata->last_good_deployment);

	for (i = 0; i < 2; i++) {
		const struct fu_deployment *deployment =
			&metadata->deployment[i];

		printf("deployment %d: %s successful=%u tries=%u\n", i,
		       fu_state_name(deployment->state), deployment->successful,
		       deployment->tries_remaining);
		for (j = 0; j < FU_COMPONENT_COUNT; j++) {
			const struct fu_component_slot *component =
				&deployment->component[j];

			printf("  %-10s slot=%c version=%u rollback=%u valid=%u\n",
			       fu_component_name(j), 'a' + component->slot,
			       component->version, component->rollback_index,
			       component->valid);
		}
	}
}

static int fu_export_selection(const struct fu_layout *layout,
			       const struct fu_metadata *metadata, u8 selected)
{
	const struct fu_deployment *deployment =
		&metadata->deployment[selected];
	char value[24];
	int i;

	snprintf(value, sizeof(value), "%u", selected);
	env_set("fu_deployment", value);
	env_set("fu_storage",
		layout->type == FU_STORAGE_MMC ? "mmc" : "spi-nor");

	for (i = 0; i < FU_COMPONENT_COUNT; i++) {
		const struct fu_component_slot *component =
			&deployment->component[i];
		const struct fu_region *region;

		snprintf(value, sizeof(value), "%c", 'a' + component->slot);
		if (i == FU_COMPONENT_BOOTLOADER)
			env_set("fu_bootloader_slot", value);
		else if (i == FU_COMPONENT_KERNEL)
			env_set("fu_kernel_slot", value);
		else
			env_set("fu_rootfs_slot", value);

		region = fu_component_region(layout, i, component->slot);
		if (!region)
			continue;
		snprintf(value, sizeof(value), "%llx",
			 (unsigned long long)region->offset);
		env_set(i == FU_COMPONENT_KERNEL ? "fu_kernel_offset" :
						   "fu_rootfs_offset",
			value);
		snprintf(value, sizeof(value), "%llx",
			 (unsigned long long)region->size);
		env_set(i == FU_COMPONENT_KERNEL ? "fu_kernel_size" :
						   "fu_rootfs_size",
			value);
	}

	return 0;
}

static bool fu_file_name_valid(const char *file)
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

static int fu_tftp_image(const char *file, const void **fit, size_t *size)
{
	ulong addr;

	if (!fu_file_name_valid(file))
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

static int fu_get_image(int argc, char *const argv[], const void **fit,
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
		return fu_tftp_image(file, fit, size);
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

static int fu_metadata_store_is_blank(const struct fu_metadata_store *store)
{
	u8 buf[FU_METADATA_SIZE];
	int copy, i, ret;

	for (copy = 0; copy < FU_METADATA_COPIES; copy++) {
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

static int fu_update_release(const struct fu_layout *layout,
			     const struct fu_storage *storage,
			     const struct fu_metadata_store *store,
			     const void *fit, size_t size)
{
	struct fu_release_component *item;
	struct fu_package package;
	struct fu_metadata metadata;
	struct fu_release release;
	const void *component_fit;
	u8 digest[FU_SHA256_SIZE];
	size_t component_size;
	u8 deployment;
	int digest_size, i, ret;

	memset(&release, 0, sizeof(release));
	ret = fu_verify_release(layout, fit, size, &release);
	if (ret)
		return ret;
	ret = fu_metadata_load(store, &metadata);
	if (ret)
		return ret;
	ret = fu_metadata_begin_release(&metadata, release.version,
					&deployment);
	if (ret)
		return ret;

	for (i = 0; i < release.count; i++) {
		item = &release.component[i];
		if (item->component == FU_COMPONENT_BOOTLOADER &&
		    !release.allow_bootloader)
			return -EPERM;

		ret = fu_tftp_image(item->file, &component_fit, &component_size);
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
		if (digest_size != FU_SHA256_SIZE ||
		    memcmp(digest, item->sha256, FU_SHA256_SIZE)) {
			ret = -EBADMSG;
			goto unmap_component;
		}

		ret = fu_verify_package(layout, component_fit, component_size,
					&package);
		if (ret)
			goto unmap_component;
		if (package.component != item->component ||
		    package.rollback_index < release.rollback_index) {
			ret = -EPERM;
			goto unmap_component;
		}
		ret = fu_write_package(layout, storage, &metadata, &package,
				       component_fit, component_size, deployment,
				       NULL);

unmap_component:
		unmap_sysmem(component_fit);
		if (ret)
			return ret;
	}

	ret = fu_metadata_commit_release(&metadata, deployment,
					 layout->attempts);
	if (ret)
		return ret;
	ret = fu_metadata_save(store, &metadata);
	if (ret)
		return ret;

	printf("Updated release %u; deployment %u is pending (%u tries)\n",
	       release.version, deployment, layout->attempts);
	return 0;
}

static int do_fu(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct fu_metadata_store store;
	struct fu_store_ctx store_ctx;
	struct fu_metadata metadata;
	struct fu_storage storage;
	struct fu_layout layout;
	const void *fit;
	size_t fit_size;
	u8 selected, slot;
	int deployment, ret;

	if (argc < 2)
		return CMD_RET_USAGE;

	ret = fu_open(&layout, &storage, &store, &store_ctx);
	if (ret) {
		printf("fu: invalid or unavailable layout (%d)\n", ret);
		return CMD_RET_FAILURE;
	}

	if (!strcmp(argv[1], "list")) {
		printf("product: %s  layout: %s  storage: %s\n", layout.product,
		       layout.layout_id,
		       layout.type == FU_STORAGE_MMC ? "mmc" : "spi-nor");
		for (int i = 0; i < FU_REGION_COUNT; i++)
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
		fu_metadata_init(&metadata, slot);
		ret = fu_metadata_save(&store, &metadata);
		if (!ret)
			ret = fu_metadata_save(&store, &metadata);
		if (!ret)
			printf("Initialized metadata with slot %c active\n",
			       'a' + slot);
		goto out;
	}

	ret = fu_metadata_load(&store, &metadata);
	if (ret == -ENODATA && !strcmp(argv[1], "select") &&
	    layout.auto_init && !fu_metadata_store_is_blank(&store)) {
		fu_metadata_init(&metadata, layout.default_slot);
		ret = fu_metadata_save(&store, &metadata);
		if (!ret)
			ret = fu_metadata_save(&store, &metadata);
		if (!ret)
			printf("Initialized blank metadata with slot %c active\n",
			       'a' + layout.default_slot);
	}
	if (ret) {
		printf("fu: metadata unavailable (%d); run 'fu init a|b'\n",
		       ret);
		return CMD_RET_FAILURE;
	}

	if (!strcmp(argv[1], "status")) {
		fu_print_metadata(&metadata);
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "select")) {
		ret = fu_metadata_select(&metadata, &selected);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = fu_metadata_save(&store, &metadata);
			if (ret)
				goto out;
		}
		fu_export_selection(&layout, &metadata, selected);
		printf("Selected deployment %u: boot=%c kernel=%c rootfs=%c\n",
		       selected,
		       'a' + metadata.deployment[selected]
				       .component[FU_COMPONENT_BOOTLOADER]
				       .slot,
		       'a' + metadata.deployment[selected]
				       .component[FU_COMPONENT_KERNEL]
				       .slot,
		       'a' + metadata.deployment[selected]
				       .component[FU_COMPONENT_ROOTFS]
				       .slot);
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "mark-good")) {
		deployment = argc == 3 ? dectoul(argv[2], NULL) : -1;
		ret = fu_metadata_mark_good(&metadata, deployment);
		if (!ret)
			ret = fu_metadata_save(&store, &metadata);
		goto out;
	}

	if (!strcmp(argv[1], "mark-bad")) {
		if (argc != 3)
			return CMD_RET_USAGE;
		deployment = dectoul(argv[2], NULL);
		ret = fu_metadata_mark_bad(&metadata, deployment);
		if (!ret)
			ret = fu_metadata_save(&store, &metadata);
		goto out;
	}

	if (!strcmp(argv[1], "rollback")) {
		ret = fu_metadata_rollback(&metadata);
		if (!ret)
			ret = fu_metadata_save(&store, &metadata);
		goto out;
	}

	if (!strcmp(argv[1], "boot")) {
		if (argc < 3 || argc > 4)
			return CMD_RET_USAGE;
		deployment = dectoul(argv[2], NULL);
		if (deployment > 1 ||
		    metadata.deployment[deployment].state == FU_STATE_EMPTY ||
		    metadata.deployment[deployment].state == FU_STATE_FAILED)
			return CMD_RET_FAILURE;
		if (argc == 4 && !strcmp(argv[3], "once")) {
			metadata.boot_once_deployment = deployment;
		} else {
			metadata.pending_deployment = deployment;
			if (!metadata.deployment[deployment].tries_remaining)
				metadata.deployment[deployment].tries_remaining =
					layout.attempts;
		}
		ret = fu_metadata_save(&store, &metadata);
		goto out;
	}

	if (!strcmp(argv[1], "update") && argc == 5 &&
	    !strcmp(argv[4], "all")) {
		if (strcmp(argv[2], "tftp"))
			return CMD_RET_USAGE;
		ret = fu_get_image(2, argv + 2, &fit, &fit_size);
		if (ret)
			goto out;
		ret = fu_update_release(&layout, &storage, &store, fit, fit_size);
		unmap_sysmem(fit);
		goto out;
	}

	if (!strcmp(argv[1], "verify") || !strcmp(argv[1], "update") ||
	    !strcmp(argv[1], "restore")) {
		enum fu_component expected;
		int image_arg = 2;
		int expected_component = -1;

		if (!strcmp(argv[1], "restore")) {
			if (argc < 4 || fu_component_from_name(argv[2], &expected))
				return CMD_RET_USAGE;
			expected_component = expected;
			image_arg = 3;
		}
		ret = fu_get_image(argc - image_arg, argv + image_arg, &fit,
				   &fit_size);
		if (ret)
			goto out;
		if (!strcmp(argv[1], "verify")) {
			struct fu_package package;
			struct fu_release release;

			ret = fu_verify_package(&layout, fit, fit_size,
						&package);
			if (!ret) {
				printf("Valid %s package, version %u, rollback %u\n",
				       fu_component_name(package.component),
				       package.version, package.rollback_index);
			} else if (ret == -EOPNOTSUPP) {
				memset(&release, 0, sizeof(release));
				ret = fu_verify_release(&layout, fit, fit_size, &release);
				if (!ret)
					printf("Valid release package, version %u, rollback %u\n",
					       release.version,
					       release.rollback_index);
			}
		} else {
			ret = fu_update_fit(&layout, &storage, &store, fit,
					    fit_size, expected_component);
		}
		unmap_sysmem(fit);
		goto out;
	}

	return CMD_RET_USAGE;

out:
	if (ret)
		printf("fu: operation failed (%d)\n", ret);
	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_LONGHELP(fu, "list\n"
		    "fu status\n"
		    "fu init <a|b>\n"
		    "fu verify <tftp file|addr address size>\n"
		    "fu update <tftp file|addr address size>\n"
		    "fu update tftp <release-file> all\n"
		    "fu restore <component> <tftp file|addr address size>\n"
		    "fu select\n"
		    "fu boot <deployment> [once]\n"
		    "fu mark-good [deployment]\n"
		    "fu mark-bad <deployment>\n"
		    "fu rollback");

U_BOOT_CMD(fu, 7, 1, do_fu, "RAW A/B firmware upgrade", fu_help_text);
