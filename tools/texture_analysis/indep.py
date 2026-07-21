"""Independent decode of the captured guest bytes.

Clean-room: Xenos 2D tiled addressing straight from the XDK formula
(texture_util::GetTiledOffset2D), 8-in-16 byte swap, then a textbook DXT5
decoder. Shares no code with the renderer, so agreement is meaningful.

  my decode CLEAN, renderer corrupt  -> our untile/BC path is wrong
  both corrupt                       -> the bytes are another asset's
"""
import os
import re
import sys
import numpy as np
from PIL import Image

D = r"D:\nx1-rexglue\nx1_mp\out\build\win-amd64-release\texdump"
OUT = r"C:\Users\gosha\AppData\Local\Temp\claude\D--nx1-rexglue\916bdd44-64e3-4b66-8797-2e6e62e12c1e\scratchpad"


def tiled_offset_2d(x, y, pitch, bpb_log2):
    """Port of texture_util::GetTiledOffset2D (util.cpp:424). Scalar, on purpose."""
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2)
            + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def decode_dxt5_block(b):
    """b = 16 bytes, little-endian order. Returns 4x4 RGBA uint8."""
    a0, a1 = b[0], b[1]
    abits = int.from_bytes(b[2:8], "little")
    alpha = [a0, a1]
    if a0 > a1:
        alpha += [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
    else:
        alpha += [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255]

    c0 = b[8] | (b[9] << 8)
    c1 = b[10] | (b[11] << 8)
    cbits = int.from_bytes(b[12:16], "little")

    def rgb(c):
        r = (c >> 11) & 0x1F
        g = (c >> 5) & 0x3F
        bl = c & 0x1F
        return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (bl << 3) | (bl >> 2))

    e0, e1 = rgb(c0), rgb(c1)
    # DXT5 always uses the 4-colour (opaque) interpolation.
    cols = [e0, e1,
            tuple((2 * e0[i] + e1[i]) // 3 for i in range(3)),
            tuple((e0[i] + 2 * e1[i]) // 3 for i in range(3))]

    out = np.zeros((4, 4, 4), dtype=np.uint8)
    for i in range(16):
        ai = (abits >> (3 * i)) & 7
        ci = (cbits >> (2 * i)) & 3
        out[i // 4, i % 4, :3] = cols[ci]
        out[i // 4, i % 4, 3] = alpha[ai]
    return out


def decode(path, w, h, pitch_blocks, bpb=16, swap16=True):
    raw = open(path, "rb").read()
    nbx, nby = w // 4, h // 4
    img = np.zeros((h, w, 4), dtype=np.uint8)
    missing = 0
    for by in range(nby):
        for bx in range(nbx):
            off = tiled_offset_2d(bx, by, pitch_blocks, 4)
            if off + bpb > len(raw):
                missing += 1
                continue
            blk = bytearray(raw[off:off + bpb])
            if swap16:
                blk[0::2], blk[1::2] = blk[1::2], blk[0::2]
            img[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4] = decode_dxt5_block(blk)
    return img, missing


def score(img):
    """Same corrupt metric the renderer uses: black-or-noise 4x4 blocks."""
    a = img[:, :, :3].astype(np.float32).mean(axis=2)
    h, w = a.shape
    t = a[:h // 4 * 4, :w // 4 * 4].reshape(h // 4, 4, w // 4, 4)
    t = t.transpose(0, 2, 1, 3).reshape(h // 4, w // 4, 16)
    bad = (t.mean(axis=2) < 8) | (t.std(axis=2) > 40)
    return 100.0 * bad.mean()


for fn in sorted(os.listdir(D)):
    m = re.match(r"(bad\d+)_([0-9A-F]+)_(\d+)x(\d+)_f(\d+)\.bin$", fn)
    if not m:
        continue
    tag, addr, w, h, fmt = m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), m.group(5)
    txt = os.path.join(D, fn[:-4] + ".txt")
    pitch_blocks = w // 4
    if os.path.exists(txt):
        s = open(txt).read()
        mm = re.search(r"block_pitch_h=(\d+)", s)
        if mm:
            pitch_blocks = int(mm.group(1))
    mine, missing = decode(os.path.join(D, fn), w, h, pitch_blocks)
    ours = np.asarray(Image.open(os.path.join(D, fn[:-4] + ".bmp")).convert("RGB"))
    ms, os_ = score(mine), score(ours[:, :, ::-1])
    same = "n/a"
    if ours.shape[0] == h and ours.shape[1] == w:
        d = np.abs(mine[:, :, :3].astype(int) - ours[:, :, ::-1].astype(int)).mean()
        same = f"{d:.1f}"
    print(f"{tag} {addr} {w}x{h} f{fmt}: independent={ms:5.1f}%  renderer={os_:5.1f}%  "
          f"mean|diff|={same}  oob_blocks={missing}")
    Image.fromarray(mine[:, :, :3]).save(os.path.join(OUT, f"indep_{tag}_{addr}.png"))
