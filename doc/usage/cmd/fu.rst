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

A release FIT is a manifest, signed in production, containing component file names and complete
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

On platforms without an SPL selector, "fu select" selects a deployment and
persists the decreased attempt count before booting an unconfirmed deployment.
On platforms with the trusted "spl-selects" property, the first mutable SPL
stage has already persisted that transition; "fu select" only exports the same
selection to the U-Boot environment. It exports:

- "fu_deployment"
- "fu_bootloader_slot"
- "fu_kernel_slot" and "fu_kernel_offset"
- "fu_rootfs_slot" and "fu_rootfs_offset"

After Linux health checks pass, it records the booted deployment in the
persistent U-Boot environment. The next U-Boot boot calls "fu mark-good" before
selection. If all attempts are used without confirmation, the next R5 SPL boot
marks the pending deployment failed and loads the last-good deployment.

AM62x RAW A/B boot flow
----------------------

For the "am62x-sd-ab-v1" layout the boot chain is::

    ROM -> tiboot3_a -> tispl_a or tispl_b -> uboot_a or uboot_b
        -> kernel_a or kernel_b -> rootfs_a or rootfs_b

The ROM always loads "tiboot3_a" at offset 0. A bootloader update package
contains the complete AM62x boot chain: "tiboot3", "tispl", and "uboot". The
command writes them to the target slot in reverse boot order, with "tiboot3"
last, before committing the pending deployment metadata.

The AM62x offsets and sectors are:

================== ============ ============
Region             Byte offset  512-byte LBA
================== ============ ============
tiboot3_a          0x00000000   0x0000
tispl_a            0x00100000   0x0800
uboot_a            0x00300000   0x1800
metadata copy 0    0x00500000   0x2800
metadata copy 1    0x00510000   0x2880
environment        0x00600000   0x3000
environment backup 0x00620000   0x3100
tiboot3_b          0x00700000   0x3800
tispl_b            0x00800000   0x4000
uboot_b            0x00a00000   0x5000
kernel_a           0x01000000   0x8000
kernel_b           0x02800000   0x14000
rootfs_a           0x04000000   0x20000
rootfs_b           0x0c000000   0x60000
================== ============ ============

A factory image must install "tiboot3.bin", "tispl.bin", and "u-boot.img" at
the A offsets above, then initialize metadata once::

    fu init a
    fu status

The first boot also auto-initializes completely erased metadata. Explicit
"fu init" remains the recommended factory step and must not be used as a normal
upgrade command. After first booting this U-Boot over an older installation,
load the new compiled-in environment once:

    env default -a
    saveenv

This installs the RAW A/B boot script, redundant MMC environment settings, and
the default network addresses.

Component FIT requirements
--------------------------

Every component FIT default configuration must contain::

    fu,product = "ti-am62x";
    fu,layout-id = "am62x-sd-ab-v1";
    fu,version = <VERSION>;
    fu,rollback-index = <ROLLBACK_INDEX>;

"VERSION" describes the component version. "ROLLBACK_INDEX" must never be lower
than the installed component value.

A kernel package uses "fu,type = kernel" and references both "kernel" and "fdt"
properties. The complete FIT is written to the kernel RAW region and is booted
with "bootm". Every referenced image must contain a hash node.

A rootfs package uses "fu,type = rootfs" and references the SquashFS image with
the "firmware" property. The referenced image data, not the FIT wrapper, is
written to the rootfs RAW region.

A bootloader package uses "fu,type = bootloader". Its configuration must
reference images named exactly "tiboot3", "tispl", and "uboot". A minimal
unsigned ITS for development is::

    /dts-v1/;

    / {
        description = "AM62x bootloader update";
        #address-cells = <1>;

        images {
            tiboot3 {
                data = /incbin/("tiboot3.bin");
                type = "firmware";
                arch = "arm";
                compression = "none";
                hash-1 { algo = "sha256"; };
            };
            uboot {
                data = /incbin/("u-boot.img");
                type = "firmware";
                arch = "arm64";
                compression = "none";
                hash-1 { algo = "sha256"; };
            };
            tispl {
                data = /incbin/("tispl.bin");
                type = "firmware";
                arch = "arm64";
                compression = "none";
                hash-1 { algo = "sha256"; };
            };
        };

        configurations {
            default = "conf-1";
            conf-1 {
                firmware = "tiboot3";
                loadables = "tispl", "uboot";
                fu,type = "bootloader";
                fu,product = "ti-am62x";
                fu,layout-id = "am62x-sd-ab-v1";
                fu,version = <2>;
                fu,rollback-index = <2>;
            };
        };
    };

Build and verify it with::

    mkimage -f bootloader.its bootloader.itb
    fu verify tftp bootloader.itb

Release manifest
----------------

A release FIT configuration uses "fu,type = release" and lists one or more
unique components in "fu,components". Each component needs a TFTP file name and
the lowercase hexadecimal SHA-256 digest of the complete component FIT::

    fu,type = "release";
    fu,product = "ti-am62x";
    fu,layout-id = "am62x-sd-ab-v1";
    fu,version = <2>;
    fu,rollback-index = <2>;
    fu,components = "bootloader", "kernel", "rootfs";
    fu,bootloader-file = "release/bootloader.itb";
    fu,bootloader-sha256 = "<64 hexadecimal characters>";
    fu,kernel-file = "release/kernel.itb";
    fu,kernel-sha256 = "<64 hexadecimal characters>";
    fu,rootfs-file = "release/rootfs.itb";
    fu,rootfs-sha256 = "<64 hexadecimal characters>";
    fu,allow-bootloader;

The release configuration must also reference a hashed "firmware" image.
Bootloader updates require both "fu,allow-bootloader" in the manifest and
"allow-bootloader-update" in the trusted control device tree. The command
writes and verifies every component before committing one pending deployment.

Linux health confirmation
-------------------------

The AM62x default environment is redundant on MMC device 1. Linux uses::

    /dev/mmcblk1 0x600000 0x10000
    /dev/mmcblk1 0x620000 0x10000

as "/etc/fw_env.config". The kernel command line contains
"fu.deployment=<id>". After all health checks pass, record that id and reboot::

    fw_setenv fu_confirm 1
    reboot

Use the actual id from "/proc/cmdline". On the next boot, U-Boot marks that
deployment good, clears "fu_confirm", saves the redundant environment, and then
continues normal selection.

Examples
--------

Load and update directly from TFTP::

    fu update tftp bootloader.itb
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

On AM62x SD RAW boot, the ROM always reads "tiboot3_a" at offset 0 and cannot
select "tiboot3_b" from the metadata. The debug-oriented bootloader update
writes U-Boot first, tispl second, and tiboot3 last in the target slot. Updating
slot A therefore overwrites the ROM-loaded first stage and is not power-fail
safe; a failed update may require reflashing the card. R5 SPL commits one
metadata selection, A53 SPL reuses it, and U-Boot proper loads kernel and rootfs
from that same deployment.

The optional "auto-init" layout property initializes metadata only when both
copies contain erased bytes. Corrupt non-empty metadata still requires an
explicit factory recovery with "fu init".
