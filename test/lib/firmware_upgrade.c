// SPDX-License-Identifier: GPL-2.0+
#include <firmware_upgrade.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <test/lib.h>
#include <test/test.h>
#include <test/ut.h>

struct fu_test_store {
	u8 copy[FU_METADATA_COPIES][FU_METADATA_SIZE];
};

static int fu_test_read(void *ctx, unsigned int copy, void *buf, size_t len)
{
	struct fu_test_store *store = ctx;

	if (copy >= FU_METADATA_COPIES || len > FU_METADATA_SIZE)
		return -EINVAL;
	memcpy(buf, store->copy[copy], len);
	return 0;
}

static int fu_test_write(void *ctx, unsigned int copy, const void *buf,
			 size_t len)
{
	struct fu_test_store *store = ctx;

	if (copy >= FU_METADATA_COPIES || len > FU_METADATA_SIZE)
		return -EINVAL;
	memcpy(store->copy[copy], buf, len);
	return 0;
}

static void fu_test_setup(struct fu_test_store *data,
			  struct fu_metadata_store *store)
{
	memset(data, 0, sizeof(*data));
	store->read = fu_test_read;
	store->write = fu_test_write;
	store->ctx = data;
}

static int lib_test_fu_metadata_lifecycle(struct unit_test_state *uts)
{
	struct fu_metadata_store store;
	struct fu_test_store data;
	struct fu_metadata metadata;
	u8 deployment;

	fu_test_setup(&data, &store);
	ut_asserteq(-ENODATA, fu_metadata_load(&store, &metadata));

	fu_metadata_init(&metadata, 0);
	ut_assertok(fu_metadata_save(&store, &metadata));
	ut_assertok(fu_metadata_save(&store, &metadata));
	ut_assertok(fu_metadata_load(&store, &metadata));
	ut_asserteq(2, metadata.sequence);
	ut_asserteq(0, metadata.active_deployment);

	ut_assertok(fu_metadata_prepare_update(&metadata, FU_COMPONENT_KERNEL,
					       1, 4, 7, 3, &deployment));
	ut_asserteq(1, deployment);
	ut_assertok(fu_metadata_prepare_update(&metadata, FU_COMPONENT_ROOTFS,
					       1, 5, 8, 3, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(1,
		    metadata.deployment[1].component[FU_COMPONENT_KERNEL].slot);
	ut_asserteq(1,
		    metadata.deployment[1].component[FU_COMPONENT_ROOTFS].slot);
	ut_assertok(fu_metadata_save(&store, &metadata));

	ut_asserteq(1, fu_metadata_select(&metadata, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(2, metadata.deployment[1].tries_remaining);
	ut_assertok(fu_metadata_mark_good(&metadata, -1));
	ut_assertok(fu_metadata_save(&store, &metadata));
	ut_asserteq(1, metadata.active_deployment);
	ut_asserteq(1, metadata.last_good_deployment);
	ut_asserteq(FU_SLOT_NONE, metadata.pending_deployment);

	return 0;
}

LIB_TEST(lib_test_fu_metadata_lifecycle, 0);

static int lib_test_fu_metadata_rollback(struct unit_test_state *uts)
{
	struct fu_metadata_store store;
	struct fu_test_store data;
	struct fu_metadata metadata;
	u8 deployment;

	fu_test_setup(&data, &store);
	fu_metadata_init(&metadata, 0);
	ut_assertok(fu_metadata_save(&store, &metadata));
	ut_assertok(fu_metadata_save(&store, &metadata));
	ut_assertok(fu_metadata_prepare_update(&metadata, FU_COMPONENT_ROOTFS,
					       1, 2, 2, 1, &deployment));

	ut_asserteq(1, fu_metadata_select(&metadata, &deployment));
	ut_asserteq(0, metadata.deployment[1].tries_remaining);
	ut_asserteq(1, fu_metadata_select(&metadata, &deployment));
	ut_asserteq(0, deployment);
	ut_asserteq(FU_STATE_FAILED, metadata.deployment[1].state);
	ut_asserteq(FU_SLOT_NONE, metadata.pending_deployment);

	return 0;
}

LIB_TEST(lib_test_fu_metadata_rollback, 0);

static int lib_test_fu_metadata_cross_stage_selection(struct unit_test_state *uts)
{
	struct fu_metadata_store store;
	struct fu_test_store data;
	struct fu_metadata metadata;
	u8 deployment;

	fu_test_setup(&data, &store);
	fu_metadata_init(&metadata, 0);
	ut_assertok(fu_metadata_save(&store, &metadata));
	ut_assertok(fu_metadata_save(&store, &metadata));
	ut_assertok(fu_metadata_prepare_update(&metadata,
					       FU_COMPONENT_BOOTLOADER, 1,
					       2, 2, 1, &deployment));

	ut_asserteq(1, fu_metadata_select(&metadata, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(0, metadata.deployment[1].tries_remaining);
	ut_assertok(fu_metadata_get_selected(&metadata, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(0, metadata.deployment[1].tries_remaining);

	ut_assertok(fu_metadata_save(&store, &metadata));
	ut_assertok(fu_metadata_load(&store, &metadata));
	ut_assertok(fu_metadata_get_selected(&metadata, &deployment));
	ut_asserteq(1, deployment);

	ut_asserteq(1, fu_metadata_select(&metadata, &deployment));
	ut_asserteq(0, deployment);
	ut_asserteq(0, metadata.selected_deployment);
	ut_asserteq(FU_STATE_FAILED, metadata.deployment[1].state);
	ut_asserteq(FU_SLOT_NONE, metadata.pending_deployment);

	return 0;
}

LIB_TEST(lib_test_fu_metadata_cross_stage_selection, 0);

static int lib_test_fu_metadata_boot_once_cross_stage(struct unit_test_state *uts)
{
	struct fu_metadata metadata;
	u8 deployment;

	fu_metadata_init(&metadata, 0);
	ut_assertok(fu_metadata_prepare_update(&metadata, FU_COMPONENT_ROOTFS,
					       1, 2, 2, 3, &deployment));
	metadata.pending_deployment = FU_SLOT_NONE;
	metadata.boot_once_deployment = deployment;

	ut_asserteq(1, fu_metadata_select(&metadata, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(FU_SLOT_NONE, metadata.boot_once_deployment);
	ut_assertok(fu_metadata_get_selected(&metadata, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(3, metadata.deployment[1].tries_remaining);

	ut_asserteq(1, fu_metadata_select(&metadata, &deployment));
	ut_asserteq(0, deployment);
	ut_assertok(fu_metadata_get_selected(&metadata, &deployment));
	ut_asserteq(0, deployment);

	return 0;
}

LIB_TEST(lib_test_fu_metadata_boot_once_cross_stage, 0);

static int lib_test_fu_metadata_redundancy(struct unit_test_state *uts)
{
	struct fu_metadata_store store;
	struct fu_test_store data;
	struct fu_metadata metadata;
	u8 newest;

	fu_test_setup(&data, &store);
	fu_metadata_init(&metadata, 0);
	ut_assertok(fu_metadata_save(&store, &metadata));
	ut_assertok(fu_metadata_save(&store, &metadata));
	newest = metadata.source_copy;
	data.copy[newest][0] ^= 0xff;

	ut_assertok(fu_metadata_load(&store, &metadata));
	ut_asserteq(1, metadata.sequence);

	return 0;
}

LIB_TEST(lib_test_fu_metadata_redundancy, 0);

static int lib_test_fu_release_transaction(struct unit_test_state *uts)
{
	struct fu_metadata metadata;
	u8 deployment;
	int ret;

	fu_metadata_init(&metadata, 0);
	metadata.deployment[0].release_version = 3;

	ut_assertok(fu_metadata_begin_release(&metadata, 4, &deployment));
	ut_asserteq(1, deployment);
	ut_asserteq(FU_SLOT_NONE, metadata.pending_deployment);
	ut_asserteq(FU_STATE_WRITING, metadata.deployment[deployment].state);

	ret = fu_metadata_stage_component(&metadata, deployment,
					  FU_COMPONENT_KERNEL, 1, 10, 20);
	ut_assertok(ret);
	ret = fu_metadata_stage_component(&metadata, deployment,
					  FU_COMPONENT_ROOTFS, 1, 11, 21);
	ut_assertok(ret);
	ut_asserteq(FU_SLOT_NONE, metadata.pending_deployment);

	ut_assertok(fu_metadata_commit_release(&metadata, deployment, 3));
	ut_asserteq(deployment, metadata.pending_deployment);
	ut_asserteq(FU_STATE_READY, metadata.deployment[deployment].state);
	ut_asserteq(4, metadata.deployment[deployment].release_version);
	ut_asserteq(1, metadata.deployment[deployment]
				.component[FU_COMPONENT_KERNEL].slot);
	ut_asserteq(1, metadata.deployment[deployment]
				.component[FU_COMPONENT_ROOTFS].slot);

	return 0;
}

LIB_TEST(lib_test_fu_release_transaction, 0);

static int lib_test_fu_release_rejects_downgrade(struct unit_test_state *uts)
{
	struct fu_metadata metadata;
	u8 deployment;

	fu_metadata_init(&metadata, 0);
	metadata.deployment[0].release_version = 7;

	ut_asserteq(-EPERM,
		    fu_metadata_begin_release(&metadata, 6, &deployment));
	ut_asserteq(FU_SLOT_NONE, metadata.pending_deployment);

	return 0;
}

LIB_TEST(lib_test_fu_release_rejects_downgrade, 0);
