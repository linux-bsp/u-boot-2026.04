# SPDX-License-Identifier: GPL-2.0+
"""Host regression checks; no board/network I/O. Run this file with python3.

Boot scripts use Bash command doubles for control-flow checks, not hardware
emulation. The actual migration function is compiled against an in-memory env.
"""
import ast
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
BOARD = ROOT / 'board/ti/am62x'


def defaults():
    text = subprocess.check_output([
        'cc', '-E', '-P', '-x', 'assembler-with-cpp', '-I', str(ROOT / 'include'),
        str(BOARD / 'am62x_raw_ab.env')], text=True)
    header = subprocess.check_output([
        'awk', '-f', str(ROOT / 'scripts/env2string.awk')], input=text, text=True)
    value = ast.literal_eval(header.split('CONFIG_EXTRA_ENV_TEXT ', 1)[1].strip())
    return dict(item.split('=', 1) for item in value.split('\0') if item)


COMMANDS = r'''
run() { local name; for name in "$@"; do eval "${!name}" || return $?; done; }
setenv() { local name="$1"; shift; if test "$#" = 0; then unset "$name"; else printf -v "$name" '%s' "$*"; fi; }
setexpr() { local name="$1"; shift; printf -v "$name" '%d' "$(( $* ))"; }
fw() {
    echo "CMD:fw $*"
    if test "$1" = select; then
        test "$FAIL" != select || return 1
        fw_rootfs_slot=b; fw_deployment=1; fw_kernel_offset=0x1000000; fw_kernel_size=0x1800000
    fi
}
mmc() { echo "CMD:mmc $*"; test "$FAIL" != "mmc-$1"; }
tftpboot() { echo "CMD:tftp $*"; test "$FAIL" != "$2"; }
wdt() { echo "CMD:wdt $*"; return 0; }
saveenv() { echo 'CMD:saveenv'; }
bootm() { echo "BOOTM:$*"; echo "ARGS:$bootargs"; }
booti() { echo "BOOTI:$*"; echo "ARGS:$bootargs"; }
run bootcmd
rc=$?
echo "RESULT:$rc"
'''


class BootEnvironment(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.env = defaults()

    def boot(self, tftp, nfs, fail='', **extra):
        values = dict(self.env, tftp_kernel=tftp, nfs_root=nfs, FAIL=fail,
                      fw_confirm='1', ipaddr='192.168.10.211', serverip='192.168.10.246',
                      gatewayip='192.168.10.1')
        values.update(extra)
        setup = '\n'.join(f'{key}={shlex.quote(value)}' for key, value in values.items())
        return subprocess.check_output(['bash', '--noprofile', '--norc', '-c', setup + '\n' + COMMANDS],
                                       env={'PATH': os.environ['PATH'], 'LC_ALL': 'C'}, text=True)

    def test_four_combinations(self):
        for tftp, nfs in [('0', '0'), ('1', '0'), ('0', '1'), ('1', '1')]:
            with self.subTest(tftp=tftp, nfs=nfs):
                out = self.boot(tftp, nfs)
                self.assertIn('RESULT:0', out)
                self.assertIn('BOOTI:' if tftp == '1' else 'BOOTM:', out)
                self.assertEqual('CMD:tftp' in out, tftp == '1')
                self.assertEqual('CMD:mmc read' in out, tftp == '0')
                self.assertEqual('CMD:fw select' in out, (tftp, nfs) != ('1', '1'))
                if nfs == '1':
                    self.assertIn('root=/dev/nfs rw', out)
                    self.assertIn('nfsroot=192.168.10.246:/srv/nfs/sadboard-am625,vers=3,tcp', out)
                    self.assertIn('ip=192.168.10.211:192.168.10.246:192.168.10.1:255.255.255.0:am625:eth0:off', out)
                    self.assertNotIn('rootfstype=squashfs', out)
                else:
                    self.assertIn('root=/dev/mmcblk1p14 rootfstype=squashfs ro rootwait', out)
                product = (tftp, nfs) == ('0', '0')
                for marker in ['CMD:fw mark-good', 'CMD:saveenv', 'CMD:wdt start', 'fw.deployment=1']:
                    self.assertEqual(marker in out, product, marker)

    def test_missing_flags_preserve_product_boot(self):
        out = self.boot('', '')
        self.assertIn('BOOTM:', out)
        self.assertIn('CMD:fw mark-good', out)

    def test_network_paths_are_configurable(self):
        out = self.boot('1', '1', tftp_kernel_file='debug/Image', tftp_fdt_file='debug/board.dtb',
                        nfsroot='/exports/test', nfsopts='vers=3,tcp,nolock', netdev='eth1')
        for expected in ['CMD:tftp 0x90000000 debug/Image', 'CMD:tftp 0x88000000 debug/board.dtb',
                         'nfsroot=192.168.10.246:/exports/test,vers=3,tcp,nolock', ':eth1:off']:
            self.assertIn(expected, out)

    def test_failed_download_never_boots_or_falls_back(self):
        for failure in ['Image', 'k3-am625-alientek.dtb']:
            with self.subTest(failure=failure):
                out = self.boot('1', '1', fail=failure)
                self.assertIn('RESULT:1', out)
                self.assertNotIn('BOOTI:', out)
                self.assertNotIn('BOOTM:', out)
                self.assertNotIn('CMD:mmc', out)
                if failure == 'Image':
                    self.assertNotIn('CMD:tftp 0x88000000', out)

    def test_storage_failures_stop_mixed_boot(self):
        for tftp, nfs, failure in [('1', '0', 'select'), ('0', '1', 'select'),
                                   ('0', '1', 'mmc-dev'), ('0', '1', 'mmc-read')]:
            with self.subTest(tftp=tftp, nfs=nfs, failure=failure):
                out = self.boot(tftp, nfs, fail=failure)
                self.assertIn('RESULT:1', out)
                self.assertNotIn('BOOTI:', out)
                self.assertNotIn('BOOTM:', out)

    def test_full_network_boot_does_not_require_selection(self):
        self.assertIn('BOOTI:', self.boot('1', '1', fail='select'))

    def test_compiled_environment_migration(self):
        source = (BOARD / 'evm.c').read_text()
        start = source.index('static void migrate_fw_environment(void)')
        function = source[start:source.index('\n}\n', start) + 3]
        entries = ',\n'.join('{' + json.dumps(k) + ', ' + json.dumps(v) + '}' for k, v in self.env.items())
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct entry { const char *key; const char *value; };
static struct entry factory[] = { FACTORY };
static struct entry current[256];
static int count, saves;
static const char *fail_import;
static const char *lookup(struct entry *items, size_t n, const char *key) {
    for (size_t i = 0; i < n; i++) if (!strcmp(items[i].key, key)) return items[i].value;
    return NULL;
}
static const char *env_get(const char *key) { return lookup(current, count, key); }
static int env_set(const char *key, const char *value) {
    for (int i = 0; i < count; i++) if (!strcmp(current[i].key, key)) {
        current[i].value = value ? strdup(value) : NULL; return 0;
    }
    assert(count < 256);
    current[count++] = (struct entry){strdup(key), value ? strdup(value) : NULL};
    return 0;
}
static int env_get_default_into(const char *key, char *value, size_t size) {
    const char *found = lookup(factory, ARRAY_SIZE(factory), key);
    if (!found) return -1;
    return snprintf(value, size, "%s", found);
}
static int env_set_default_vars(int n, char *const vars[], int flags) {
    (void)flags;
    for (int i = 0; i < n; i++) {
        if (fail_import && !strcmp(fail_import, vars[i])) return 0;
        env_set(vars[i], lookup(factory, ARRAY_SIZE(factory), vars[i]));
    }
    return 1;
}
static int env_save(void) { saves++; return 0; }
FUNCTION
int main(void) {
    env_set("fw_env_version", "2");
    env_set("ipaddr", "192.168.10.211"); env_set("serverip", "192.168.10.246");
    env_set("gatewayip", "192.168.10.1"); env_set("netmask", "255.255.255.0");
    env_set("ethaddr", "1c:63:49:22:c4:f7");
    env_set("tftp_kernel", "1"); env_set("nfsroot", "/custom/root");
    migrate_fw_environment();
    assert(!strcmp(env_get("fw_env_version"), "3") && saves == 1);
    assert(!strcmp(env_get("ipaddr"), "192.168.10.211"));
    assert(!strcmp(env_get("serverip"), "192.168.10.246"));
    assert(!strcmp(env_get("gatewayip"), "192.168.10.1"));
    assert(!strcmp(env_get("netmask"), "255.255.255.0"));
    assert(!strcmp(env_get("ethaddr"), "1c:63:49:22:c4:f7"));
    assert(!strcmp(env_get("tftp_kernel"), "1"));
    assert(!strcmp(env_get("nfsroot"), "/custom/root"));
    assert(!strcmp(env_get("nfs_root"), "0"));
    assert(!strcmp(env_get("tftp_kernel_file"), "Image"));
    assert(!strcmp(env_get("fw_boot"), lookup(factory, ARRAY_SIZE(factory), "fw_boot")));
    migrate_fw_environment(); assert(saves == 1);
    count = 0; saves = 0; env_set("fw_env_version", "2"); fail_import = "nfsroot";
    migrate_fw_environment();
    assert(!strcmp(env_get("fw_env_version"), "2") && saves == 0);
    fail_import = NULL; migrate_fw_environment();
    assert(!strcmp(env_get("fw_env_version"), "3") && saves == 1);
    assert(!strcmp(env_get("tftp_kernel"), "0"));
    assert(!strcmp(env_get("nfs_root"), "0"));
    return 0;
}
'''.replace('FACTORY', entries).replace('FUNCTION', function)
        with tempfile.TemporaryDirectory(prefix='am62x-env-') as directory:
            path = Path(directory)
            (path / 'test.c').write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', str(path / 'test.c'),
                            '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True, stdout=subprocess.PIPE, text=True)


if __name__ == '__main__':
    unittest.main()
