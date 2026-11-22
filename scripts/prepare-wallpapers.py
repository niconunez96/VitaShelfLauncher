#!/usr/bin/env python3
"""Prepare the four user-supplied wallpapers and settings thumbnails (Pillow)."""
import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]
NAMES = ('dark-blue', 'violet', 'blue-waves', 'light-blue')


def prepare(inputs, output):
    output.mkdir(parents=True, exist_ok=True)
    provenance = []
    for name, source, top in zip(NAMES, inputs, (0, 0, 0, 0)):
        with Image.open(source) as original:
            image = original.convert('RGB')
        bounds = (0, top, image.width, image.height)
        image = ImageOps.fit(image.crop(bounds), (960, 544), Image.Resampling.LANCZOS)
        image.save(output / f'{name}.png', optimize=True)
        image.resize((240, 136), Image.Resampling.LANCZOS).save(output / f'wallpaper_{name}.png', optimize=True)
        provenance.append({
            'file': f'wallpapers/{name}.png',
            'thumbnail': f'wallpapers/wallpaper_{name}.png',
            'source': 'User-supplied image attachment; ownership and upstream license not specified.',
            'sha256_source': hashlib.sha256(source.read_bytes()).hexdigest(),
            'crop_before_fit': bounds,
            'treatment': 'Center-crop without distortion, Lanczos resize to 960x544 and 240x136.',
        })
    (output / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', type=Path, nargs=4, metavar='PNG', help='Dark blue, violet, blue waves, light blue, in order')
    parser.add_argument('--output', type=Path, default=ROOT / 'assets/blueglass/wallpapers')
    args = parser.parse_args()
    prepare(args.inputs, args.output)
