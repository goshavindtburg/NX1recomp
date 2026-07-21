"""Read a run's log across rotation.

The logger rotates at 5 MB: nx1_mp_NNN.log is the CURRENT tail, nx1_mp_NNN.1.log the most
recent rotated chunk, .2 older, and so on. Reading only the base file silently loses
everything but the last 5 MB -- which already cost one wrong reading this session (a REFTEX
count went 300 -> 0 between two greps purely because rotation happened in between).
"""
import os, re, glob

LOGDIR = r"D:\nx1-rexglue\nx1_mp\out\build\win-amd64-release\logs"


def run_parts(run):
    """Every file for run NNN, oldest first."""
    base = os.path.join(LOGDIR, f"nx1_mp_{run:03d}")
    parts = []
    for f in glob.glob(base + ".*.log"):
        m = re.search(r"\.(\d+)\.log$", f)
        if m:
            parts.append((int(m.group(1)), f))
    parts.sort(key=lambda p: -p[0])          # .2 before .1
    files = [f for _, f in parts]
    if os.path.exists(base + ".log"):
        files.append(base + ".log")          # base is newest
    return files


def lines(run):
    for f in run_parts(run):
        with open(f, errors="ignore") as fh:
            yield from fh


def latest_run():
    runs = set()
    for f in glob.glob(os.path.join(LOGDIR, "nx1_mp_*.log")):
        m = re.search(r"nx1_mp_(\d+)", os.path.basename(f))
        if m:
            runs.add(int(m.group(1)))
    return max(runs) if runs else None


if __name__ == "__main__":
    r = latest_run()
    print(f"latest run {r}")
    for f in run_parts(r):
        print(f"  {os.path.basename(f):24s} {os.path.getsize(f)/1e6:6.2f} MB")
