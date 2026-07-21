.. SPDX-License-Identifier: GPL-2.0+

fw command
==========

Synopsis
--------

::

    fw list
    fw status
    fw init <a|b>
    fw verify <tftp file|addr address size>
    fw update <tftp file|addr address size>
    fw update tftp <release-file> all
    fw restore <component> <tftp file|addr address size>
    fw select
    fw boot <deployment> [once]
    fw mark-good [deployment]
    fw mark-bad <deployment>
    fw rollback

Description
-----------

The "fw" command updates component FIT packages in fixed RAW regions on eMMC,
SD, or SPI NOR. It verifies the default FIT configuration, all referenced image
hashes, "fw,product", "fw,layout-id", and the rollback index before writing an
inactive slot. Data is read back before redundant metadata is committed.

Separate component updates made before the next boot are merged into the same
pending deployment. For example, updating rootfs and then kernel creates one
deployment containing both inactive slots.

A release FIT is a manifest, signed in production, containing component file names and complete
file SHA-256 digests. "fw update tftp release.itb all" downloads each listed
component, verifies the manifest digest and the component FIT, and writes all
target slots before committing one pending deployment. A failed download,
verification, or write leaves the current deployment selected. Bootloader
updates additionally require both manifest permission and the trusted layout's
"allow-bootloader-update" property. "fw restore" uses the same verified,
read-back-checked path to recover a component into its inactive slot.

Starting a release update while another deployment is pending replaces that
pending deployment. The manager first persists a non-bootable writing state,
then rebuilds the target from the current confirmed deployment. This permits a
mistaken package to be replaced immediately without booting or confirming it,
while preventing a failed replacement from booting partially overwritten data.

"fw init" is a destructive factory operation for metadata only. It declares
that all components in the selected slot are the installed, confirmed
deployment. It does not write component images.

On platforms without an SPL selector, "fw select" selects a deployment and
persists the decreased attempt count before booting an unconfirmed deployment.
On platforms with the trusted "spl-selects" property, the first mutable SPL
stage has already persisted that transition; "fw select" only exports the same
selection to the U-Boot environment. It exports:

- "fw_deployment"
- "fw_bootloader_slot"
- "fw_kernel_slot" and "fw_kernel_offset"
- "fw_rootfs_slot" and "fw_rootfs_offset"

After Linux health checks pass, it records the booted deployment in the
persistent U-Boot environment. The next U-Boot boot calls "fw mark-good" before
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

    fw init a
    fw status

The first boot also auto-initializes completely erased metadata. Explicit
"fw init" remains the recommended factory step and must not be used as a normal
upgrade command.

The firmware manager uses the "FWMD" metadata magic and "fw,*" FIT properties.
Metadata and packages generated for the old command namespace are intentionally
not accepted. After first booting this U-Boot over an older installation, load
the new compiled-in environment and initialize metadata once::

    env default -a
    saveenv
    fw init a

This installs the RAW A/B boot script, redundant MMC environment settings, and
the default network addresses.

Component FIT requirements
--------------------------

Every component FIT default configuration must contain::

    fw,product = "ti-am62x";
    fw,layout-id = "am62x-sd-ab-v1";
    fw,version = <VERSION>;
    fw,rollback-index = <ROLLBACK_INDEX>;

"VERSION" describes the component version. "ROLLBACK_INDEX" must never be lower
than the installed component value.

A kernel package uses "fw,type = kernel" and references both "kernel" and "fdt"
properties. The complete FIT is written to the kernel RAW region and is booted
with "bootm". Every referenced image must contain a hash node.

A rootfs package uses "fw,type = rootfs" and references the SquashFS image with
the "firmware" property. The referenced image data, not the FIT wrapper, is
written to the rootfs RAW region.

A bootloader package uses "fw,type = bootloader". Its configuration must
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
                fw,type = "bootloader";
                fw,product = "ti-am62x";
                fw,layout-id = "am62x-sd-ab-v1";
                fw,version = <2>;
                fw,rollback-index = <2>;
            };
        };
    };

Build and verify it with::

    mkimage -f bootloader.its bootloader.itb
    fw verify tftp bootloader.itb

Release manifest
----------------

A release FIT configuration uses "fw,type = release" and lists one or more
unique components in "fw,components". Each component needs a TFTP file name and
the lowercase hexadecimal SHA-256 digest of the complete component FIT::

    fw,type = "release";
    fw,product = "ti-am62x";
    fw,layout-id = "am62x-sd-ab-v1";
    fw,version = <2>;
    fw,rollback-index = <2>;
    fw,components = "bootloader", "kernel", "rootfs";
    fw,bootloader-file = "bootloader.itb";
    fw,bootloader-sha256 = "<64 hexadecimal characters>";
    fw,kernel-file = "kernel.itb";
    fw,kernel-sha256 = "<64 hexadecimal characters>";
    fw,rootfs-file = "rootfs.itb";
    fw,rootfs-sha256 = "<64 hexadecimal characters>";
    fw,allow-bootloader;

The release configuration must also reference a hashed "firmware" image.
Bootloader updates require both "fw,allow-bootloader" in the manifest and
"allow-bootloader-update" in the trusted control device tree. The command
writes and verifies every component before committing one pending deployment.

Linux health confirmation
-------------------------

The Buildroot image installs "/usr/sbin/fwctl" and the
"S99fw-mark-good" init script. The kernel command line contains
"fw.deployment=<id>". With "AUTO_MARK_GOOD=1" in "/etc/default/fwctl",
the init script confirms the current deployment directly in redundant metadata
after its optional health check succeeds.

Inspect or confirm the current deployment manually with::

    fwctl status
    fwctl mark-good

Examples
--------

Load and update directly from TFTP::

    fw update tftp bootloader.itb
    fw update tftp kernel.itb
    fw update tftp rootfs.itb
    fw update tftp release.itb all

The AM62x default environment provides shortcuts for these commands::

    run upk    # kernel.itb
    run upr    # rootfs.itb
    run upb    # bootloader.itb
    run upa    # release.itb, all components

Use an image already in memory::

    tftp ${loadaddr} kernel.itb
    fw verify addr ${loadaddr} ${filesize}
    fw update addr ${loadaddr} ${filesize}

Initialize a factory image and select boot offsets::

    fw init a
    fw select

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
explicit factory recovery with "fw init".
