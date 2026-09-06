#!/usr/bin/env python3
from pathlib import Path
from PIL import Image

p = Path(__file__).resolve().parents[1] / "icon.bmp"
im = Image.open(p)
print("icon:", im.size, im.mode)
if im.size != (32, 32):
    raise SystemExit("ERROR: icon must be 32x32")
if im.mode != "P":
    raise SystemExit("ERROR: icon must be indexed/paletted BMP")
colors = im.getcolors(maxcolors=256) or []
print("palette colors used:", len(colors))
if len(colors) > 16:
    raise SystemExit("ERROR: icon uses more than 16 colors")
print("OK")
