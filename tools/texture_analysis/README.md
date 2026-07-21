# Texture-analysis scripts (D3D9 speckle investigation)

Offline analysis for the dumps the native D3D9 renderer writes to
`nx1_mp/out/build/win-amd64-release/texdump/`. All read-only; none touch the game.

Written during the 2026-07-21 session. Paths are hardcoded to that build directory — edit the
`D` / `LOGDIR` constants if the tree moves.

| script | what it answers |
|---|---|
| `loglib.py` | Reads a run's log **across rotation**. The logger rotates at 5 MB: `nx1_mp_NNN.log` is the current tail, `.1.log` the previous chunk, `.2` older. Reading only the base file silently loses everything but the last 5 MB — that cost one wrong reading (a REFTEX count went 300 → 0 between two greps purely because rotation happened in between). Import this rather than opening logs directly. |
| `indep.py` | **Clean-room decoder.** Xenos tiled addressing straight from the XDK formula plus a textbook DXT decoder, sharing no code with the renderer. Decodes a captured `.bin` and compares against the renderer's own `.bmp`. This is what proves whether corruption is in the bytes or in our decode. |
| `watchdec.py` | Decodes every version of every `watch_*` address (see ADDRWATCH) to show how a slot's content changes over time. |
| `texset.py` | Diffs the texture SET the reference loads (REFTEX) against the set we decode (D9TEX). Needs no per-draw synchronisation, unlike FETCHCMP. |
| `drift.py` | Diffs every live TOML value against its compiled default. Config drift has cost this investigation repeatedly; run it before trusting any experiment. |
| `sheets.py` | Contact sheets of dumped BMPs, so a capture can be eyeballed in one image. |
| `checkraw.py` | Re-derives each capture's geometry from its raw fetch dwords per `GPUTEXTURE_FETCH_CONSTANT`, to check our parse. |
| `overlap.py` | Finds captures whose byte range runs into a later texture's base (over-reading). |
| `dupes.py` | Finds base addresses bound with more than one geometry. |
| `pagemap.py` | Maps decoded blocks back to guest pages via the tiling formula, to test whether corruption is page-coherent. |

## Two traps these scripts have already fallen into

**Do not score corruption with a within-block variance metric.** The `noisy%` figures in
`sheets.py`/`watchdec.py` measure standard deviation *inside* each 4×4 block, but this artifact is
flat-coloured blocks varying *between* each other — so unmistakable rainbow noise scores ~3%. Judge
dumps by eye, or write a between-block metric.

**`Image.open(p).convert("RGB")` is already RGB.** Adding `[:, :, ::-1]` double-swaps and
manufactures a large mean-difference between two identical images — it briefly looked like proof
the renderer's decode was broken.

More generally: content cannot distinguish corruption from legitimately dark or sparse art. Two
automatic "find the corrupt texture" selectors were built and both captured pristine decal atlases.
The operator's eyes are the only selector that has never returned a false positive.
