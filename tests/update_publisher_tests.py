"""Publisher interoperability using a public RFC 8032 vector, not an operational key."""
import base64
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from scripts.publish_updates import sign_document
VALIDATOR=Path(sys.argv.pop(1)).resolve() if len(sys.argv)>1 else ROOT/'build/validate_update'
class Publisher(unittest.TestCase):
    def test_exact_bytes_signature_and_no_overwrite(self):
        with tempfile.TemporaryDirectory(prefix='winstaller-publisher-test-') as directory:
            root=Path(directory)
            # RFC 8032 test vector #1 encoded as PKCS#8 DER. This material is public.
            seed='9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60'
            key=root/'public-test-vector.der';key.write_bytes(bytes.fromhex('302e020100300506032b657004220420'+seed))
            now=int(time.time())
            doc={'document_type':'catalog','schema_version':1,'revision':1,'published_at':now-10,'expires_at':now+3600,'min_catalog_api':1,'targets':[]}
            source=root/'catalog.json';raw=(json.dumps(doc,indent=3)+'\n').encode();source.write_bytes(raw)
            output=root/'signed.json';sign_document(source,output,key,'test')
            envelope=json.loads(output.read_text())
            self.assertEqual(base64.b64decode(envelope['payload_base64']),raw)
            config=root/'config.json';config.write_text(json.dumps({'catalog_url':'https://raw.githubusercontent.com/yskyh06/Winstaller/catalog/catalog-v1.signed.json','release_url':'https://raw.githubusercontent.com/yskyh06/Winstaller/catalog/updates/stable-v1.signed.json','release_sequence':1,'keys':[{'id':'test','public_hex':'d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a','roles':['catalog']}]}))
            result=subprocess.run([str(VALIDATOR),str(config),'catalog',str(output)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            before=output.read_bytes()
            with self.assertRaises(FileExistsError):sign_document(source,output,key,'test')
            self.assertEqual(output.read_bytes(),before)
    def test_duplicate_keys_rejected_before_signing(self):
        with tempfile.TemporaryDirectory(prefix='winstaller-publisher-test-') as directory:
            root=Path(directory);source=root/'duplicate.json';source.write_text('{"document_type":"catalog","document_type":"release"}')
            with self.assertRaises(ValueError):sign_document(source,root/'out.json',root/'nonexistent-key','test')
            self.assertFalse((root/'out.json').exists())
if __name__=='__main__':unittest.main()
