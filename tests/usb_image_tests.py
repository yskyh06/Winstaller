import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parent.parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT))
from scripts import create_usb_image as b

VALID_7Z = """
Listing archive: /fake.iso

--
Path = /fake.iso
Type = UDF

----------
Path = EFI/BOOT/BOOTX64.EFI
Folder = -
Size = 1024
Symbolic Link =

Path = arch/boot/x86_64/vmlinuz-linux
Folder = -
Size = 2048
Symbolic Link =

Path = arch/boot/x86_64/initramfs-linux.img
Folder = -
Size = 4096
Symbolic Link =

Path = arch/x86_64/airootfs.sfs
Folder = -
Size = 8192
Symbolic Link =

Path = boot/grub/grub.cfg
Folder = -
Size = 512
Symbolic Link =
"""


class TestUsbImage(unittest.TestCase):
    @patch("subprocess.run")
    def test_real_7z_listing_success(self, mock_run):
        mock_run.return_value = MagicMock(stdout=VALID_7Z)
        b.validate_iso_tree("/fake.iso")

    @patch("subprocess.run")
    def test_traversal_and_link_rejection(self, mock_run):
        for bad in [
            "Path = ../evil\nFolder = -\nSize = 10\nSymbolic Link = \n",
            "Path = sub/link\nFolder = -\nSize = 10\nSymbolic Link = /target\n",
        ]:
            mock_run.return_value = MagicMock(stdout="----------\n" + bad)
            with self.assertRaises(ValueError):
                b.validate_iso_tree("/fake.iso")

    @patch("subprocess.run")
    def test_missing_required_path(self, mock_run):
        listing = "----------\nPath = EFI/BOOT/BOOTX64.EFI\nFolder = -\nSize = 10\nSymbolic Link = \n"
        mock_run.return_value = MagicMock(stdout=listing)
        with self.assertRaises(ValueError):
            b.validate_iso_tree("/fake.iso")

    @patch("subprocess.run")
    def test_casefold_duplicate(self, mock_run):
        dup = VALID_7Z + "\nPath = boot/grub/GRUB.CFG\nFolder = -\nSize = 512\nSymbolic Link = \n"
        mock_run.return_value = MagicMock(stdout=dup)
        with self.assertRaises(ValueError):
            b.validate_iso_tree("/fake.iso")

    def test_layout_boundaries(self):
        with self.assertRaises(ValueError):
            b.get_layout(1793)
        layout = b.get_layout(2048)
        p1, p2 = layout["partitions"]
        self.assertEqual(p1["size_mib"], 510)
        self.assertEqual(p2["size_mib"], 1536)
        self.assertEqual(p2["start_sector"], p1["start_sector"] + p1["size_sectors"])

    def test_cli_rejects_existing_output_and_symlink(self):
        with tempfile.TemporaryDirectory() as td:
            iso = os.path.join(td, "source.iso")
            Path(iso).touch()
            out_file = os.path.join(td, "exist.raw")
            Path(out_file).touch()
            out_sym = os.path.join(td, "sym.raw")
            os.symlink(out_file, out_sym)
            for out in [out_file, out_sym]:
                with patch("sys.argv", ["p", "--iso", iso, "--output", out, "--size-mib", "2048"]):
                    with self.assertRaises(SystemExit) as error:
                        b.main()
                    self.assertIn("Output path already exists", str(error.exception))
                    self.assertEqual(Path(iso).stat().st_size, 0)
                    self.assertEqual(Path(out_file).stat().st_size, 0)

    def test_output_parent_device_alias(self):
        with tempfile.TemporaryDirectory() as td:
            iso = Path(td) / "source.iso"
            iso.touch()
            alias = Path(td) / "device-alias"
            alias.symlink_to("/dev", target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "resolves under /dev"):
                b.preflight(str(iso), str(alias / "litewin-test-new.img"), check_tools=False)

    @patch("subprocess.run")
    def test_special_file_mode_rejected(self, mock_run):
        mock_run.return_value = MagicMock(stdout=VALID_7Z + "\nPath = evil\nFolder = -\nSize = 0\nMode = 120777\n")
        with self.assertRaisesRegex(ValueError, "Disallowed mode"):
            b.validate_iso_tree("/fake.iso")

    def test_plan_needs_no_external_tools(self):
        from contextlib import redirect_stdout
        import io
        import json
        with tempfile.TemporaryDirectory() as td:
            iso = Path(td) / "source.iso"
            iso.touch()
            output = io.StringIO()
            with patch("sys.argv", ["p", "--iso", str(iso), "--size-mib", "2048", "--plan"]):
                with patch("shutil.which", return_value=None), redirect_stdout(output):
                    self.assertEqual(b.main(), 0)
            self.assertEqual(json.loads(output.getvalue())["partitions"][0]["fs"], "exfat")

    def test_patch_configs_preserves_and_fixes(self):
        with tempfile.TemporaryDirectory() as td:
            cfg_dir = os.path.join(td, "boot", "grub")
            os.makedirs(cfg_dir)
            cfg = os.path.join(cfg_dir, "grub.cfg")
            with open(cfg, "w", encoding="utf-8") as f:
                f.write("search --label LITEWIN_LIVE\noptions archisolabel=LITEWIN_LIVE\n")
            b.patch_configs(td)
            with open(cfg, "r", encoding="utf-8") as f:
                content = f.read()
            self.assertNotIn("LITEWIN_LIVE", content)
            self.assertIn("LITEWINBOOT", content)


if __name__ == "__main__":
    unittest.main()
