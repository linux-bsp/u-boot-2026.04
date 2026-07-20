.. SPDX-License-Identifier: GPL-2.0+

fu command
==========

Synopsis
--------

::

    fu list
    fu status
    fu init <a|b>
    fu verify <tftp file|addr address size>
    fu update <tftp file|addr address size>
    fu update tftp <release-file> all
    fu restore <component> <tftp file|addr address size>
    fu select
    fu boot <deployment> [once]
    fu mark-good [deployment]
    fu mark-bad <deployment>
    fu rollback

Description
-----------

The "fu" command updates component FIT packages in fixed RAW regions on eMMC,
SD, or SPI NOR. It verifies the default FIT configuration, all referenced image
hashes, "fu,product", "fu,layout-id", and the rollback index before writing an
inactive slot. Data is read back before redundant metadata is committed.

Separate component updates made before the next boot are merged into the same
pending deployment. For example, updating rootfs and then kernel creates one
deployment containing both inactive slots.

A release FIT is a signed manifest containing component file names and complete
file SHA-256 digests. "fu update tftp release.itb all" downloads each listed
component, verifies the manifest digest and the component FIT, and writes all
target slots before committing one pending deployment. A failed download,
verification, or write leaves the current deployment selected. Bootloader
updates additionally require both manifest permission and the trusted layout's
"allow-bootloader-update" property. "fu restore" uses the same verified,
read-back-checked path to recover a component into its inactive slot.

"fu init" is a destructive factory operation for metadata only. It declares
that all components in the selected slot are the installed, confirmed
deployment. It does not write component images.

"fu select" runs from the board boot command before loading Linux. It selects a
deployment and persists the decreased attempt count before booting an
unconfirmed deployment. It exports:

- "fu_deployment"
- "fu_bootloader_slot"
- "fu_kernel_slot" and "fu_kernel_offset"
- "fu_rootfs_slot" and "fu_rootfs_offset"

Linux calls "fu mark-good" after its health checks pass. If all attempts are
used, the next "fu select" marks the pending deployment failed and exports the
last-good deployment.

Examples
--------

Load and update directly from TFTP::

    fu update tftp kernel.itb
    fu update tftp rootfs.itb
    fu update tftp release/release.itb all

Use an image already in memory::

    tftp ${loadaddr} kernel.itb
    fu verify addr ${loadaddr} ${filesize}
    fu update addr ${loadaddr} ${filesize}

Initialize a factory image and select boot offsets::

    fu init a
    fu select

Security
--------

Production systems must put required FIT public keys in the U-Boot control
device tree. Hash-only FIT files detect corruption but do not authenticate the
publisher. The fixed layout must also be part of the trusted control device
tree.

The command can write bootloader banks in the order U-Boot, SPL, tiboot3. This
does not by itself provide bootloader rollback. The platform ROM or an earlier
immutable selector must be able to choose the previous bank. Boards without
that mechanism must omit "allow-bootloader-update".

The optional "auto-init" layout property initializes metadata only when both
copies contain erased bytes. Corrupt non-empty metadata still requires an
explicit factory recovery with "fu init".
