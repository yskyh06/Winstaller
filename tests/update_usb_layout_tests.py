import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from scripts.create_usb_image import get_layout
from scripts.update_usb_layout import COMPONENTS,prepare_slots,digest
class Slots(unittest.TestCase):
    def test_layout_preserves_v1_and_v2_capacity(self):
        self.assertEqual(get_layout(7000)['partitions'][1]['size_mib'],1536)
        v2=get_layout(7000,True)
        self.assertEqual(v2['partitions'][1]['size_mib'],4096)
        self.assertTrue(v2['partuuid'])
        with self.assertRaises(ValueError):get_layout(4000,True)
    def test_factory_and_stable_loader(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            config=root/'boot/grub/grub.cfg';config.parent.mkdir(parents=True)
            config.write_text('# old live entry\n# Detect installed Linux ROOT partition\n# keep internal disk boot\n')
            for role,path in COMPONENTS.items():
                file=root/path;file.parent.mkdir(parents=True,exist_ok=True);file.write_bytes(role.encode()*10)
            prepare_slots(root,'test-partuuid','ABCD-1234')
            slot=root/'winstaller/slots/A';env=slot/'ready.env'
            self.assertEqual(env.stat().st_size,1024)
            self.assertEqual((slot/'ready.sha256').read_text(),digest(env)+'  ready.env\n')
            self.assertEqual(json.loads((root/'winstaller/device.json').read_text())['layout_version'],2)
            self.assertIn('load_env --file',config.read_text())
            self.assertIn('ready.env winstaller_generation',config.read_text())
            self.assertIn('archisodevice=PARTUUID=test-partuuid',config.read_text())
            self.assertIn('# keep internal disk boot',config.read_text())
            self.assertFalse((root/COMPONENTS['rootfs']).exists())
            with self.assertRaises(ValueError):prepare_slots(root,'test-partuuid','ABCD-1234')
if __name__=='__main__':unittest.main()
