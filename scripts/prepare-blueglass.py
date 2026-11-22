#!/usr/bin/env python3
"""Build Vita-sized UI resources from upstream vectors and FLUX downloads.

Requires Pillow, fonttools[woff] and CairoSVG. Network requests only fetch the
explicit upstream font/logo inventory; FLUX generation is a separate operation.
"""
import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import urllib.request
import urllib.parse

import cairosvg
from fontTools.ttLib import TTFont
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageOps

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets/blueglass"
LOGOS = {
    "nes": "nes-w", "famicom": "famicom", "snes": "snes", "gameboy": "gb",
    "gameboycolor": "gbc", "gba": "gba", "nds": "nds", "genesis": "genesis",
    "mastersystem": "mastersystem", "gamegear": "gamegear-w", "psx": "psx",
    "psp": "psp", "vita": "psvita-w", "arcade": "arcade", "pce": "pcengine-w",
    "atari2600": "atari2600", "wonderswan": "wonderswan-w", "homebrew": "homebrew",
    "dreamcast": "dreamcast-w", "n3ds": "3ds-w", "gamecube": "gc-w",
    "ps2": "ps2", "wii": "wii-w", "neogeo": "neogeo",
}


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "VitaShelf Launcher-asset-preparation"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def svg(body, size=64, color="white"):
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 64 64"><g fill="none" stroke="{color}" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round">{body}</g></svg>'.encode()


def png_svg(data, target, size):
    image = Image.open(io.BytesIO(cairosvg.svg2png(bytestring=data, output_width=size[0]*2, output_height=size[1]*2))).convert("RGBA")
    image = image.resize(size, Image.Resampling.LANCZOS)
    image.save(target, optimize=True)


def controls_assets():
    """Bake classic dark face buttons into one atlas; preserve the old RomM PNG."""
    (OUT / "ui").mkdir(parents=True,exist_ok=True)
    (OUT / "sources").mkdir(parents=True,exist_ok=True)
    original=ROOT / "assets/aurora"
    (OUT / "ui/ui_romm.png").write_bytes((original / "ui_icons/ui_romm.png").read_bytes())
    for name in ("romm-isotipo.svg","RomM-AGPL-3.0.txt"):
        (OUT / "sources" / name).write_bytes((original / "licenses" / name).read_bytes())
    buttons=[
        ("cross","#91a4ed",'<path d="m17 16 30 30m0-30-30 30"/>'),
        ("circle","#f36b6a",'<circle cx="32" cy="31" r="16.5"/>'),
        ("square","#e79cce",'<path d="M17 16h30v30H17z"/>'),
        ("triangle","#64d3c1",'<path d="m32 12 17.32 30H14.68z"/>'),
    ]
    atlas=Image.new("RGBA",(128,128))
    for index,(name,color,shape) in enumerate(buttons):
        vector=f'''<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
<defs>
 <radialGradient id="face" cx="35%" cy="17%" r="86%">
  <stop stop-color="#454b56"/><stop offset=".5" stop-color="#242830"/><stop offset="1" stop-color="#11141b"/>
 </radialGradient>
 <linearGradient id="rim" x1="0" y1="0" x2="0" y2="1">
  <stop stop-color="#7b8491"/><stop offset=".48" stop-color="#343a46"/><stop offset="1" stop-color="#0c1017"/>
 </linearGradient>
 <linearGradient id="shine" x1="0" y1="0" x2="0" y2="1">
  <stop stop-color="white" stop-opacity=".16"/><stop offset="1" stop-color="white" stop-opacity="0"/>
 </linearGradient>
 <clipPath id="cap"><circle cx="32" cy="31" r="27"/></clipPath>
</defs>
<ellipse cx="32" cy="34" rx="28.5" ry="28" fill="#10172a" opacity=".22"/>
<circle cx="32" cy="31" r="28.5" fill="url(#rim)"/>
<circle cx="32" cy="31" r="26.5" fill="url(#face)" stroke="#0f131b" stroke-width="1"/>
<ellipse cx="27" cy="12" rx="29" ry="22" fill="url(#shine)" clip-path="url(#cap)"/>
<g fill="none" stroke="{color}" stroke-width="4.2" stroke-linecap="round" stroke-linejoin="round">{shape}</g>
</svg>'''.encode()
        (OUT / "sources" / f"button-{name}.svg").write_bytes(vector)
        image=Image.open(io.BytesIO(cairosvg.svg2png(bytestring=vector,output_width=256,output_height=256))).convert("RGBA")
        image=image.resize((64,64),Image.Resampling.LANCZOS)
        atlas.alpha_composite(image,((index%2)*64,(index//2)*64))
    atlas.save(OUT / "ui/ui_buttons.png",optimize=True)
    manifest=OUT / "provenance.json"
    provenance=json.loads(manifest.read_text()) if manifest.exists() else []
    provenance=[entry for entry in provenance if entry["file"] not in ("ui/ui_romm.png","ui/ui_buttons.png")]
    provenance.extend([
        {"file":"ui/ui_romm.png","source":"assets/aurora/ui_icons/ui_romm.png",
         "upstream":"https://github.com/rommapp/romm/tree/master/frontend/assets",
         "upstream_git_blob":"491e995d35661dbdc4f87cd2960f2ff87a336cf5",
         "sha256_source":hashlib.sha256((original / "ui_icons/ui_romm.png").read_bytes()).hexdigest(),
         "treatment":"Unchanged previously accepted circular RomM PNG."},
        {"file":"ui/ui_buttons.png","source":[f"sources/button-{name}.svg" for name,_,_ in buttons],
         "treatment":"Original DualShock-style vector recreations, rasterized with supersampling into four 64x64 atlas cells."},
    ])
    manifest.write_text(json.dumps(provenance,indent=2)+"\n")


def console_assets():
    """Bake an opaque console material and a hollow 96px nine-slice focus ring."""
    (OUT / "ui").mkdir(parents=True, exist_ok=True)
    scale = 4
    size = (136, 170)
    surface = Image.new("RGB", size)
    pixels = []
    for y in range(size[1]):
        for x in range(size[0]):
            u, v = x / (size[0]-1), y / (size[1]-1)
            sheen = math.exp(-((u-.12)/.65)**2-((v+.05)/.48)**2)
            light = math.exp(-((u-.05)/.28)**2-((v+.02)/.22)**2)
            pixels.append((int(32+22*sheen+7*light),
                           int(40+20*sheen+8*light),
                           int(62+28*sheen+12*light)))
    surface.putdata(pixels)
    surface = surface.resize((size[0]*scale, size[1]*scale), Image.Resampling.BICUBIC)
    bounds = (0, 0, surface.width-1, surface.height-1)
    ImageDraw.Draw(surface).rounded_rectangle(bounds, radius=14*scale,
                                             outline=(88, 101, 133), width=scale)
    mask = Image.new("L", surface.size)
    ImageDraw.Draw(mask).rounded_rectangle(bounds, radius=14*scale, fill=255)
    surface.putalpha(mask)
    surface.resize(size, Image.Resampling.LANCZOS).save(OUT / "ui/console_surface.png", optimize=True)

    ring = Image.new("L", (96*scale, 96*scale))
    bounds = (17*scale, 17*scale, 79*scale-1, 79*scale-1)
    ImageDraw.Draw(ring).rounded_rectangle(bounds, radius=15*scale, outline=255, width=3*scale)
    focus = Image.new("RGBA", ring.size)
    for blur, opacity in ((5, 180), (2, 230)):
        bloom = Image.new("RGBA", ring.size, (30, 100, 255, 0))
        bloom.putalpha(ring.filter(ImageFilter.GaussianBlur(blur*scale)).point(lambda a: a*opacity//255))
        focus = Image.alpha_composite(focus, bloom)
    draw = ImageDraw.Draw(focus)
    draw.rounded_rectangle(bounds, radius=15*scale, outline=(75, 146, 255, 255), width=3*scale)
    draw.rounded_rectangle((18*scale, 18*scale, 78*scale-1, 78*scale-1),
                           radius=14*scale, outline=(245, 250, 255, 255), width=5)
    # Bound the inner bloom so focus can never tint the body of a selected card.
    hollow = Image.new("L", ring.size)
    ImageDraw.Draw(hollow).rounded_rectangle((24*scale, 24*scale, 72*scale-1, 72*scale-1),
                                            radius=8*scale, fill=255)
    focus.putalpha(ImageChops.subtract(focus.getchannel("A"), hollow))
    focus = focus.resize((96, 96), Image.Resampling.LANCZOS)
    # Keep the stretched middle slice exactly transparent, including filter fringes.
    ImageDraw.Draw(focus).rectangle((31, 31, 64, 64), fill=(0, 0, 0, 0))
    focus.save(OUT / "ui/console_focus.png", optimize=True)

    manifest = OUT / "provenance.json"
    provenance = json.loads(manifest.read_text()) if manifest.exists() else []
    files = ("ui/console_surface.png", "ui/console_focus.png")
    provenance = [entry for entry in provenance if entry["file"] not in files]
    for filename in files:
        provenance.append({"file": filename, "source": "scripts/prepare-blueglass.py --console-cards",
                           "treatment": "Original procedural artwork: opaque blue-night material with upper-left sheen; separate hollow electric-blue focus."})
    manifest.write_text(json.dumps(provenance, indent=2)+"\n")


def static_assets():
    for folder in ("fonts", "logos", "ui", "backgrounds", "sources"):
        (OUT / folder).mkdir(parents=True, exist_ok=True)
    provenance = []
    for weight in ("Regular", "Medium", "SemiBold", "Bold"):
        url = f"https://raw.githubusercontent.com/rsms/inter/master/docs/font-files/Inter-{weight}.woff2"
        data = fetch(url)
        font = TTFont(io.BytesIO(data))
        font.flavor = None
        font.save(OUT / "fonts" / f"Inter-{weight}.ttf")
        provenance.append({"file": f"fonts/Inter-{weight}.ttf", "source": url, "sha256_source": hashlib.sha256(data).hexdigest()})
    (OUT / "fonts/OFL.txt").write_bytes(fetch("https://raw.githubusercontent.com/rsms/inter/master/LICENSE.txt"))
    # Pin the source revision once so every logo in this set has the same provenance.
    repo = "fabricecaruso/es-theme-carbon"
    revision = json.loads(fetch(f"https://api.github.com/repos/{repo}/commits/master"))["sha"]
    for name, upstream in LOGOS.items():
        url = f"https://raw.githubusercontent.com/{repo}/{revision}/art/logos/{upstream}.svg"
        data = fetch(url)
        (OUT / "sources" / f"{name}.svg").write_bytes(data)
        rendered = Image.open(io.BytesIO(cairosvg.svg2png(bytestring=data, output_width=768))).convert("RGBA")
        bounds = rendered.getbbox()
        if bounds:
            rendered = rendered.crop(bounds)
        rendered.thumbnail((240, 112), Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", (256, 128))
        canvas.alpha_composite(rendered, ((256-rendered.width)//2, (128-rendered.height)//2))
        canvas.save(OUT / "logos" / f"platform_{name}.png", optimize=True)
        provenance.append({"file": f"logos/platform_{name}.png", "source": url, "sha256_source": hashlib.sha256(data).hexdigest()})
    (OUT / "sources/Carbon-README.txt").write_bytes(fetch(f"https://raw.githubusercontent.com/{repo}/{revision}/readme.txt"))
    symbols = {
        "library": '<rect x="12" y="12" width="9" height="40" rx="2"/><rect x="28" y="12" width="9" height="40" rx="2"/><rect x="44" y="12" width="8" height="40" rx="2"/><path d="M16 20h1m15 0h1m15 0h1"/>',
        "store": '<path d="M15 23h34l4 31H11zM24 24v-9a8 8 0 0 1 16 0v9"/>',
        "settings": '<path d="m27 8-2 8-7 3-7-2-5 9 6 6-1 7-5 6 6 9 8-3 7 3 2 7h11l2-8 7-3 7 2 5-9-6-6 1-7 5-6-6-9-8 3-7-3-2-7z" transform="translate(0 -3) scale(.95)"/><circle cx="31" cy="29" r="9"/>',
        "download": '<path d="M32 8v33m-12-12 12 12 12-12M12 43v11h40V43"/>',
        "check": '<path d="m15 32 11 11 24-25"/>',
        "star": '<path d="m32 7 8 16 18 3-13 13 3 18-16-9-16 9 3-18L6 26l18-3z"/>',
        "home": '<path d="m8 29 24-21 24 21M15 25v30h13V38h9v17h12V25"/>',
        "key": '<circle cx="23" cy="23" r="13"/><path d="m32 32 23 23m-9-9 7-7m-14 0 7-7"/>',
        "sync": '<path d="M52 25A21 21 0 0 0 16 16l-6 8m0-15v15h15M12 39a21 21 0 0 0 36 9l6-8m0 15V40H39"/>',
        "folder": '<path d="M8 18h20l6 7h22v29H8zM8 25h48"/>',
        "general": '<path d="M19 20h26c7 0 16 31 8 33-5 1-11-10-15-10H26c-4 0-10 11-15 10-8-2 1-33 8-33zM21 27v13m-6-7h12m17-4h1m4 9h1"/>',
        "interface": '<rect x="9" y="10" width="46" height="43" rx="4"/><circle cx="23" cy="23" r="4"/><path d="m12 48 14-15 10 10 8-8 10 13"/>',
        "storage": '<path d="M18 8h28l7 9v39H12V16zM22 8v14h22V8"/>',
        "info": '<circle cx="32" cy="32" r="24"/><path d="M32 29v17m0-28v1"/>',
        "server": '<rect x="10" y="8" width="44" height="14" rx="4"/><rect x="10" y="25" width="44" height="14" rx="4"/><rect x="10" y="42" width="44" height="14" rx="4"/><path d="M18 15h1m-1 17h1m-1 17h1"/>',
    }
    for name, body in symbols.items():
        png_svg(svg(body), OUT / "ui" / f"ui_{name}.png", (64, 64))
    emblem = b'<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64"><rect x="11" y="11" width="42" height="42" rx="9" transform="rotate(45 32 32)" fill="#f778b4" stroke="white" stroke-width="2"/><path d="M27 16h10v11h11v10H37v11H27V37H16V27h11z" fill="#71dfb7"/></svg>'
    png_svg(emblem, OUT / "ui/emblem.png", (64, 64))
    png_svg(emblem, OUT / "logos/platform_generic.png", (256, 128))
    # Bake soft focus into a nine-slice texture; no runtime blur or large stacks of geometry.
    scale = 4
    focus = Image.new("RGBA", (96*scale, 96*scale))
    draw = ImageDraw.Draw(focus)
    draw.rounded_rectangle((15*scale,15*scale,81*scale-1,81*scale-1), radius=18*scale, outline=(20,103,255,240), width=7*scale)
    focus = focus.filter(ImageFilter.GaussianBlur(5*scale))
    draw = ImageDraw.Draw(focus)
    draw.rounded_rectangle((16*scale,16*scale,80*scale-1,80*scale-1), radius=17*scale, outline=(120,214,255,255), width=3*scale)
    draw.rounded_rectangle((18*scale,18*scale,78*scale-1,78*scale-1), radius=15*scale, outline=(255,255,255,255), width=2*scale)
    focus.resize((96,96), Image.Resampling.LANCZOS).save(OUT / "ui/focus.png", optimize=True)
    (OUT / "provenance.json").write_text(json.dumps(provenance, indent=2)+"\n")
    controls_assets()
    console_assets()


def background(name, source):
    image = Image.open(io.BytesIO(fetch(source)) if source.startswith("https://") else source).convert("RGB")
    image = ImageOps.fit(image, (960, 544), method=Image.Resampling.LANCZOS)
    image.save(OUT / "backgrounds" / f"{name}.png", optimize=True)


def finish_assets():
    # Contrast adaptation only: original vector contours and brand colors remain.
    for path in (OUT / "logos").glob("*.png"):
        logo = Image.open(path).convert("RGBA")
        logo.putdata([(245, 248, 255, a) if max(r,g,b)<85 else (r,g,b,a)
                      for r,g,b,a in logo.getdata()])
        logo.save(path, optimize=True)
    base = Image.open(OUT / "backgrounds/general.png").convert("RGBA")
    provenance = json.loads((OUT / "provenance.json").read_text())
    revision = provenance[4]["source"].split("/")[5]
    systems = {"psx":"psx", "nes":"nes", "wonderswan":"wonderswan", "dreamcast":"dreamcast",
               "n3ds":"3ds", "gamecube":"gc", "ps2":"ps2", "neogeo":"neogeo"}
    provenance = [entry for entry in provenance if not entry["file"].startswith("backgrounds/")]
    for system, upstream in systems.items():
        url = f"https://raw.githubusercontent.com/fabricecaruso/es-theme-carbon/{revision}/art/consoles/{upstream}.png"
        data = fetch(url)
        (OUT / "sources" / f"{system}-console.png").write_bytes(data)
        console = Image.open(io.BytesIO(data)).convert("RGBA")
        if console.getbbox():
            console = console.crop(console.getbbox())
        console = ImageOps.contain(console, (670,390), Image.Resampling.LANCZOS)
        composed = base.crop((0,0,650,544)).resize((960,544),Image.Resampling.LANCZOS)
        composed.alpha_composite(console, (960-console.width+20, 70))
        composed.convert("RGB").save(OUT / "backgrounds" / f"{system}.png", optimize=True)
        provenance.append({"file": f"backgrounds/{system}.png", "source": url,
                           "treatment": "Original hardware composited on the FLUX general background."})
    for system in ("generic", "homebrew"):
        composed = base.copy()
        logo = Image.open(OUT / "logos" / f"platform_{system}.png").convert("RGBA")
        logo = logo.resize((512,256), Image.Resampling.LANCZOS)
        composed.alpha_composite(logo,(430,90))
        composed.convert("RGB").save(OUT / "backgrounds" / f"{system}.png", optimize=True)
    (OUT / "provenance.json").write_text(json.dumps(provenance,indent=2)+"\n")


def compact_logos():
    """Arrange the original marks/wordmarks on small square transparent plates."""
    for name in LOGOS:
        image=Image.open(io.BytesIO(cairosvg.svg2png(bytestring=(OUT / "sources" / f"{name}.svg").read_bytes(),output_width=768))).convert("RGBA")
        image=image.crop(image.getbbox())
        image.putdata([(245,248,255,a) if max(r,g,b)<85 else (r,g,b,a) for r,g,b,a in image.getdata()])
        canvas=Image.new("RGBA",(128,128))
        split={"psx":.27,"gamecube":.23,"dreamcast":.21}.get(name)
        if split:
            edge=int(image.width*split)
            mark=image.crop((0,0,edge,image.height))
            word=image.crop((edge,0,image.width,image.height))
            mark=mark.crop(mark.getbbox()); word=word.crop(word.getbbox())
            if name=="psx":
                header=ImageOps.contain(mark,(58,58),Image.Resampling.LANCZOS)
                header_white=Image.new("RGBA",header.size,"white")
                header_white.putalpha(header.getchannel("A"))
                header_canvas=Image.new("RGBA",(64,64))
                header_canvas.alpha_composite(header_white,((64-header.width)//2,(64-header.height)//2))
                header_canvas.save(OUT / "ui/ui_psx.png",optimize=True)
            mark=ImageOps.contain(mark,(64,68),Image.Resampling.LANCZOS)
            word=ImageOps.contain(word,(116,30),Image.Resampling.LANCZOS)
            canvas.alpha_composite(mark,((128-mark.width)//2,8))
            canvas.alpha_composite(word,((128-word.width)//2,88))
        else:
            image=ImageOps.contain(image,(116,104),Image.Resampling.LANCZOS)
            canvas.alpha_composite(image,((128-image.width)//2,(128-image.height)//2))
        canvas.save(OUT / "logos" / f"platform_{name}.png",optimize=True)
    emblem=Image.open(OUT / "ui/emblem.png").convert("RGBA")
    canvas=Image.new("RGBA",(128,128)); canvas.alpha_composite(emblem,(32,32))
    canvas.save(OUT / "logos/platform_generic.png",optimize=True)


def service_resize(image, endpoint, size):
    """Send raster bytes to imageproxy; the service owns trimming and resampling."""
    body = io.BytesIO()
    image.save(body, format="PNG")
    query = urllib.parse.urlencode(dict(width=size[0], height=size[1], mode="contain", trim="alpha"))
    url = endpoint + ("&" if "?" in endpoint else "?") + query
    request = urllib.request.Request(url, data=body.getvalue(), headers={"Content-Type": "application/octet-stream"})
    with urllib.request.urlopen(request, timeout=30) as response:
        data = response.read(512*1024+1)
        if response.headers.get_content_type() != "image/png" or len(data) > 512*1024:
            raise ValueError("imageproxy returned an invalid logo response")
    result = Image.open(io.BytesIO(data))
    if not (0 < result.width <= size[0] and 0 < result.height <= size[1]):
        raise ValueError("imageproxy returned an oversized logo")
    return result.convert("RGBA")


def service_logos(endpoint):
    """Rebuild card plates and tight header variants from the retained originals."""
    (OUT / "headers").mkdir(parents=True, exist_ok=True)
    provenance = json.loads((OUT / "provenance.json").read_text())
    provenance = [entry for entry in provenance if not entry["file"].startswith("headers/")]
    for name in [*LOGOS, "generic"]:
        source = f"sources/{name}.svg" if name != "generic" else "ui/emblem.png"
        if name == "generic":
            image = Image.open(OUT / source).convert("RGBA")
        else:
            vector = (OUT / source).read_bytes()
            image = Image.open(io.BytesIO(cairosvg.svg2png(bytestring=vector, output_width=1024))).convert("RGBA")
            image.putdata([(245,248,255,a) if max(r,g,b)<85 else (r,g,b,a) for r,g,b,a in image.getdata()])
        canvas = Image.new("RGBA", (128,128))
        split = {"psx":.27, "gamecube":.23, "dreamcast":.21}.get(name)
        if split:
            edge = int(image.width*split)
            mark = service_resize(image.crop((0,0,edge,image.height)), endpoint, (72,76))
            word = service_resize(image.crop((edge,0,image.width,image.height)), endpoint, (116,28))
            canvas.alpha_composite(mark, ((128-mark.width)//2, 4))
            canvas.alpha_composite(word, ((128-word.width)//2, 92))
        elif name == "gba":
            # Stack the original GAME BOY / ADVANCE lettering, not a replacement icon.
            # The italic words overlap horizontally; a vertical crop cuts the A/Y.
            mask = Image.new("L", image.size)
            ImageDraw.Draw(mask).polygon([(0,0), (int(image.width*.535),0),
                (int(image.width*.496),int(image.height*.62)), (int(image.width*.496),image.height),
                (0,image.height)], fill=255)
            empty = Image.new("RGBA", image.size)
            first = service_resize(Image.composite(image,empty,mask), endpoint, (116,48))
            second = service_resize(Image.composite(empty,image,mask), endpoint, (116,48))
            top = (128-first.height-second.height-6)//2
            canvas.alpha_composite(first, ((128-first.width)//2, top))
            canvas.alpha_composite(second, ((128-second.width)//2, top+first.height+6))
        else:
            fitted = service_resize(image, endpoint, (116,108))
            canvas.alpha_composite(fitted, ((128-fitted.width)//2, (128-fitted.height)//2))
        canvas.save(OUT / "logos" / f"platform_{name}.png", optimize=True)
        header = service_resize(canvas if split or name == "gba" else image, endpoint, (128,64))
        header.save(OUT / "headers" / f"header_{name}.png", optimize=True)
        card_file = f"logos/platform_{name}.png"
        treatment = "Original lettering/marks composed on a 128x128 card; imageproxy contain + trim=alpha."
        entry = next((entry for entry in provenance if entry["file"] == card_file), None)
        if entry is None:
            provenance.append({"file":card_file, "source":source, "treatment":treatment})
        else:
            entry["treatment"] = treatment
        provenance.append({"file":f"headers/header_{name}.png", "source":source,
                           "treatment":"Original logo, alpha-trimmed and fitted within 128x64 by imageproxy."
                           + (" Original GAME BOY / ADVANCE lettering stacked." if name == "gba" else "")})
    (OUT / "provenance.json").write_text(json.dumps(provenance, indent=2)+"\n")
    print("Prepared 25 card logos and 25 transparent header logos through imageproxy.")


def preview_fixtures(directory):
    """Optional host-only sample covers; never included in the application VPK."""
    target=Path(directory)
    target.mkdir(parents=True,exist_ok=True)
    games=["Bloody Roar II (USA)","Castlevania - Symphony of the Night (USA)","Crash Bandicoot (USA)",
           "Gran Turismo (USA)","Metal Gear Solid (USA) (Disc 1)","Resident Evil (USA)",
           "Silent Hill (USA)","Dino Crisis (USA)","Tomb Raider (USA)","Ridge Racer (USA)"]
    for index,name in enumerate(games):
        url="https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation/master/Named_Boxarts/"+urllib.parse.quote(name+".png")
        try:
            data=fetch(url)
            # This upstream inventory uses relative Git symlinks for aliases.
            if not data.startswith(b"\x89PNG") and len(data)<1024:
                alias=data.decode().strip()
                if alias.endswith(".png"):
                    data=fetch(urllib.parse.urljoin(url,urllib.parse.quote(alias)))
            image=Image.open(io.BytesIO(data)).convert("RGB")
            image.thumbnail((256,320),Image.Resampling.LANCZOS)
            image.save(target/f"{index}.png")
        except urllib.error.HTTPError as error:
            print(f"Preview fixture unavailable: {name} ({error.code})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--static", action="store_true")
    parser.add_argument("--finish", action="store_true")
    parser.add_argument("--compact", action="store_true")
    parser.add_argument("--controls",action="store_true")
    parser.add_argument("--console-cards", action="store_true", help="Rebuild console material and border-only focus locally")
    parser.add_argument("--logo-service", metavar="URL", help="Rebuild console card/header logos using an imageproxy /image endpoint and retained sources")
    parser.add_argument("--background", nargs=2, metavar=("NAME", "SOURCE"))
    parser.add_argument("--preview-fixtures",metavar="DIRECTORY")
    args = parser.parse_args()
    if args.static:
        static_assets()
    if args.background:
        background(*args.background)
    if args.finish:
        finish_assets()
    if args.preview_fixtures:
        preview_fixtures(args.preview_fixtures)
    if args.compact or args.static:
        compact_logos()
    if args.controls:
        controls_assets()
    if args.console_cards:
        console_assets()
    if args.logo_service:
        service_logos(args.logo_service)
