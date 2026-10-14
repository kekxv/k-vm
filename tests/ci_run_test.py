"""CI diagnostics contract: preserve failures and collect Bazel test output."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools' / 'ci_run.py'


class CiRunTest(unittest.TestCase):
    def test_failure_writes_summary_and_preserves_status(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            summary = root / 'summary.md'
            env = dict(os.environ, RUNNER_TEMP=temp, GITHUB_STEP_SUMMARY=str(summary))
            result = subprocess.run([sys.executable, str(SCRIPT), '--', sys.executable,
                                     '-c', 'print("failure <detail>"); raise SystemExit(7)'],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 7, result.stderr)
            self.assertIn('failure &lt;detail&gt;', summary.read_text())
            self.assertIn('7', summary.read_text())
            self.assertTrue(list((root / 'k-vm-ci-logs').glob('*/command.log')))

    def test_bazel_failure_collects_only_failed_test_logs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            summary = root / 'summary.md'
            failed = root / 'test.log'
            failed.write_text('Assertion failed: filesystem <unsafe>')
            (root / 'passed.log').write_text('Successful test output should not enter Summary')
            fake = root / 'bazel.py'
            fake.write_text('''import json, sys
from pathlib import Path
bep = next(a.split('=', 1)[1] for a in sys.argv if a.startswith('--build_event_json_file='))
Path(bep).write_text(json.dumps({'testResult': {'status': 'PASSED', 'testActionOutput': [{'name': 'test.log', 'uri': Path(sys.argv[-1]).with_name('passed.log').as_uri()}]}}) + '\\n' + json.dumps({'id': {'testResult': {'label': '//tests:broken'}},
 'testResult': {'status': 'FAILED', 'testActionOutput': [
 {'name': 'test.log', 'uri': Path(sys.argv[-1]).as_uri()}]}}) + '\\n')
print('FAIL: //tests:broken')
sys.exit(3)
''')
            # A Python stand-in for Bazel exercises the actual wrapper and BEP parser.
            env = dict(os.environ, RUNNER_TEMP=temp, GITHUB_STEP_SUMMARY=str(summary))
            result = subprocess.run([sys.executable, str(SCRIPT), '--bazel', '--',
                                     sys.executable, str(fake), 'test', str(failed)], env=env,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 3, result.stderr)
            self.assertIn('//tests:broken', summary.read_text())
            self.assertIn('filesystem &lt;unsafe&gt;', summary.read_text())
            self.assertNotIn('Successful test output', summary.read_text())
            self.assertTrue(list((root / 'k-vm-ci-logs').glob('*/failed-*.log')))

    def test_success_does_not_write_failure_summary(self):
        with tempfile.TemporaryDirectory() as temp:
            summary = Path(temp) / 'summary.md'
            env = dict(os.environ, RUNNER_TEMP=temp, GITHUB_STEP_SUMMARY=str(summary))
            result = subprocess.run([sys.executable, str(SCRIPT), '--', sys.executable,
                                     '-c', 'print("ok")'], env=env, capture_output=True)
            self.assertEqual(result.returncode, 0)
            self.assertFalse(summary.exists())


if __name__ == '__main__':
    unittest.main()
