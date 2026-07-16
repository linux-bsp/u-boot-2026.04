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
