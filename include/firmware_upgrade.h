/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __FIRMWARE_UPGRADE_H
#define __FIRMWARE_UPGRADE_H

#include <linux/types.h>

#define FU_METADATA_SIZE 128
#define FU_METADATA_COPIES 2
#define FU_SLOT_NONE 0xff
#define FU_DEFAULT_ATTEMPTS 3

enum fu_component {
	FU_COMPONENT_BOOTLOADER,
	FU_COMPONENT_KERNEL,
	FU_COMPONENT_ROOTFS,
	FU_COMPONENT_COUNT,
};

enum fu_deployment_state {
	FU_STATE_EMPTY,
	FU_STATE_WRITING,
	FU_STATE_READY,
	FU_STATE_BOOT_TESTING,
	FU_STATE_CONFIRMED,
	FU_STATE_FAILED,
};

struct fu_component_slot {
	u8 slot;
	u8 valid;
	u32 version;
	u32 rollback_index;
};

struct fu_deployment {
	struct fu_component_slot component[FU_COMPONENT_COUNT];
	u8 state;
	u8 tries_remaining;
	u8 successful;
	u32 release_version;
};

struct fu_metadata {
	u32 sequence;
	u8 active_deployment;
	u8 pending_deployment;
	u8 last_good_deployment;
	u8 boot_once_deployment;
	u8 update_state;
	struct fu_deployment deployment[2];
	u8 source_copy;
};

struct fu_metadata_store {
	int (*read)(void *ctx, unsigned int copy, void *buf, size_t len);
	int (*write)(void *ctx, unsigned int copy, const void *buf, size_t len);
	void *ctx;
};

const char *fu_component_name(enum fu_component component);
const char *fu_state_name(enum fu_deployment_state state);
void fu_metadata_init(struct fu_metadata *metadata, u8 slot);
int fu_metadata_load(const struct fu_metadata_store *store,
		     struct fu_metadata *metadata);
int fu_metadata_save(const struct fu_metadata_store *store,
		     struct fu_metadata *metadata);
int fu_metadata_select(struct fu_metadata *metadata, u8 *deployment);
int fu_metadata_prepare_update(struct fu_metadata *metadata,
			       enum fu_component component, u8 slot,
			       u32 version, u32 rollback_index, u8 attempts,
			       u8 *deployment);
int fu_metadata_begin_release(struct fu_metadata *metadata,
			      u32 release_version, u8 *deployment);
int fu_metadata_stage_component(struct fu_metadata *metadata, u8 deployment,
				enum fu_component component, u8 slot,
				u32 version, u32 rollback_index);
int fu_metadata_commit_release(struct fu_metadata *metadata, u8 deployment,
			       u8 attempts);
int fu_metadata_mark_good(struct fu_metadata *metadata, int deployment);
int fu_metadata_mark_bad(struct fu_metadata *metadata, u8 deployment);
int fu_metadata_rollback(struct fu_metadata *metadata);

#endif
