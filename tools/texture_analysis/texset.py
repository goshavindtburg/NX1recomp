"""Diff the SET of textures the reference loads (REFTEX) against the set we decode (D9TEX).

No per-draw synchronisation needed -- that approach is a dead end, since the two backends run
independently and FETCHCMP reported 27,616 bogus mismatches because of it.

  we decode textures the reference never loads -> we bind or resolve differently
  sets match                                   -> same textures, so the difference is WHICH
                                                  subresource/level, not which texture
"""
import re, collections
import loglib

RX_REF = re.compile(r"REFTEX base=([0-9A-F]+) mip=([0-9A-F]+) (\d+)x(\d+) fmt=(\d+)")
RX_D9 = re.compile(r"D9TEX base=([0-9A-F]+) mip=([0-9A-F]+) (\d+)x(\d+) fmt=(\d+)")

run = loglib.latest_run()
ref, d9 = set(), set()
ref_by_addr, d9_by_addr = collections.defaultdict(set), collections.defaultdict(set)
for line in loglib.lines(run):
    m = RX_REF.search(line)
    if m:
        k = (m.group(1), int(m.group(3)), int(m.group(4)), int(m.group(5)))
        ref.add(k)
        ref_by_addr[m.group(1)].add(k[1:])
        continue
    m = RX_D9.search(line)
    if m:
        k = (m.group(1), int(m.group(3)), int(m.group(4)), int(m.group(5)))
        d9.add(k)
        d9_by_addr[m.group(1)].add(k[1:])

print(f"run {run}")
print(f"  REFTEX distinct textures: {len(ref)}  ({len(ref_by_addr)} addresses)")
print(f"  D9TEX  distinct textures: {len(d9)}  ({len(d9_by_addr)} addresses)")

both_addr = set(ref_by_addr) & set(d9_by_addr)
print(f"\n  addresses BOTH loaded: {len(both_addr)}")
print(f"  addresses only WE loaded:        {len(set(d9_by_addr) - set(ref_by_addr))}")
print(f"  addresses only the REFERENCE loaded: {len(set(ref_by_addr) - set(d9_by_addr))}")

# For shared addresses, do we agree on geometry?
agree = [a for a in both_addr if ref_by_addr[a] & d9_by_addr[a]]
disagree = sorted(both_addr - set(agree))
print(f"\n  of the shared addresses, geometry agrees on {len(agree)}, "
      f"DISAGREES on {len(disagree)}")
for a in disagree[:25]:
    print(f"    {a}: ours={sorted(d9_by_addr[a])}  ref={sorted(ref_by_addr[a])}")

# Where we load extra geometries at an address the reference also uses
extra = [(a, d9_by_addr[a] - ref_by_addr[a]) for a in agree if d9_by_addr[a] - ref_by_addr[a]]
print(f"\n  shared addresses where we ALSO load a geometry the reference never does: {len(extra)}")
for a, e in extra[:25]:
    print(f"    {a}: extra={sorted(e)}  ref={sorted(ref_by_addr[a])}")
