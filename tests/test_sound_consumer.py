"""The scan command uses raw sound policy and fails on provider outages."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Consumer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory(prefix='consumer-')
        cls.root = Path(cls.scratch.name)
        cls.clip = cls.root/'synthetic.mkv'
        subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'color=c=black:s=64x64:r=10',
                        '-f', 'lavfi', '-i', 'anullsrc=r=16000:cl=mono', '-t', '3',
                        '-threads', '1', '-c:v', 'ffv1', '-c:a', 'pcm_s16le', str(cls.clip)], check=True)
        cls.provider = cls.root/'provider.py'
        cls.provider.write_text('''import struct,sys
mode=sys.argv[1]; size=32000 if mode.startswith('sound') else 320*320*4
while len(sys.stdin.buffer.read(size)) == size:
    if mode.endswith('fail'): sys.exit(23)
    rows=[0.0]*120
    if mode == 'sound':
        rows[:18]=[0,.7,0,0,0,0,1,.7,0,0,0,0,2,.7,0,0,0,0]
    sys.stdout.buffer.write(struct.pack('<120f',*rows));sys.stdout.buffer.flush()
''')
        cls.policy = cls.root/'policy'
        cls.policy.write_text('dog=.65\nspeech=.9\n')

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def run_scan(self, obj='object', sound='sound', *args):
        env = dict(os.environ, KILIX_LOOK_HOME=str(self.root/'look'),
                   KILIX_SOUND_HOME=str(self.root/'sound'),
                   KILIX_OBJECT_DETECTOR=f'{sys.executable} {self.provider} {obj}',
                   KILIX_SOUND_CLASSIFIER=f'{sys.executable} {self.provider} {sound}',
                   KILIX_SOUND_THRESHOLDS=str(self.policy))
        return subprocess.run([str(ROOT/'build/kilix-look'), 'scan', str(self.clip),
                               '--listen', '--whole-frame', '--decode', '64x64',
                               '--seconds', '3', '--hear', '.8', *args], env=env,
                              capture_output=True, text=True, timeout=15)

    def test_raw_scores_obey_per_event_override_and_scalar_fallback(self):
        r = self.run_scan()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((self.root/'look/ffmpeg-audio.log').is_file())
        self.assertIn('  heard dog 0.70', r.stdout)
        self.assertNotIn('  heard speech', r.stdout)
        self.assertNotIn('  heard shout', r.stdout)

    def test_sound_failure_is_not_successful_silence(self):
        r = self.run_scan('object', 'sound-fail')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('classifier', r.stderr)

    def test_object_failure_is_not_a_successful_empty_scan(self):
        r = self.run_scan('object-fail', 'sound')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('kilix-look:', r.stderr)

if __name__ == '__main__':
    unittest.main()
