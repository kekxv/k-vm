"""Exercise release CLI sequencing without writing to GitHub."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools' / 'publish_release.sh'
FAKE_GH = '''#!/usr/bin/env python3
import json, os, sys
with open(os.environ['GH_TEST_LOG'], 'a') as log:
    log.write(json.dumps(sys.argv[1:]) + '\\n')
if sys.argv[1:3] == ['release', 'view']:
    sys.exit(0 if os.environ.get('GH_TEST_EXISTS') else 1)
if sys.argv[1:3] == ['release', 'upload'] and os.environ.get('GH_TEST_FAIL_UPLOAD'):
    sys.exit(7)
'''


class PublishReleaseTest(unittest.TestCase):
    def run_publish(self, *, exists=False, fail_upload=False):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            gh = root / 'gh'
            gh.write_text(FAKE_GH)
            gh.chmod(0o755)
            (root / 'dist').mkdir()
            (root / 'dist' / 'binary.tar.gz').write_text('fixture')
            (root / 'dist' / 'base.tar.xz').write_text('fixture')
            log = root / 'commands.jsonl'
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ['PATH'],
                       RELEASE_TAG='v0.1.0', RELEASE_SHA='a' * 40, GH_TEST_LOG=str(log))
            # Do not inherit a runner's summary or external fake-CLI settings.
            for key in ('GITHUB_STEP_SUMMARY', 'GH_TEST_EXISTS', 'GH_TEST_FAIL_UPLOAD'):
                env.pop(key, None)
            if exists:
                env['GH_TEST_EXISTS'] = '1'
            if fail_upload:
                env['GH_TEST_FAIL_UPLOAD'] = '1'
            result = subprocess.run(['bash', str(SCRIPT)], cwd=root, env=env, capture_output=True)
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            return result, calls

    def test_new_release_uses_exact_commit_and_uploads_before_publish(self):
        result, calls = self.run_publish()
        self.assertEqual(result.returncode, 0, result.stderr)
        create = next(call for call in calls if call[:2] == ['release', 'create'])
        self.assertIn('--draft', create)
        self.assertEqual(create[create.index('--target') + 1], 'a' * 40)
        upload = next(i for i, call in enumerate(calls) if call[:2] == ['release', 'upload'])
        publish = next(i for i, call in enumerate(calls) if '--draft=false' in call)
        self.assertLess(upload, publish)
        self.assertIn('dist/binary.tar.gz', calls[upload])
        self.assertIn('dist/base.tar.xz', calls[upload])

    def test_upload_failure_keeps_release_unpublished(self):
        result, calls = self.run_publish(fail_upload=True)
        self.assertEqual(result.returncode, 7)
        self.assertFalse(any('--draft=false' in call for call in calls))

    def test_existing_draft_retry_updates_commit_before_upload(self):
        result, calls = self.run_publish(exists=True)
        self.assertEqual(result.returncode, 0)
        self.assertFalse(any(call[:2] == ['release', 'create'] for call in calls))
        update = next((i for i, call in enumerate(calls) if '--target' in call), None)
        self.assertIsNotNone(update, 'a draft from an interrupted run must point at this build commit')
        upload = next(i for i, call in enumerate(calls) if call[:2] == ['release', 'upload'])
        self.assertLess(update, upload)


if __name__ == '__main__':
    unittest.main()
