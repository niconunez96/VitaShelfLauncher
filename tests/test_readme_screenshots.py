"""README screenshot contract for public demo and configuration help."""
import pathlib
import struct
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
README = (ROOT / "README.md").read_text()
SCREENSHOTS = (
    "docs/images/vitashelf-home.png",
    "docs/images/vitashelf-romm-connection.png",
    "docs/images/vitashelf-roms-grid.png",
    "docs/images/vitashelf-roms-list.png",
    "docs/images/vitashelf-roms-carousel.png",
)
GUIDES = (
    "guides/configure_romm.md",
    "guides/configure_image_service.md",
    "guides/configure_steamgriddb.md",
    "guides/initialize_games.md",
    "guides/customize_appearance.md",
    "guides/problems_debugging.md",
)


class ReadmeScreenshotTests(unittest.TestCase):
    def test_readme_links_current_public_screenshots(self):
        self.assertIn("## Screenshots", README)
        for relative_path in SCREENSHOTS:
            self.assertIn(f"]({relative_path})", README)

    def test_readme_links_existing_setup_guides(self):
        self.assertIn("## Setup guides", README)
        for relative_path in GUIDES:
            with self.subTest(relative_path=relative_path):
                self.assertIn(f"]({relative_path})", README)
                self.assertTrue((ROOT / relative_path).is_file())

    def test_screenshots_are_vita_resolution_pngs(self):
        for relative_path in SCREENSHOTS:
            with self.subTest(relative_path=relative_path):
                data = (ROOT / relative_path).read_bytes()
                self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
                self.assertEqual(struct.unpack(">II", data[16:24]), (960, 544))


if __name__ == "__main__":
    unittest.main()
