"""Seamless co-op (BBSC): run.sh's environment for the module (gpu/bbsc) and the launcher's switch."""
from paths import ROOT
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

BBSC_KEYS = ('BB_ONLINE', 'BB_SHADNET_SERVER', 'BB_SHADNET_WEBAPI', 'BB_COMMUNITY_SERVER',
             'BBSC_USER_DIR', 'BBSC_DATA_DIR', 'SHADPS4_BLOODBORNE_SEAMLESS_COOP')


class RunEnvironmentTests(unittest.TestCase):
    """run.sh with stand-ins for the preparation and the probe, which records its environment."""

    def environment(self, **extra):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)
            out = data / 'out'
            out.mkdir()
            # The patch compiler checks its writes against the eboot's load segments.
            elf = bytearray(120)
            struct.pack_into('<Q', elf, 0x20, 64)
            struct.pack_into('<HH', elf, 0x36, 56, 1)
            struct.pack_into('<IIQQQQQQ', elf, 64, 1, 0, 0, 0, 0, 0, 0x6000000, 0)
            (out / 'eboot.elf').write_bytes(elf)
            (data / 'eboot.bin').touch()
            python = data / 'python'
            python.write_text(f'#!{sys.executable}\n'
                              'import subprocess, sys\n'
                              'if sys.argv[1] in ("scripts/patches.py", "scripts/mods.py"):\n'
                              '    sys.exit(subprocess.call([sys.executable, *sys.argv[1:]]))\n')
            python.chmod(0o755)
            probe = data / 'probe'
            probe.write_text(f'#!{sys.executable}\n'
                             'import json, os, sys\n'
                             f'keys = {BBSC_KEYS!r}\n'
                             f'open({str(data / "environment")!r}, "w").write('
                             'json.dumps({key: os.environ.get(key) for key in keys}))\n')
            probe.chmod(0o755)
            env = {key: value for key, value in os.environ.items()
                   if key not in BBSC_KEYS and not key.startswith(('BB_', 'BBSC_'))}
            env.update(BB_PREBUILT='1', BB_PROBE=str(probe), PYTHON=str(python),
                       BB_DATA_DIR=str(data), BB_CONFIG=str(data / 'bbport.ini'),
                       BB_GAME_DIR=str(data), BB_USER_DIR=str(data / 'user'), **extra)
            subprocess.run([shutil.which('bash'), 'run.sh'], cwd=ROOT, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=30)
            values = json.loads((data / 'environment').read_text())
            return {key: value.replace(str(data), '<data>') if value else value
                    for key, value in values.items()}

    def test_seamless_co_op_at_a_bbsc_server(self):
        self.assertEqual(self.environment(BB_BBSC='1', BB_BBSC_SERVER='10.0.0.5'), {
            'BB_ONLINE': '1',
            'BB_SHADNET_SERVER': '10.0.0.5:31313',
            'BB_SHADNET_WEBAPI': 'http://10.0.0.5:31315',
            'BB_COMMUNITY_SERVER': 'http://10.0.0.5:20443',
            'BBSC_USER_DIR': '<data>/user',
            'BBSC_DATA_DIR': '<data>/user/bbsc',
            'SHADPS4_BLOODBORNE_SEAMLESS_COOP': '0',
        })

    def test_seamless_co_op_keeps_the_online_addresses_without_a_bbsc_server(self):
        values = self.environment(BB_BBSC='1', BB_SHADNET_SERVER='example.org:31313')
        self.assertEqual(values['BB_ONLINE'], '1')
        self.assertEqual(values['BB_SHADNET_SERVER'], 'example.org:31313')
        self.assertEqual(values['BB_SHADNET_WEBAPI'], 'http://example.org:31315')

    def test_without_seamless_co_op_nothing_changes(self):
        values = self.environment()
        self.assertEqual(values, dict.fromkeys(BBSC_KEYS))


class LauncherTests(unittest.TestCase):
    def setUp(self):
        try:
            import gi
            gi.require_version('Gtk', '4.0')
            gi.require_version('Adw', '1')
            from gi.repository import Adw  # noqa: F401
        except (ImportError, ValueError) as error:
            self.skipTest(f'no GTK 4 / libadwaita: {error}')
        sys.path.insert(0, str(ROOT / 'launcher'))
        import bbport_launcher
        self.launcher = bbport_launcher

    def environment(self, **settings):
        s = dict(self.launcher.DEFAULTS, game_dir='/games/CUSA03173', user_dir='', **settings)
        with mock.patch.object(self.launcher, 'online_module', return_value=True), \
                mock.patch.object(self.launcher, 'bbsc_module', return_value=True):
            return self.launcher.game_environment(s)

    def test_switch_and_server(self):
        env = self.environment(online=True, bbsc=True, bbsc_server=' 10.0.0.5 ')
        self.assertEqual((env['BB_BBSC'], env['BB_BBSC_SERVER']), ('1', '10.0.0.5'))

    def test_co_op_needs_online_play(self):
        env = self.environment(online=False, bbsc=True, bbsc_server='10.0.0.5')
        self.assertNotIn('BB_BBSC', env)


if __name__ == '__main__':
    unittest.main()
