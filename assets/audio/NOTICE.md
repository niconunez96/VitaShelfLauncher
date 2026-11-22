# VitaShelf UI sounds

These six original synthesized cues were created for VitaShelf Launcher with
`scripts/generate-ui-audio.py`. Their soft glass timbres and airy transitions
take aesthetic inspiration from the PS5 interface. They contain no recordings
or samples extracted from Sony software or hardware.

Regenerate from the repository root with:

```sh
python3 scripts/generate-ui-audio.py
```

The generator uses only Python's standard library and fixed noise seeds.
Output is mono, 48 kHz, signed 16-bit little-endian PCM WAV, matching the Vita
player. Navigation lasts 85 ms; other cues last 165–560 ms. Peaks range from
20% to 36% of full scale before the player's volume control.

Existing filenames are retained to preserve the packaged action mappings.
