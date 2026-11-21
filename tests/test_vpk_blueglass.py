"""Check a built VPK contains exactly the prepared Blueglass runtime pixels/fonts."""
from pathlib import Path
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


class BlueglassPackageTests(unittest.TestCase):
    def test_runtime_resources_match_sources_and_sources_are_not_shipped(self):
        with zipfile.ZipFile(ROOT / "dist/vitashelflauncher.vpk") as package:
            self.assertIsNone(package.testzip())
            names=set(package.namelist())
            for folder in ("logos","headers","backgrounds","ui","fonts"):
                for source in (ROOT / "assets/blueglass" / folder).iterdir():
                    relative=source.relative_to(ROOT).as_posix()
                    self.assertIn(relative,names)
                    self.assertEqual(package.read(relative),source.read_bytes(),relative)
            self.assertFalse(any(name.startswith("assets/blueglass/sources/") for name in names))
            self.assertFalse(any(name.startswith("assets/aurora/") for name in names))
            self.assertIn("assets/licenses/Blueglass-NOTICE.md",names)
            self.assertIn("assets/licenses/Carbon-README.txt",names)
            self.assertIn("assets/licenses/RomM-AGPL-3.0.txt",names)
            self.assertIn("assets/licenses/romm-isotipo.svg",names)
            param_sfo = package.read("sce_sys/param.sfo")
            self.assertIn(b"VitaShelf Launcher", param_sfo)
            self.assertIn(b"VSHELF001", param_sfo)


if __name__ == "__main__":
    unittest.main()
