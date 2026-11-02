#!/usr/bin/env python3
"""Exercise real fw commands against sandbox MMC; never touches physical storage.
Usage: python3 test/fw-kernel-sandbox.py BUILD_DIR HOST_BIN
"""
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
host = Path(sys.argv[2]).resolve()
env = dict(os.environ, PATH=str(host)+':'+os.environ['PATH'])
with tempfile.TemporaryDirectory(prefix='fw-kernel-sandbox-') as work:
    root = Path(work)
    (root/'kernel').write_bytes(b'SANDBOX_KERNEL_PAYLOAD_12345678')
    (root/'ramdisk').write_bytes(b'SANDBOX_RAMDISK_PAYLOAD_12345678')
    subprocess.run([str(host/'dtc'), '-I', 'dts', '-O', 'dtb', '-o', str(root/'board.dtb')],
        input=b'/dts-v1/; / { compatible="sandbox"; };', check=True, env=env)
    for version in (2, 3):
        its = root/f'kernel-{version}.its'
        its.write_text(f'''/dts-v1/;
/ {{ description="sandbox kernel"; #address-cells=<1>;
images {{
kernel {{ description="kernel"; data=/incbin/("kernel"); type="kernel"; arch="arm64";
os="linux"; compression="none"; load=<0x80000000>; entry=<0x80000000>; hash {{ algo="sha256"; }}; }};
ramdisk {{ description="recovery"; data=/incbin/("ramdisk"); type="ramdisk"; arch="arm64";
os="linux"; compression="none"; hash {{ algo="sha256"; }}; }};
fdt {{ description="dtb"; data=/incbin/("board.dtb"); type="flat_dt"; arch="arm64";
compression="none"; hash {{ algo="sha256"; }}; }};
}};
configurations {{ default="c"; c {{ description="sandbox"; kernel="kernel"; fdt="fdt"; ramdisk="ramdisk";
fw,type="kernel"; fw,product="sandbox"; fw,layout-id="sandbox-raw-v1";
fw,version=<{version}>; fw,rollback-index=<0>; }}; }};
}};
''')
        subprocess.run([str(host/'mkimage'), '-f', str(its), str(root/f'kernel-{version}.itb')],
                       env=env, check=True, stdout=subprocess.DEVNULL)
    valid=(root/'kernel-2.itb').read_bytes()
    assert len(valid)<=4096
    (root/'corrupt.itb').write_bytes(valid.replace(b'SANDBOX_KERNEL',b'CORRUPT_KERNEL'))
    (root/'corrupt-ramdisk.itb').write_bytes(valid.replace(b'SANDBOX_RAMDISK',b'CORRUPT_RAMDISK'))
    oversized=bytearray(valid); oversized[4:8]=(8192).to_bytes(4,'big')
    (root/'oversized.itb').write_bytes(oversized)
    commands=['fw init a', 'fw select',
        'if fw load-kernel 2000000; then echo BAD_BLANK; else echo PASS_BLANK; fi',
        f'host load hostfs - 1000000 {root}/kernel-2.itb',
        'iminfo 1000000', 'fw update addr 1000000 ${filesize}', 'fw select',
        'if fw load-kernel 2000000; then echo PASS_VALID; else echo BAD_VALID; fi',
        'setenv ota_serverip 192.0.2.1', 'setenv kernel_addr_r 2000000',
        'if fw recover-kernel; then echo PASS_HEALTHY; else echo BAD_HEALTHY; fi']
    for file, label in [('corrupt.itb','HASH'),('corrupt-ramdisk.itb','RAMDISK'),('kernel-3.itb','VERSION'),('oversized.itb','SIZE')]:
        commands += [f'host load hostfs - 1000000 {root}/{file}', 'mmc dev 0',
            'mmc write 1000000 48 8',
            f'if fw load-kernel 2000000; then echo BAD_{label}; else echo PASS_{label}; fi']
    commands += ['fw init a', 'fw select',
        'if fw recover-kernel; then echo BAD_UNKNOWN; else echo PASS_UNKNOWN; fi', 'poweroff']
    result=subprocess.run([str(build/'u-boot'), '-T', '-c', '; '.join(commands)],
        cwd=root, env=env, capture_output=True, text=True, timeout=60)
    print(result.stdout)
    print(result.stderr, file=sys.stderr)
    assert result.returncode==0, result.returncode
    printed = re.findall(r'Hash value:\s*([0-9a-f]+)', result.stdout)
    assert printed and all(len(value) == 8 for value in printed), printed
    host_info = subprocess.check_output([str(host/'dumpimage'), '-l', str(root/'kernel-2.itb')], text=True)
    host_hashes = re.findall(r'Hash value:\s*([0-9a-f]+)', host_info)
    assert host_hashes and all(len(value) == 64 for value in host_hashes), host_hashes
    for label in ('BLANK','VALID','HEALTHY','HASH','RAMDISK','VERSION','SIZE','UNKNOWN'):
        assert 'PASS_'+label in result.stdout and 'BAD_'+label not in result.stdout, label
    print('PASS: bounded verified kernel loading, healthy no-op recovery, unknown-version refusal')
