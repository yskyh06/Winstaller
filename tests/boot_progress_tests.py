"""Boot copy errors and monotonic stage state, without disks or mounts."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv[1]).resolve())
ROOT = Path(__file__).resolve().parents[1]
sys.argv = [sys.argv[0]]

class BootTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='winstaller-boot-')
        self.root = Path(self.tmp.name)
        self.state = self.root/'state'
        self.state.mkdir()
        (self.state/'enabled').write_text('1\n')
        self.env = dict(os.environ, WINSTALLER_BOOT_STATE_DIR=str(self.state))

    def tearDown(self):
        self.tmp.cleanup()

    def run_boot(self, *args):
        return subprocess.run([BINARY, *map(str, args)], env=self.env, capture_output=True, timeout=10)

    def test_stage_order_and_validation(self):
        self.assertEqual(self.run_boot('--state', 85, 'Starting services').returncode, 0)
        self.assertEqual(self.run_boot('--state', 12, 'Retrying media').returncode, 0)
        self.assertEqual((self.state/'state').read_text(), '85\nRetrying media\n')
        self.assertNotEqual(self.run_boot('--state', '100oops', 'Ready').returncode, 0)
        self.assertNotEqual(self.run_boot('--state', 101, 'Ready').returncode, 0)

    def test_copy_success(self):
        source = self.root/'source.sfs'
        target = self.root/'target.sfs'
        source.write_bytes(b'live-image' * 100000)
        result = self.run_boot('--copy', source, target)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(source.read_bytes(), target.read_bytes())
        self.assertEqual((self.state/'state').read_text(), '80\nMounting the live system\n')

    def test_copier_failure_does_not_complete(self):
        tools = self.root/'bin'
        tools.mkdir()
        copier = tools/'cp'
        copier.write_text('#!/bin/sh\nprintf partial > "$3"\nexit 1\n')
        copier.chmod(0o755)
        self.env['PATH'] = str(tools)
        source = self.root/'source.sfs'
        source.write_bytes(b'live-image' * 10000)
        result = self.run_boot('--copy', source, self.root/'target.sfs')
        self.assertNotEqual(result.returncode, 0)
        text = (self.state/'state').read_text()
        self.assertIn('RAM copy failed', text)
        self.assertLess(int(text.splitlines()[0]), 80)

    def test_disabled_display_does_not_write_state(self):
        (self.state/'enabled').unlink()
        self.assertEqual(self.run_boot('--state', 95, 'Initializing').returncode, 0)
        self.assertFalse((self.state/'state').exists())

    def test_upstream_hook_drift_rejected(self):
        spec = importlib.util.spec_from_file_location('prepare', ROOT/'scripts/prepare_boot_progress.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        source = self.root/'archiso'
        source.write_text('a changed upstream hook\n')
        with self.assertRaisesRegex(ValueError, 'changed'):
            module.prepare(self.root, source, 'test')

unittest.main()
