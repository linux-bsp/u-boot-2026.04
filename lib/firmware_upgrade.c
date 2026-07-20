// SPDX-License-Identifier: GPL-2.0+
#include <firmware_upgrade.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <u-boot/crc.h>
#include <asm/byteorder.h>

#define FU_METADATA_MAGIC 0x46554d44 /* FUMD */
#define FU_METADATA_VERSION 1

struct fu_disk_component {
	u8 slot;
	u8 valid;
	__le16 reserved;
	__le32 version;
	__le32 rollback_index;
} __packed;

struct fu_disk_deployment {
	struct fu_disk_component component[FU_COMPONENT_COUNT];
	u8 state;
	u8 tries_remaining;
	u8 successful;
	u8 reserved;
	__le32 release_version;
} __packed;

struct fu_disk_metadata {
	__le32 magic;
	__le16 format_version;
	__le16 header_size;
	__le32 sequence;
	u8 active_deployment;
	u8 pending_deployment;
	u8 last_good_deployment;
	u8 boot_once_deployment;
	u8 update_state;
	u8 selected_deployment;
	u8 reserved0[2];
	struct fu_disk_deployment deployment[2];
	u8 reserved1[16];
	__le32 crc32;
} __packed;

static_assert(sizeof(struct fu_disk_metadata) == FU_METADATA_SIZE);

const char *fu_component_name(enum fu_component component)
{
	static const char *const names[] = { "bootloader", "kernel", "rootfs" };

	if (component >= FU_COMPONENT_COUNT)
		return "invalid";

	return names[component];
}

const char *fu_state_name(enum fu_deployment_state state)
{
	static const char *const names[] = {
		"empty",	"writing",   "ready",
		"boot-testing", "confirmed", "failed",
	};

	if (state > FU_STATE_FAILED)
		return "invalid";

	return names[state];
}

static bool fu_deployment_valid(const struct fu_deployment *deployment)
{
	int i;

	if (deployment->state == FU_STATE_EMPTY ||
	    deployment->state == FU_STATE_FAILED)
		return false;

	for (i = 0; i < FU_COMPONENT_COUNT; i++) {
		if (!deployment->component[i].valid ||
		    deployment->component[i].slot > 1)
			return false;
	}

	return true;
}

void fu_metadata_init(struct fu_metadata *metadata, u8 slot)
{
	struct fu_deployment *deployment;
	int i;

	memset(metadata, 0, sizeof(*metadata));
	metadata->pending_deployment = FU_SLOT_NONE;
	metadata->boot_once_deployment = FU_SLOT_NONE;
	metadata->selected_deployment = 0;
	metadata->source_copy = 1;
	metadata->active_deployment = 0;
	metadata->last_good_deployment = 0;

	deployment = &metadata->deployment[0];
	deployment->state = FU_STATE_CONFIRMED;
	deployment->successful = 1;
	for (i = 0; i < FU_COMPONENT_COUNT; i++) {
		deployment->component[i].slot = slot;
		deployment->component[i].valid = 1;
	}
	metadata->deployment[1].state = FU_STATE_EMPTY;
}

static int fu_metadata_decode(const void *buf, struct fu_metadata *metadata)
{
	const struct fu_disk_metadata *disk = buf;
	u32 expected_crc, actual_crc;
	int i, j;

	if (le32_to_cpu(disk->magic) != FU_METADATA_MAGIC ||
	    le16_to_cpu(disk->format_version) != FU_METADATA_VERSION ||
	    le16_to_cpu(disk->header_size) != FU_METADATA_SIZE)
		return -EINVAL;

	expected_crc = le32_to_cpu(disk->crc32);
	actual_crc = crc32(0, buf, offsetof(struct fu_disk_metadata, crc32));
	if (actual_crc != expected_crc)
		return -EBADMSG;

	memset(metadata, 0, sizeof(*metadata));
	metadata->sequence = le32_to_cpu(disk->sequence);
	metadata->active_deployment = disk->active_deployment;
	metadata->pending_deployment = disk->pending_deployment;
	metadata->last_good_deployment = disk->last_good_deployment;
	metadata->boot_once_deployment = disk->boot_once_deployment;
	metadata->selected_deployment = disk->selected_deployment;
	metadata->update_state = disk->update_state;

	for (i = 0; i < 2; i++) {
		struct fu_deployment *dst = &metadata->deployment[i];
		const struct fu_disk_deployment *src = &disk->deployment[i];

		dst->state = src->state;
		dst->tries_remaining = src->tries_remaining;
		dst->successful = src->successful;
		dst->release_version = le32_to_cpu(src->release_version);
		for (j = 0; j < FU_COMPONENT_COUNT; j++) {
			dst->component[j].slot = src->component[j].slot;
			dst->component[j].valid = src->component[j].valid;
			dst->component[j].version =
				le32_to_cpu(src->component[j].version);
			dst->component[j].rollback_index =
				le32_to_cpu(src->component[j].rollback_index);
		}
	}

	if (metadata->active_deployment > 1 ||
	    metadata->last_good_deployment > 1 ||
	    (metadata->pending_deployment != FU_SLOT_NONE &&
	     metadata->pending_deployment > 1) ||
	    (metadata->boot_once_deployment != FU_SLOT_NONE &&
	     metadata->boot_once_deployment > 1) ||
	    metadata->selected_deployment > 1)
		return -EINVAL;

	return 0;
}

static void fu_metadata_encode(const struct fu_metadata *metadata, void *buf)
{
	struct fu_disk_metadata *disk = buf;
	u32 crc;
	int i, j;

	memset(disk, 0, sizeof(*disk));
	disk->magic = cpu_to_le32(FU_METADATA_MAGIC);
	disk->format_version = cpu_to_le16(FU_METADATA_VERSION);
	disk->header_size = cpu_to_le16(FU_METADATA_SIZE);
	disk->sequence = cpu_to_le32(metadata->sequence);
	disk->active_deployment = metadata->active_deployment;
	disk->pending_deployment = metadata->pending_deployment;
	disk->last_good_deployment = metadata->last_good_deployment;
	disk->boot_once_deployment = metadata->boot_once_deployment;
	disk->selected_deployment = metadata->selected_deployment;
	disk->update_state = metadata->update_state;

	for (i = 0; i < 2; i++) {
		const struct fu_deployment *src = &metadata->deployment[i];
		struct fu_disk_deployment *dst = &disk->deployment[i];

		dst->state = src->state;
		dst->tries_remaining = src->tries_remaining;
		dst->successful = src->successful;
		dst->release_version = cpu_to_le32(src->release_version);
		for (j = 0; j < FU_COMPONENT_COUNT; j++) {
			dst->component[j].slot = src->component[j].slot;
			dst->component[j].valid = src->component[j].valid;
			dst->component[j].version =
				cpu_to_le32(src->component[j].version);
			dst->component[j].rollback_index =
				cpu_to_le32(src->component[j].rollback_index);
		}
	}

	crc = crc32(0, buf, offsetof(struct fu_disk_metadata, crc32));
	disk->crc32 = cpu_to_le32(crc);
}

static bool fu_sequence_newer(u32 lhs, u32 rhs)
{
	return (s32)(lhs - rhs) > 0;
}

int fu_metadata_load(const struct fu_metadata_store *store,
		     struct fu_metadata *metadata)
{
	struct fu_metadata copy[FU_METADATA_COPIES];
	u8 buf[FU_METADATA_SIZE];
	bool valid[FU_METADATA_COPIES] = {};
	int i, ret;

	for (i = 0; i < FU_METADATA_COPIES; i++) {
		ret = store->read(store->ctx, i, buf, sizeof(buf));
		if (!ret && !fu_metadata_decode(buf, &copy[i]))
			valid[i] = true;
	}

	if (!valid[0] && !valid[1])
		return -ENODATA;

	i = valid[1] && (!valid[0] ||
			 fu_sequence_newer(copy[1].sequence, copy[0].sequence));
	*metadata = copy[i];
	metadata->source_copy = i;

	return 0;
}

int fu_metadata_save(const struct fu_metadata_store *store,
		     struct fu_metadata *metadata)
{
	struct fu_metadata check;
	u8 buf[FU_METADATA_SIZE], verify[FU_METADATA_SIZE];
	unsigned int target = 1 - metadata->source_copy;
	int ret;

	metadata->sequence++;
	fu_metadata_encode(metadata, buf);
	ret = store->write(store->ctx, target, buf, sizeof(buf));
	if (ret)
		goto restore_sequence;

	ret = store->read(store->ctx, target, verify, sizeof(verify));
	if (ret)
		goto restore_sequence;
	ret = fu_metadata_decode(verify, &check);
	if (ret || check.sequence != metadata->sequence) {
		ret = ret ?: -EIO;
		goto restore_sequence;
	}

	metadata->source_copy = target;
	return 0;

restore_sequence:
	metadata->sequence--;
	return ret;
}

int fu_metadata_select(struct fu_metadata *metadata, u8 *deployment)
{
	struct fu_deployment *candidate;
	u8 selected;
	bool changed = false;

	if (metadata->boot_once_deployment != FU_SLOT_NONE) {
		selected = metadata->boot_once_deployment;
		if (!fu_deployment_valid(&metadata->deployment[selected]))
			return -EINVAL;
		metadata->boot_once_deployment = FU_SLOT_NONE;
		metadata->selected_deployment = selected;
		*deployment = selected;
		return 1;
	}

	if (metadata->pending_deployment != FU_SLOT_NONE) {
		selected = metadata->pending_deployment;
		candidate = &metadata->deployment[selected];
		if (fu_deployment_valid(candidate) &&
		    candidate->tries_remaining) {
			candidate->state = FU_STATE_BOOT_TESTING;
			candidate->tries_remaining--;
			metadata->selected_deployment = selected;
			*deployment = selected;
			return 1;
		}

		candidate->state = FU_STATE_FAILED;
		metadata->pending_deployment = FU_SLOT_NONE;
		metadata->active_deployment = metadata->last_good_deployment;
		changed = true;
	}

	selected = metadata->active_deployment;
	if (!metadata->deployment[selected].successful ||
	    !fu_deployment_valid(&metadata->deployment[selected]))
		selected = metadata->last_good_deployment;
	if (!metadata->deployment[selected].successful ||
	    !fu_deployment_valid(&metadata->deployment[selected]))
		return -ENOENT;

	if (metadata->selected_deployment != selected) {
		metadata->selected_deployment = selected;
		changed = true;
	}
	*deployment = selected;
	return changed;
}

int fu_metadata_get_selected(const struct fu_metadata *metadata, u8 *deployment)
{
	u8 selected;

	if (!metadata || !deployment)
		return -EINVAL;
	selected = metadata->selected_deployment;
	if (selected > 1 ||
	    !fu_deployment_valid(&metadata->deployment[selected]))
		return -ENOENT;

	*deployment = selected;
	return 0;
}

int fu_metadata_prepare_update(struct fu_metadata *metadata,
			       enum fu_component component, u8 slot,
			       u32 version, u32 rollback_index, u8 attempts,
			       u8 *deployment)
{
	struct fu_deployment *source, *target;
	u8 source_id, target_id;

	if (component >= FU_COMPONENT_COUNT || slot > 1 || !attempts)
		return -EINVAL;

	source_id = metadata->active_deployment;
	source = &metadata->deployment[source_id];
	if (!source->successful || !fu_deployment_valid(source)) {
		source_id = metadata->last_good_deployment;
		source = &metadata->deployment[source_id];
	}
	if (!source->successful || !fu_deployment_valid(source))
		return -ENOENT;

	if (metadata->pending_deployment != FU_SLOT_NONE) {
		target_id = metadata->pending_deployment;
		target = &metadata->deployment[target_id];
		if (!fu_deployment_valid(target))
			return -EINVAL;
	} else {
		target_id = 1 - source_id;
		target = &metadata->deployment[target_id];
		*target = *source;
		metadata->pending_deployment = target_id;
	}

	target->component[component].slot = slot;
	target->component[component].valid = 1;
	target->component[component].version = version;
	target->component[component].rollback_index = rollback_index;
	target->state = FU_STATE_READY;
	target->tries_remaining = attempts;
	target->successful = 0;
	metadata->update_state = FU_STATE_READY;
	*deployment = target_id;

	return 0;
}

int fu_metadata_begin_release(struct fu_metadata *metadata,
			      u32 release_version, u8 *deployment)
{
	struct fu_deployment *source, *target;
	u8 source_id, target_id;

	if (!release_version || !deployment)
		return -EINVAL;
	if (metadata->pending_deployment != FU_SLOT_NONE)
		return -EBUSY;

	source_id = metadata->active_deployment;
	source = &metadata->deployment[source_id];
	if (!source->successful || !fu_deployment_valid(source)) {
		source_id = metadata->last_good_deployment;
		source = &metadata->deployment[source_id];
	}
	if (!source->successful || !fu_deployment_valid(source))
		return -ENOENT;
	if (release_version < source->release_version)
		return -EPERM;

	target_id = 1 - source_id;
	target = &metadata->deployment[target_id];
	*target = *source;
	target->state = FU_STATE_WRITING;
	target->tries_remaining = 0;
	target->successful = 0;
	target->release_version = release_version;
	metadata->update_state = FU_STATE_WRITING;
	*deployment = target_id;

	return 0;
}

int fu_metadata_stage_component(struct fu_metadata *metadata, u8 deployment,
				enum fu_component component, u8 slot,
				u32 version, u32 rollback_index)
{
	struct fu_component_slot *target;

	if (deployment > 1 || component >= FU_COMPONENT_COUNT || slot > 1)
		return -EINVAL;
	if (metadata->deployment[deployment].state != FU_STATE_WRITING)
		return -EINVAL;

	target = &metadata->deployment[deployment].component[component];
	target->slot = slot;
	target->valid = 1;
	target->version = version;
	target->rollback_index = rollback_index;

	return 0;
}

int fu_metadata_commit_release(struct fu_metadata *metadata, u8 deployment,
			       u8 attempts)
{
	struct fu_deployment *target;

	if (deployment > 1 || !attempts)
		return -EINVAL;
	target = &metadata->deployment[deployment];
	if (target->state != FU_STATE_WRITING ||
	    !fu_deployment_valid(target))
		return -EINVAL;

	target->state = FU_STATE_READY;
	target->tries_remaining = attempts;
	target->successful = 0;
	metadata->pending_deployment = deployment;
	metadata->update_state = FU_STATE_READY;

	return 0;
}

int fu_metadata_mark_good(struct fu_metadata *metadata, int deployment)
{
	struct fu_deployment *target;

	if (deployment < 0)
		deployment = metadata->pending_deployment;
	if (deployment < 0 || deployment > 1)
		return -EINVAL;

	target = &metadata->deployment[deployment];
	if (!fu_deployment_valid(target))
		return -EINVAL;
	target->state = FU_STATE_CONFIRMED;
	target->successful = 1;
	target->tries_remaining = 0;
	metadata->active_deployment = deployment;
	metadata->last_good_deployment = deployment;
	metadata->selected_deployment = deployment;
	metadata->pending_deployment = FU_SLOT_NONE;
	metadata->update_state = FU_STATE_CONFIRMED;

	return 0;
}

int fu_metadata_mark_bad(struct fu_metadata *metadata, u8 deployment)
{
	if (deployment > 1)
		return -EINVAL;

	metadata->deployment[deployment].state = FU_STATE_FAILED;
	metadata->deployment[deployment].successful = 0;
	metadata->deployment[deployment].tries_remaining = 0;
	if (metadata->pending_deployment == deployment)
		metadata->pending_deployment = FU_SLOT_NONE;
	if (metadata->active_deployment == deployment)
		metadata->active_deployment = metadata->last_good_deployment;
	if (metadata->selected_deployment == deployment)
		metadata->selected_deployment = metadata->last_good_deployment;
	metadata->update_state = FU_STATE_FAILED;

	return 0;
}

int fu_metadata_rollback(struct fu_metadata *metadata)
{
	u8 target = metadata->last_good_deployment;

	if (target > 1 || !metadata->deployment[target].successful ||
	    !fu_deployment_valid(&metadata->deployment[target]))
		return -ENOENT;

	if (metadata->pending_deployment != FU_SLOT_NONE)
		metadata->deployment[metadata->pending_deployment].state =
			FU_STATE_FAILED;
	metadata->pending_deployment = FU_SLOT_NONE;
	metadata->boot_once_deployment = FU_SLOT_NONE;
	metadata->active_deployment = target;
	metadata->selected_deployment = target;
	metadata->update_state = FU_STATE_CONFIRMED;

	return 0;
}
