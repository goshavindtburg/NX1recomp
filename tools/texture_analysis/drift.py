"""Diff every live TOML value against its compiled default.

Config drift has now cost this investigation three times in one session, so check the whole
file mechanically instead of spot-checking the cvar currently under suspicion.
"""
import os, re, glob

TOML = r"D:\nx1-rexglue\nx1_mp\out\build\win-amd64-release\nx1_mp.toml"
SRC = [r"D:\nx1-rexglue\nx1_mp\src", r"D:\nx1-rexglue\rexglue-sdk\src",
       r"D:\nx1-rexglue\rexglue-sdk\include"]

# --- compiled defaults ---------------------------------------------------
defaults = {}
pat = re.compile(r"REXCVAR_DEFINE_(\w+)\s*\(\s*(\w+)\s*,\s*([^,]+?)\s*,", re.S)
for root in SRC:
    for ext in ("*.cpp", "*.h"):
        for path in glob.glob(os.path.join(root, "**", ext), recursive=True):
            if "out" + os.sep in path:
                continue
            try:
                txt = open(path, encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            for typ, name, dflt in pat.findall(txt):
                defaults[name] = (typ.lower(), dflt.strip(), os.path.basename(path))

# --- live values ---------------------------------------------------------
live = {}
for line in open(TOML, encoding="utf-8", errors="ignore"):
    m = re.match(r"^([A-Za-z_][\w]*)\s*=\s*(.+?)\s*$", line)
    if m:
        live[m.group(1)] = m.group(2)


def norm(v):
    v = v.strip().strip('"').lower()
    return {"true": "1", "false": "0", "0u": "0"}.get(v, v.rstrip("u"))


print(f"{len(live)} live keys, {len(defaults)} compiled defaults\n")
drift = []
for k, v in sorted(live.items()):
    if k not in defaults:
        continue
    typ, dflt, where = defaults[k]
    if norm(v) != norm(dflt):
        drift.append((k, dflt, v, where))

print(f"=== {len(drift)} keys DIFFER from their compiled default ===")
for k, dflt, v, where in drift:
    print(f"  {k:42s} default={dflt:<10s} live={v:<12s} ({where})")

missing = [k for k in defaults if k not in live]
print(f"\n=== {len(missing)} compiled cvars absent from the TOML (using defaults) ===")
for k in sorted(missing)[:40]:
    print(f"  {k:42s} default={defaults[k][1]}")
