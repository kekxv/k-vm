"""Release tags come from the project module, not dependency versions."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools' / 'release_version.py'


class ReleaseVersionTest(unittest.TestCase):
    def tag(self, source):
        spec = importlib.util.spec_from_file_location('release_version', SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'MODULE.bazel'
            path.write_text(source)
            return module.release_tag(path)

    def test_project_version_ignores_dependencies_and_comments(self):
        self.assertEqual(self.tag('''# version = "9.9.9"
bazel_dep(name="dependency", version="4.3.0")
module(name="k_vm", version="0.1.0")
'''), 'v0.1.0')

    def test_prerelease_version(self):
        self.assertEqual(self.tag("module(name='k_vm', version='0.2.0-rc.1')"), 'v0.2.0-rc.1')

    def test_missing_or_duplicate_module_is_rejected(self):
        for source in ('module(name="k_vm")', 'bazel_dep(name="x", version="1.0.0")',
                       'module(version="1.0.0")\nmodule(version="2.0.0")'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                self.tag(source)

    def test_nonliteral_and_unsafe_versions_are_rejected(self):
        for version in ('VERSION', '""', '"0.1.0/unsafe"', '"$(echo unsafe)"'):
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.tag('module(name="k_vm", version=' + version + ')')


if __name__ == '__main__':
    unittest.main()
