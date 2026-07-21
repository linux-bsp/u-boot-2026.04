/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __FIRMWARE_MANAGER_H
#define __FIRMWARE_MANAGER_H

#include <linux/types.h>

#define FW_METADATA_SIZE 128
#define FW_METADATA_COPIES 2
#define FW_SLOT_NONE 0xff
#define FW_DEFAULT_ATTEMPTS 3

enum fw_component {
	FW_COMPONENT_BOOTLOADER,
	FW_COMPONENT_KERNEL,
	FW_COMPONENT_ROOTFS,
	FW_COMPONENT_COUNT,
};

enum fw_deployment_state {
	FW_STATE_EMPTY,
	FW_STATE_WRITING,
	FW_STATE_READY,
	FW_STATE_BOOT_TESTING,
	FW_STATE_CONFIRMED,
	FW_STATE_FAILED,
};

struct fw_component_slot {
	u8 slot;
	u8 valid;
	u32 version;
	u32 rollback_index;
};

struct fw_deployment {
	struct fw_component_slot component[FW_COMPONENT_COUNT];
	u8 state;
	u8 tries_remaining;
	u8 successful;
	u32 release_version;
};

struct fw_metadata {
	u32 sequence;
	u8 active_deployment;
	u8 pending_deployment;
	u8 last_good_deployment;
	u8 boot_once_deployment;
	u8 selected_deployment;
	u8 update_state;
	struct fw_deployment deployment[2];
	u8 source_copy;
};

struct fw_metadata_store {
	int (*read)(void *ctx, unsigned int copy, void *buf, size_t len);
	int (*write)(void *ctx, unsigned int copy, const void *buf, size_t len);
	void *ctx;
};

const char *fw_component_name(enum fw_component component);
const char *fw_state_name(enum fw_deployment_state state);
void fw_metadata_init(struct fw_metadata *metadata, u8 slot);
int fw_metadata_load(const struct fw_metadata_store *store,
		     struct fw_metadata *metadata);
int fw_metadata_save(const struct fw_metadata_store *store,
		     struct fw_metadata *metadata);
int fw_metadata_select(struct fw_metadata *metadata, u8 *deployment);
int fw_metadata_get_selected(const struct fw_metadata *metadata,
			     u8 *deployment);
int fw_metadata_prepare_update(struct fw_metadata *metadata,
			       enum fw_component component, u8 slot,
			       u32 version, u32 rollback_index, u8 attempts,
			       u8 *deployment);
int fw_metadata_begin_release(struct fw_metadata *metadata,
			      u32 release_version, u8 *deployment);
int fw_metadata_stage_component(struct fw_metadata *metadata, u8 deployment,
				enum fw_component component, u8 slot,
				u32 version, u32 rollback_index);
int fw_metadata_commit_release(struct fw_metadata *metadata, u8 deployment,
			       u8 attempts);
int fw_metadata_mark_good(struct fw_metadata *metadata, int deployment);
int fw_metadata_mark_bad(struct fw_metadata *metadata, u8 deployment);
int fw_metadata_rollback(struct fw_metadata *metadata);

#endif
