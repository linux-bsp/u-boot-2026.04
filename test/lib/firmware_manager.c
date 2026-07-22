// SPDX-License-Identifier: GPL-2.0+
#include <firmware_manager.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <test/lib.h>
#include <test/test.h>
#include <test/ut.h>

struct fw_test_store {
	u8 copy[FW_METADATA_COPIES][FW_METADATA_SIZE];
};

static int fw_test_read(void *ctx, unsigned int copy, void *buf, size_t len)
{
	struct fw_test_store *store = ctx;

	if (copy >= FW_METADATA_COPIES || len > FW_METADATA_SIZE)
		return -EINVAL;
	memcpy(buf, store->copy[copy], len);
	return 0;
}

static int fw_test_write(void *ctx, unsigned int copy, const void *buf,
			 size_t len)
{
	struct fw_test_store *store = ctx;

	if (copy >= FW_METADATA_COPIES || len > FW_METADATA_SIZE)
		return -EINVAL;
	memcpy(store->copy[copy], buf, len);
	return 0;
}

static void fw_test_setup(struct fw_test_store *data,
			  struct fw_metadata_store *store)
{
	memset(data, 0, sizeof(*data));
	store->read = fw_test_read;
	store->write = fw_test_write;
	store->ctx = data;
}

static int lib_test_fw_metadata_lifecycle(struct unit_test_state *uts)
{
	struct fw_metadata_store store;
	struct fw_test_store data;
	struct fw_metadata metadata;
	u8 deployment;

	fw_test_setup(&data, &store);
	ut_asserteq(-ENODATA, fw_metadata_load(&store, &metadata));

	fw_metadata_init(&metadata, 0);
	ut_asserteq(FW_STATE_CONFIRMED, metadata.update_state);
	ut_assertok(fw_metadata_save(&store, &metadata));
	ut_assertok(fw_metadata_save(&store, &metadata));
	ut_assertok(fw_metadata_load(&store, &metadata));
	ut_asserteq(2, metadata.sequence);
	ut_asserteq(0, metadata.active_deployment);

	ut_assertok(fw_metadata_prepare_update(&metadata, FW_COMPONENT_KERNEL,
					       1, 4, 7, 3, &deployment));
	ut_asserteq(1, deployment);
	ut_assertok(fw_metadata_prepare_update(&metadata, FW_COMPONENT_ROOTFS,
					       1, 5, 8, 3, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(1,
		    metadata.deployment[1].component[FW_COMPONENT_KERNEL].slot);
	ut_asserteq(1,
		    metadata.deployment[1].component[FW_COMPONENT_ROOTFS].slot);
	ut_assertok(fw_metadata_save(&store, &metadata));

	ut_asserteq(1, fw_metadata_select(&metadata, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(2, metadata.deployment[1].tries_remaining);
	ut_assertok(fw_metadata_mark_good(&metadata, -1));
	ut_assertok(fw_metadata_save(&store, &metadata));
	ut_asserteq(1, metadata.active_deployment);
	ut_asserteq(1, metadata.last_good_deployment);
	ut_asserteq(FW_SLOT_NONE, metadata.pending_deployment);

	return 0;
}

LIB_TEST(lib_test_fw_metadata_lifecycle, 0);

static int lib_test_fw_metadata_rollback(struct unit_test_state *uts)
{
	struct fw_metadata_store store;
	struct fw_test_store data;
	struct fw_metadata metadata;
	u8 deployment;

	fw_test_setup(&data, &store);
	fw_metadata_init(&metadata, 0);
	ut_assertok(fw_metadata_save(&store, &metadata));
	ut_assertok(fw_metadata_save(&store, &metadata));
	ut_assertok(fw_metadata_prepare_update(&metadata, FW_COMPONENT_ROOTFS,
					       1, 2, 2, 1, &deployment));

	ut_asserteq(1, fw_metadata_select(&metadata, &deployment));
	ut_asserteq(0, metadata.deployment[1].tries_remaining);
	ut_asserteq(1, fw_metadata_select(&metadata, &deployment));
	ut_asserteq(0, deployment);
	ut_asserteq(FW_STATE_FAILED, metadata.deployment[1].state);
	ut_asserteq(FW_SLOT_NONE, metadata.pending_deployment);

	return 0;
}

LIB_TEST(lib_test_fw_metadata_rollback, 0);

static int lib_test_fw_metadata_cross_stage_selection(struct unit_test_state *uts)
{
	struct fw_metadata_store store;
	struct fw_test_store data;
	struct fw_metadata metadata;
	u8 deployment;

	fw_test_setup(&data, &store);
	fw_metadata_init(&metadata, 0);
	ut_assertok(fw_metadata_save(&store, &metadata));
	ut_assertok(fw_metadata_save(&store, &metadata));
	ut_assertok(fw_metadata_prepare_update(&metadata,
					       FW_COMPONENT_BOOTLOADER, 1,
					       2, 2, 1, &deployment));

	ut_asserteq(1, fw_metadata_select(&metadata, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(0, metadata.deployment[1].tries_remaining);
	ut_assertok(fw_metadata_get_selected(&metadata, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(0, metadata.deployment[1].tries_remaining);

	ut_assertok(fw_metadata_save(&store, &metadata));
	ut_assertok(fw_metadata_load(&store, &metadata));
	ut_assertok(fw_metadata_get_selected(&metadata, &deployment));
	ut_asserteq(1, deployment);

	ut_asserteq(1, fw_metadata_select(&metadata, &deployment));
	ut_asserteq(0, deployment);
	ut_asserteq(0, metadata.selected_deployment);
	ut_asserteq(FW_STATE_FAILED, metadata.deployment[1].state);
	ut_asserteq(FW_SLOT_NONE, metadata.pending_deployment);

	return 0;
}

LIB_TEST(lib_test_fw_metadata_cross_stage_selection, 0);

static int lib_test_fw_metadata_boot_once_cross_stage(struct unit_test_state *uts)
{
	struct fw_metadata metadata;
	u8 deployment;

	fw_metadata_init(&metadata, 0);
	ut_assertok(fw_metadata_prepare_update(&metadata, FW_COMPONENT_ROOTFS,
					       1, 2, 2, 3, &deployment));
	metadata.pending_deployment = FW_SLOT_NONE;
	metadata.boot_once_deployment = deployment;

	ut_asserteq(1, fw_metadata_select(&metadata, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(FW_SLOT_NONE, metadata.boot_once_deployment);
	ut_assertok(fw_metadata_get_selected(&metadata, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(3, metadata.deployment[1].tries_remaining);

	ut_asserteq(1, fw_metadata_select(&metadata, &deployment));
	ut_asserteq(0, deployment);
	ut_assertok(fw_metadata_get_selected(&metadata, &deployment));
	ut_asserteq(0, deployment);

	return 0;
}

LIB_TEST(lib_test_fw_metadata_boot_once_cross_stage, 0);

static int lib_test_fw_metadata_redundancy(struct unit_test_state *uts)
{
	struct fw_metadata_store store;
	struct fw_test_store data;
	struct fw_metadata metadata;
	u8 newest;

	fw_test_setup(&data, &store);
	fw_metadata_init(&metadata, 0);
	ut_assertok(fw_metadata_save(&store, &metadata));
	ut_assertok(fw_metadata_save(&store, &metadata));
	newest = metadata.source_copy;
	data.copy[newest][0] ^= 0xff;

	ut_assertok(fw_metadata_load(&store, &metadata));
	ut_asserteq(1, metadata.sequence);

	return 0;
}

LIB_TEST(lib_test_fw_metadata_redundancy, 0);

static int lib_test_fw_release_transaction(struct unit_test_state *uts)
{
	struct fw_metadata metadata;
	u8 deployment;
	int ret;

	fw_metadata_init(&metadata, 0);
	metadata.deployment[0].release_version = 3;

	ut_assertok(fw_metadata_begin_release(&metadata, 4, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(FW_SLOT_NONE, metadata.pending_deployment);
	ut_asserteq(FW_STATE_WRITING, metadata.deployment[deployment].state);

	ret = fw_metadata_stage_component(&metadata, deployment,
					  FW_COMPONENT_KERNEL, 1, 10, 20);
	ut_assertok(ret);
	ret = fw_metadata_stage_component(&metadata, deployment,
					  FW_COMPONENT_ROOTFS, 1, 11, 21);
	ut_assertok(ret);
	ut_asserteq(FW_SLOT_NONE, metadata.pending_deployment);

	ut_assertok(fw_metadata_commit_release(&metadata, deployment, 3));
	ut_asserteq(deployment, metadata.pending_deployment);
	ut_asserteq(FW_STATE_READY, metadata.deployment[deployment].state);
	ut_asserteq(4, metadata.deployment[deployment].release_version);
	ut_asserteq(1, metadata.deployment[deployment]
				.component[FW_COMPONENT_KERNEL].slot);
	ut_asserteq(1, metadata.deployment[deployment]
				.component[FW_COMPONENT_ROOTFS].slot);

	return 0;
}

LIB_TEST(lib_test_fw_release_transaction, 0);

static int lib_test_fw_release_restarts_pending(struct unit_test_state *uts)
{
	struct fw_metadata metadata;
	struct fw_component_slot *source, *target;
	u8 deployment, pending;

	fw_metadata_init(&metadata, 0);
	metadata.deployment[0].release_version = 3;
	ut_assertok(fw_metadata_prepare_update(&metadata,
					       FW_COMPONENT_KERNEL, 1, 9, 9, 3,
					       &pending));
	ut_asserteq(1, pending);
	ut_asserteq(pending, metadata.pending_deployment);

	ut_assertok(fw_metadata_begin_release(&metadata, 4, &deployment));
	ut_asserteq(pending, deployment);
	ut_asserteq(FW_SLOT_NONE, metadata.pending_deployment);
	ut_asserteq(FW_SLOT_NONE, metadata.boot_once_deployment);
	ut_asserteq(0, metadata.selected_deployment);
	ut_asserteq(FW_STATE_WRITING,
		    metadata.deployment[deployment].state);
	ut_asserteq(0, metadata.deployment[deployment].tries_remaining);
	ut_asserteq(0, metadata.deployment[deployment].successful);
	ut_asserteq(4, metadata.deployment[deployment].release_version);

	source = &metadata.deployment[0].component[FW_COMPONENT_KERNEL];
	target = &metadata.deployment[deployment]
			  .component[FW_COMPONENT_KERNEL];
	ut_asserteq(source->slot, target->slot);
	ut_asserteq(source->version, target->version);
	ut_asserteq(source->rollback_index, target->rollback_index);

	return 0;
}

LIB_TEST(lib_test_fw_release_restarts_pending, 0);

static int lib_test_fw_release_rejects_downgrade(struct unit_test_state *uts)
{
	struct fw_metadata metadata;
	u8 deployment;

	fw_metadata_init(&metadata, 0);
	metadata.deployment[0].release_version = 7;

	ut_asserteq(-EPERM,
		    fw_metadata_begin_release(&metadata, 6, &deployment));
	ut_asserteq(FW_SLOT_NONE, metadata.pending_deployment);

	return 0;
}

LIB_TEST(lib_test_fw_release_rejects_downgrade, 0);
