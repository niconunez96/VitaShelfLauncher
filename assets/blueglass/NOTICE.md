# Blueglass artwork attribution

- **Inter**: Copyright (c) 2016 The Inter Project Authors, https://github.com/rsms/inter.
  SIL Open Font License 1.1; full text in `fonts/OFL.txt`. Upstream static WOFF2
  files were converted to TrueType containers, retaining their glyphs and names.
- **Platform logos and eight console illustrations**: Carbon theme by Rookervik,
  based on Simple by Nils Bonenberger, maintained at
  https://github.com/fabricecaruso/es-theme-carbon. The complete upstream attribution
  and CC BY-NC-SA notice are preserved in `sources/Carbon-README.txt` (also bundled).
  Logos/trademarks belong to their respective owners. Changes: rasterization,
  transparent padding, white adaptation of dark monochrome details, resizing,
  stacking of original marks/lettering for cards (including GAME BOY / ADVANCE),
  alpha-trimmed header variants. Adapted Carbon
  artwork retains its upstream CC BY-NC-SA terms. Original SVGs and source console
  images are retained under `sources/`; exact revisions and URLs are in `provenance.json`.
- **Selectable wallpapers**: four images supplied by the project owner for this change
  (dark blue, violet, blue waves, and light blue), replacing the former generated backgrounds.
  Upstream authorship and licensing were not specified; no ownership or new license is claimed.
  Processing removes top-edge remnants, crops without distortion, and resizes to 960×544 with 240×136 thumbnails.
  Source hashes and crop bounds are recorded in `wallpapers/provenance.json` (also bundled).
- **RomM settings icon**: the previously accepted circular PNG bundled at
  `ui/ui_romm.png`, retained byte-for-byte without recoloring.
  The symbol belongs to the RomM project, https://github.com/rommapp/romm/tree/master/frontend/assets.
  Original raster Git blob: `491e995d35661dbdc4f87cd2960f2ff87a336cf5`;
  original SVG Git blob: `bc212d664f66168853e43545efb6489d00b07b09`.
  The original SVG and upstream GNU Affero General Public License v3 are retained
  in `sources/` and bundled under `assets/licenses/`. This does not change the
  licensing of unrelated artwork or imply trademark endorsement.
- **UI symbols, focus texture and VitaShelf Launcher emblem**: original vector/geometry artwork
  built by `scripts/prepare-blueglass.py`. Controller button symbols are original
  vector recreations of the classic DualShock dark buttons and colored outlines,
  not official Sony assets. They are prerendered into one 128×128 atlas with four
  64×64 cells, not substituted with text glyphs. The white PlayStation header
  mark is an adaptation of the attributed original platform logo above.

Game covers and per-game backgrounds are not bundled. They are supplied by the
user's RomM server and cached separately on the Vita.
