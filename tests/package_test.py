"""Exercise the source ZIP and reject omitted local include/build/doc inputs."""
import importlib.util
from pathlib import Path
import re
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("package", ROOT / "package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class SourcePackageTests(unittest.TestCase):
    def test_source_zip_is_self_contained(self):
        members = {name: (ROOT / name).read_bytes() for name in package.SOURCE_FILES}
        self.assertEqual(len(members), len(package.SOURCE_FILES))
        for folder in ("src", "tests", "tools", "docs", ".github/workflows"):
            for path in (ROOT / folder).rglob("*"):
                if path.is_file() and "__pycache__" not in path.parts:
                    self.assertIn(path.relative_to(ROOT).as_posix(), members)
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "source.zip"
            package.write_zip(archive, members)
            with zipfile.ZipFile(archive) as source:
                self.assertIsNone(source.testzip())
                source.extractall(Path(directory) / "tree")
            tree = Path(directory) / "tree"
            for name in members:
                path = tree / name
                self.assertEqual(path.read_bytes(), members[name])
                if path.suffix in (".cpp", ".h", ".inl"):
                    for include in re.findall(r'^\s*#include\s+"([^"]+)"', path.read_text(encoding="utf-8"), re.M):
                        self.assertTrue((path.parent / include).is_file(), (name, include))
            self.assertIn("VERSION " + package.VERSION, (tree / "CMakeLists.txt").read_text())


if __name__ == "__main__":
    unittest.main()
