"""Map each decoded block back to its guest byte offset via the Xenos tiling
formula, then ask: is the corruption PAGE-COHERENT, and where does it sit in
linear guest memory (prefix? suffix? scattered?).

Corruption that is a contiguous SUFFIX  => the delivery stopped short.
Corruption that is page-coherent scatter => individual pages were skipped/stale.
Corruption that ignores page boundaries  => it is not a memory-delivery effect
                                            at all, it is our decode.
"""
import os
import numpy as np
from PIL import Image

D = r"D:\nx1-rexglue\nx1_mp\out\build\win-amd64-release\texdump"


def tiled_offset_2d(x, y, pitch, bpb_log2):
    """Vectorised port of texture_util::GetTiledOffset2D (util.cpp:424)."""
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (
        ((offset & ~0x1FF) << 3)
        + ((y & 16) << 7)
        + ((offset & 0x1C0) << 2)
        + ((((((y & 8) >> 2) + (x >> 3)) & 3)) << 6)
        + (offset & 0x3F)
    )


CASES = [
    # name,                                   w,   h,   bytes/block, blockdim
    ("MIXED_15196000_512x512_f49.bmp", 512, 512, 16, 4),
    ("MIXED_12D98000_512x256_f49.bmp", 512, 256, 16, 4),
    ("MIXED_15E2F000_512x256_f20.bmp", 512, 256, 16, 4),
    ("MIXED_15EBA000_256x256_f20.bmp", 256, 256, 16, 4),
]

for name, w, h, bpb, bdim in CASES:
    p = os.path.join(D, name)
    if not os.path.exists(p):
        print("MISSING", name)
        continue
    a = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32)
    nbx, nby = w // bdim, h // bdim
    t = a.reshape(nby, bdim, nbx, bdim, 3).transpose(0, 2, 1, 3, 4).reshape(nby, nbx, -1)
    mean, std = t.mean(axis=2), t.std(axis=2)
    black = mean < 8.0
    noisy = std > 40.0
    corrupt = black | noisy

    bx = np.arange(nbx)[None, :].repeat(nby, 0)
    by = np.arange(nby)[:, None].repeat(nbx, 1)
    bpb_log2 = int(np.log2(bpb))
    off = tiled_offset_2d(bx, by, nbx, bpb_log2)
    page = off >> 12

    npages = int(page.max()) + 1
    print(f"\n=== {name}  {nbx}x{nby} blocks, {npages} pages, "
          f"{corrupt.mean()*100:.1f}% corrupt ===")

    # per-page corruption fraction
    frac = np.zeros(npages)
    cnt = np.zeros(npages)
    for pg in range(npages):
        m = page == pg
        if m.sum():
            frac[pg] = corrupt[m].mean()
            cnt[pg] = m.sum()

    valid = cnt > 0
    fv = frac[valid]
    # page coherence: how many pages are all-clean or all-corrupt vs mixed?
    pure_clean = (fv < 0.02).sum()
    pure_corr = (fv > 0.98).sum()
    mixed = len(fv) - pure_clean - pure_corr
    print(f"  pages: {len(fv)} total | all-clean {pure_clean} | "
          f"all-corrupt {pure_corr} | MIXED {mixed}")
    print(f"  -> page-coherent fraction: "
          f"{100*(pure_clean+pure_corr)/len(fv):.1f}%")

    # is corruption a linear suffix?
    order = np.argsort(np.where(valid)[0])
    seq = fv[order]
    first_bad = next((i for i, v in enumerate(seq) if v > 0.5), None)
    if first_bad is not None:
        tail = seq[first_bad:]
        print(f"  first mostly-corrupt page at index {first_bad}/{len(seq)}; "
              f"of pages after it, {100*(tail>0.5).mean():.1f}% are corrupt")
    print("  page corrupt%: " +
          " ".join(f"{v*100:3.0f}" for v in seq[:48]) +
          (" ..." if len(seq) > 48 else ""))
