"""Does the byte range we read for one texture RUN INTO the next texture's base?

Every capture records base and the byte count we actually read. If [base, base+bytes)
contains another captured texture's base address, we are reading that texture's memory as
if it were part of ours -- which decodes as foreign garbage in exactly the region past the
neighbour's start.

Where both captures exist, the overlap is checked byte-for-byte, so this is not an
inference about addresses: it is the same bytes appearing in both files.
"""
import os, re

D = r"D:\nx1-rexglue\nx1_mp\out\build\win-amd64-release\texdump"

tex = []
for fn in sorted(os.listdir(D)):
    m = re.match(r"(c\d+_\d+)_([0-9A-F]+)_(\d+)x(\d+)_f(\d+)\.txt$", fn)
    if not m:
        continue
    txt = open(os.path.join(D, fn)).read()
    b = int(re.search(r"(?:^|\s)bytes=(\d+)", txt).group(1))
    tex.append(dict(tag=m.group(1), addr=int(m.group(2), 16), w=int(m.group(3)),
                    h=int(m.group(4)), fmt=int(m.group(5)), bytes=b, stem=fn[:-4]))

tex.sort(key=lambda t: t["addr"])
print(f"{len(tex)} captures\n")

hits = []
for i, t in enumerate(tex):
    end = t["addr"] + t["bytes"]
    for u in tex[i + 1:]:
        if u["addr"] >= end:
            break
        if u["addr"] == t["addr"]:
            continue  # same base, different geometry -- handled separately
        hits.append((t, u))

print(f"=== reads that run INTO a later texture's base: {len(hits)} ===\n")
for t, u in hits:
    gap = u["addr"] - t["addr"]
    over = t["addr"] + t["bytes"] - u["addr"]
    print(f"  {t['stem']}")
    print(f"     reads {t['bytes']:6d} B from {t['addr']:08X}, but {u['stem'][:20]} starts at "
          f"{u['addr']:08X} (+{gap} B)")
    print(f"     -> {over} B of our read ({100*over/t['bytes']:.0f}%) is the NEXT texture")
    # byte-for-byte proof where we have both files
    pa = os.path.join(D, t["stem"] + ".bin")
    pb = os.path.join(D, u["stem"] + ".bin")
    if os.path.exists(pa) and os.path.exists(pb):
        A = open(pa, "rb").read()
        B = open(pb, "rb").read()
        seg = A[gap:gap + min(over, len(B))]
        ref = B[:len(seg)]
        if seg:
            same = sum(x == y for x, y in zip(seg, ref)) / len(seg)
            print(f"     overlap is {100*same:.2f}% byte-identical to that texture's own capture")
    print()

# what size WOULD fit in the gap?
print("=== implied real size, if the allocation ends where the neighbour begins ===")
for t, u in hits:
    gap = u["addr"] - t["addr"]
    bpb = 8 if t["fmt"] in (18, 58, 59, 60) else 16
    blocks = gap // bpb
    bw = t["w"] // 4
    if bw and blocks % bw == 0:
        print(f"  {t['stem'][:26]}: declared {t['w']}x{t['h']}, gap {gap} B "
              f"=> {t['w']}x{blocks // bw * 4} would fit exactly")
