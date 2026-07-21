"""Does the same guest base address get bound with DIFFERENT dimensions?

If so, the larger binding reads past the end of whatever actually lives there and
pulls in the next allocation -- which would explain corruption that starts exactly
at the smaller texture's size, with everything before it clean.
"""
import os, re, collections

D = r"D:\nx1-rexglue\nx1_mp\out\build\win-amd64-release\texdump"

recs = []
for fn in sorted(os.listdir(D)):
    m = re.match(r"c(\d+)_(\d+)_([0-9A-F]+)_(\d+)x(\d+)_f(\d+)\.txt$", fn)
    if not m:
        continue
    info = {}
    for line in open(os.path.join(D, fn)):
        for k, v in re.findall(r"(\w+)=([0-9A-Fa-fx]+)", line):
            info[k] = v
    recs.append(dict(batch=m.group(1), idx=m.group(2), addr=m.group(3),
                     w=int(m.group(4)), h=int(m.group(5)), fmt=int(m.group(6)),
                     bytes=int(info.get("bytes", 0)), mip=info.get("mip_address", "?"),
                     pitch=int(info.get("pitch_pixels", 0)), frame=info.get("frame", "?"),
                     stem=fn[:-4]))

print(f"{len(recs)} captures\n")
by_addr = collections.defaultdict(list)
for r in recs:
    by_addr[r["addr"]].append(r)

multi = {a: v for a, v in by_addr.items()
         if len({(x["w"], x["h"], x["fmt"]) for x in v}) > 1}
print(f"=== base addresses bound with MORE THAN ONE geometry: {len(multi)} ===")
for a, v in sorted(multi.items()):
    shapes = sorted({(x["w"], x["h"], x["fmt"], x["bytes"], x["mip"]) for x in v})
    print(f"\n  {a}:")
    for w, h, f, b, mp in shapes:
        who = [x for x in v if (x["w"], x["h"], x["fmt"]) == (w, h, f)]
        print(f"     {w:4d}x{h:<4d} f{f:<3d} {b:7d} B  mip={mp}  "
              f"[{', '.join(x['batch']+'/'+x['idx'] for x in who)}]  frames {[x['frame'] for x in who]}")

# How often does one binding read strictly more bytes than another at the same address?
over = 0
for a, v in multi.items():
    bs = sorted({x["bytes"] for x in v})
    if len(bs) > 1:
        over += 1
print(f"\n=== addresses where one binding reads MORE bytes than another: {over} ===")
