# SPDX-License-Identifier: GPL-2.0+
"""Host integration: production image builder, veritysetup and U-Boot parser.
Run HOST_DIR=/path/to/host python3 test/py/tests/test_fw_verity.py.
Only temporary files are modified; no device-mapper or hardware writes.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
WORKSPACE = ROOT.parents[1]
HOST = Path(os.environ.get('HOST_DIR', WORKSPACE / 'output/am625/host'))
BUILDER = WORKSPACE / 'bsp-base-external/support/scripts/rootfs-verity.sh'

class RootfsVerity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='fw-verity-test-')
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.dtb = self.dir/'board.dtb'
        self.source = self.dir/'rootfs.squashfs'
        self.image = self.dir/'rootfs-verity.squashfs'
        self.source.write_bytes(bytes(range(256)) * 3201)
        self.dtb.write_bytes(subprocess.check_output([str(HOST/'bin/dtc'), '-I', 'dts', '-O', 'dtb'], input=b'/dts-v1/; / { chosen {}; };'))
        subprocess.run([str(BUILDER), str(HOST), str(self.source), str(self.image), str(self.dtb)], check=True, capture_output=True)
        self.tool = HOST/'sbin/veritysetup'
        if not self.tool.exists(): self.tool = HOST/'bin/veritysetup'
        self.include = self.dir/'include'
        self.include.mkdir()
        shutil.copy(ROOT/'include/fw_verity.h', self.include)
        self.wrapper = self.dir/'parser.c'
        self.wrapper.write_text(r'''#include <fw_verity.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *base = malloc(n + 8), *dtb = base ? base + (argc == 5 ? 4 : 0) : NULL, args[768];
    if (!dtb || fread(dtb, 1, n, f) != (size_t)n) return 2;
    fclose(f);
    int rc = fw_verity_args(dtb, n, argv[2], strtoull(argv[3], NULL, 0), args, sizeof(args));
    free(base); if (rc) { fprintf(stderr, "error=%d\n", rc); return 1; }
    puts(args); return 0;
}
''')
        self.parser = self.dir/'parser'
        subprocess.run(['cc','-DUSE_HOSTCC','-D_POSIX_C_SOURCE=200809L','-std=c11','-Wall','-Wextra','-Werror',
            '-I'+str(self.include),'-I'+str(ROOT/'scripts/dtc/libfdt'),str(ROOT/'lib/fw_verity.c'),str(self.wrapper),
            str(ROOT/'scripts/dtc/libfdt/fdt.c'),str(ROOT/'scripts/dtc/libfdt/fdt_ro.c'),'-o',str(self.parser)],check=True)

    def prop(self, name, kind='s'):
        return subprocess.check_output([str(HOST/'bin/fdtget'),'-t',kind,str(self.dtb),'/chosen/fw-verity',name],text=True).strip()

    def verify(self, image=None):
        blocks = int(self.prop('data-blocks','u'))
        return subprocess.run([str(self.tool),'verify','--format=1','--hash=sha256','--no-superblock',
            '--data-block-size=4096','--hash-block-size=4096','--data-blocks='+str(blocks),
            '--hash-offset='+str(blocks*4096),'--salt='+self.prop('salt'),str(image or self.image),
            str(image or self.image), self.prop('root-hash')],capture_output=True)

    def parse(self, device='/dev/mmcblk1p13', capacity=128*1024*1024):
        return subprocess.run([str(self.parser),str(self.dtb),device,str(capacity)],text=True,capture_output=True)

    def test_valid_image_and_both_slots(self):
        self.assertEqual(self.verify().returncode,0)
        for device in ['/dev/mmcblk1p13','/dev/mmcblk1p14']:
            result=self.parse(device)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('root=/dev/ram0 rdinit=/init',result.stdout)
            self.assertIn('fw.rootdev='+device,result.stdout)
            self.assertNotIn('dm-mod.create',result.stdout)
            self.assertIn('fw.verity=1',result.stdout)
        self.assertNotEqual(self.parse(capacity=4096).returncode,0)
        self.assertNotEqual(self.parse(device='/dev/x panic=0').returncode,0)

    def test_fit_property_alignment(self):
        result = subprocess.run([str(self.parser), str(self.dtb), '/dev/mmcblk1p13',
                                 str(128*1024*1024), 'unaligned'], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_data_and_hash_tree_corruption_detected(self):
        for offset in [13,int(self.prop('hash-start-block','u'))*4096+13]:
            damaged=self.dir/'damaged'
            data=bytearray(self.image.read_bytes());data[offset]^=1;damaged.write_bytes(data)
            self.assertNotEqual(self.verify(damaged).returncode,0)
        self.assertEqual(self.verify().returncode,0)

    def test_deterministic_regeneration_and_invalid_parameters(self):
        original=self.image.read_bytes();root=self.prop('root-hash')
        subprocess.run([str(BUILDER),str(HOST),str(self.source),str(self.image),str(self.dtb)],check=True,capture_output=True)
        self.assertEqual(self.image.read_bytes(),original)
        self.assertEqual(self.prop('root-hash'),root)
        subprocess.run([str(HOST/'bin/fdtput'),'-t','u',str(self.dtb),'/chosen/fw-verity','hash-start-block','1'],check=True)
        self.assertNotEqual(self.parse().returncode,0)

if __name__ == '__main__': unittest.main()
