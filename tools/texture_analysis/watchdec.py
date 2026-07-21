"""Decode every version of every watched address.

If a version decodes CLEAN, the correct bytes ARE at that address at some moment and we
sampled the wrong one -- a timing bug. If no version is ever clean, the content is never
there and the address itself is wrong.
"""
import os, re, collections
import numpy as np
from PIL import Image

D = r"D:\nx1-rexglue\nx1_mp\out\build\win-amd64-release\texdump"
OUT = r"C:\Users\gosha\AppData\Local\Temp\claude\D--nx1-rexglue\916bdd44-64e3-4b66-8797-2e6e62e12c1e\scratchpad"


def tiled(x, y, pitch, bl):
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bl + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bl
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2)
            + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F))


def dxt_block(b, dxt1):
    o = np.zeros((4, 4, 3), np.uint8)
    off = 0 if dxt1 else 8
    c0 = b[off] | (b[off + 1] << 8)
    c1 = b[off + 2] | (b[off + 3] << 8)
    bits = int.from_bytes(b[off + 4:off + 8], "little")
    f = lambda c: ((((c >> 11) & 31) << 3) | (((c >> 11) & 31) >> 2),
                   (((c >> 5) & 63) << 2) | (((c >> 5) & 63) >> 4),
                   ((c & 31) << 3) | ((c & 31) >> 2))
    e0, e1 = f(c0), f(c1)
    if c0 > c1 or not dxt1:
        cols = [e0, e1, tuple((2 * e0[i] + e1[i]) // 3 for i in range(3)),
                tuple((e0[i] + 2 * e1[i]) // 3 for i in range(3))]
    else:
        cols = [e0, e1, tuple((e0[i] + e1[i]) // 2 for i in range(3)), (0, 0, 0)]
    for i in range(16):
        o[i // 4, i % 4] = cols[(bits >> (2 * i)) & 3]
    return o


def decode(raw, w, h, fmt):
    dxt1 = fmt == 18
    bpb = 8 if dxt1 else 16
    bl = 3 if dxt1 else 4
    pb = max(w // 4, 32) if w // 4 < 32 else w // 4
    img = np.zeros((h, w, 3), np.uint8)
    for by in range(h // 4):
        for bx in range(w // 4):
            o = tiled(bx, by, pb, bl)
            if o + bpb > len(raw):
                continue
            blk = bytearray(raw[o:o + bpb])
            blk[0::2], blk[1::2] = blk[1::2], blk[0::2]
            img[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4] = dxt_block(blk, dxt1)
    return img


def noisy(a, w, h):
    g = a.astype(float).mean(axis=2)
    t = g[:h // 4 * 4, :w // 4 * 4].reshape(h // 4, 4, w // 4, 4)
    t = t.transpose(0, 2, 1, 3).reshape(h // 4, w // 4, 16)
    return 100 * (t.std(axis=2) > 40).mean()


groups = collections.defaultdict(list)
for fn in sorted(os.listdir(D)):
    m = re.match(r"watch_([0-9A-F]+)_(\d+)x(\d+)_f(\d+)_v(\d+)\.bin$", fn)
    if m:
        groups[m.group(1)].append((int(m.group(5)), fn, int(m.group(2)),
                                   int(m.group(3)), int(m.group(4))))

print(f"{'address':10s} {'dims':>10s} {'fmt':>4s}  noise% per version (v0 -> vN)")
interesting = []
for addr, versions in sorted(groups.items()):
    versions.sort()
    scores, imgs = [], []
    for _, fn, w, h, fmt in versions:
        raw = open(os.path.join(D, fn), "rb").read()
        im = decode(raw, w, h, fmt)
        scores.append(noisy(im, w, h))
        imgs.append(im)
    w, h, fmt = versions[0][2], versions[0][3], versions[0][4]
    print(f"{addr:10s} {w:4d}x{h:<5d} f{fmt:<3d} " + " ".join(f"{s:5.1f}" for s in scores))
    if max(scores) - min(scores) > 3.0:
        interesting.append(addr)
        S = 96
        sheet = Image.new("RGB", (len(imgs) * (S + 4), S), (30, 30, 30))
        for i, im in enumerate(imgs):
            sheet.paste(Image.fromarray(im).resize((S, S), Image.NEAREST), (i * (S + 4), 0))
        sheet.save(os.path.join(OUT, f"watch_{addr}.png"))

print(f"\naddresses whose content changes quality across versions: {interesting}")
