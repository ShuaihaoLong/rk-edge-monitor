import importlib.util
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch
import datetime as dt

spec=importlib.util.spec_from_file_location('recording_service',Path(__file__).parents[1]/'src/recording/service.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class RecordingTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.root=Path(self.temp.name)
        self.config=dict(root=self.root,timezone='Asia/Shanghai',days=7,reserve=64*1024**2,max_bytes=0)
        self.store=module.Store(self.config)
    def tearDown(self):
        self.temp.cleanup()
    def segment(self,age=120):
        stamp=dt.datetime.now(dt.timezone.utc)-dt.timedelta(seconds=age)
        path=self.root/'camera'/stamp.strftime('%Y-%m-%d')/(stamp.strftime('%H-%M-%S-%f')+'.mp4')
        path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'fixture')
        os.utime(path,(time.time()-age,time.time()-age))
        return path
    @staticmethod
    def probe(*args,**kwargs):
        return type('Probe',(),{'stdout':json.dumps({'streams':[{'codec_name':'h264'}],'format':{'duration':'60.125'}})})()
    def test_recovery_idempotence_listing_playback(self):
        path=self.segment()
        with patch.object(module.subprocess,'run',self.probe):
            self.store.reconcile();self.store.reconcile()
        with self.store.connect() as db:
            rows=db.execute('SELECT * FROM recordings').fetchall()
        self.assertEqual(len(rows),1);row=rows[0]
        self.assertEqual(row['status'],'recovered')
        self.assertEqual(row['end_ms']-row['start_ms'],60125)
        date=dt.datetime.fromtimestamp(row['start_ms']/1000,module.ZoneInfo('Asia/Shanghai')).date().isoformat()
        self.assertEqual(len(self.store.listing(date)['recordings']),1)
        playback=self.store.playback(row['start_ms'],60)
        self.assertIn('format=mp4',playback['url'])
        self.assertIn(f'/api/recordings/{row["id"]}/media#t=0.000',playback['media_url'])
        with self.assertRaises(FileNotFoundError):self.store.playback(row['end_ms']+1,60)
        with self.assertRaises(ValueError):self.store.playback(row['start_ms'],float('nan'))
        path.unlink();self.store.reconcile()
        self.assertFalse(self.store.listing(date)['recordings'])
    def test_open_protection_and_cleanup(self):
        path=self.segment(age=10)
        self.store.receive(dict(kind='segment_open',path=str(path)))
        self.store.config['max_bytes']=1
        with patch.object(module.subprocess,'run',side_effect=AssertionError('active file probed')):
            self.store.reconcile()
        self.assertTrue(path.exists())
        os.utime(path,(time.time()-120,time.time()-120))
        with patch.object(module.subprocess,'run',self.probe):self.store.reconcile()
        self.assertFalse(path.exists())
    def test_path_escape_and_symlink(self):
        with self.assertRaises(ValueError):self.store.safe_file('../secret.mp4')
        folder=self.root/'camera'/'2026-01-01';folder.mkdir(parents=True)
        path=folder/'00-00-00-000000.mp4';path.symlink_to('/etc/passwd')
        with self.assertRaises(ValueError):self.store.safe_file(str(path))
    def test_aggregate_events_and_deduplicate(self):
        stamp=int(time.time()*1000)
        def event(revision,offset,objects):
            return dict(status='ok',received_at_ms=stamp+offset,session='one',source_generation=1,
                        revision=revision,sequence=revision,objects=objects)
        objects=[dict(class_id=0,label='person',confidence=.9)]
        self.store.receive(event(1,0,objects));self.store.receive(event(1,0,objects))
        self.store.receive(event(2,1000,objects));self.store.receive(event(3,5000,[]))
        with self.store.connect() as db:rows=db.execute('SELECT * FROM events').fetchall()
        self.assertEqual(len(rows),1);self.assertEqual(rows[0]['end_ms']-rows[0]['start_ms'],1000)
        self.assertFalse(self.store.active)
    def test_interrupted_delete_finishes(self):
        path=self.segment()
        with patch.object(module.subprocess,'run',self.probe):self.store.inspect(path,completed=True)
        with self.store.connect() as db:db.execute("UPDATE recordings SET status='deleting'")
        with patch.object(module.subprocess,'run',side_effect=AssertionError('deletion was reindexed')):
            self.store.reconcile()
        self.assertFalse(path.exists())
    def test_cross_midnight(self):
        a,b=self.store.day_range('2026-09-19')
        self.assertEqual(b-a,86400000)
        self.assertEqual(dt.datetime.fromtimestamp(a/1000,dt.timezone.utc).hour,16)

    def test_ad_store_mode_and_playlist(self):
        ads = module.AdStore(self.root / 'ads')
        self.assertEqual(ads.mode(), 'ad')
        self.assertEqual(ads.list(), [])
        ads.set_mode('live')
        self.assertEqual(ads.mode(), 'live')
        ads.set_mode('ad')
        with self.assertRaises(ValueError):
            ads.set_mode('invalid')

if __name__=='__main__':unittest.main()
