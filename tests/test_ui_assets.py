"""Validate the bundled Blueglass inventory and native PNG contracts."""
import json
import re
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets/blueglass"


class ArtworkInventoryTests(unittest.TestCase):
    def test_every_console_has_a_tightly_fitted_header_logo(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        platforms = re.search(r"set\(VP_BLUEGLASS_LOGOS\s+(.*?)\)",cmake,re.S).group(1).split()
        names = {name.replace("platform_", "header_", 1) for name in platforms}
        self.assertEqual({p.stem for p in (ASSETS / "headers").glob("*.png")}, names)
        self.assertIn("assets/blueglass/headers/", cmake)
        for name in names:
            data = (ASSETS / "headers" / f"{name}.png").read_bytes()
            width, height = struct.unpack(">II", data[16:24])
            self.assertTrue(1 <= width <= 128 and 1 <= height <= 64, name)
            self.assertEqual(data[25], 6, name)
            self.check_png(ASSETS / "headers" / f"{name}.png", (width,height), 64*1024)

    def test_every_logo_has_a_packaged_native_background(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        names = re.search(r"set\(VP_BLUEGLASS_LOGOS\s+(.*?)\)",cmake,re.S).group(1).split()
        self.assertEqual(len(names),25)
        self.assertEqual({p.stem for p in (ASSETS / "logos").glob("*.png")},set(names))
        for name in names:
            self.check_png(ASSETS / "logos" / f"{name}.png",(128,128),512*1024)
            self.check_png(ASSETS / "backgrounds" / f"{name.removeprefix('platform_')}.png",(960,544),2*1024*1024)
        self.check_png(ASSETS / "backgrounds/general.png",(960,544),2*1024*1024)

    def test_graphic_symbols_and_inter_weights_are_explicitly_bundled(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        names = re.search(r"set\(VP_BLUEGLASS_UI\s+(.*?)\)",cmake,re.S).group(1).split()
        self.assertTrue({"console_surface", "console_focus"}.issubset(names))
        for name in names:
            size={"ui_buttons":(128,128), "focus":(96,96), "console_focus":(96,96),
                  "console_surface":(136,170)}.get(name,(64,64))
            self.check_png(ASSETS / "ui" / f"{name}.png",size,512*1024)
        self.assertEqual((ASSETS / "ui/ui_romm.png").read_bytes(),
                         (ROOT / "assets/aurora/ui_icons/ui_romm.png").read_bytes())
        for weight,expected in (("Regular",400),("Medium",500),("SemiBold",600),("Bold",700)):
            data=(ASSETS / "fonts" / f"Inter-{weight}.ttf").read_bytes()
            self.assertEqual(data[:4],b"\x00\x01\x00\x00")
            tables=struct.unpack(">H",data[4:6])[0]
            os2=None
            for index in range(tables):
                record=12+index*16
                if data[record:record+4]==b"OS/2":
                    os2=struct.unpack(">I",data[record+8:record+12])[0]
                    break
            self.assertIsNotNone(os2,weight)
            self.assertEqual(struct.unpack(">H",data[os2+4:os2+6])[0],expected,weight)
        self.assertIn("SIL OPEN FONT LICENSE",(ASSETS / "fonts/OFL.txt").read_text())
        self.assertTrue((ASSETS / "NOTICE.md").is_file())
        self.assertTrue(json.loads((ASSETS / "provenance.json").read_text()))
        self.assertNotIn("?token=",(ASSETS / "provenance.json").read_text())

    def check_png(self,path,size,limit):
        data=path.read_bytes()
        self.assertEqual(data[:8],b"\x89PNG\r\n\x1a\n",str(path))
        self.assertEqual(struct.unpack(">II",data[16:24]),size,str(path))
        self.assertEqual(data[24],8,str(path))
        self.assertIn(data[25],(2,6),str(path))
        self.assertEqual(data[28],0,str(path))
        self.assertLessEqual(len(data),limit,str(path))


if __name__ == "__main__":
    unittest.main()
