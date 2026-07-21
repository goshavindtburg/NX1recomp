import os, re
from PIL import Image, ImageDraw

D = r"D:\nx1-rexglue\nx1_mp\out\build\win-amd64-release\texdump"
OUT = r"C:\Users\gosha\AppData\Local\Temp\claude\D--nx1-rexglue\916bdd44-64e3-4b66-8797-2e6e62e12c1e\scratchpad"

files = sorted(f for f in os.listdir(D) if re.match(r"c\d+_\d+_.*\.bmp$", f))
print(f"{len(files)} images")

CELL = 132
COLS = 8
PER = 40  # 5 rows per sheet

for si in range(0, len(files), PER):
    chunk = files[si:si + PER]
    rows = (len(chunk) + COLS - 1) // COLS
    sheet = Image.new("RGB", (COLS * CELL, rows * (CELL + 12)), (30, 30, 30))
    d = ImageDraw.Draw(sheet)
    for i, fn in enumerate(chunk):
        im = Image.open(os.path.join(D, fn)).convert("RGB").resize((CELL - 4, CELL - 4), Image.NEAREST)
        x, y = (i % COLS) * CELL, (i // COLS) * (CELL + 12)
        sheet.paste(im, (x + 2, y + 12))
        m = re.match(r"c(\d+)_(\d+)_([0-9A-F]+)_(\d+)x(\d+)_f(\d+)", fn)
        d.text((x + 3, y + 1), f"{m.group(1)}/{m.group(2)} {m.group(3)[:6]} f{m.group(6)}",
               fill=(200, 200, 120))
    n = si // PER + 1
    sheet.save(os.path.join(OUT, f"sheet{n:02d}.png"))
    print(f"sheet{n:02d}.png  <- {chunk[0]} .. {chunk[-1]}")
