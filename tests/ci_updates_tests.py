"""Offline CI signing, coherent ISO extraction and fail-closed publication tests."""
import base64
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts import ci_updates as ci, github_updates as gh
from scripts.publish_updates import digest, prepare_release, sign_document
VALIDATOR = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else ROOT/'build/validate_update'


class UpdatesCI(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='winstaller-ci-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # Published RFC 8032 vector #1; never an operational signing key.
        der = bytes.fromhex('302e020100300506032b657004220420'+'9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')
        self.key = self.root/'vector.der'; self.key.write_bytes(der)
        self.pem = subprocess.run(['openssl','pkey','-inform','DER','-in',str(self.key)], check=True, capture_output=True).stdout.decode()
        self.config = json.loads((ROOT/ci.CONFIG).read_text())
        self.config['keys'] = [{'id':'test','public_hex':'d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a','roles':['catalog','release']}]
        self.config_path = self.root/'config.json'; self.config_path.write_text(json.dumps(self.config))
        self.source = self.root/'source.json'; self.signed = self.root/'signed.json'

    def catalog(self, revision=1, expired=False):
        now = int(time.time())
        doc = {'document_type':'catalog','schema_version':1,'revision':revision,'published_at':now-100,'expires_at':now-1 if expired else now+3600,'min_catalog_api':1,'targets':[]}
        self.source.write_text(json.dumps(doc))
        return doc

    def test_empty_catalog_requires_explicit_opt_in(self):
        self.source.write_text('{"targets":[]}')
        with self.assertRaises(ValueError):ci.prepare_catalog(self.source,self.signed,1,30)
        ci.prepare_catalog(self.source,self.signed,1,30,True)
        self.assertEqual(json.loads(self.signed.read_text())['revision'],1)

    def test_key_match_sign_verify_and_cleanup(self):
        self.catalog()
        scratch = self.root/'runner-temp'; scratch.mkdir()
        with patch.dict(os.environ, {'WINSTALLER_SIGNING_KEY':self.pem,'RUNNER_TEMP':str(scratch)}):
            ci.sign_ci(self.source,self.signed,'catalog','test',self.config_path,VALIDATOR)
            self.assertNotIn('WINSTALLER_SIGNING_KEY',os.environ)
        self.assertEqual(list(scratch.iterdir()),[])
        self.assertEqual(ci.archive_payload(self.signed.read_bytes(),self.config,'catalog')['revision'],1)

    def test_wrong_key_and_invalid_schema_leave_no_signed_artifact(self):
        self.catalog()
        self.config['keys'][0]['public_hex']='00'*32
        self.config_path.write_text(json.dumps(self.config))
        with patch.dict(os.environ, {'WINSTALLER_SIGNING_KEY':self.pem}), self.assertRaises(ValueError):
            ci.sign_ci(self.source,self.signed,'catalog','test',self.config_path,VALIDATOR)
        self.assertFalse(self.signed.exists())
        self.config['keys'][0]['public_hex']='d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a'
        self.config_path.write_text(json.dumps(self.config))
        self.source.write_text('{"document_type":"catalog"}')
        with patch.dict(os.environ, {'WINSTALLER_SIGNING_KEY':self.pem}), self.assertRaises(ValueError):
            ci.sign_ci(self.source,self.signed,'catalog','test',self.config_path,VALIDATOR)
        self.assertFalse(self.signed.exists())

    def test_expired_previous_is_authenticated_for_rollback_guard(self):
        old=self.catalog(expired=True)
        sign_document(self.source,self.signed,self.key,'test')
        self.assertEqual(ci.archive_payload(self.signed.read_bytes(),self.config,'catalog'),old)
        with self.assertRaises(ValueError):ci.validate_signed(self.signed,'catalog',self.config_path,VALIDATOR)
        with self.assertRaises(ValueError):ci.check_monotonic(old,old,'catalog')
        ci.check_monotonic(dict(old,revision=2),old,'catalog')
        outer=json.loads(self.signed.read_text()); outer['payload_base64']=base64.b64encode(b'{}').decode()
        with self.assertRaises(ValueError):ci.archive_payload(json.dumps(outer).encode(),self.config,'catalog')

    def test_release_requires_new_sequence_and_id(self):
        old={'revision':1,'release_sequence':2,'release_id':'release-2'}
        for candidate in [dict(old,revision=2),dict(old,revision=2,release_id='release-3')]:
            with self.assertRaises(ValueError):ci.check_monotonic(candidate,old,'release')
        ci.check_monotonic({'revision':2,'release_sequence':3,'release_id':'release-3'},old,'release')

    def test_catalog_source_mismatch_does_not_write(self):
        self.catalog();sign_document(self.source,self.signed,self.key,'test')
        class Fake:
            def head(self):return None
            def read_file(self,*args):return None
            def write_metadata(self,*args):raise AssertionError('must not publish')
        self.source.write_text('{}')
        with self.assertRaises(ValueError):gh.publish_catalog(Fake(),self.source,self.signed,self.config_path,VALIDATOR)

    def release_fixture(self):
        iso=self.root/'iso';assets=self.root/'assets';iso.mkdir();assets.mkdir()
        image=iso/'test.iso';image.write_bytes(b'test ISO')
        (iso/'SHA256SUMS').write_text(digest(image)+'  test.iso\n')
        (assets/'SOURCE_ISO_SHA256').write_text(digest(image))
        doc={'release_id':'release-2','release_sequence':2,'revision':2,'version':'1.2.0','release_notes':'Test release.','components':[]}
        for role in ['kernel','initramfs','rootfs']:
            p=assets/(role+'.bin');p.write_bytes(role.encode())
            doc['components'].append({'role':role,'url':f'https://github.com/{gh.REPOSITORY}/releases/download/release-2/{p.name}','sha256':digest(p),'size_bytes':p.stat().st_size})
        (assets/'release-v1.json').write_text(json.dumps(doc));(assets/'release-v1.signed.json').write_text('{}')
        return iso,assets,doc

    def test_corrupt_asset_prevents_upload(self):
        iso,assets,doc=self.release_fixture()
        self.assertEqual(len(gh.release_files(assets,iso,doc)),7)
        (assets/'kernel.bin').write_bytes(b'corrupt')
        with self.assertRaises(ValueError):gh.release_files(assets,iso,doc)

    def publication(self, fail_upload=False, wrong_digest=False, race=False):
        iso,assets,doc=self.release_fixture(); events=[]
        files=gh.release_files(assets,iso,doc)
        server=[{'name':p.name,'size':p.stat().st_size,'digest':'sha256:'+digest(p),'state':'uploaded'} for p in files]
        if wrong_digest:server[0]['digest']='sha256:'+'00'*32
        class Fake:
            def request(self,method,path,**kwargs):
                if kwargs.get('missing'):return None
                return {'assets':server,'draft':False,'prerelease':False}
            def head(self):return 'changed' if race else 'head'
            def write_metadata(self,*args):events.append('stable')
        def fake_gh(args):
            events.append(args[1])
            if fail_upload:raise ValueError('upload interrupted')
        with patch.object(gh,'inspected_candidate',return_value=(doc,'head')), patch.object(gh,'gh',side_effect=fake_gh),patch.object(gh.time,'sleep'):
            if fail_upload or wrong_digest or race:
                with self.assertRaises(ValueError):gh.publish_release(Fake(),assets,iso,self.config_path,VALIDATOR,'a'*40)
                self.assertNotIn('stable',events)
            else:
                gh.publish_release(Fake(),assets,iso,self.config_path,VALIDATOR,'a'*40)
                self.assertEqual(events,['create','edit','stable'])
        return events

    def test_stable_metadata_is_last_write(self):self.publication()
    def test_upload_failure_retains_old_stable(self):self.publication(fail_upload=True)
    def test_wrong_remote_digest_retains_draft(self):self.assertEqual(self.publication(wrong_digest=True),['create'])
    def test_concurrent_metadata_change_prevents_overwrite(self):self.publication(race=True)

    def test_git_transaction_preserves_tree_and_never_forces(self):
        calls=[]
        def request(method,path,data=None,**kwargs):
            calls.append((method,path,data))
            if method=='GET':return {'tree':{'sha':'existing-tree'}}
            return {'sha':'new-object'}
        api=object.__new__(gh.GitHub);api.request=request
        api.write_metadata('parent',{'catalog-v1.json':b'{}'},'test')
        tree=next(data for _,path,data in calls if path=='git/trees')
        self.assertEqual(tree['base_tree'],'existing-tree')
        self.assertEqual(calls[-1],('PATCH','git/refs/heads/catalog',{'sha':'new-object','force':False}))
        calls.clear();api.write_metadata(None,{'catalog-v1.json':b'{}'},'initial')
        self.assertEqual(calls[-1][0:2],('POST','git/refs'))
        commit=next(data for _,path,data in calls if path=='git/commits')
        self.assertEqual(commit['parents'],[])

    def test_real_iso_identity_and_trust_checks(self):
        live=self.root/'live';(live/'etc/winstaller').mkdir(parents=True)
        conf=copy.deepcopy(self.config);conf['release_sequence']=2
        (live/'etc/winstaller/updates.json').write_text(json.dumps(conf))
        (live/'etc/winstaller-version').write_text('1.2.0\n')
        image_tree=self.root/'image-tree';boot=image_tree/'arch/boot/x86_64';boot.mkdir(parents=True)
        rootfs=image_tree/'arch/x86_64';rootfs.mkdir(parents=True)
        (boot/'vmlinuz-linux').write_bytes(b'fixture kernel');(boot/'initramfs-linux.img').write_bytes(b'fixture initramfs')
        subprocess.run(['mksquashfs',str(live),str(rootfs/'airootfs.sfs'),'-noappend','-processors','1'],check=True,capture_output=True)
        image=self.root/'fixture.iso'
        subprocess.run(['genisoimage','-quiet','-R','-o',str(image),str(image_tree)],check=True,capture_output=True)
        prepare_release(image,self.root/'prepared','release-2','1.2.0',2,2,30,digest(image),'Test release.',self.config_path)
        doc=json.loads((self.root/'prepared/release-v1.json').read_text())
        self.assertEqual(len(doc['components']),3)
        with self.assertRaisesRegex(ValueError,'version'):
            prepare_release(image,self.root/'wrong-version','release-3','1.3.0',2,3,30,digest(image),'Test.',self.config_path)
        self.config['keys'][0]['public_hex']='00'*32;self.config_path.write_text(json.dumps(self.config))
        with self.assertRaisesRegex(ValueError,'trust'):
            prepare_release(image,self.root/'wrong-trust','release-3','1.2.0',2,3,30,digest(image),'Test.',self.config_path)


if __name__=='__main__':unittest.main()
