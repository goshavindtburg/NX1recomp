"""Re-derive each captured texture's geometry from its RAW fetch dwords.

Answers the one open question on the proven over-read: did the guest actually ask for the
size we used, or did we misread the size field?

Layout per the XDK's GPUTEXTURE_FETCH_CONSTANT (2D):
  raw[2]: width_minus_1  = bits  0..12
          height_minus_1 = bits 13..25
  raw[0]: pitch          = ((d0 >> 22) & 0x1FF) << 5
          tiled          = bit 31
  raw[1]: format = bits 0..5, base_address = ((d1 >> 12) & 0xFFFFF) << 12
"""
import os, re

D = r"D:\nx1-rexglue\nx1_mp\out\build\win-amd64-release\texdump"

bad = 0
rows = []
for fn in sorted(os.listdir(D)):
    if not re.match(r"c\d+_\d+_.*\.txt$", fn):
        continue
    txt = open(os.path.join(D, fn)).read()
    mr = re.search(r"raw=([0-9A-F:]+)", txt)
    if not mr:
        continue
    raw = [int(x, 16) for x in mr.group(1).split(":")]
    # Anchored: a bare "width=" regex also matches inside "block_width=", which silently
    # compared the block dimensions and reported all 193 captures as disagreeing.
    got = {k: int(v) for k, v in
           re.findall(r"(?:^|\s)(width|height|format|pitch_pixels)=(\d+)", txt)}
    base = int(re.search(r"base=([0-9A-F]+)", txt).group(1), 16)

    w = (raw[2] & 0x1FFF) + 1
    h = ((raw[2] >> 13) & 0x1FFF) + 1
    pitch = ((raw[0] >> 22) & 0x1FF) << 5
    fmt = raw[1] & 0x3F
    addr = ((raw[1] >> 12) & 0xFFFFF) << 12

    ok = (w == got["width"] and h == got["height"] and fmt == got["format"]
          and pitch == got["pitch_pixels"] and addr == base)
    if not ok:
        bad += 1
        rows.append((fn, got, dict(w=w, h=h, fmt=fmt, pitch=pitch, addr=addr), raw))

print(f"checked captures with raw dwords; parse disagreements: {bad}")
for fn, got, mine, raw in rows:
    print(f"\n  {fn}")
    print(f"     renderer says {got['width']}x{got['height']} fmt={got['format']} "
          f"pitch={got['pitch_pixels']}")
    print(f"     raw dwords say {mine['w']}x{mine['h']} fmt={mine['fmt']} "
          f"pitch={mine['pitch']} base={mine['addr']:08X}")
    print(f"     raw={':'.join('%08X' % r for r in raw)}")
