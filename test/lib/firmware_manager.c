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
					       1, 4, 7, 0, 3, &deployment));
	ut_asserteq(1, deployment);
	ut_assertok(fw_metadata_prepare_update(&metadata, FW_COMPONENT_ROOTFS,
					       1, 5, 8, 0, 3, &deployment));
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
					       1, 2, 2, 0, 1, &deployment));

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
					       2, 2, 0, 1, &deployment));

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
					       1, 2, 2, 0, 3, &deployment));
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
					  FW_COMPONENT_KERNEL, 1, 10, 20,
					  30000000);
	ut_assertok(ret);
	ret = fw_metadata_stage_component(&metadata, deployment,
					  FW_COMPONENT_ROOTFS, 1, 11, 21,
					  30000001);
	ut_assertok(ret);
	ut_asserteq(FW_SLOT_NONE, metadata.pending_deployment);

	ut_assertok(fw_metadata_commit_release(&metadata, deployment, 3));
	ut_asserteq(deployment, metadata.pending_deployment);
	ut_asserteq(FW_STATE_READY, metadata.deployment[deployment].state);
	ut_asserteq(4, metadata.deployment[deployment].release_version);
	ut_asserteq(30000000, metadata.deployment[deployment]
				.component[FW_COMPONENT_KERNEL].build_time_minutes);
	ut_asserteq(30000001, metadata.deployment[deployment]
				.component[FW_COMPONENT_ROOTFS].build_time_minutes);
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
					       FW_COMPONENT_KERNEL, 1, 9, 9, 0, 3,
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

static int lib_test_fw_component_write_interruption(struct unit_test_state *uts)
{
	struct fw_metadata_store store;
	struct fw_test_store data;
	struct fw_metadata metadata, restored;
	u8 target, selected;

	fw_test_setup(&data, &store);
	fw_metadata_init(&metadata, 0);
	ut_assertok(fw_metadata_save(&store, &metadata));
	ut_assertok(fw_metadata_begin_component(&metadata, FW_COMPONENT_KERNEL,
					      &target));
	ut_assertok(fw_metadata_save(&store, &metadata));
	/* Reset during the raw write: only the confirmed source is selected. */
	ut_assertok(fw_metadata_load(&store, &restored));
	ut_asserteq(FW_STATE_WRITING, restored.deployment[target].state);
	ut_asserteq(FW_SLOT_NONE, restored.pending_deployment);
	ut_assert(fw_metadata_select(&restored, &selected) >= 0);
	ut_asserteq(0, selected);
	ut_asserteq(0, restored.deployment[target].tries_remaining);
	ut_asserteq(-EINVAL, fw_metadata_mark_good(&restored, target));
	restored.boot_once_deployment = target;
	ut_asserteq(-EINVAL, fw_metadata_select(&restored, &selected));
	restored.selected_deployment = target;
	ut_asserteq(-ENOENT, fw_metadata_get_selected(&restored, &selected));
	ut_assertok(fw_metadata_stage_component(&metadata, target,
			FW_COMPONENT_KERNEL, 1, 9, 9, 0));
	ut_assertok(fw_metadata_commit_release(&metadata, target, 3));
	ut_assertok(fw_metadata_save(&store, &metadata));
	/* A following rootfs update retains the already staged kernel. */
	ut_assertok(fw_metadata_begin_component(&metadata, FW_COMPONENT_ROOTFS,
					      &target));
	ut_asserteq(1, metadata.deployment[target].component[FW_COMPONENT_KERNEL].slot);
	ut_asserteq(9, metadata.deployment[target].component[FW_COMPONENT_KERNEL].version);
	ut_assertok(fw_metadata_save(&store, &metadata));
	ut_assertok(fw_metadata_load(&store, &restored));
	ut_assert(fw_metadata_select(&restored, &selected) >= 0);
	ut_asserteq(0, selected);
	ut_assertok(fw_metadata_stage_component(&metadata, target,
			FW_COMPONENT_ROOTFS, 1, 9, 9, 0));
	ut_assertok(fw_metadata_commit_release(&metadata, target, 3));
	ut_assertok(fw_metadata_select(&metadata, &selected) - 1);
	ut_asserteq(target, selected);
	return 0;
}

LIB_TEST(lib_test_fw_component_write_interruption, 0);

static int lib_test_fw_component_identity(struct unit_test_state *uts)
{
	struct fw_metadata metadata;
	u8 target;

	fw_metadata_init(&metadata, 0);
	metadata.deployment[0].identity.valid = 1;
	strcpy(metadata.deployment[0].identity.tag, "v0.0.1");
	ut_assertok(fw_metadata_begin_component(&metadata,
			FW_COMPONENT_BOOTLOADER, &target));
	ut_asserteq(1, metadata.deployment[target].identity.valid);
	ut_assertok(fw_metadata_stage_component(&metadata, target,
			FW_COMPONENT_BOOTLOADER, 1, 1, 1, 0));
	ut_assertok(fw_metadata_commit_release(&metadata, target, 3));
	ut_assertok(fw_metadata_begin_component(&metadata, FW_COMPONENT_KERNEL,
					      &target));
	ut_asserteq(0, metadata.deployment[target].identity.valid);
	ut_asserteq(1, metadata.deployment[target].component[FW_COMPONENT_BOOTLOADER].slot);
	return 0;
}

LIB_TEST(lib_test_fw_component_identity, 0);

static int lib_test_fw_release_accepts_older_tag(struct unit_test_state *uts)
{
	struct fw_metadata metadata;
	u8 deployment;

	fw_metadata_init(&metadata, 0);
	metadata.deployment[0].release_version = 7;

	ut_assertok(fw_metadata_begin_release(&metadata, 6, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(6, metadata.deployment[deployment].release_version);
	ut_asserteq(7, metadata.deployment[0].release_version);
	ut_asserteq(FW_SLOT_NONE, metadata.pending_deployment);

	return 0;
}

LIB_TEST(lib_test_fw_release_accepts_older_tag, 0);

static int lib_test_fw_metadata_identity(struct unit_test_state *uts)
{
    struct fw_metadata_store store;
    struct fw_test_store data;
    struct fw_metadata metadata, restored;
    u8 target;

    fw_test_setup(&data, &store);
    fw_metadata_init(&metadata, 0);
    metadata.deployment[0].identity.valid = 1;
    strcpy(metadata.deployment[0].identity.tag, "v1.0.12");
    memset(metadata.deployment[0].identity.id, 0x12, 32);
    memset(metadata.deployment[0].identity.hash, 0xab, 64);
    metadata.deployment[0].identity.size[0] = 12345;
    metadata.deployment[0].identity.size[1] = 67890;
    ut_assertok(fw_metadata_save(&store, &metadata));
    ut_assertok(fw_metadata_save(&store, &metadata));
    ut_assertok(fw_metadata_load(&store, &restored));
    ut_assertok(memcmp(&metadata.deployment[0].identity,
                       &restored.deployment[0].identity,
                       sizeof(struct fw_identity)));
    data.copy[metadata.source_copy][160] ^= 1;
    ut_assertok(fw_metadata_load(&store, &restored));
    ut_asserteq(metadata.sequence - 1, restored.sequence);
    /* The valid header alone cannot make a torn new record bootable. */
    memcpy(data.copy[metadata.source_copy], data.copy[restored.source_copy], 128);
    memset(data.copy[metadata.source_copy] + 128, 0xff, FW_METADATA_SIZE - 128);
    ut_assertok(fw_metadata_load(&store, &restored));
    ut_asserteq(metadata.sequence - 1, restored.sequence);
    ut_assertok(fw_metadata_prepare_update(&restored, FW_COMPONENT_BOOTLOADER,
                                          1, 13, 0, 0, 3, &target));
    ut_assert(restored.deployment[target].identity.valid);
    ut_assertok(fw_metadata_begin_release(&restored, 65549, &target));
    ut_assert(!restored.deployment[target].identity.valid);
    ut_assert(restored.deployment[0].identity.valid);
    return 0;
}
LIB_TEST(lib_test_fw_metadata_identity, 0);
