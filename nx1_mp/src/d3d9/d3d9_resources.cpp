/**
 * @file    d3d9_resources.cpp
 * @brief   Guest -> host resource translation for the native D3D9 renderer.
 */

// rex headers must precede <windows.h> (pulled in transitively by d3d9.h):
// rex/thread.h declares Sleep(std::chrono::milliseconds), which the Win32
// Sleep macro otherwise mangles.
#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging/macros.h>

#include "d3d9_log.h"
#include <rex/math.h>
#include <rex/string/buffer.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <d3dcompiler.h>
#include <xxhash.h>

#include "d3d9_flat_map.h"
#include "d3d9_resources.h"
#include "guest_d3d.h"

// Process memory accounting for the periodic MEM stats line (see LogCacheStats). MUST come
// after the headers above: psapi.h uses BOOL/DWORD and does not include windows.h itself.
#include <psapi.h>

/// Paint one PreferLargestForSurface branch white, to identify which one serves the confetti.
///   1 = the `substitute` path (smaller than retained -> retained served)
///   2 = the `equal` path (same area -> current served, retained bypassed)
///   3 = the `adopt`/`fresh` path (largest yet seen -> becomes retained)
/// Back away from a surface until it speckles; whichever mode turns it white is the branch
/// that produced it. Off (0) by default.
/// Keep showing a surface's better-populated texture rather than adopting a much sparser one.
/// See BestTexture::src_permille for why size-based retention could not do this.
REXCVAR_DEFINE_BOOL(nx1_d3d9_prefer_populated, false, "GPU",
                    "Hold a surface on its best-populated texture instead of adopting a "
                    "drastically sparser one (the incomplete-texture speckle)");
REXCVAR_DEFINE_UINT32(nx1_d3d9_prefer_populated_drop, 400, "GPU",
                      "Permille by which a new texture's source must be sparser than the retained "
                      "one before nx1_d3d9_prefer_populated refuses it (400 = 40%)");

REXCVAR_DEFINE_BOOL(nx1_d3d9_prefer_largest, true, "GPU",
                    "Substitute the highest-resolution texture a surface has shown when the "
                    "engine swaps a sampler to a smaller (often unstreamed) LOD. Turn OFF to "
                    "bind exactly what the guest asked for");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_lod, 0, "GPU",
                      "Debug: paint one prefer-largest LOD branch white (1=substitute, "
                      "2=equal, 3=adopt)");

/// Diagnostic: fill every CPU-generated mip level with a flat, distinct colour instead of the
/// filtered image. Separates the two ways the built chain can be wrong: if distant surfaces
/// show clean colour bands, the plumbing (level count, strides, UpdateTexture) is sound and
/// the fault is in BoxFilterHalf/EncodeBcImage; if they still show noise, the plumbing is
/// wrong and the filtered pixels were never the problem.
/// Verify that decoded level 0 actually lands in the staging surface UpdateTexture reads.
///
/// Diagnostic only, and NOT cheap: it copies the level twice and compares. Run it for one session,
/// read the STAGECHECK line, turn it off. See the site for why the double read is the control
/// rather than an accident.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_stagecheck, false, "GPU",
                    "Read decoded level 0 back out of the staging surface twice and compare "
                    "against what we decoded: separates an unstable readback from a write that "
                    "never landed");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_mipfill, 0, "GPU",
                      "Debug mip chain: 1=flat colour per level (tests plumbing), "
                      "2=synthetic gradient as the level-0 source (tests filter+encoder "
                      "without our BC decoder)");

/// Trace everything that happens to one guest address: every guest write we are told about,
/// every resolve that targets it, and every decode we run from it. Set it to a texture that
/// renders as garbage and the log answers the question the screen cannot -- is this memory
/// ever written with good data, is it a resolve destination we are failing to track, or does
/// nothing ever touch it? Hex, e.g. 05091000.
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_track_sampler_n, 0, "GPU",
                      "Debug: with dbg_blend_ps set, RE-POINT dbg_track_addr at whatever texture "
                      "that material currently binds to this sampler (1 = sampler 0, 2 = sampler "
                      "1, ...). Tracking a fixed ADDRESS is unreliable here: the streaming pool "
                      "reassigns addresses, so by the time an address from a dump is entered it "
                      "often belongs to something else -- which shows up as POLL/WRITE lines with "
                      "no BIND or DECODE at all");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_track_addr, 0, "GPU",
                      "Debug: log every write/resolve/decode touching this guest address, and "
                      "paint whatever samples it solid white so it can be identified on screen");
/// How many bytes from nx1_d3d9_dbg_track_addr count as "the tracked texture" when matching DMA
/// copy destinations. The DMA test used to anchor on the base address alone and so only saw
/// copies covering page 0 -- see MirrorDmaCopy. A texture is delivered by many 1-4 page copies to
/// scattered destinations, so the span is what has to be watched. 64 KB covers a 256x256 BC
/// texture including its mip chain; raise it for larger ones.
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_track_bytes, 65536, "GPU",
                      "Debug: byte span from nx1_d3d9_dbg_track_addr treated as the tracked "
                      "texture when matching DMA copy destinations");

/// Drop a deferred DMA copy whose SOURCE a later copy overwrote while it was queued, instead of
/// replaying it. Measured at 43% of landed retries (101 of 236, run 055): the staging page had
/// been recycled to another texture, so replaying planted that texture's bytes into this one.
/// Off restores the old behaviour for an A/B.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dma_drop_clobbered, true, "GPU",
                    "Drop a deferred image-cache copy whose source was overwritten while it "
                    "waited, rather than moving bytes that have already been relocated");

/// Treat an image-cache copy whose source and destination OVERLAP as a pool compaction: copy
/// every page verbatim in memmove order, never skipping empty pages and never deferring. The
/// zero-page skip and the retry queue are both correct for a staging FILL and both corrupt a
/// MOVE -- see CopyLiveSourcePages. Off restores the old behaviour for an A/B.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dma_move_verbatim, true, "GPU",
                    "Copy overlapping (pool-compaction) image-cache moves verbatim and in "
                    "memmove order, without the empty-page skip or deferral");

/// Dump the next N texture DECODES: the full fetch constant, the first bytes of the guest
/// source, and the decoded image. Unlike the mip dump this fires for every decode, so with
/// the mirror off and committing off (which make a sprite re-decode whenever the guest writes
/// it) firing a weapon will capture the muzzle flash on the spot -- the targeting problem that
/// otherwise makes these sprites impossible to catch.
/// Do not SHOW a texture decoded from an incomplete source.
///
/// Retrying cannot help the common case: the engine streams by proximity, so a distant texture's
/// bytes are genuinely absent until the player approaches, and re-decoding just re-reads the same
/// empty slot. Measured -- speckle clears within a second on nearby surfaces (the retry working)
/// but persists on anything further out until approached.
///
/// The console never shows this. While an image is still streaming the engine draws its own
/// placeholder or a coarser mip; only our renderer decodes the half-filled slot and puts the
/// noise on screen. So substitute rather than display: a flat neutral texel is what a
/// not-yet-resident image is supposed to look like, and it is strictly closer to the reference
/// than garbage is. The real decode replaces it the moment the source completes.
/// Dump the decoded image whose content hash matches this (low 32 bits), once.
///
/// 34 DISTINCT texture addresses were measured converging on ONE identical content hash, and
/// several other hashes were reached by a dozen addresses each. Many slots holding the same
/// image is not corruption -- it is one image being written over many textures, which is exactly
/// what the image cache's default-pixel placeholder would look like from here. Identifying that
/// image by eye settles what the speckle actually IS, rather than inferring it from byte
/// statistics (which has misled this project three times).
/// Dump the image that MANY DISTINCT textures converge on, detected live.
///
/// Targeting a hash copied out of an earlier log does not work: content hashes do not survive a
/// session, because the streaming pool loads different images at different addresses each run.
/// Detect the convergence itself instead -- when N distinct texture addresses decode to the
/// SAME content, that image is being written over many slots, and identifying it settles whether
/// the speckle is the image cache's not-resident placeholder rather than damaged data.
/// Measured: 34 distinct addresses reached one hash in a single run, with several more hashes
/// reached by a dozen addresses each.
/// SLOT REUSE: does a texture's new content belong to a DIFFERENT texture we have already seen?
///
/// This is the last standing explanation. Sources are complete (PARTIALSRC 0.4% over 19k decodes),
/// memory is written, nothing tears, decodes are deterministic -- and yet the picture is wrong,
/// with dumps showing real texel data from other images (smoke sprites rendering as rust and
/// brickwork). If the streaming pool reassigns an address while we still bind it, we would decode
/// whatever now lives there, faithfully, and paint another texture's content onto this surface.
///
/// Testable directly: index every decode's content hash by address, and when a texture's content
/// CHANGES, look the new hash up. A hit at a different address names both parties -- the surface
/// that went wrong and the texture whose bytes it is now showing.
///
/// Hashes reached by many addresses are EXCLUDED: small shared dummy and flat maps legitimately
/// converge (measured: 16x16, 32x32, 64x64 solid-black images at 8+ addresses each), and counting
/// those as reuse is the false positive that makes this test useless.
/// Sample the guest source cheaply enough to run on every texture once per frame.
///
/// Full hashing per bind is exactly what the write-watch exists to avoid: the cache early-out
/// runs ~80k times a frame. This reads 64 widely-spaced 8-byte samples instead, which catches a
/// slot whose occupant was REPLACED (a different image entirely) while costing ~512 bytes of
/// reads per texture per frame. It will not catch a subtle in-place edit, and is not meant to --
/// the write-watch covers those.
/// COVERAGE IS THE WHOLE POINT, and 64 samples was not enough where it mattered.
///
/// The original took 64 eight-byte reads -- 512 bytes total, at fixed stride boundaries. For the
/// texture class that actually speckles (128x128 DXT1 diffuse, 8 KB, *largely zeros*) that is
/// 6.25% coverage at fixed offsets, and if those offsets land in regions that are zero in BOTH the
/// previous and the new occupant the hash is IDENTICAL and no rebuild fires. The probe was blind
/// precisely where the bug lives: a sparse texture replaced by another sparse texture is the
/// easiest possible collision.
///
/// So: hash EVERYTHING for small textures -- 64 KB covers the entire affected class at trivial
/// cost -- and fall back to dense sampling above that, where a full hash of every bound texture
/// every frame would be real bandwidth. Sampling remains a compromise for large textures; if the
/// speckle turns out to live there too, raise the threshold rather than widening the stride.
/// MEASURED NEGATIVE, so it does not get to cost frames. Full-coverage hashing of every texture
/// <=64 KB every frame did NOT raise detections (CONTENTPROBE per decode went 0.236 -> 0.199,
/// i.e. down, across two uncontrolled sessions) and coincided with 60 -> 41 FPS. The sparse-
/// collision theory is unsupported. Default to cheap sampling -- still 8x the original 64, which
/// costs ~4 KB per texture per frame -- and leave the full path available for a deliberate test.
REXCVAR_DEFINE_UINT32(nx1_d3d9_probe_full_bytes, 0, "GPU",
                      "Textures up to this many guest bytes are content-probed in FULL; larger "
                      "ones fall back to sampling. 0 = always sample (default: full hashing was "
                      "measured to add cost without adding detections)");
REXCVAR_DEFINE_UINT32(nx1_d3d9_probe_samples, 512, "GPU",
                      "Eight-byte samples taken when a texture is not probed in full (the "
                      "original 64 was thin enough to collide for sparse textures)");

/// Hash a texture's REFERENCED blocks, at their tiled addresses.
///
/// Two wrong versions preceded this and each failed differently, so the constraint is narrow:
///   - hashing ceil(w/bw)*ceil(h/bh)*bpb bytes LINEARLY from the base reads the right AMOUNT at
///     the wrong PLACES: the data is tiled, so that range is a scattered subset of tiles and
///     misses most of the image (2 KB of a 64x64 DXT1's 8 KB footprint).
///   - hashing block_pitch_h*block_pitch_v*bpb covers the whole footprint but includes the tile
///     and pitch PADDING, which the XDK documents the guest as packing other allocations into.
///     That padding churns constantly, so the probe fired on every texture every frame:
///     CONTENTPROBE went 3,513 -> 1,211,952 rebuilds (~224/frame) and the frame rate collapsed.
///
/// So walk the VISIBLE blocks only, at the same GetTiledOffset2D addresses DetileMip2D uses, and
/// sample a bounded number of them spread across the image.
uint64_t ProbeTiledContent(const uint8_t* base, const rex::graphics::TextureExtent& ex,
                           uint32_t bpb, bool tiled, uint32_t max_samples) {
  if (!base || !ex.block_width || !ex.block_height) {
    return 0;
  }
  namespace tu = rex::graphics::texture_util;
  const uint32_t bpb_log2 = bpb ? uint32_t(__builtin_ctz(bpb)) : 0;  // Log2Exact is declared below
  const uint64_t total = uint64_t(ex.block_width) * ex.block_height;
  const uint32_t step = uint32_t(std::max<uint64_t>(1, total / std::max(1u, max_samples)));
  uint64_t h = 1469598103934665603ull;
  for (uint64_t i = 0; i < total; i += step) {
    const uint32_t bx = uint32_t(i % ex.block_width);
    const uint32_t by = uint32_t(i / ex.block_width);
    const size_t off = tiled ? size_t(tu::GetTiledOffset2D(int32_t(bx), int32_t(by),
                                                           ex.block_pitch_h, bpb_log2))
                             : (size_t(by) * ex.block_pitch_h + bx) * bpb;
    uint64_t v = 0;
    std::memcpy(&v, base + off, std::min<size_t>(sizeof(v), bpb));
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}

uint64_t ProbeGuestContent(const uint8_t* p, uint32_t bytes) {
  if (!p || bytes < 8) {
    return 0;
  }
  uint64_t h = 1469598103934665603ull;
  if (bytes <= REXCVAR_GET(nx1_d3d9_probe_full_bytes)) {
    // Full coverage: every eight-byte word, so ANY difference in the slot's contents changes the
    // hash. No collision class left to reason about.
    uint32_t off = 0;
    for (; off + 8 <= bytes; off += 8) {
      uint64_t v;
      std::memcpy(&v, p + off, sizeof(v));
      h = (h ^ v) * 1099511628211ull;
    }
    if (off < bytes) {  // ragged tail
      uint64_t v = 0;
      std::memcpy(&v, p + off, bytes - off);
      h = (h ^ v) * 1099511628211ull;
    }
    return h ? h : 1;
  }
  const uint32_t samples = std::max(64u, REXCVAR_GET(nx1_d3d9_probe_samples));
  const uint32_t stride = bytes / samples < 8 ? 8 : bytes / samples;
  for (uint32_t off = 0; off + 8 <= bytes; off += stride) {
    uint64_t v;
    std::memcpy(&v, p + off, sizeof(v));
    h = (h ^ v) * 1099511628211ull;
  }
  return h ? h : 1;
}

REXCVAR_DEFINE_BOOL(nx1_d3d9_probe_needs_write, false, "GPU",
                    "Adopt a content-probe change only when a write was actually reported for "
                    "that texture since its last decode -- the reference's rule, which never "
                    "polls. Off by default: it can also block legitimate streaming-in");

REXCVAR_DEFINE_BOOL(nx1_d3d9_content_probe, true, "GPU",
                    "Re-check a cached texture's guest content once per frame and rebuild it if "
                    "the slot's occupant changed. Without this the pool can move a different "
                    "image of the same size and format into a cached address and we keep serving "
                    "the previous texture -- the surface renders another texture's bytes");

REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_slotreuse, false, "GPU",
                    "When a texture's decoded content changes, report whether the new content was "
                    "previously decoded at a DIFFERENT address -- i.e. the pool reused the slot");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_convergence_n, 0, "GPU",
                      "Dump a decoded image once this many DISTINCT texture addresses have "
                      "produced identical content (0 = off, 8 is a good start)");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_dump_hash_lo32, 0, "GPU",
                      "Dump the decoded image whose FNV content hash has these low 32 bits, to "
                      "texdump/hashdump_<lo32>_<addr>.bmp -- identifies what many textures are "
                      "all turning into");

REXCVAR_DEFINE_BOOL(nx1_d3d9_hide_partial, false, "GPU",
                    "Bind a flat placeholder instead of a texture decoded from a source with "
                    "empty pages, until its data actually arrives");

REXCVAR_DEFINE_BOOL(nx1_d3d9_retry_partial, false, "GPU",
                    "Re-decode a texture whose guest source had EMPTY pages, instead of keeping "
                    "that decode forever. The mirror snapshots on first touch, so a texture first "
                    "seen mid-stream is captured half-written and held that way for the session -- "
                    "which is a permanent speckle on whichever surfaces bind it. Uses the same "
                    "exponential backoff as the decoded-to-nothing case, so a pool that never "
                    "arrives does not re-decode every frame");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_partial_src, 0, "GPU",
                      "Debug: report up to N textures decoded from an INCOMPLETE guest source "
                      "(one or more all-zero 4 KB pages inside the declared range), plus a hash of "
                      "the decoded result. A texture that decodes to garbage only while its "
                      "surface speckles is not a decode bug -- it is being decoded from data that "
                      "is not all there yet, and this says so directly");

/// KEEP-BEST DECODE RETENTION.
///
/// Texture pool slots never settle. Tracking four of them while the camera did not move once
/// (run 006) shows every one churning in both directions and none reaching a stable full state:
///   11F66000: 118897 -> 123347 -> 116307 -> 92300 -> 88612 -> 77807 -> 82819 (of 262144)
///   11F26000: 138292 -> 126673 -> 120224 -> 201899 -> 203393
///   118AC000:  88396 ->  94909 -> 150232 -> 231695
/// We re-decode on every write notification, so every dip in those curves reaches the screen. That
/// is why merely turning the camera makes distant buildings speckle: the streamer reprioritises on
/// what is visible, the slot churns, and we sample the bottom of the curve.
///
/// The reference is immune for a reason that is about SAMPLING RATE, not about bytes: it uploads a
/// page when it first becomes valid and keeps that copy until something invalidates it -- one
/// coherent snapshot, where we take dozens.
///
/// So refuse a re-decode whose source is markedly POORER than the source behind the decode we
/// already hold. Note this is NOT capture-first, which memory records as tried and failed
/// ("commit_textures = true ... the FIRST decode is already bad") -- the first sample is already
/// mid-churn. Capture-BEST is a different rule and on the numbers above would hold 11F26000 at
/// 203k rather than showing 120k.
///
/// It is a content heuristic, and this project has been burned by those. What makes it defensible:
/// it compares a texture against ITS OWN previous decode rather than judging whether art looks
/// corrupt, it mutates NO guest memory (unlike the density guard, which refused copies), and the
/// churn data above gives it a predicted effect to be falsified against.
///
/// ESCAPES, so a slot that legitimately becomes sparser is not frozen forever: a layout change
/// always wins (different texture entirely), and after nx1_d3d9_keep_best_max refusals the entry
/// accepts whatever it is given. Without those this would be the commit-freeze bug again, which
/// froze textures as garbage for a whole session.
/// SYNTHETIC TEXTURE TEST -- replace every decoded texture with a known pattern.
///
/// Everything upstream of the host texture is now measured clean: the reference reads byte-
/// identical degraded data (REFUPLOAD), our descriptors match the PM4 register file exactly
/// (FETCHCMP: 0 mismatches over 14.6M valid comparisons with the reference synchronised), the
/// decode is byte-verified against two independent implementations, and the mip chain is
/// exonerated (nomips still speckles). The only untested segment left is
///   staging surface -> UpdateTexture -> D3DPOOL_DEFAULT texture -> sampler -> shader
/// and D3D9 cannot read a DEFAULT-pool BC texture back, so it cannot be inspected directly.
///
/// So push a KNOWN pattern through it instead. A checkerboard whose colour is derived from the
/// texture's base address means each texture is visually distinct, which also shows whether
/// neighbouring surfaces really receive different textures.
///   pattern renders CLEANLY  -> upload and sampling are sound; whatever we hand them is bad, and
///                               the decode verification is measuring a buffer that is not what
///                               reaches the GPU.
///   pattern SPECKLES too     -> the corruption happens AFTER the texture is filled, and no amount
///                               of byte work upstream could ever have fixed it.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_synthetic_tex, false, "GPU",
                    "Replace every decoded texture with a synthetic checkerboard keyed to its "
                    "address, to test the upload/sampling half of the path");

REXCVAR_DEFINE_BOOL(nx1_d3d9_keep_best, false, "GPU",
                    "Refuse a re-decode whose guest source is markedly less populated than the "
                    "source behind the decode already held (keep the better decode)");
REXCVAR_DEFINE_UINT32(nx1_d3d9_keep_best_drop_pct, 20, "GPU",
                      "How much less populated (percent) a re-decode's source must be before "
                      "nx1_d3d9_keep_best refuses it");
REXCVAR_DEFINE_UINT32(nx1_d3d9_keep_best_max, 240, "GPU",
                      "Consecutive re-decodes nx1_d3d9_keep_best may refuse for one entry before "
                      "accepting anyway, so a slot that legitimately got sparser is not frozen");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_texdump, 0, "GPU",
                      "Debug: dump the next N texture decodes (fetch constant + image)");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_texdump_maxdim, 0, "GPU",
                      "Debug: with dbg_texdump, only dump textures whose width AND height are <= "
                      "this (0 = any size). Aims the dump at the engine's small swapped-in LODs");

/// Xenos format filter, +1 so that 0 can mean "any" (format 0 is a real format). Size alone is too
/// coarse: a size-filtered dump returns whichever small texture happens to rebuild first, and
/// reasoning about the wrong one produced a fix for a real but unrelated bug.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_texdump_force, false, "GPU",
                    "Debug: force textures matching the dump filter to re-decode, so a texture "
                    "that is cached and never rewritten can still be dumped");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_texdump_fmt1, 0, "GPU",
                      "Debug: with dbg_texdump, only dump textures whose Xenos format is "
                      "(this value - 1); 0 = any format");

/// Honour the packed-mip tile offset when detiling level 0 (see the long note at the decode).
/// Kept toggleable because it changes the source addressing of every packed texture at once, and
/// an A/B is the only honest way to show it is the fix rather than a plausible story.
/// DEFAULT OFF. Measured with the counters below: armed (207 offsets genuinely applied) and the
/// opaque-glass symptom was unchanged. It matches what the SDK's GetMipLocation does for mip 0, so
/// it is probably more correct than passing (0,0) -- but "probably more correct" with no observed
/// benefit is exactly the kind of change that has cost this renderer black screens before. Left in,
/// off, with its instrumentation, so the next investigation starts from a measured negative rather
/// than re-deriving the theory.
REXCVAR_DEFINE_BOOL(nx1_d3d9_packed_mip_offset, false, "GPU",
                    "Apply the Xenos packed-mip sub-tile offset when decoding level 0 of a "
                    "texture whose mip tail is packed (textures <= 16 texels)");

/// Read texture texels from the CPU mirror rather than live guest memory.
///
/// The mirror captures a page on first touch and then HOLDS it, ignoring later guest writes,
/// so the streaming pool recycling a slot cannot re-poison a good decode. The cost is
/// staleness: any texture whose data the guest writes AFTER we snapshot stays frozen at
/// whatever the memory held at capture time -- zeros, or the previous occupant's texels.
///
/// The reference backend reads live guest memory at draw time and renders all of this content
/// correctly, which means the data IS reachable and the snapshot is what loses it. Turn this
/// off to read live like the reference does.
/// The decode reads the snapshot mirror; every census in this investigation read LIVE guest
/// memory. A divergence between the two is the one thing none of them could see, and it is the
/// mechanical explanation for why the content probe forced 2,457 rebuilds, drove SLOTREUSE to
/// zero, and left the speckle completely unmoved: the rebuild re-read the same stale snapshot.
/// WHOSE BYTES ARE THESE? An automatic detector for the artifact, built because the bug is NOT
/// reproducible per surface -- the wall that speckles this run will not speckle next run, so
/// hunting a nominated surface with the picker cannot work.
///
/// The corrupt images show rectangular blocks of OTHER textures' content. That is a checkable
/// claim: index every decoded source page by a hash of its contents, and when a page's content was
/// FIRST seen under a different texture's address, this texture is holding that texture's bytes.
/// Report any decode where enough pages trace elsewhere. Whatever surface happens to be corrupt
/// that run gets flagged, with the addresses its foreign blocks came from.
///
/// Legitimately shared pages must be excluded or they drown it: all-zero and uniform pages recur
/// across many textures for real reasons (transparent regions, flat maps), which is the same trap
/// the convergence detector hit -- there, tiny solid-black images reached 8+ addresses each. So a
/// page seen at more than kPageOriginMaxAddrs addresses is treated as legitimately shared.
/// HARDWARE-FIDELITY CENSUS. Every entry here is a documented Xbox 360 field that we either do not
/// apply or approximate, where the XDK says what the hardware does but nothing says whether NX1
/// EXERCISES it. Fixing them blind is how this project has repeatedly traded one wrong case for
/// another, so each is measured before it is acted on.
///
/// Deliberately accumulates OR-masks and min/max rather than logging per occurrence: the question
/// is always "does this value ever appear", which a mask answers in a couple of instructions at a
/// call site that already has the decoded struct in hand.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_hwcensus, false, "GPU",
                    "Census the documented Xenos fields we do not currently honour (texel exponent "
                    "bias, stacked textures, per-component texture signs, clamp modes and policy, "
                    "border colour, mip filter). Decides which fidelity gaps are real for NX1");
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_pageorigin, false, "GPU",
                    "Flag decodes whose source pages were first seen under a DIFFERENT texture's "
                    "address -- automatic detection of a texture holding foreign blocks");
/// Write a PROVENANCE MASK for each flagged texture: one pixel per block, green where the block's
/// bytes came from this texture's own pages and red where they came from another texture's. The
/// question this answers cannot be answered from addresses: does the SHAPE of the foreign region
/// match the shape of the corruption on screen? If a wall is corrupt across its whole face while
/// the mask shows one red corner, the 8.8% is not the artifact and the fault is downstream. If the
/// shapes agree, it is.
/// Paint every block whose source page the provenance detector flagged as foreign. The screen
/// then answers whether those pages are the corruption, which shape-matching cannot.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_paint_foreign, false, "GPU",
                    "Replace blocks read from another texture's page with a magenta marker. If "
                    "the corrupt areas turn magenta, the provenance finding is the artifact");
/// Dump the DECODED IMAGE of textures the provenance detector flagged, paired with the mask of
/// which blocks it flagged. The question every other instrument dances around -- is the decoded
/// picture coherent, or a patchwork -- is answered by looking at it, and has never been asked for
/// a single confirmed surface.
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_dump_mixed, 0, "GPU",
                      "Dump the decoded image (MIXED_<addr>.bmp) plus its provenance mask "
                      "(prov_<addr>.bmp) for this many PAGEORIGIN-flagged textures. A coherent "
                      "image means the flag is benign page sharing, not corruption");
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_pageorigin_dump, 0, "GPU",
                      "Write texdump/prov_<addr>_f<frame>.bmp for this many flagged textures: one "
                      "pixel per block, red = block read from another texture's page");
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_pageorigin_pct, 10, "GPU",
                      "Report a decode when at least this percent of its pages trace to another "
                      "texture's address");
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_mirrorstale, false, "GPU",
                    "Debug: at every decode, compare the snapshot the decode is about to use "
                    "against live guest memory and report how often it is stale");
/// ON matches the reference. The old help text claimed the opposite -- that reading LIVE guest
/// memory is "as the reference backend does" -- and that is simply false, which is probably how
/// the shipped config came to have it off.
///
/// The reference never reads guest RAM at bind time. `SharedMemory::RequestRanges` consults a
/// per-4 KB VALID bitmask, uploads only the invalid pages (`UploadRanges` memcpy's from
/// TranslatePhysical into a 512 MB GPU buffer), and the texture load shader then reads THAT
/// BUFFER. A page is captured once and HELD until something explicitly invalidates it -- a CPU
/// write via page protection, or a GPU write via RangeWrittenByGpu.
///
/// That difference matters exactly here: if a page holds the right bytes when the reference first
/// uploads it and the streaming pool recycles that memory afterwards, the reference still has the
/// good copy while a live read gets the recycled bytes.
///
/// An earlier A/B "exonerated" this -- but it was judged by the MIXED provenance rate, which has
/// since been refuted against the dumped images (it flags perfectly clean textures). It was never
/// judged by looking at the screen.
REXCVAR_DEFINE_BOOL(nx1_d3d9_texture_mirror, true, "GPU",
                    "Decode textures from the CPU mirror snapshot, capturing each page once and "
                    "holding it until invalidated -- which is what the reference backend does. "
                    "Off = re-read live guest memory at every decode");

/// THE GPU-WRITE WATCH. Host page protection sees CPU stores only, and this title streams its
/// textures with a GPU memexport blit, so our texture cache never hears about the writes that
/// deliver them: 3036 of 3047 DECODECHANGE lines report "page writes 0 -> 0" -- content changed
/// completely while neither a CPU write nor any of our 379,158 mirrored DMA copies touched the
/// page. Subscribing to the reference's SharedMemory global watch is how the correct renderer
/// learns about them (SharedMemory::RangeWrittenByGpu is the announced chokepoint).
///
/// Off = the previous behaviour, for a clean A/B against the speckle.
/// CLAMP A DECODE TO THE MEMORY THE POOL ACTUALLY ALLOCATED FOR IT.
///
/// The guest's fetch constant is not always consistent with its own allocator. Measured over 193
/// captured textures, 22 (11%, a lower bound -- an overlap is only visible when the neighbour was
/// captured too) declare more bytes than the distance to the next live texture's base, and three
/// were proved byte-for-byte: the tail of our read IS the neighbouring texture, 100.00% identical
/// to that texture's own capture. The implied size is always a clean one (256x256 declared where
/// 256x128 fits), i.e. the pool allocated for a SMALLER image than the descriptor describes,
/// because the slot was reassigned and a stale descriptor is still bound.
///
/// Our fetch parse is NOT at fault -- every geometry field re-derives exactly from the raw dwords
/// across all 193 captures. We read what the guest asked for; the guest asked for too much.
///
/// So decode only as far as the next live base and zero the rest, rather than rendering another
/// asset's bytes as texels. Only bases seen in the last few frames count: a stale entry sitting
/// inside our range would clamp a perfectly good texture.
/// Give every distinct texture DESCRIPTOR its own cache entry, as the reference does.
///
/// Off = the old behaviour, one entry per (base address, sampler), so several descriptors sharing
/// an address evict each other and each re-decode pairs one descriptor's geometry with another
/// texture's bytes. See the key computation for the measurements behind this.
///
/// Costs cache entries -- an address bound under three descriptors now holds three decodes -- but
/// eviction already reclaims anything unbound for ~600 frames.
REXCVAR_DEFINE_BOOL(nx1_d3d9_key_by_layout, true, "GPU",
                    "Key the texture cache on the full descriptor (address + sampler + layout) "
                    "instead of address + sampler, so two textures sharing an address stop "
                    "overwriting each other's decode");

/// DEFAULT OFF -- MEASURED HARMFUL. It fires at scale (445 decodes, 22.7 MB withheld in one run)
/// and the speckle is completely unchanged, so the over-read is real but not the artifact. Worse,
/// it truncates legitimate textures to black: ground surfaces went black in a run with it on,
/// because the "next live base" is sometimes a stale descriptor rather than a real allocation
/// boundary. Kept as a cvar for measurement only; do not ship it on.
REXCVAR_DEFINE_BOOL(nx1_d3d9_clamp_alloc, false, "GPU",
                    "Stop a texture decode at the next live texture's base address instead of "
                    "reading into it. Off = previous behaviour, for an A/B against the speckle");

REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_texset, false, "GPU",
                    "Log each distinct texture this renderer decodes once (D9TEX), in the same "
                    "form as the reference's REFTEX, so the two SETS can be diffed without any "
                    "per-draw synchronisation");

/// Operator-aimed capture: dump every distinct block-compressed texture bound over the next N
/// frames (full guest source + our decode + parameters), capped at 64 textures.
///
/// Deliberately has NO corruption heuristic. Its predecessor scored decoded images and captured
/// eight "22-99% corrupt" textures that were all perfect -- decal atlases on transparency read as
/// corrupt to any content-based test. The operator can see the speckle; aiming at it by hand is
/// the only selector in this investigation that has never returned a false positive.
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_dump_frame, 0, "GPU",
                      "Dump full guest bytes + decode + params for every distinct BC texture bound "
                      "over the next N frames, while the game scene is rendering. Point at the "
                      "artifact, then arm this");
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_dump_frame_max, 256, "GPU",
                      "Cap on distinct textures a frame capture will dump. A gameplay frame binds "
                      "~1500, so this bounds the disk cost; raise it if the artifact is missed");

/// DMA COVERAGE CENSUS over EVERY decode, not just provenance-flagged ones.
///
/// The existing coverage cross-reference only runs on pages the provenance detector flags, and
/// that detector is now refuted: measured against the dumped images it flags perfectly clean
/// textures at 12.5-75% and misses most genuinely corrupt blocks, so anything selected by it
/// inherits a bias of unknown sign. This asks the same question of everything we decode.
///
/// What it tests, specifically: `ImageCache_MoveImage`/`DmaCopyDelayed` take the copy size from
/// the LOW half of the packed 8-byte ImageAllocInfo, a choice the hook's own comment records as
/// verified against EXACTLY ONE destination (dst ACAF8000 moves +3000, lo=3000 where hi=5000). If
/// `lo` is the base-level size where the image needs more, we mirror a PREFIX and leave the
/// remainder holding the previous occupant -- which is the shape the decoded images actually show
/// (15196000: first 64 KB = one full tile row clean, then tile-granular garbage).
///
/// UNDERCOPIED > 0 with `alt would fit` tracking it = the size half is wrong, and it is a
/// one-line change. UNDERCOPIED ~0 = the copies do span the textures and the prefix pattern comes
/// from somewhere else; believe that only if COVERED is large (if NOCOPY dominates, the census
/// is simply not seeing the delivery path and decides nothing).
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_dmacover, false, "GPU",
                    "Per-decode DMA coverage census: for every texture, did a mirrored copy span "
                    "the bytes we decode, and would the other ImageAllocInfo half have fit");

/// Re-arm the guest write-watch on every decode instead of trusting our own `watch_armed_` bit.
///
/// We share the page protection with the reference's SharedMemory, which arms and unprotects the
/// same pages on its own schedule. A cached "armed" bit that disagrees with the OS makes us
/// permanently blind to that page, because the re-arm is gated on the very signal we stop
/// receiving. Diagnostic first: watch whether the DECODECHANGE no-write-delta fraction (64.5%
/// measured) collapses. Costs one call per 4 KB page per decode.
REXCVAR_DEFINE_BOOL(nx1_d3d9_rearm_watch, false, "GPU",
                    "Re-arm the page write-watch on every decode rather than trusting our cached "
                    "armed bit, which the reference's SharedMemory can invalidate behind us");

REXCVAR_DEFINE_BOOL(nx1_d3d9_gpu_write_watch, true, "GPU",
                    "Dirty textures when the reference reports the GPU wrote their guest memory. "
                    "Host page protection cannot see those writes, so without this a texture "
                    "streamed in by memexport keeps whatever we decoded first");

/// Ceiling on a single GPU-write range, in MB.
///
/// This exists because the invalidation is NOT always precise: when a memexporting draw's
/// destinations cannot be determined, the reference falls back to
/// `RangeWrittenByGpu(0, kBufferSize)` (d3d12/command_processor.cpp:4614) -- a 512 MB blast that
/// means "something, somewhere". Honouring it would dirty every texture in the cache on every
/// such draw and re-decode the world. Ranges wider than this are counted as blasts and dropped,
/// so the watch acts only on the precise ranges. Raise it to find out what the blasts were
/// hiding; 0 disables the ceiling entirely (expect a rebuild storm).
REXCVAR_DEFINE_UINT32(nx1_d3d9_gpu_write_watch_max_mb, 16, "GPU",
                      "Drop GPU-write invalidations wider than this many MB (0 = no limit). The "
                      "reference blasts the whole 512 MB buffer when a memexport draw's "
                      "destination is unknown, and acting on that rebuilds every texture");

/// Commit-and-freeze. A texture drawn cleanly for kCommitFrames stops honouring guest writes,
/// so the streaming pool recycling its slot cannot re-poison a good decode. The risk is the
/// mirror image of the benefit: if the decode was ALREADY wrong when it committed -- decoded
/// while only part of the texture had streamed in, so some tiles still hold the previous
/// occupant -- freezing makes that permanent. Turn this off to find out which side of that
/// trade an artifact is on: with committing disabled the write-watch keeps re-decoding, so a
/// texture that was merely early will correct itself once its data lands.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_redecode_all, false, "GPU",
                    "Debug: one-shot -- dirty EVERY cached texture so it re-decodes from its "
                    "guest memory as it is right now, then clears itself. Answers the one "
                    "question a tracked texture cannot when the guest never writes to it: is the "
                    "source correct and our cached decode merely stale, or is the source itself "
                    "wrong? If the speckle clears, we missed the write that filled it");

REXCVAR_DEFINE_BOOL(nx1_d3d9_writes_always_invalidate, true, "GPU",
                    "Honour a guest write even to a committed texture -- the reference model. "
                    "Off restores the old freeze, which cannot distinguish protecting a good "
                    "decode from preserving a bad one");

REXCVAR_DEFINE_BOOL(nx1_d3d9_commit_textures, true, "GPU",
                    "Freeze a texture's decode after it has been drawn cleanly for 32 frames");

REXCVAR_DEFINE_BOOL(nx1_d3d9_evict_textures, true, "GPU",
                    "Release cached textures that have not been bound for ~600 frames. Off "
                    "keeps every decode for the session: re-decoding is a gamble because guest "
                    "texture memory changes through a path the write-watch cannot see, so an "
                    "evicted GOOD decode may come back as the pool's later garbage");

/// Checkerboard hunt: log the full decode geometry of every texture >= 256x256, once per address.
/// The artifact varies across launches, so the evidence is a diff of a good run against a bad one
/// -- not anything visible within either.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_decode_log, false, "GPU",
                    "Log decode geometry (address alignment, pitch, tiling, swizzle) for every "
                    "texture >= 256x256, once per base address");

/// The 32x32-table detile fast path. On by default because it is verified exhaustively against
/// GetTiledOffset2D, but cvar-gated because it measured no performance benefit at all -- so if
/// anything tile-shaped ever looks wrong, this is the cheapest thing in the renderer to rule
/// out, and nothing is lost by leaving it off.
REXCVAR_DEFINE_BOOL(nx1_d3d9_fast_detile, true, "GPU",
                    "Use the table-driven tiled-texture address path (off = per-block reference)");

/// Diagnostic: dump the mip pipeline's intermediate images for the next N block-compressed
/// textures built. Writes texdump/mip_<addr>_L<level>.bmp straight from the Rgba8 buffers, so
/// what lands on disk is exactly what DecodeBcImage produced and what BoxFilterHalf produced
/// -- no inference. Level 0 garbled means our BC decoder; level 0 clean but lower levels
/// garbled means the filter.
REXCVAR_DEFINE_BOOL(nx1_d3d9_resolve_size_check, true, "GPU",
                    "Require a resolve-map hit to match the bind's dimensions before serving "
                    "the render target. Off = legacy address-only matching, which serves stale "
                    "targets forever for addresses the streaming pool has since recycled");

/// The premature-decode test. Every other explanation for the speckle has now been eliminated by
/// measurement, and the one symptom none of them accounted for is that it HEALS -- on approach,
/// and eventually on its own -- while the only configuration that ever rendered a clean frame was
/// the one that slowed the entire pipeline down. Both point at the decode happening before the
/// guest has finished landing the texture.
///
/// The write-watch cannot detect this: DECODECHANGE reports thousands of textures whose content
/// changed between decodes with "page writes 0 -> 0", so this memory moves through paths page
/// protection never sees. The only way to find out is to re-read and compare.
///
/// Set to a frame count (30 is a reasonable start) to force each texture to re-decode on a
/// geometric ladder after its first decode. If the speckle clears, the decode was early.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_tornpages, false, "GPU",
                    "Copy every mirror page TWICE and compare, to detect guest writes landing "
                    "while we snapshot. A non-zero torn count means our decodes read half-written "
                    "memory, which is what a decode that flickers between correct and corrupt "
                    "from a fixed address requires");

REXCVAR_DEFINE_UINT32(nx1_d3d9_redecode_delay, 0, "GPU",
                      "Frames after a decode to force a re-decode (0 = off). Four rechecks on a "
                      "widening ladder; DECODECHANGE reports which found different bytes");

/// Latch the tracker onto the FIRST small DXT1 diffuse that decodes from a holey source.
///
/// Picking by eye cannot catch this: by the time a surface looks wrong its first decode is long
/// past, and the streaming pool has usually moved the address since. The corrupt class is
/// specific -- 128x128 fmt=18 diffuse maps whose guest memory is largely zeros (dumped as black
/// blocks; measured at nonzero=1896/8192) -- so the decode itself can recognise one and start
/// tracking at the exact moment it goes bad, capturing the DMACOPY / WRITE / POLL history from
/// first bind rather than from whenever a human noticed.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_autotrack_partial, false, "GPU",
                    "Point dbg_track_addr at the first texture that decodes with empty source "
                    "pages, log why it qualified, and flag it when its guest mip chain looks "
                    "delivered while its base does not. Releases when it decodes cleanly");

/// Size bound for autotrack candidates. This was a hardcoded 128 alongside a fmt==18 test, which
/// excluded every 256x256 specimen the speckle investigation ever chased -- so autotrack could
/// never have fired for one, and its silence read as "nothing to track". 512 covers the world
/// textures; raise it to catch a larger one, lower it to narrow a noisy hunt.
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_autotrack_max_dim, 512, "GPU",
                      "Largest texture dimension autotrack will latch onto (see "
                      "nx1_d3d9_dbg_autotrack_partial)");

/// Per-page zero percentage at or above which autotrack calls a page STARVED. The decode's own
/// `partial_pages` cannot be used for this: it counts a page only when it is ENTIRELY zero AND
/// never written, and the specimens are 76-89% zero, so they never qualified and two hunts found
/// nothing. 75 sits between the observed starved bases (76-89%) and delivered art (0-3%).
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_autotrack_zero_pct, 75, "GPU",
                      "Percent of a page that must be zero for autotrack to call it starved");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_framediff, 0, "GPU",
                      "Debug: dump the next N resolves as a PAIR -- our rendered target and "
                      "the guest-RAM contents at the same address -- for backend comparison");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_classify, 0, "GPU",
                      "Debug: log the next N textures whose SOURCE bytes classify as "
                      "placeholder-like (short repeat period) or garbage-like (saturated byte "
                      "histogram), with the evidence for the call");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_mipdump, 0, "GPU",
                      "Debug: dump decoded mip levels for the next N BC textures built");

/// Diagnostic: paint textures white by how their mip chain was provided.
///   1 = CPU-built (block compressed)   2 = driver auto-generated   3 = no chain at all
/// The remaining speckled surfaces will be one of these three, and each implicates something
/// different: our encoder, the driver's minification of our level 0, or plain aliasing.
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_mipsrc, 0, "GPU",
                      "Debug: paint textures white by mip source (1=cpu-built, 2=driver, "
                      "3=none)");

/// Diagnostic: drop ONLY the CPU-built block-compressed mip chain, leaving the driver's
/// auto-generated chains alone. nx1_d3d9_mips 0 disables both at once, which cannot tell our
/// BC encoder apart from the driver's minification -- and with mips off entirely a minified
/// surface aliases, which reads as speckle too. This isolates the half we wrote.
// Category switches for the renderer's recurring logging -- see d3d9_log.h for why. Registered
// under "Logging" so the F4 overlay gives them their own sidebar entry (it builds the sidebar from
// the set of cvar categories). Defaults true: turning logging off is opt-in, so nothing silently
// stops reporting.
REXCVAR_DEFINE_BOOL(nx1_d3d9_log_stats, true, "Logging",
                    "Periodic renderer statistics (PROF, cache/MEM, mipgen, resolve rollups)");
REXCVAR_DEFINE_BOOL(nx1_d3d9_log_texture, true, "Logging",
                    "Per-texture events (decode changes, new textures, tracked addresses)");
REXCVAR_DEFINE_BOOL(nx1_d3d9_log_dma, true, "Logging",
                    "Image-cache DMA and command-buffer traffic (DMACOPY, DMARETRY, CMDBUF)");
REXCVAR_DEFINE_BOOL(nx1_d3d9_log_misc, true, "Logging",
                    "Other recurring renderer logging not covered by the categories above");

REXCVAR_DEFINE_BOOL(nx1_d3d9_bc_mips, true, "GPU",
                    "Build mip chains for block-compressed textures on the CPU (diagnostic: "
                    "set false to leave BC textures unmipped while keeping driver auto-mips)");

/// Fit BC1 endpoints along the block's PRINCIPAL AXIS instead of its bounding box.
///
/// MEASURED, on the first mip chains ever dumped from a user-picked speckling surface: the
/// bounding-box fit this replaces injects mean|diff| 29.1 (13D6E000) and 12.0 (1361E000) per
/// channel into EVERY generated level, and loses 21% of the image's contrast in one re-encode.
/// A principal-axis fit on the identical input scores 19.0 and 9.8 -- 34.8% and 18.1% less error
/// -- and restores most of that contrast. Level 0 is never re-encoded, so all of this lands
/// exclusively on the distant LODs, which is the shape of the artifact.
///
/// Set false to A/B against the old fit. NOTE it does not rebuild textures already resident, so
/// a surface keeps its old mips until something dirties it.
REXCVAR_DEFINE_BOOL(nx1_d3d9_bc_pca, true, "GPU",
                    "Fit BC1 mip endpoints along the block's principal axis rather than its "
                    "bounding box (false = the old min/max fit, for A/B)");

/// Decode the guest's own mip chain from mip_address instead of generating one from level 0.
///
/// DISPROVEN TWICE, kept only as a diagnostic. Applied to every declared chain it painted the
/// world with garbage; restricted to textures declaring a SAMPLED chain (mip_filter != kBaseMap)
/// it still did. The dumps settle it: gmip_*_L0 comes out a clean, correct image while every
/// guest level below it is pure noise -- not a scrambled L0 (which would mean our addressing is
/// wrong) and not a coherent foreign image (a recycled pool slot), just noise. And those same
/// fetch constants ask for the levels with mip_filter=0/1, mip_max=6..7, lod_bias=0 and
/// grad_exp=0/0 -- no LOD adjustment of any kind, so on hardware they WOULD be sampled.
///
/// The conclusion is the one the original comment in this file already reached: this build
/// never fills the mip pool. The game's streaming system populates mip_address on console; the
/// recompilation does not. That is a residency gap upstream of the renderer -- nothing the
/// texture path can fix -- so we generate our own chain from level 0, which is data we know is
/// good (nx1_d3d9_dbg_nomips renders the scene cleanly, which is level 0 alone).
REXCVAR_DEFINE_BOOL(nx1_d3d9_guest_mips, false, "GPU",
                    "Decode the guest's mip chain from mip_address for textures whose mip "
                    "filter actually samples it (not kBaseMap). OFF: measured, this build "
                    "never fills the mip pool -- the dumps are noise while level 0 is clean, "
                    "and the fetch constants ask for those levels with no LOD bias at all");

// Defined in d3d9_renderer.cpp beside the sampler-state translation that honours it.
REXCVAR_DECLARE(bool, nx1_d3d9_basemap);

// Reference-side upload capture, defined in the SDK (d3d12/shared_memory.cpp). Declared here so a
// texture dump can AIM it at the very texture being dumped -- see the mip dump site.
REXCVAR_DECLARE(uint32_t, nx1_refupload_addr);
REXCVAR_DECLARE(uint32_t, nx1_refupload_bytes);

/// Point the reference-side upload capture at whatever texture we just dumped.
///
/// Without this, pairing the two halves is a chicken-and-egg problem: the reference watch is aimed
/// by ADDRESS, but pool addresses are reassigned every launch, so an address read out of one run's
/// log captures unrelated memory in the next. A whole capture was wasted on that -- and on my own
/// note claiming the picker's TRACK ALL button already did this, which it never did (the string
/// `refupload` did not appear anywhere in nx1_mp/src before this).
///
/// Aiming from inside the dump makes the two halves refer to the same texture BY CONSTRUCTION,
/// in the same run, with no address copied by hand.
/// Auto-capture a decode that looks corrupt, because the artifact is now a ONE-FRAME flicker that
/// cannot be dumped by hand. Percentage of corrupt-looking 4x4 blocks at or above which to dump;
/// 0 = off. Clean world textures score 2-5%, a confirmed-bad specimen scored 45.7%, so ~25 is a
/// reasonable first threshold. See the trigger site for why a content score is safe HERE
/// specifically when this file records it being unsafe elsewhere.
/// Capture the ONE-FRAME FLICKER by mechanism instead of by content score.
///
/// A decode hash going X -> Y -> X is a surface that was momentarily wrong and fixed itself. No
/// content classifier can identify that: black-heavy UI and high-frequency detail art both trip a
/// corruption threshold (a clean wood-grain wall scored 58% "noise"), so such a threshold samples
/// high-variance textures rather than broken ones. A hash reverting to its own previous value
/// cannot be produced by the texture merely being detailed.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_dump_flicker, false, "GPU",
                    "Dump the source bytes when a texture's decode returns to its previous hash "
                    "after one differing decode -- the one-frame flicker, caught by mechanism");
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_dump_flicker_max, 24, "GPU",
                      "Stop after this many flicker captures");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_autodump_corrupt, 0, "GPU",
                      "Dump any level-0 decode scoring at least this % corrupt blocks (0 = off). "
                      "Catches a one-frame flicker that cannot be captured by hand");
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_autodump_max, 12, "GPU",
                      "Stop after this many auto-dumps, so one bad frame cannot fill the disk");

REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_dump_aims_refupload, true, "GPU",
                    "When a texture dump fires, point nx1_refupload_addr/bytes at that same "
                    "texture so the reference's uploaded bytes can be paired with our decode");

REXCVAR_DEFINE_BOOL(nx1_d3d9_mips, true, "GPU",
                    "Give textures a mip chain, filtered down from level 0 by the driver. Off "
                    "leaves every minified surface aliasing (the coloured speckle).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);


#ifdef _WIN32

namespace nx1::d3d9 {

//--- Multi-texture tracking (see guest_d3d.h) ---------------------------------
namespace {
/// The set, plus its live count. Written from the main thread (overlay button), read from the
/// render thread, the async decode worker and the guest DMA thread -- hence atomics rather than a
/// lock: every reader only needs a consistent view of one entry, and a torn read at worst logs or
/// misses one line of debug output. A mutex here would sit on the DMA hook's hot path.
std::array<std::atomic<uint32_t>, kTrackSetMax> g_track_addr{};
std::array<std::atomic<uint32_t>, kTrackSetMax> g_track_span{};
std::atomic<uint32_t> g_track_count{0};
}  // namespace

void SetTrackSet(const TrackedTex* list, uint32_t count) {
  count = std::min(count, kTrackSetMax);
  for (uint32_t i = 0; i < kTrackSetMax; ++i) {
    g_track_addr[i].store(i < count ? list[i].addr : 0u, std::memory_order_relaxed);
    g_track_span[i].store(i < count ? list[i].span : 0u, std::memory_order_relaxed);
  }
  g_track_count.store(count, std::memory_order_release);
}

uint32_t TrackedMatch(uint32_t addr) {
  if (!addr) {
    return 0;
  }
  // PHYSICAL comparison. Texture base addresses arrive already physical, but resolve destinations
  // are 0xA/0xB-window EAs, and the resolve call sites used to normalise BOTH sides themselves.
  // Masking here keeps that behaviour for every caller instead of leaving it to each one -- the
  // "compare physically or the match can never happen" trap that MirrorDmaCopy documents.
  const uint32_t want = addr & 0x1FFFFFFF;
  if ((REXCVAR_GET(nx1_d3d9_dbg_track_addr) & 0x1FFFFFFF) == want) {
    return addr;
  }
  const uint32_t n = g_track_count.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t a = g_track_addr[i].load(std::memory_order_relaxed);
    if (a && (a & 0x1FFFFFFF) == want) {
      return addr;
    }
  }
  return 0;
}

uint32_t TrackedList(TrackedTex* out, uint32_t max) {
  uint32_t written = 0;
  // The primary comes first and is never duplicated by a set entry.
  if (const uint32_t primary = REXCVAR_GET(nx1_d3d9_dbg_track_addr); primary && written < max) {
    out[written++] = {primary, REXCVAR_GET(nx1_d3d9_dbg_track_bytes)};
  }
  const uint32_t n = g_track_count.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n && written < max; ++i) {
    const uint32_t a = g_track_addr[i].load(std::memory_order_relaxed);
    if (!a) {
      continue;
    }
    bool dup = false;
    for (uint32_t k = 0; k < written; ++k) dup = dup || out[k].addr == a;
    if (!dup) {
      out[written++] = {a, g_track_span[i].load(std::memory_order_relaxed)};
    }
  }
  return written;
}

namespace {

// xenos::VertexFormat
enum : uint32_t {
  kFmt_8_8_8_8 = 6,
  kFmt_2_10_10_10 = 7,
  kFmt_10_11_11 = 16,
  kFmt_11_11_10 = 17,
  kFmt_16_16 = 25,
  kFmt_16_16_16_16 = 26,
  kFmt_16_16_FLOAT = 31,
  kFmt_16_16_16_16_FLOAT = 32,
  kFmt_32 = 33,
  kFmt_32_32 = 34,
  kFmt_32_32_32_32 = 35,
  kFmt_32_FLOAT = 36,
  kFmt_32_32_FLOAT = 37,
  kFmt_32_32_32_32_FLOAT = 38,
  kFmt_32_32_32_FLOAT = 57,
};

struct HostFormat {
  D3DDECLTYPE type;
  uint8_t src_size;
  uint8_t dst_size;
  bool expand;
};

/// Pick the host input-assembler format for one guest element, or fail.
///
/// The "no expand" cases all share one property: the guest bytes become the host
/// bytes under the very dword byteswap the GPU's 8in32 endian mode would apply.
/// Xenos numbers a packed element's components from the *low* bits of the
/// byteswapped dword, which is precisely where D3D9 reads component 0 from.
/// `swizzle` is the D3DDECLTYPE's GPUSWIZZLE field (bits 10..21), or kGpuSwizzleAbgr when the
/// caller has no declaration to read it from. It is load-bearing for exactly one pair of types:
/// the XDK defines D3DDECLTYPE_D3DCOLOR and D3DDECLTYPE_UBYTE4N identically -- 8_8_8_8, 8IN32,
/// UNSIGNED, FRACTION -- and distinguishes them ONLY by ARGB vs ABGR here. Ignoring it mapped
/// every D3DCOLOR to UBYTE4N, which hands the shader (B,G,R,A) where hardware hands it (R,G,B,A).
/// NX1's own 8_8_8_8 declaration type (0x182886) carries 0x60A = ARGB, so this was live.
bool PickHostFormat(uint32_t format, bool is_signed, bool is_normalized, uint32_t swizzle,
                    HostFormat* out) {
  switch (format) {
    case kFmt_32_FLOAT:
      *out = {D3DDECLTYPE_FLOAT1, 4, 4, false};
      return true;
    case kFmt_32_32_FLOAT:
      *out = {D3DDECLTYPE_FLOAT2, 8, 8, false};
      return true;
    case kFmt_32_32_32_FLOAT:
      *out = {D3DDECLTYPE_FLOAT3, 12, 12, false};
      return true;
    case kFmt_32_32_32_32_FLOAT:
      *out = {D3DDECLTYPE_FLOAT4, 16, 16, false};
      return true;
    case kFmt_16_16_FLOAT:
      *out = {D3DDECLTYPE_FLOAT16_2, 4, 4, false};
      return true;
    case kFmt_16_16_16_16_FLOAT:
      *out = {D3DDECLTYPE_FLOAT16_4, 8, 8, false};
      return true;

    case kFmt_8_8_8_8:
      if (is_signed) {
        *out = {D3DDECLTYPE_FLOAT4, 4, 16, true};
      } else if (is_normalized && swizzle == nx1::d3d9::kGpuSwizzleArgb) {
        // D3DCOLOR. Host D3D9's D3DDECLTYPE_D3DCOLOR has identical semantics, and after our dword
        // byteswap the bytes are already in the B,G,R,A order it expects -- an exact mapping, not
        // an approximation.
        *out = {D3DDECLTYPE_D3DCOLOR, 4, 4, false};
      } else {
        *out = {is_normalized ? D3DDECLTYPE_UBYTE4N : D3DDECLTYPE_UBYTE4, 4, 4, false};
      }
      return true;

    case kFmt_16_16:
      if (is_signed) {
        *out = {is_normalized ? D3DDECLTYPE_SHORT2N : D3DDECLTYPE_SHORT2, 4, 4, false};
      } else if (is_normalized) {
        *out = {D3DDECLTYPE_USHORT2N, 4, 4, false};
      } else {
        *out = {D3DDECLTYPE_FLOAT2, 4, 8, true};
      }
      return true;

    case kFmt_16_16_16_16:
      if (is_signed) {
        *out = {is_normalized ? D3DDECLTYPE_SHORT4N : D3DDECLTYPE_SHORT4, 8, 8, false};
      } else if (is_normalized) {
        *out = {D3DDECLTYPE_USHORT4N, 8, 8, false};
      } else {
        *out = {D3DDECLTYPE_FLOAT4, 8, 16, true};
      }
      return true;

    // D3D9's DEC3N/UDEC3 are optional caps that most modern drivers report as
    // unsupported, and 10_11_11 has no equivalent at all. Decode to floats.
    case kFmt_2_10_10_10:
      *out = {D3DDECLTYPE_FLOAT4, 4, 16, true};
      return true;
    case kFmt_10_11_11:
    case kFmt_11_11_10:
      *out = {D3DDECLTYPE_FLOAT3, 4, 12, true};
      return true;

    // Integer vertex inputs: vs_3_0 has none.
    case kFmt_32:
      *out = {D3DDECLTYPE_FLOAT1, 4, 4, true};
      return true;
    case kFmt_32_32:
      *out = {D3DDECLTYPE_FLOAT2, 8, 8, true};
      return true;
    case kFmt_32_32_32_32:
      *out = {D3DDECLTYPE_FLOAT4, 16, 16, true};
      return true;

    default:
      return false;
  }
}

inline uint32_t Swap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint16_t Swap16(uint16_t v) { return __builtin_bswap16(v); }

struct RawVertexUsage {
  uint8_t usage;        ///< D3DDECLUSAGE
  uint8_t usage_index;
};

/// Assign a host vertex semantic to one vfetch attribute, in vfetch-listing order.
///
/// This MUST mirror GetNx1RawVertexUsage in rexglue's shader.cpp exactly: that is
/// the function XenosRecomp used to name each shader's input registers, so the
/// host declaration only binds if we reproduce the same usage/index assignment.
RawVertexUsage Nx1RawVertexUsage(size_t ordinal, uint32_t format, uint32_t& packed_count,
                                 uint32_t& texcoord_count, uint32_t& color_count) {
  if (ordinal == 0) {
    return {D3DDECLUSAGE_POSITION, 0};
  }
  switch (format) {
    case kFmt_2_10_10_10:
    case kFmt_10_11_11:
    case kFmt_11_11_10:
      switch (packed_count++) {
        case 0:
          return {D3DDECLUSAGE_NORMAL, 0};
        case 1:
          return {D3DDECLUSAGE_TANGENT, 0};
        case 2:
          return {D3DDECLUSAGE_BINORMAL, 0};
        default:
          break;
      }
      break;
    case kFmt_8_8_8_8:
      return {D3DDECLUSAGE_COLOR, uint8_t(color_count++)};
    default:
      break;
  }
  return {D3DDECLUSAGE_TEXCOORD, uint8_t(texcoord_count++)};
}

//--- Texture formats --------------------------------------------------------

#ifndef MAKEFOURCC
#define MAKEFOURCC(a, b, c, d)                                                     \
  (uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | \
   (uint32_t(uint8_t(d)) << 24))
#endif

// xenos::TextureFormat values NX1's materials actually use.
enum : uint32_t {
  kTex_8 = 2,
  kTex_1_5_5_5 = 3,
  kTex_5_6_5 = 4,
  kTex_8_8_8_8 = 6,
  kTex_8_A = 8,
  kTex_8_8 = 10,
  kTex_8_8_8_8_A = 14,
  kTex_4_4_4_4 = 15,
  kTex_DXT1 = 18,
  kTex_DXT2_3 = 19,
  kTex_DXT4_5 = 20,
  kTex_DXT3A = 58,
  kTex_DXT5A = 59,
  kTex_16_FLOAT = 30,
  kTex_16_16_FLOAT = 31,
  kTex_16_16_16_16_FLOAT = 32,
  kTex_DXN = 49,
  kTex_8_8_8_8_AS_16_16_16_16 = 50,
  kTex_DXT1_AS_16_16_16_16 = 51,
  kTex_DXT2_3_AS_16_16_16_16 = 52,
  kTex_DXT4_5_AS_16_16_16_16 = 53,
};

// Single-channel Xenos BC alpha formats have no D3D9 equivalent. The guest sets a
// RRRR fetch swizzle (replicate the one channel into all four) that we cannot
// reproduce on a fixed-function D3D9 sampler, so we CPU-decode them to A8R8G8B8
// with the value written into every channel -- then any shader swizzle reads it.
enum class TexDecode { kNone, kDXT3A, kDXT5A, kDXN, kColorSwizzle };

/// XE_GPU_TEXTURE_SWIZZLE with each component taken from its own source channel: x<-X, y<-Y,
/// z<-Z, w<-W, i.e. 0 | 1<<3 | 2<<6 | 3<<9. Anything else means the fetch reorders, replicates
/// or constant-fills channels, and a host format that samples RGBA straight cannot honour it.
inline constexpr uint32_t kIdentitySwizzle = 0u | (1u << 3) | (2u << 6) | (3u << 9);

struct HostTextureFormat {
  D3DFORMAT d3d;
  /// 32-bit RGBA source landing in A8R8G8B8: apply the fetch constant's channel swizzle
  /// after the endian swap (see MakeSwizzle32). The other formats' swizzles are the
  /// natural replicating ones -- (X,X,X,Y) for 8_8 into A8L8, (X,X,X,1) for 8 into L8 --
  /// which their host format already reproduces.
  bool swizzle32;
  bool valid;
  TexDecode decode = TexDecode::kNone;
  /// A block-compressed format the D3D9 runtime does not recognise as one (a FourCC it
  /// passes straight through to the driver). It sizes such a surface as if it were one byte
  /// per texel: LockRect reports `pitch == width` rather than the block row's
  /// `blocks_wide * bytes_per_block`, and the smallest levels get less storage than a single
  /// block. Both have to be worked around -- see the upload in GetTexture.
  bool opaque_block = false;
};

HostTextureFormat PickHostTextureFormat(uint32_t xenos_format) {
  switch (xenos_format) {
    case kTex_DXT1:
    case kTex_DXT1_AS_16_16_16_16:
      return {D3DFMT_DXT1, false, true};
    case kTex_DXT2_3:
    case kTex_DXT2_3_AS_16_16_16_16:
      return {D3DFMT_DXT3, false, true};
    case kTex_DXT4_5:
    case kTex_DXT4_5_AS_16_16_16_16:
      return {D3DFMT_DXT5, false, true};
    case kTex_DXN:  // BC5 / two-channel normal maps
      // NOT host ATI2: Xenos k_DXN's hardware fetch swizzle is RGGG -- (x, y, y, y), Y
      // replicated into green, blue and ALPHA (rexglue-sdk d3d12 texture_cache host format
      // table). The world's bump-lit shaders read normal maps as `.xw`, so host BC5's
      // constant `.w = 1` silently corrupts the bump-basis weight that scales the primary
      // (sun + baked shadow) lightmap on every lightmapped surface -- the flat world.
      // Decode on the CPU to A8R8G8B8 laid out exactly as the hardware returns it.
      return {D3DFMT_A8R8G8B8, false, true, TexDecode::kDXN};
    case kTex_DXT3A:  // single-channel explicit-alpha BC -> decode + replicate
      return {D3DFMT_A8R8G8B8, false, true, TexDecode::kDXT3A};
    case kTex_DXT5A:  // single-channel interpolated-alpha BC -> decode + replicate
      return {D3DFMT_A8R8G8B8, false, true, TexDecode::kDXT5A};

    case kTex_8_8_8_8:
    case kTex_8_8_8_8_A:
    case kTex_8_8_8_8_AS_16_16_16_16:
      return {D3DFMT_A8R8G8B8, true, true};
    case kTex_5_6_5:
      return {D3DFMT_R5G6B5, false, true};
    case kTex_1_5_5_5:
      return {D3DFMT_A1R5G5B5, false, true};
    case kTex_4_4_4_4:
      return {D3DFMT_A4R4G4B4, false, true};
    case kTex_8:
    case kTex_8_A:
      return {D3DFMT_L8, false, true};
    case kTex_8_8:
      return {D3DFMT_A8L8, false, true};
    case kTex_16_FLOAT:
      return {D3DFMT_R16F, false, true};
    case kTex_16_16_FLOAT:
      return {D3DFMT_G16R16F, false, true};
    case kTex_16_16_16_16_FLOAT:
      return {D3DFMT_A16B16G16R16F, false, true};

    default:
      return {D3DFMT_UNKNOWN, false, false};
  }
}

inline uint32_t Log2Exact(uint32_t v) { return v ? __builtin_ctz(v) : 0; }

/// Can the driver filter a mip chain down from level 0 for this format?
///
/// This must be tested against D3D_OK exactly, *not* with SUCCEEDED(). When a format is usable
/// but the driver will not auto-generate its mips, CheckDeviceFormat returns D3DOK_NOAUTOGEN
/// (0x0008652B) -- which is a **success** code, so SUCCEEDED() is true for it. Reading that as
/// "supported" is how every format in the game came back supported while not one of them
/// actually got a chain: we created the textures with D3DUSAGE_AUTOGENMIPMAP, called
/// GenerateMipSubLevels(), and were silently handed a single level. The whole mip path was
/// inert, and every minified surface aliased into coloured speckle.
bool SupportsAutoMips(IDirect3DDevice9Ex* device, D3DFORMAT format) {
  static std::unordered_map<uint32_t, bool> cache;
  const auto it = cache.find(uint32_t(format));
  if (it != cache.end()) {
    return it->second;
  }
  HRESULT hr = E_FAIL;
  IDirect3D9* d3d = nullptr;
  D3DDEVICE_CREATION_PARAMETERS params{};
  D3DDISPLAYMODE mode{};
  if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
    if (SUCCEEDED(device->GetCreationParameters(&params)) &&
        SUCCEEDED(d3d->GetAdapterDisplayMode(params.AdapterOrdinal, &mode))) {
      hr = d3d->CheckDeviceFormat(params.AdapterOrdinal, params.DeviceType, mode.Format,
                                  D3DUSAGE_AUTOGENMIPMAP, D3DRTYPE_TEXTURE, format);
    }
    d3d->Release();
  }
  const bool supported = hr == D3D_OK;
  cache.emplace(uint32_t(format), supported);
  REXGPU_INFO("nx1_d3d9: auto mip generation for host format 0x{:08X}: {} (hr {:#010x})",
              uint32_t(format), supported ? "supported" : "NOT supported (single level)",
              static_cast<uint32_t>(hr));
  return supported;
}

/// Read a big-endian dword out of a guest buffer.
inline uint32_t LoadBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return Swap32(v);
}

inline float Snorm(uint32_t bits, uint32_t width) {
  const int32_t v = int32_t(bits << (32 - width)) >> (32 - width);
  const float f = float(v) / float((1u << (width - 1)) - 1u);
  // Xenos' default signed-repeating-fraction mode clamps -max to exactly -1.
  return f < -1.0f ? -1.0f : f;
}

inline float Unorm(uint32_t bits, uint32_t width) {
  return float(bits) / float((1u << width) - 1u);
}

/// Decode one packed element into `out[0..3]`.
void Expand(const uint8_t* src, const ConvertOp& op, float out[4]) {
  out[0] = out[1] = out[2] = 0.0f;
  out[3] = 1.0f;

  uint32_t offsets[4] = {0, 0, 0, 0};
  uint32_t widths[4] = {0, 0, 0, 0};
  uint32_t components = 0;

  switch (op.format) {
    case kFmt_2_10_10_10:
      components = 4;
      widths[0] = widths[1] = widths[2] = 10;
      widths[3] = 2;
      offsets[1] = 10;
      offsets[2] = 20;
      offsets[3] = 30;
      break;
    case kFmt_10_11_11:
      components = 3;
      widths[0] = widths[1] = 11;
      widths[2] = 10;
      offsets[1] = 11;
      offsets[2] = 22;
      break;
    case kFmt_11_11_10:
      components = 3;
      widths[0] = 10;
      widths[1] = widths[2] = 11;
      offsets[1] = 10;
      offsets[2] = 21;
      break;
    case kFmt_8_8_8_8:
      components = 4;
      widths[0] = widths[1] = widths[2] = widths[3] = 8;
      offsets[1] = 8;
      offsets[2] = 16;
      offsets[3] = 24;
      break;
    case kFmt_16_16:
    case kFmt_16_16_16_16: {
      const uint32_t n = op.format == kFmt_16_16 ? 2 : 4;
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t dword = LoadBE32(src + (i / 2) * 4);
        const uint32_t bits = (dword >> ((i & 1) * 16)) & 0xFFFF;
        out[i] = op.is_signed ? (op.is_normalized ? Snorm(bits, 16) : float(int16_t(bits)))
                              : (op.is_normalized ? Unorm(bits, 16) : float(bits));
      }
      return;
    }
    case kFmt_32:
    case kFmt_32_32:
    case kFmt_32_32_32_32: {
      const uint32_t n = op.src_size / 4;
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t bits = LoadBE32(src + i * 4);
        out[i] = op.is_signed ? float(int32_t(bits)) : float(bits);
      }
      return;
    }
    default:
      return;
  }

  const uint32_t dword = LoadBE32(src);
  for (uint32_t i = 0; i < components; ++i) {
    const uint32_t bits = (dword >> offsets[i]) & ((1u << widths[i]) - 1u);
    out[i] = op.is_signed ? (op.is_normalized ? Snorm(bits, widths[i])
                                              : float(int32_t(bits << (32 - widths[i])) >>
                                                      (32 - widths[i])))
                          : (op.is_normalized ? Unorm(bits, widths[i]) : float(bits));
  }
}

/// Byteswap `bytes` (a multiple of 4) of big-endian data into `dst`.
void SwapDwords(uint8_t* dst, const uint8_t* src, size_t bytes) {
  for (size_t i = 0; i < bytes; i += 4) {
    const uint32_t v = LoadBE32(src + i);
    std::memcpy(dst + i, &v, sizeof(v));
  }
}

/// Overwrite a staging surface's level 0 with a known checkerboard. See nx1_d3d9_dbg_synthetic_tex.
///
/// Writes real BC blocks rather than raw bytes: a DXT1/DXT5 block whose two endpoints are EQUAL
/// decodes to a solid colour whatever the index bits say, so a constant-colour block is trivial to
/// synthesise and cannot be mistaken for a decode artifact. Formats other than BC1/BC2/BC3 and
/// A8R8G8B8 are left alone -- a pattern this test cannot synthesise correctly would be
/// indistinguishable from the corruption it is meant to detect.
void FillSyntheticLevel(IDirect3DTexture9* staging, uint32_t level, uint32_t width,
                        uint32_t height, D3DFORMAT fmt, uint32_t seed);

void FillSyntheticPattern(IDirect3DTexture9* staging, uint32_t width, uint32_t height,
                          D3DFORMAT fmt, uint32_t seed) {
  if (!staging) {
    return;
  }
  // EVERY LEVEL, not just level 0.
  //
  // The first version filled level 0 only, and the result was the most informative accident of the
  // investigation: up close (level 0) the pattern was pixel-perfect, and at distance (levels 1+,
  // still holding the decode) it broke up. That localises the fault to the mip chain -- which we
  // GENERATE on the host -- and it directly contradicts the earlier "nomips still speckles, so the
  // chain is exonerated" reading. That reading was void: nx1_d3d9_dbg_nomips never armed in any of
  // 26 logs (its NOMIPS counter, which prints on the first forced application, never appeared).
  //
  // Filling every level makes the test decisive: if the pattern is then clean at ALL distances,
  // upload and sampling are sound at every level and our mip GENERATION is the bug. If it still
  // breaks at range with every level synthetic, the fault is in level selection or sampling, not
  // in the data.
  const DWORD level_count = staging->GetLevelCount();
  for (DWORD level = 0; level < level_count; ++level) {
    const uint32_t lw = std::max(width >> level, 1u);
    const uint32_t lh = std::max(height >> level, 1u);
    FillSyntheticLevel(staging, level, lw, lh, fmt, seed);
  }
}

/// One mip level of the synthetic pattern. Split out so the loop above reads plainly.
void FillSyntheticLevel(IDirect3DTexture9* staging, uint32_t level, uint32_t width, uint32_t height,
                        D3DFORMAT fmt, uint32_t seed) {
  D3DLOCKED_RECT lr{};
  if (FAILED(staging->LockRect(level, &lr, nullptr, 0))) {
    return;
  }
  // Two distinct colours per texture, derived from its address, so different textures are visibly
  // different on screen.
  auto rgb565 = [](uint32_t r, uint32_t g, uint32_t b) -> uint16_t {
    return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  };
  const uint32_t h = seed >> 12;
  const uint16_t ca = rgb565((h * 37) & 0xFF, (h * 91) & 0xFF, (h * 151) & 0xFF);
  const uint16_t cb = rgb565(255 - ((h * 37) & 0xFF), 255 - ((h * 91) & 0xFF), 60);
  const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
  auto* base = static_cast<uint8_t*>(lr.pBits);
  const bool bc1 = fmt == D3DFMT_DXT1;
  const bool bc23 = fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3 || fmt == D3DFMT_DXT4 ||
                    fmt == D3DFMT_DXT5;
  if (bc1 || bc23) {
    const uint32_t stride = bc1 ? 8 : 16;
    for (uint32_t by = 0; by < bh; ++by) {
      uint8_t* row = base + size_t(by) * lr.Pitch;
      for (uint32_t bx = 0; bx < bw; ++bx) {
        uint8_t* blk = row + size_t(bx) * stride;
        // 4x4-block checkerboard: 8 blocks (32 texels) per square, large enough to read at range.
        const uint16_t c = (((bx >> 3) + (by >> 3)) & 1) ? ca : cb;
        if (bc23) {
          blk[0] = 0xFF;  // BC4 alpha: a0 = a1 = 255 -> fully opaque regardless of the index bits
          blk[1] = 0xFF;
          std::memset(blk + 2, 0, 6);
          blk += 8;
        }
        // Equal endpoints -> every palette entry is the same colour -> solid block.
        blk[0] = uint8_t(c & 0xFF);
        blk[1] = uint8_t(c >> 8);
        blk[2] = uint8_t(c & 0xFF);
        blk[3] = uint8_t(c >> 8);
        std::memset(blk + 4, 0, 4);
      }
    }
  } else if (fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8) {
    for (uint32_t y = 0; y < height; ++y) {
      auto* row = reinterpret_cast<uint32_t*>(base + size_t(y) * lr.Pitch);
      for (uint32_t x = 0; x < width; ++x) {
        const uint16_t c = (((x >> 5) + (y >> 5)) & 1) ? ca : cb;
        const uint32_t r = ((c >> 11) & 0x1F) << 3, g = ((c >> 5) & 0x3F) << 2,
                       b = (c & 0x1F) << 3;
        row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
      }
    }
  }
  staging->UnlockRect(level);
}

const uint8_t* TranslatePhysical(uint32_t guest_physical) {
  auto* memory = rex::system::kernel_state()->memory();
  return memory->TranslatePhysical<const uint8_t*>(guest_physical);
}

// For CPU-side guest pointers (UP-draw vertex/index scratch), which are full
// effective addresses in the physical-mirror window (0xE0..) rather than raw GPU
// fetch addresses. TranslateVirtual folds in the heap host_address_offset -- the
// 0x1000 page offset TranslatePhysical drops on the 0xE0 heap on Windows.
const uint8_t* TranslateGuestPointer(uint32_t guest_addr) {
  auto* memory = rex::system::kernel_state()->memory();
  return memory->TranslateVirtual<const uint8_t*>(guest_addr);
}

uint64_t MixKey(uint64_t a, uint64_t b) {
  uint64_t h = a * 0x9E3779B97F4A7C15ull;
  h ^= b + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h;
}

/// Hash everything about a layout that changes the bytes it produces -- and nothing about
/// which shader or declaration it came from. See VertexLayout::content_key.
uint64_t LayoutContentKey(const VertexLayout& layout) {
  uint64_t h = MixKey(0x5644656C6C61796Full, layout.stream_count);
  for (uint32_t s = 0; s < kMaxHostStreams; ++s) {
    h = MixKey(h, (uint64_t(layout.guest_stride[s]) << 32) | layout.host_stride[s]);
    h = MixKey(h, layout.bulk_swap[s] ? 1 : 0);
  }
  for (const ConvertOp& op : layout.ops) {
    h = MixKey(h, (uint64_t(op.src_offset) << 48) | (uint64_t(op.dst_offset) << 32) |
                      (uint64_t(op.stream) << 24) | (uint64_t(op.format) << 16) |
                      (uint64_t(op.src_size) << 8) | op.dst_size);
    h = MixKey(h, (op.is_signed ? 1u : 0u) | (op.is_normalized ? 2u : 0u) | (op.expand ? 4u : 0u));
  }
  return h;
}

// last_frame gates frame-coherent reuse: a resource bound by many draws in one
// frame is hashed/validated only on its first use that frame, not per draw.
struct VertexBufferEntry {
  IDirect3DVertexBuffer9* vb = nullptr;
  uint32_t bytes = 0;
  uint32_t vertex_count = 0;
  uint64_t content_hash = 0;
  uint64_t last_frame = 0;
  /// How many times this entry's contents have been rewritten, and whether it has been moved
  /// to a DYNAMIC buffer as a result. See the rebuild path in GetVertexBuffer.
  uint32_t rebuilds = 0;
  bool dynamic = false;
};

/// Rebuilds after which a vertex buffer is treated as dynamic geometry and recreated with
/// D3DUSAGE_DYNAMIC. Low, because the cost of being wrong is small in either direction.
inline constexpr uint32_t kDynamicRebuilds = 3;

struct IndexBufferEntry {
  IDirect3DIndexBuffer9* ib = nullptr;
  uint32_t bytes = 0;
  uint32_t index_size = 0;
  uint64_t content_hash = 0;
  uint64_t last_frame = 0;
  /// Host-order copy of the indices. A draw's fetch constant covers the whole pooled
  /// vertex buffer, not the model in it, so the only way to know how many vertices a draw
  /// actually touches is to look at its indices -- and scanning this beats re-reading
  /// big-endian guest memory.
  std::vector<uint8_t> cpu;
};

/// Memoized "highest index this draw range references", so the scan happens once per
/// distinct draw rather than once per frame per draw.
using IndexRangeMap = FlatMap<uint32_t>;

struct TextureEntry {
  IDirect3DTexture9* tex = nullptr;
  /// Volume textures are a separate D3D9 type. The composite's colour-grading LUT is a 3D
  /// texture and its tone-map curve a 1D one; leaving either unbound samples black, which
  /// blacks out the whole composited frame.
  IDirect3DVolumeTexture9* vol = nullptr;
  /// Cube maps (environment/reflection), a third D3D9 type again.
  IDirect3DCubeTexture9* cube = nullptr;
  /// Host bytes this entry's texture occupies (level 0 plus its chain), for the MEM stats line.
  /// Counts alone cannot say whether a cache is oversized: 1726 textures is unremarkable, 1726
  /// textures holding 4 GB is the whole bug. Recorded at creation.
  uint32_t host_bytes = 0;
  uint64_t layout_key = 0;  ///< base address + format + dims + mips; a change rebuilds it
  uint64_t last_frame = 0;
  /// Content hash for the cube/volume paths, which still re-read on a content change. The 2D path
  /// decodes from the CPU mirror (MirrorSnapshot) and re-reads only when the write-watch fires.
  uint64_t content_hash = 0;
  /// Write-watch state. We snapshot a texture once and re-read its guest memory only when the guest
  /// actually writes it (dirty set by DrainMemoryWrites from the invalidation callback). watch_addr/
  /// size record the physical range so an invalidation can be matched back to this entry.
  bool dirty = false;
  uint32_t watch_addr = 0;
  uint32_t watch_size = 0;
  /// Second watched range: the guest mip chain at mip_address, when this texture's levels were
  /// decoded from it (mip_source == 3). The mip pool streams and relocates like the base pool,
  /// so a write there must dirty the entry exactly as a base write does.
  uint32_t mip_watch_addr = 0;
  uint32_t mip_watch_size = 0;
  /// The mip_address this entry's levels were decoded from. NOT part of the cache key (the pool
  /// relocates mip tails, and keying on it would duplicate entries); instead a mismatch at bind
  /// time marks the entry dirty so it re-decodes from the new location.
  uint32_t mip_addr_seen = 0;
  /// Commit-and-freeze. While a texture is on screen its slot holds valid bytes, and the write-watch
  /// keeps the decode tracking them (clean up close). But when it goes non-resident the streaming
  /// pool recycles its slot, dribbling high-noise garbage into the exact bytes -- and re-reading that
  /// is the confetti-on-backup. A texture drawn cleanly for kCommitFrames is settled, so we commit
  /// it: stop honouring writes and hold the good decode. good_frames counts frames it has been drawn.
  uint32_t good_frames = 0;
  /// Was the decode that produced this texture made from a source with EMPTY pages? Such a
  /// decode must never be committed: committing stops honouring writes, so a texture decoded
  /// before its contents streamed in is frozen as garbage for the rest of the session. Measured
  /// exactly that -- source went 0 -> 8121/8192 nonzero AFTER the entry committed, and it then
  /// reported dirty=0 committed=1 on every subsequent frame and never re-decoded.
  bool decoded_from_partial = false;
  /// Populated (non-zero) source bytes behind the decode this entry currently holds, and how many
  /// re-decodes have been REFUSED for being poorer than it. See nx1_d3d9_keep_best.
  uint32_t decoded_nonzero = 0;
  uint32_t decoded_sampled = 0;
  uint32_t keepbest_refusals = 0;
  /// HIGH-WATER MARK: the best source population ever seen for this slot, with the sample count it
  /// was measured over. Only ever rises; cleared when layout_key changes (a different texture).
  ///
  /// The first keep-best compared against `decoded_nonzero` -- the population of whatever decode we
  /// last ACCEPTED -- and that is why it did nothing (2129 refusals, speckle unchanged). When the
  /// escape hatch fired we accepted a trough decode AND reset the baseline to that trough, so the
  /// ratchet collapsed and everything after passed. It was a comparison against the last thing we
  /// happened to take, which is exactly the trap a churning slot sets.
  ///
  /// Measured justification (run 010, texture 10DD4000, from the reference's own upload dumps): the
  /// SAME pages read ~99% populated at submissions 11623/12591/12945 and 12-49% at 12075/15363/
  /// 15364. The complete texture genuinely exists and recurs, so waiting for it is not a judgement
  /// about what art should look like. Per-slot for that reason -- a global "reject below 90%" would
  /// freeze legitimately sparse alpha-masked art forever.
  uint32_t best_nonzero = 0;
  uint32_t best_sampled = 0;
  /// Cheap content fingerprint of the guest source, taken at decode and re-checked once per
  /// frame on bind. layout_key cannot distinguish two textures when the pool moves a different
  /// image of the SAME dimensions and format into this address -- measured 156 times in one run
  /// -- so cache identity must depend on what the slot HOLDS, not only where it is.
  uint64_t probe_hash = 0;
  uint64_t probe_frame = 0;
  bool committed = false;
  /// Consecutive fully-transparent decodes of a broadcast-swizzle sprite, and the frame before
  /// which not to bother trying again. A sprite whose texel pool never streams in (this build has
  /// no imagefile for some of them -- live RAM reads 0) would otherwise be held dirty and
  /// re-decoded EVERY frame forever: measured as a steady 7-8 rebuilds/frame, ~4 ms, all of it
  /// spent producing the same blank image. Back off exponentially instead, capped so a pool that
  /// does eventually arrive is still picked up within ~1 second.
  uint32_t zero_retries = 0;
  uint64_t retry_frame = 0;
  /// Which invalidation source last marked this entry dirty. Attributed at rebuild
  /// time so we can see WHICH source makes us re-read memory the reference does not.
  DirtySource dirty_source = DirtySource::kDebug;
  /// Did any real write get reported for this entry since its last decode? Drives
  /// nx1_d3d9_probe_needs_write: a probe-detected change with no reported write came
  /// through a path the reference ignores.
  bool write_seen_since_decode = false;
  /// PREMATURE-DECODE TEST. Frame at which to force one re-decode regardless of the write-watch,
  /// or 0 for none. The watch cannot help here: guest texture memory demonstrably changes through
  /// paths it never sees (thousands of DECODECHANGE entries, every one at "page writes 0 -> 0"),
  /// so a decode taken before the data landed is served forever and nothing signals it.
  uint64_t recheck_frame = 0;
  uint32_t rechecks_left = 0;
  bool ladder_done = false;  ///< the ladder runs ONCE per entry, not once per decode
  /// How this texture's mip chain was actually provided: 0 = none, 1 = CPU-built (block
  /// compressed), 2 = driver auto-generated, 3 = decoded from the guest's own chain at
  /// mip_address. Classified by OUTCOME, not by intent, so the nx1_d3d9_bc_mips diagnostic
  /// cannot mislabel a BC texture as auto-mipped when it in fact ended up with no chain at all.
  uint8_t mip_source = 0;
};

struct ResolvedTarget {
  IDirect3DTexture9* tex = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  bool owned = true;  ///< false when it aliases a target we don't own (a depth texture)
};

struct SurfaceSize {
  uint32_t width = 0;
  uint32_t height = 0;
};

/// One host render target standing in for a guest D3DSurface.
struct HostTarget {
  IDirect3DTexture9* tex = nullptr;      ///< null when this aliases the backbuffer
  IDirect3DSurface9* surface = nullptr;  ///< the bindable surface
  uint32_t width = 0;
  uint32_t height = 0;
  bool is_depth = false;
  bool is_backbuffer = false;
};

/// D3DFMT_INTZ: a depth-stencil format that can also be sampled as a texture. This is
/// how a D3D9 renderer gets the shadow maps and the scene depth back into a shader.
#ifndef D3DFMT_INTZ
#define D3DFMT_INTZ (D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z')))
#endif

using LayoutMap = std::unordered_map<uint64_t, VertexLayout>;
using VertexBufferMap = FlatMap<VertexBufferEntry>;
/// Guest-memory hash of a vertex buffer, memoized for the frame it was taken in.
struct VertexHash {
  uint64_t frame = 0;  ///< frame_ + 1, so a default-constructed entry never matches frame 0
  uint64_t hash = 0;
};
using VertexHashMap = FlatMap<VertexHash>;
using IndexBufferMap = FlatMap<IndexBufferEntry>;
using TextureMap = FlatMap<TextureEntry>;

/// The largest-resolution texture a geometry surface has bound on a given sampler. `tex` is AddRef'd
/// so it outlives eviction of the base-keyed TextureMap entry it came from. See PreferLargestForSurface.
struct BestTexture {
  IDirect3DBaseTexture9* tex = nullptr;
  uint32_t area = 0;  ///< width * height of the retained texture
  uint64_t last_frame = 0;
  /// Guest allocation the retained texture was decoded from. Same dimensions do NOT mean same
  /// content: the streaming pool rebinds a different allocation at the same declared size, and
  /// comparing addresses is what separates that from a texture updating in place.
  uint32_t base_address = 0;
  /// How POPULATED the guest source of the retained texture was, as a permille of sampled bytes.
  ///
  /// Area is the wrong axis on its own, and the note in PreferLargestForSurface records why:
  /// substituting the retained texture was tried and did nothing, "which means the retained
  /// texture carries the same bad texels". Of course it did -- retention was keyed on SIZE, so a
  /// sparse texture could be retained and then serve a different sparse texture.
  ///
  /// The census (run 023, first-of-frame filtered) says the axis that matters is population: every
  /// real surface->texture swap went from a 12-23% populated source to an 83-99% one. So retain
  /// the best-POPULATED source this surface has shown, and refuse to fall back to a much sparser
  /// one. That is new evidence, which is what that note asked for.
  uint32_t src_permille = 0;
};
using BestTextureMap = FlatMap<BestTexture>;

using ResolveMap = FlatMap<ResolvedTarget>;
/// Keyed by EDRAM region, not by D3DSurface pointer -- see ReadSurfaceEdramKey.
using TargetMap = std::unordered_map<uint64_t, HostTarget>;

/// Physical page behind a guest address. The 360 maps the same RAM through several
/// virtual windows, and a resolve destination is not written and read through the same
/// one: the engine hands D3DDevice_Resolve a texture whose header base sits in the
/// uncached 0xA0000000 window (0xBF6D8000 for the scene depth), then samples that exact
/// same surface through a fetch constant holding the physical address (0x1F6D8000).
/// Keying the resolve map by the raw address therefore misses on every read-back -- the
/// lighting shaders sample raw untiled memory instead of the scene, the depth buffer and
/// the shadow maps. Key by the physical page and both windows agree.
constexpr uint32_t PhysicalAddress(uint32_t address) { return address & 0x1FFFFFFF; }

/// Untile (or straight-copy) one 2D mip into `dst`, tightly packed at
/// `dst_row_bytes` per block-row, applying the fetch constant's endian swap.
/// `src` points at the level's base in guest memory.
///
/// `offset_x`/`offset_y` are the level's origin *within* that image, in blocks. They are
/// non-zero only for a packed mip tail: once a level shrinks to 16 texels or less, Xenos
/// stops giving it its own image and parks it alongside its siblings inside one 32x32-block
/// tile (see TextureInfo::GetPackedTileOffset).
/// CAUSAL TEST FOR THE PROVENANCE FINDING. Set to the foreign-page list for the texture about to
/// be detiled; every block whose SOURCE lands in one of those pages is replaced with a solid
/// marker instead of the real data. Then the screen answers the question directly:
///
///   corrupt areas turn magenta -> the pages the detector flags ARE the corruption
///   corruption survives, magenta lands elsewhere -> the detector measures something benign
///
/// Needed because the mask/screenshot "match" argued from SHAPE, and a page boundary in a tiled
/// texture is always a horizontal band -- so any page-granular effect produces bands on both
/// sides. Shape agreement there is structurally guaranteed and proves nothing about cause.
///
/// Patched inside DetileMip2D because every decode path (colour-swizzle, DXN, BC-alpha, direct BC,
/// mip-staged) funnels through it, so one hook covers every format without touching each branch.
const std::vector<uint32_t>* g_paint_pages = nullptr;

/// Write a visually loud block in whatever format the scratch holds. Not exact per format -- the
/// point is "obviously not the texture".
inline void PaintMarkerBlock(uint8_t* d, uint32_t bytes_per_block) {
  switch (bytes_per_block) {
    case 8:  // DXT1 / DXT3A / DXT5A / CTX1: two identical magenta 5-6-5 endpoints, zero indices
      d[0] = 0x1F; d[1] = 0xF8; d[2] = 0x1F; d[3] = 0xF8;
      d[4] = d[5] = d[6] = d[7] = 0;
      break;
    case 16:  // DXT2_3 / DXT4_5 / DXN: opaque alpha half, then the same magenta colour block
      d[0] = 0xFF; d[1] = 0xFF;
      for (uint32_t i = 2; i < 8; ++i) d[i] = 0;
      d[8] = 0x1F; d[9] = 0xF8; d[10] = 0x1F; d[11] = 0xF8;
      d[12] = d[13] = d[14] = d[15] = 0;
      break;
    default:  // uncompressed: fill the texel with magenta in any channel order
      for (uint32_t i = 0; i < bytes_per_block; ++i) d[i] = (i & 1) ? 0x00 : 0xFF;
      break;
  }
}

/// Counted, because "no magenta on screen" has two completely different meanings and the screen
/// cannot tell them apart: the marker never got written (instrument broken or not reaching these
/// textures), or it was written into textures that were not in view. Without this the first run of
/// this test was unreadable.
std::atomic<uint64_t> g_painted_blocks{0};
std::atomic<uint64_t> g_painted_textures{0};

inline bool PaintThisBlock(uint32_t src_off) {
  if (!g_paint_pages) {
    return false;
  }
  const uint32_t pg = src_off >> 12;
  if (std::find(g_paint_pages->begin(), g_paint_pages->end(), pg) == g_paint_pages->end()) {
    return false;
  }
  g_painted_blocks.fetch_add(1, std::memory_order_relaxed);
  return true;
}

/// BYTE EXTENT OF A TILED SURFACE -- which is NOT block_pitch_h * block_pitch_v * bpb.
///
/// The XDK ships `XGAddress2DTiledExtent` precisely because that area formula is wrong: it
/// documents the result as "the maximum texel offset that can be referenced by a fetch into the
/// surface", to sub-tile granularity (8x8 for 1 byte/block, 8x4 for 2). For 1- and 2-byte blocks
/// the tiled address scatters BEYOND the area -- a 32x32-block 1-bpb surface addresses 2560 bytes
/// where the area is only 1024. Sizing our snapshot from the area therefore has DetileMip2D read
/// up to ~1.5 KB past the end of our own allocation.
///
/// Computed here rather than guessed, using our own GetTiledOffset2D -- already verified
/// algebraically equivalent to XGAddress2DTiledOffset. Note the periodic decomposition the fast
/// detile path uses is documented as exact only for bpb_log2 >= 2, i.e. exactly NOT the case that
/// needs this, so the max is taken over the real grid.
///
/// For 4 bytes/block and up the extent equals the area (measured across pitches and shapes), so
/// those short-circuit and cost nothing. The 1/2-byte cases are cached because the answer depends
/// only on the layout, never on the texture.
/// EVERY address the guest has ever handed to Resolve/ResolveEx, recorded BEFORE any accept or
/// reject decision. This is deliberately not the resolve MAP: the map holds destinations we
/// successfully took, and the failure mode under investigation is a destination we saw and then
/// dropped (zero extent, no device yet, wrong window) or keyed in a way a later texture bind
/// cannot match. Those are invisible if you only ever look at what the map contains.
///
/// A handful of entries in practice (8-13), so a linear scan at decode time is free.
struct ResolveDestSpan {
  uint32_t first_page, last_page;
};
std::mutex g_resolve_seen_m;
std::vector<ResolveDestSpan> g_resolve_seen;

void NoteResolveDestSeen(uint32_t dest_address, uint64_t bytes) {
  if (!dest_address) {
    return;
  }
  const uint32_t phys = dest_address & 0x1FFFFFFF;
  const uint32_t first = phys >> 12;
  const uint32_t last = uint32_t((phys + (bytes ? bytes - 1 : 0)) >> 12);
  std::lock_guard<std::mutex> lk(g_resolve_seen_m);
  for (const auto& s : g_resolve_seen) {
    if (s.first_page == first && s.last_page == last) {
      return;
    }
  }
  if (g_resolve_seen.size() < 512) {
    g_resolve_seen.push_back({first, last});
  }
}

/// Does any page of this texture fall inside a region the guest ever resolved into?
bool OverlapsResolveDest(uint32_t phys_base, uint64_t bytes) {
  const uint32_t first = (phys_base & 0x1FFFFFFF) >> 12;
  const uint32_t last = uint32_t(((phys_base & 0x1FFFFFFF) + (bytes ? bytes - 1 : 0)) >> 12);
  std::lock_guard<std::mutex> lk(g_resolve_seen_m);
  for (const auto& s : g_resolve_seen) {
    if (first <= s.last_page && s.first_page <= last) {
      return true;
    }
  }
  return false;
}

/// THE RENDER-TO-TEXTURE CENSUS, over every decode rather than a 16-line sample.
///
/// The hypothesis it exists to kill or confirm: a surface is rendered in EDRAM, resolved to main
/// memory, and later SAMPLED as a texture -- and if we miss or mis-key that resolve, GetTexture
/// falls through to untiling guest RAM, which holds the address's previous occupant. That renders
/// as structured, picture-like, byte-stable foreign content, i.e. exactly the speckle signature.
///
/// `served` says whether this bind was actually satisfied from the resolve map. The three buckets:
///   overlap + served   -> render-to-texture working as intended
///   overlap + NOT served -> WE SAW THE RESOLVE AND DID NOT USE IT. This is the bug class, and it
///                          is currently invisible: nothing else in the renderer reports it.
///   no overlap         -> this texture is not render-to-texture at all
///
/// LIMIT, stated so the result is not over-read: this can only see resolves the guest routed
/// through an entry point we hook. A resolve we never observe at all cannot appear here, and the
/// only instrument that closes THAT gap is the reference CP's RangeWrittenByGpu census, which
/// requires the reference to actually execute.
void NoteResolveTextureCensus(uint32_t phys_base, uint64_t bytes, bool served) {
  if (!REXCVAR_GET(nx1_d3d9_dbg_hwcensus)) {
    return;
  }
  static std::atomic<uint64_t> overlap_served{0}, overlap_unserved{0}, no_overlap{0}, n{0};
  if (OverlapsResolveDest(phys_base, bytes)) {
    if (served) {
      overlap_served.fetch_add(1, std::memory_order_relaxed);
    } else {
      const uint64_t k = overlap_unserved.fetch_add(1, std::memory_order_relaxed) + 1;
      if (k <= 8 || (k % 5000) == 0) {
        REXGPU_WARN("nx1_d3d9: RTTMISS texture {:08X} ({} bytes) overlaps a region the guest "
                    "RESOLVED into, yet this bind was NOT served from the resolve map -- we are "
                    "untiling guest RAM where the rendered image should be ({} so far)",
                    phys_base, bytes, k);
      }
    }
  } else {
    no_overlap.fetch_add(1, std::memory_order_relaxed);
  }
  if ((n.fetch_add(1, std::memory_order_relaxed) % 20000) == 0) {
    size_t spans = 0;
    {
      std::lock_guard<std::mutex> lk(g_resolve_seen_m);
      spans = g_resolve_seen.size();
    }
    REXGPU_WARN("nx1_d3d9: HWCENSUS rtt: {} decodes overlap a resolved region and WERE served, {} "
                "overlap and were NOT ({} = the render-to-texture bug class), {} do not overlap at "
                "all. {} distinct resolve destinations observed. Only a NON-ZERO unserved count "
                "implicates EDRAM/render-to-texture in the speckle",
                overlap_served.load(), overlap_unserved.load(), overlap_unserved.load(),
                no_overlap.load(), spans);
  }
}

/// See nx1_d3d9_dbg_hwcensus. Called with each bound texture's decoded fetch constant.
void NoteHwCensus(const nx1::d3d9::TextureFetchConstant& t) {
  if (!REXCVAR_GET(nx1_d3d9_dbg_hwcensus)) {
    return;
  }
  static std::atomic<uint32_t> exp_seen{0}, stacked_seen{0}, sign_mask{0}, clamp_mask{0};
  static std::atomic<uint32_t> policy_mask{0}, bcolor_mask{0}, mipfilter_mask{0}, border_seen{0};
  static std::atomic<uint32_t> lodbias_seen{0}, gradexp_seen{0}, bcw_seen{0};
  static std::atomic<uint64_t> n{0};
  const auto orin = [](std::atomic<uint32_t>& a, uint32_t bits) {
    if (bits && (a.load(std::memory_order_relaxed) & bits) != bits) {
      a.fetch_or(bits, std::memory_order_relaxed);
    }
  };
  if (t.exp_adjust) exp_seen.fetch_add(1, std::memory_order_relaxed);
  if (t.stacked) stacked_seen.fetch_add(1, std::memory_order_relaxed);
  if (t.border) border_seen.fetch_add(1, std::memory_order_relaxed);
  if (t.lod_bias) lodbias_seen.fetch_add(1, std::memory_order_relaxed);
  if (t.grad_exp_h || t.grad_exp_v) gradexp_seen.fetch_add(1, std::memory_order_relaxed);
  if (t.force_bcw_to_max) bcw_seen.fetch_add(1, std::memory_order_relaxed);
  orin(sign_mask, (1u << t.sign[0]) | (1u << t.sign[1]) | (1u << t.sign[2]) | (1u << t.sign[3]));
  orin(clamp_mask, (1u << t.clamp_u) | (1u << t.clamp_v) | (1u << t.clamp_w));
  orin(policy_mask, 1u << t.clamp_policy);
  orin(bcolor_mask, 1u << t.border_color);
  orin(mipfilter_mask, 1u << t.mip_filter);
  // WHICH (FORMAT, SWIZZLE) PAIRS ACTUALLY OCCUR. A swizzle we drop only matters if the title
  // binds it, and the paths differ sharply in what they honour: the 32-bit path applies the full
  // per-texel permutation, BC luminance-broadcast textures are decoded to ARGB with the swizzle
  // applied, but a BC COLOUR texture with a non-identity, non-broadcast swizzle stays compressed
  // and is sampled RGBA -- dropping it silently. This enumerates the real population so that gap
  // can be sized instead of argued about.
  {
    static std::mutex pm;
    static std::vector<uint32_t> pairs;  // format << 16 | swizzle
    const uint32_t key = (t.format << 16) | (t.swizzle & 0xFFFF);
    std::lock_guard<std::mutex> lk(pm);
    if (std::find(pairs.begin(), pairs.end(), key) == pairs.end() && pairs.size() < 64) {
      pairs.push_back(key);
      std::string all;
      for (const uint32_t k : pairs) {
        all += fmt::format("f{}:{:03X} ", k >> 16, k & 0xFFFF);
      }
      NX1_LOGW_STATS("nx1_d3d9: HWCENSUS swizzle pairs (format:swizzle) -- {}. 0x688 = identity "
                  "(ABGR), 0x60A = ARGB. A BLOCK-COMPRESSED format (18/19/20/49/58/59/60) paired "
                  "with anything other than identity or a broadcast is a swizzle we DROP",
                  all);
    }
  }
  if ((n.fetch_add(1, std::memory_order_relaxed) % 20000) == 0) {
    NX1_LOGW_STATS(
        "nx1_d3d9: HWCENSUS tex: exp_adjust!=0 {}x | stacked {}x | border {}x | lod_bias!=0 {}x | "
        "grad_exp!=0 {}x | force_bcw {}x | sign values 0x{:X} (bit n = mode n; 2=biased 1=signed) "
        "| clamp modes 0x{:X} (bits 4/5/6/7 = the BORDER modes) | clamp_policy 0x{:X} (bit1 = OGL, "
        "which demotes borders to clamp) | border_color 0x{:X} | mip_filter 0x{:X} (bit2 = BASEMAP)",
        exp_seen.load(), stacked_seen.load(), border_seen.load(), lodbias_seen.load(),
        gradexp_seen.load(), bcw_seen.load(), sign_mask.load(), clamp_mask.load(),
        policy_mask.load(), bcolor_mask.load(), mipfilter_mask.load());
  }
}

uint32_t TiledSurfaceExtentBytes(uint32_t pitch_blocks, uint32_t rows_blocks, uint32_t bpb) {
  const size_t area = size_t(pitch_blocks) * rows_blocks * bpb;
  if (bpb >= 4 || !pitch_blocks || !rows_blocks) {
    return uint32_t(area);
  }
  const uint64_t key = (uint64_t(pitch_blocks) << 40) ^ (uint64_t(rows_blocks) << 8) ^ bpb;
  static std::mutex m;
  static std::unordered_map<uint64_t, uint32_t> cache;
  {
    std::lock_guard<std::mutex> lk(m);
    const auto it = cache.find(key);
    if (it != cache.end()) {
      return it->second;
    }
  }
  namespace tu = rex::graphics::texture_util;
  const uint32_t bpb_log2 = Log2Exact(bpb);
  uint32_t max_end = uint32_t(area);
  for (uint32_t by = 0; by < rows_blocks; ++by) {
    for (uint32_t bx = 0; bx < pitch_blocks; ++bx) {
      const uint32_t end =
          uint32_t(tu::GetTiledOffset2D(int32_t(bx), int32_t(by), pitch_blocks, bpb_log2)) + bpb;
      if (end > max_end) {
        max_end = end;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lk(m);
    cache[key] = max_end;
  }
  return max_end;
}

/// `src_limit` bounds how far into `src` a block may be read, in bytes (SIZE_MAX = unbounded).
///
/// The guest's fetch constant can declare a texture LARGER than the pool actually allocated for
/// it -- measured on 22 of 193 captured textures, three of them proved byte-identical to the
/// neighbouring texture that starts partway through our read. Blocks past the limit are the NEXT
/// allocation's bytes; decoding them produces the rainbow-noise speckle. They are zero-filled
/// instead, which is both honest (we have no data for them) and obvious on screen.
void DetileMip2D(uint8_t* dst, uint32_t dst_row_bytes, const uint8_t* src,
                 const rex::graphics::TextureExtent& extent, uint32_t bytes_per_block,
                 uint32_t endian, bool tiled, uint32_t offset_x = 0, uint32_t offset_y = 0,
                 size_t src_limit = SIZE_MAX) {
  namespace tu = rex::graphics::texture_util;
  namespace tc = rex::graphics::texture_conversion;
  const auto xe_endian = static_cast<rex::graphics::xenos::Endian>(endian);
  const uint32_t blocks_wide = extent.block_width;
  const uint32_t blocks_high = extent.block_height;

  if (tiled) {
    const uint32_t pitch_blocks = extent.block_pitch_h;
    const uint32_t bpb_log2 = Log2Exact(bytes_per_block);
    // Detiling was the single most expensive thing the renderer did: GetTiledOffset2D ran once per
    // block (16k times for a 512x512 DXT1) and its bit-mixing dominated a ~673 us texture rebuild.
    //
    // The address is periodic in x and y with period 32 apart from the macro-tile term: micro uses
    // only x&7, and the (x>>3)&3 term is invariant mod 32 because x>>3 = 4*(x>>5) + ((x&31)>>3).
    // The macro delta is ((x>>5) + (y>>5)*(aligned_pitch>>5)) << (bpb_log2+7); when that is a
    // multiple of 512 it clears the final mix's low fields entirely and simply adds, scaled by 8:
    //
    //   offset(x,y) = table[y&31][x&31] + ((x>>5) + (y>>5)*(ap>>5)) * (1 << (bpb_log2+10))
    //
    // That holds only for blocks of 4 bytes or more (for 1- and 2-byte blocks the stride is 128 or
    // 256 and leaks into the mixed bits). Verified exhaustively against GetTiledOffset2D across
    // pitches 32..512 for bpb_log2 0..4: exact for 2..4, mismatching for 0..1 -- hence the guard.
    // The table fast path is cvar-gated because it bought NOTHING when measured (the loop is
    // bound by cache-miss latency on the scattered mirror reads, not by the address maths), so
    // it is pure risk. If a tiled-looking artifact appears, turn this off first: a checkerboard
    // in texture space is exactly what a wrong tiled address produces.
    if (bpb_log2 >= 2 && REXCVAR_GET(nx1_d3d9_fast_detile)) {
      const uint32_t ap_tiles = ((pitch_blocks + 31u) & ~31u) >> 5;
      const uint32_t macro_stride = 1u << (bpb_log2 + 10);
      int32_t table[32][32];
      for (uint32_t ty = 0; ty < 32; ++ty) {
        for (uint32_t tx = 0; tx < 32; ++tx) {
          table[ty][tx] = tu::GetTiledOffset2D(int32_t(tx), int32_t(ty), pitch_blocks, bpb_log2);
        }
      }
      const bool swap = xe_endian != rex::graphics::xenos::Endian::kNone;
      for (uint32_t by = 0; by < blocks_high; ++by) {
        const uint32_t y = offset_y + by;
        const int32_t* row_tab = table[y & 31];
        const uint32_t row_macro = (y >> 5) * ap_tiles * macro_stride;
        uint8_t* dst_row = dst + size_t(by) * dst_row_bytes;
        for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
          const uint32_t x = offset_x + bx;
          const uint32_t src_off = uint32_t(row_tab[x & 31]) + (x >> 5) * macro_stride + row_macro;
          uint8_t* d = dst_row + size_t(bx) * bytes_per_block;
          if (size_t(src_off) + bytes_per_block > src_limit) {
            std::memset(d, 0, bytes_per_block);  // past the allocation: not our data
            continue;
          }
          if (PaintThisBlock(src_off)) {
            PaintMarkerBlock(d, bytes_per_block);
          } else if (swap) {
            tc::CopySwapBlock(xe_endian, d, src + src_off, bytes_per_block);
          } else {
            std::memcpy(d, src + src_off, bytes_per_block);
          }
        }
      }
    } else {
      for (uint32_t by = 0; by < blocks_high; ++by) {
        uint8_t* dst_row = dst + size_t(by) * dst_row_bytes;
        for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
          const int32_t src_off = tu::GetTiledOffset2D(
              int32_t(offset_x + bx), int32_t(offset_y + by), pitch_blocks, bpb_log2);
          uint8_t* d = dst_row + size_t(bx) * bytes_per_block;
          if (size_t(src_off) + bytes_per_block > src_limit) {
            std::memset(d, 0, bytes_per_block);
            continue;
          }
          if (PaintThisBlock(uint32_t(src_off))) {
            PaintMarkerBlock(d, bytes_per_block);
          } else {
            tc::CopySwapBlock(xe_endian, d, src + src_off, bytes_per_block);
          }
        }
      }
    }
  } else {
    // Linear: block rows are 256-byte aligned in guest memory.
    const uint32_t src_row_bytes = extent.block_pitch_h * bytes_per_block;
    const uint32_t row_copy = blocks_wide * bytes_per_block;
    const size_t src_origin = size_t(offset_y) * src_row_bytes + size_t(offset_x) * bytes_per_block;
    for (uint32_t by = 0; by < blocks_high; ++by) {
      // Linear rows are copied whole, so paint per row when that row's page is flagged.
      if (PaintThisBlock(uint32_t(src_origin + size_t(by) * src_row_bytes))) {
        for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
          PaintMarkerBlock(dst + size_t(by) * dst_row_bytes + size_t(bx) * bytes_per_block,
                           bytes_per_block);
        }
        continue;
      }
      tc::CopySwapBlock(xe_endian, dst + size_t(by) * dst_row_bytes,
                        src + src_origin + size_t(by) * src_row_bytes, row_copy);
    }
  }
}

/// Untile one 3D mip slice-by-slice into `dst`, which is laid out as `depth` slices of
/// `slice_bytes`, each `row_bytes` per block-row. Xenos tiles 3D textures in 32x32x4 blocks,
/// so a slice is not simply a 2D mip -- GetTiledOffset3D walks it.
/// `rows_blocks` is the SLICE STRIDE in block rows -- the tile-padded height, not the visible one.
/// The two differ and it matters only on the linear path: the XDK requires the memory dimensions of
/// a linear texture to be whole tiles ("The dimensions of linear textures in memory must be in
/// terms of tiles", tile 32x32x4 for 3D), so consecutive slices are `pitch * PADDED_height` apart.
/// Stepping by the visible height instead walks every slice after z=0 progressively short, which is
/// wrong for any volume whose block height is not already a multiple of 32.
///
/// The TILED path is unaffected: GetTiledOffset3D aligns height internally (as XGAddress3DTiledOffset
/// does with `AlignedHeight = (Height + 31) & ~31`), so padded and visible land on the same value --
/// which is why passing the visible height went unnoticed.
void DetileMip3D(uint8_t* dst, uint32_t row_bytes, uint32_t slice_bytes, const uint8_t* src,
                 uint32_t blocks_wide, uint32_t blocks_high, uint32_t depth, uint32_t pitch_blocks,
                 uint32_t rows_blocks, uint32_t bytes_per_block, uint32_t endian, bool tiled) {
  namespace tu = rex::graphics::texture_util;
  namespace tc = rex::graphics::texture_conversion;
  const auto xe_endian = static_cast<rex::graphics::xenos::Endian>(endian);
  const uint32_t bpb_log2 = Log2Exact(bytes_per_block);
  const uint32_t slice_rows = rows_blocks ? rows_blocks : blocks_high;

  for (uint32_t z = 0; z < depth; ++z) {
    uint8_t* slice = dst + size_t(z) * slice_bytes;
    for (uint32_t by = 0; by < blocks_high; ++by) {
      uint8_t* row = slice + size_t(by) * row_bytes;
      for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
        const size_t src_off =
            tiled ? size_t(tu::GetTiledOffset3D(int32_t(bx), int32_t(by), int32_t(z), pitch_blocks,
                                                blocks_high, bpb_log2))
                  : ((size_t(z) * slice_rows + by) * pitch_blocks + bx) * bytes_per_block;
        tc::CopySwapBlock(xe_endian, row + size_t(bx) * bytes_per_block, src + src_off,
                          bytes_per_block);
      }
    }
  }
}

/// Decode linear 4x4 single-channel BC-alpha blocks (DXT3A explicit 4-bit, or DXT5A interpolated
/// 3-bit) into an A8R8G8B8 destination, HONOURING the fetch constant's channel swizzle.
///
/// This used to replicate the decoded value into R=G=B=A, i.e. it assumed swizzle RRRR (0x000).
/// The census says NOT ONE of this title's BC-alpha textures uses that:
///   DXT3A 0xA00 and DXT5A 0xA00 -> RGB <- X, **A <- constant 1**  (we were writing the mask
///     into alpha, so a texture the guest declares fully opaque came back variably transparent)
///   DXT5A 0x16D               -> **RGB <- constant 1**, A <- X    (a pure alpha mask, whose RGB
///     should be WHITE -- we were feeding the mask value into colour, darkening every surface
///     that multiplies by it exactly where the mask is low)
/// Single-channel source, so every component index resolves to the same decoded value; only the
/// CONSTANT selectors (4 = 0, 5 = 1) actually change anything -- but as measured, those are the
/// only selectors these textures use.
void DecodeBCAlphaToArgb(uint8_t* dst, uint32_t dst_pitch, const uint8_t* src, uint32_t blocks_wide,
                         uint32_t blocks_high, uint32_t width, uint32_t height, bool interpolated,
                         uint32_t swizzle) {
  const uint8_t sel[4] = {uint8_t(swizzle & 7), uint8_t((swizzle >> 3) & 7),
                          uint8_t((swizzle >> 6) & 7), uint8_t((swizzle >> 9) & 7)};
  for (uint32_t by = 0; by < blocks_high; ++by) {
    for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
      const uint8_t* blk = src + (size_t(by) * blocks_wide + bx) * 8;
      uint8_t a[16];
      if (interpolated) {
        // DXT5 alpha block: two endpoints + 16 x 3-bit palette indices (LE).
        const uint32_t a0 = blk[0], a1 = blk[1];
        uint8_t pal[8];
        pal[0] = uint8_t(a0);
        pal[1] = uint8_t(a1);
        if (a0 > a1) {
          for (uint32_t i = 1; i <= 6; ++i) pal[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
        } else {
          for (uint32_t i = 1; i <= 4; ++i) pal[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
          pal[6] = 0;
          pal[7] = 255;
        }
        uint64_t bits = 0;
        for (uint32_t i = 0; i < 6; ++i) bits |= uint64_t(blk[2 + i]) << (8 * i);
        for (uint32_t i = 0; i < 16; ++i) a[i] = pal[(bits >> (3 * i)) & 0x7];
      } else {
        // DXT3 explicit alpha: 16 x 4-bit, two texels per byte, low nibble first.
        for (uint32_t i = 0; i < 8; ++i) {
          a[i * 2 + 0] = uint8_t((blk[i] & 0x0F) * 17);
          a[i * 2 + 1] = uint8_t((blk[i] >> 4) * 17);
        }
      }
      for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint32_t py = by * 4 + ty;
        if (py >= height) break;
        uint8_t* row = dst + size_t(py) * dst_pitch;
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t px = bx * 4 + tx;
          if (px >= width) break;
          const uint8_t v = a[ty * 4 + tx];
          // Component indices 0-3 all resolve to `v` (single channel); 4 = constant 0, 5 = 1.
          const auto pick = [v](uint8_t s) -> uint8_t {
            return s == 4 ? 0x00 : (s == 5 ? 0xFF : v);
          };
          uint8_t* p = row + size_t(px) * 4;
          p[0] = pick(sel[2]);  // B <- swizzle.z
          p[1] = pick(sel[1]);  // G <- swizzle.y
          p[2] = pick(sel[0]);  // R <- swizzle.x
          p[3] = pick(sel[3]);  // A <- swizzle.w
        }
      }
    }
  }
}

//=============================================================================
// Mip-chain generation for the block-compressed formats
//
// The driver will not do it. CheckDeviceFormat answers D3DOK_NOAUTOGEN for DXT1, DXT3, DXT5
// and ATI2 -- only the uncompressed formats get a chain out of D3DUSAGE_AUTOGENMIPMAP. Since
// block compression covers every world albedo, normal and specular map in the game, that left
// the entire world sampling level 0 at any distance: undersampled, aliasing into the coloured
// speckle that no amount of sampler state could fix, because there was no second level to
// select. (D3DOK_NOAUTOGEN is a *success* code, so a SUCCEEDED() probe reports every format as
// supported -- see SupportsAutoMips.)
//
// So decode level 0, box-filter it down, and re-encode each level. The re-encode is a plain
// min/max endpoint fit rather than a rate-distortion search: these are the minified levels,
// each already an average of four texels, and level 0 is passed through untouched -- so the
// error lands where nothing can see it.
//=============================================================================

struct Rgba8 {
  uint8_t r, g, b, a;
};

const D3DFORMAT kFmtAti2 = D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '2'));

bool IsBlockCompressed(D3DFORMAT f) {
  return f == D3DFMT_DXT1 || f == D3DFMT_DXT3 || f == D3DFMT_DXT5 || f == kFmtAti2;
}

uint32_t BcBlockBytes(D3DFORMAT f) { return f == D3DFMT_DXT1 ? 8u : 16u; }

/// How many levels to build. The chain stops at 4x4 rather than 1x1: a BC level smaller than
/// one block is where the ATI2 pitch trap bites (the runtime sizes an unrecognised FourCC at a
/// byte per texel, so a 2x2 level gets 4 bytes to hold a 16-byte block, and writing it corrupts
/// the heap). Nothing is lost -- a 4x4 mip is already a single flat colour.
uint32_t BcMipLevels(uint32_t width, uint32_t height) {
  uint32_t levels = 1;
  uint32_t w = width, h = height;
  while (w > 4 && h > 4) {
    w /= 2;
    h /= 2;
    ++levels;
  }
  return levels;
}

/// The 4-colour palette of a BC1 colour block. `punchthrough` is only true for DXT1, where
/// c0 <= c1 selects a 3-colour block plus transparent black; the colour block embedded in
/// DXT3/DXT5 is always 4-colour.
void Bc1Palette(const uint8_t* blk, bool punchthrough, Rgba8 pal[4]) {
  const uint32_t c0 = uint32_t(blk[0]) | (uint32_t(blk[1]) << 8);
  const uint32_t c1 = uint32_t(blk[2]) | (uint32_t(blk[3]) << 8);
  auto expand = [](uint32_t c) {
    Rgba8 o;
    o.r = uint8_t((((c >> 11) & 0x1F) * 255 + 15) / 31);
    o.g = uint8_t((((c >> 5) & 0x3F) * 255 + 31) / 63);
    o.b = uint8_t(((c & 0x1F) * 255 + 15) / 31);
    o.a = 255;
    return o;
  };
  pal[0] = expand(c0);
  pal[1] = expand(c1);
  if (c0 > c1 || !punchthrough) {
    pal[2] = {uint8_t((2 * pal[0].r + pal[1].r) / 3), uint8_t((2 * pal[0].g + pal[1].g) / 3),
              uint8_t((2 * pal[0].b + pal[1].b) / 3), 255};
    pal[3] = {uint8_t((pal[0].r + 2 * pal[1].r) / 3), uint8_t((pal[0].g + 2 * pal[1].g) / 3),
              uint8_t((pal[0].b + 2 * pal[1].b) / 3), 255};
  } else {
    pal[2] = {uint8_t((pal[0].r + pal[1].r) / 2), uint8_t((pal[0].g + pal[1].g) / 2),
              uint8_t((pal[0].b + pal[1].b) / 2), 255};
    pal[3] = {0, 0, 0, 0};
  }
}

/// The 8-entry palette of a BC4 block (DXT5's alpha half, and each half of ATI2).
void Bc4Palette(const uint8_t* blk, uint8_t pal[8]) {
  const uint32_t a0 = blk[0], a1 = blk[1];
  pal[0] = uint8_t(a0);
  pal[1] = uint8_t(a1);
  if (a0 > a1) {
    for (uint32_t i = 1; i <= 6; ++i) pal[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (uint32_t i = 1; i <= 4; ++i) pal[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    pal[6] = 0;
    pal[7] = 255;
  }
}

void DecodeBc4Block(const uint8_t* blk, uint8_t out[16]) {
  uint8_t pal[8];
  Bc4Palette(blk, pal);
  uint64_t bits = 0;
  for (uint32_t i = 0; i < 6; ++i) bits |= uint64_t(blk[2 + i]) << (8 * i);
  for (uint32_t i = 0; i < 16; ++i) out[i] = pal[(bits >> (3 * i)) & 0x7];
}

/// Decode DXN (two BC4 planes, 16 bytes/block) to A8R8G8B8 with Xenos k_DXN fetch semantics:
/// hardware swizzle RGGG -- (x, y, y, y). Memory layout per texel is [B][G][R][A] = [Y][Y][X][Y].
void DecodeDXNToArgb(uint8_t* dst, uint32_t dst_pitch, const uint8_t* src, uint32_t blocks_wide,
                     uint32_t blocks_high, uint32_t width, uint32_t height) {
  for (uint32_t by = 0; by < blocks_high; ++by) {
    for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
      const uint8_t* blk = src + (size_t(by) * blocks_wide + bx) * 16;
      uint8_t xs[16], ys[16];
      DecodeBc4Block(blk, xs);
      DecodeBc4Block(blk + 8, ys);
      for (uint32_t py = 0; py < 4; ++py) {
        const uint32_t y = by * 4 + py;
        if (y >= height) break;
        uint8_t* row = dst + size_t(y) * dst_pitch;
        for (uint32_t px = 0; px < 4; ++px) {
          const uint32_t x = bx * 4 + px;
          if (x >= width) continue;
          uint8_t* p = row + size_t(x) * 4;
          const uint8_t xv = xs[py * 4 + px];
          const uint8_t yv = ys[py * 4 + px];
          p[0] = yv;
          p[1] = yv;
          p[2] = xv;
          p[3] = yv;
        }
      }
    }
  }
}

/// Decode one 4x4 block of any of the four BC formats into RGBA. ATI2 carries two channels
/// (X in red, Y in green); the encoder below only reads those back, so the rest is filler.
void DecodeBcBlock(D3DFORMAT fmt, const uint8_t* blk, Rgba8 out[16]) {
  if (fmt == kFmtAti2) {
    uint8_t x[16], y[16];
    DecodeBc4Block(blk, x);
    DecodeBc4Block(blk + 8, y);
    for (uint32_t i = 0; i < 16; ++i) out[i] = {x[i], y[i], 0, 255};
    return;
  }
  const uint8_t* color = fmt == D3DFMT_DXT1 ? blk : blk + 8;
  Rgba8 pal[4];
  Bc1Palette(color, /*punchthrough=*/fmt == D3DFMT_DXT1, pal);
  uint32_t bits = 0;
  for (uint32_t i = 0; i < 4; ++i) bits |= uint32_t(color[4 + i]) << (8 * i);
  for (uint32_t i = 0; i < 16; ++i) out[i] = pal[(bits >> (2 * i)) & 0x3];

  if (fmt == D3DFMT_DXT3) {
    for (uint32_t i = 0; i < 8; ++i) {
      out[i * 2 + 0].a = uint8_t((blk[i] & 0x0F) * 17);
      out[i * 2 + 1].a = uint8_t((blk[i] >> 4) * 17);
    }
  } else if (fmt == D3DFMT_DXT5) {
    uint8_t a[16];
    DecodeBc4Block(blk, a);
    for (uint32_t i = 0; i < 16; ++i) out[i].a = a[i];
  }
}

/// Decode a whole BC image into RGBA. `row_bytes` is the stride of a *block* row.
void DecodeBcImage(D3DFORMAT fmt, const uint8_t* src, uint32_t row_bytes, uint32_t width,
                   uint32_t height, std::vector<Rgba8>& out) {
  const uint32_t bpb = BcBlockBytes(fmt);
  const uint32_t bw = (width + 3) / 4;
  const uint32_t bh = (height + 3) / 4;
  out.assign(size_t(width) * height, Rgba8{0, 0, 0, 255});
  Rgba8 texels[16];
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      DecodeBcBlock(fmt, src + size_t(by) * row_bytes + size_t(bx) * bpb, texels);
      for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint32_t py = by * 4 + ty;
        if (py >= height) break;
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t px = bx * 4 + tx;
          if (px >= width) break;
          out[size_t(py) * width + px] = texels[ty * 4 + tx];
        }
      }
    }
  }
}

/// Decode a block-compressed COLOUR texture (BC1/2/3) into A8R8G8B8 while applying the fetch
/// constant's channel swizzle. A compressed host texture samples fixed RGBA, so it cannot honour a
/// swizzle that broadcasts one channel to RGB -- which is how the smoke/effect sprites are stored
/// (luminance in one channel, mask in another, e.g. swizzle 0x200 = R,G,B<-red, A<-green). Those
/// otherwise render as a single-channel, always-opaque square. `bc_fmt`/`bc_bytes` describe the
/// source blocks; component index 4 selects constant 0, 5 selects constant 1.
void DecodeBcColorSwizzledToArgb(uint8_t* dst, uint32_t dst_pitch, const uint8_t* src,
                                 uint32_t blocks_wide, uint32_t blocks_high, uint32_t width,
                                 uint32_t height, D3DFORMAT bc_fmt, uint32_t bc_bytes,
                                 uint32_t swizzle) {
  const uint8_t sel[4] = {uint8_t(swizzle & 7), uint8_t((swizzle >> 3) & 7),
                          uint8_t((swizzle >> 6) & 7), uint8_t((swizzle >> 9) & 7)};
  for (uint32_t by = 0; by < blocks_high; ++by) {
    for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
      Rgba8 tex[16];
      DecodeBcBlock(bc_fmt, src + (size_t(by) * blocks_wide + bx) * bc_bytes, tex);
      for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint32_t py = by * 4 + ty;
        if (py >= height) break;
        uint8_t* row = dst + size_t(py) * dst_pitch;
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t px = bx * 4 + tx;
          if (px >= width) break;
          const Rgba8& c = tex[ty * 4 + tx];
          // Indices 6 and 7 held 0 here, so GPUSWIZZLE_KEEP (7) forced the channel BLACK. KEEP is
          // documented "Fetch instructions only" and has no meaning in a fetch constant, so the
          // safe degradation is pass-through, not a dropped channel -- matching SwizzleSelect on
          // the 32-bit path.
          const uint8_t comp[8] = {c.r, c.g, c.b, c.a, 0, 255, c.r, c.r};
          uint8_t* p = row + size_t(px) * 4;
          p[0] = sel[2] > 5 ? c.b : comp[sel[2]];  // B <- swizzle.z
          p[1] = sel[1] > 5 ? c.g : comp[sel[1]];  // G <- swizzle.y
          p[2] = sel[0] > 5 ? c.r : comp[sel[0]];  // R <- swizzle.x
          p[3] = sel[3] > 5 ? c.a : comp[sel[3]];  // A <- swizzle.w
        }
      }
    }
  }
}

uint16_t Pack565(Rgba8 c) {
  return uint16_t(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
}

/// Fit a BC1 colour block: the endpoints are the extremes along the block's principal colour
/// axis (or its per-channel min and max with nx1_d3d9_bc_pca off -- see the fit below for what
/// that costs), and each texel takes the nearest of the four palette entries. Endpoints are forced into
/// c0 >= c1 so the block always decodes in 4-colour mode -- a 3-colour block would introduce
/// transparent texels that were never in the source.
/// `allow_punchthrough` must be true ONLY for real DXT1. BC1 encodes its 1-bit alpha in the
/// ORDERING of the endpoints -- c0 <= c1 selects the punch-through palette where index 3 is
/// transparent -- so a colour block that unconditionally sorts c0 >= c1 silently declares "fully
/// opaque" no matter what the source alpha said. That is what this function used to do, and it is
/// why generated mips turned punch-through surfaces (glass, fences, foliage) solid: level 0 is the
/// guest's own blocks and samples correctly, while every level we build below it lost the alpha.
/// DXT3/DXT5 carry alpha in a separate block and their colour block must stay in opaque mode, so
/// enabling punch-through there would corrupt them instead -- hence the flag rather than sniffing
/// the alpha channel alone.
void EncodeBc1Color(const Rgba8 in[16], uint8_t* dst, bool allow_punchthrough) {
  bool any_transparent = false;
  bool any_opaque = false;
  for (uint32_t i = 0; i < 16; ++i) {
    if (in[i].a < 128) {
      any_transparent = true;
    } else {
      any_opaque = true;
    }
  }
  const bool punchthrough = allow_punchthrough && any_transparent;

  // Fit the endpoints over the OPAQUE texels only. A transparent texel's colour is meaningless
  // (the decoder emits {0,0,0,0} for it), so letting it into the min/max drags both endpoints
  // toward black and greys out the texels that are actually visible.
  // Which texels get a vote. In punch-through mode a transparent texel's colour is meaningless
  // (the decoder emits {0,0,0,0} for it), so it must not drag the fit.
  const bool skip_transparent = punchthrough && any_opaque;
  Rgba8 lo{255, 255, 255, 255}, hi{0, 0, 0, 0};

  if (REXCVAR_GET(nx1_d3d9_bc_pca)) {
    // PRINCIPAL-AXIS FIT.
    //
    // BC1's four palette entries are colinear in RGB: they lie on the segment from c0 to c1. The
    // bounding-box fit below picks the corners of the block's axis-aligned box, and that diagonal
    // only coincides with the block's real colour axis when the channels vary together. Where they
    // are anticorrelated -- one channel rising as another falls -- the box diagonal points somewhere
    // the data never goes, every texel projects onto it badly, and the block comes back with its
    // contrast flattened and its HUE SHIFTED. That is a coloured error, and it lands only on the
    // levels we generate.
    //
    // So take the direction the colours actually vary in: the dominant eigenvector of the block's
    // covariance, by power iteration. Eight iterations of a 3x3 symmetric product is a few dozen
    // flops per block -- this runs on every block of every generated level of every texture, so a
    // real eigendecomposition is out of the question.
    float mean[3] = {0, 0, 0};
    uint32_t n = 0;
    for (uint32_t i = 0; i < 16; ++i) {
      if (skip_transparent && in[i].a < 128) continue;
      mean[0] += in[i].r; mean[1] += in[i].g; mean[2] += in[i].b;
      ++n;
    }
    if (n == 0) {  // every texel transparent: nothing to fit, any endpoints decode the same
      n = 1;
      mean[0] = in[0].r; mean[1] = in[0].g; mean[2] = in[0].b;
    }
    mean[0] /= float(n); mean[1] /= float(n); mean[2] /= float(n);

    // Upper triangle of the covariance; it is symmetric so six accumulators cover it.
    float crr = 0, crg = 0, crb = 0, cgg = 0, cgb = 0, cbb = 0;
    for (uint32_t i = 0; i < 16; ++i) {
      if (skip_transparent && in[i].a < 128) continue;
      const float dr = float(in[i].r) - mean[0];
      const float dg = float(in[i].g) - mean[1];
      const float db = float(in[i].b) - mean[2];
      crr += dr * dr; crg += dr * dg; crb += dr * db;
      cgg += dg * dg; cgb += dg * db; cbb += db * db;
    }

    // SEED WITH THE LARGEST COVARIANCE ROW, not a per-channel guess. A row of a symmetric matrix
    // cannot be orthogonal to its dominant eigenvector unless the matrix is degenerate (the flat
    // block, handled by the length test below), so this always has something to converge from.
    // Measured over 6000 synthetic blocks against an exact eigendecomposition: seeding from the
    // largest-variance CHANNEL instead left 210/6000 blocks fitting more than 1 unit worse (worst
    // 15.96), because that seed can sit nearly orthogonal to the true axis and eight iterations
    // cannot recover. Row seeding: 88/6000, worst 8.06, and a mean gap of -0.003 -- i.e. no worse
    // than the exact solve on average. Sixteen iterations only reaches 51/6000, which is not worth
    // double the cost; the residue is blocks whose top two eigenvalues are nearly equal, where the
    // axis is genuinely ambiguous and either choice fits about as well.
    const float row_r = crr * crr + crg * crg + crb * crb;
    const float row_g = crg * crg + cgg * cgg + cgb * cgb;
    const float row_b = crb * crb + cgb * cgb + cbb * cbb;
    float ax, ay, az;
    if (row_r >= row_g && row_r >= row_b) {
      ax = crr; ay = crg; az = crb;
    } else if (row_g >= row_b) {
      ax = crg; ay = cgg; az = cgb;
    } else {
      ax = crb; ay = cgb; az = cbb;
    }
    if (const float seed_len = std::sqrt(ax * ax + ay * ay + az * az); seed_len > 1e-6f) {
      ax /= seed_len; ay /= seed_len; az /= seed_len;
    } else {
      ax = 1.0f; ay = 0.0f; az = 0.0f;  // degenerate: flat block, endpoints collapse to the mean
    }
    for (uint32_t it = 0; it < 8; ++it) {
      const float nx = crr * ax + crg * ay + crb * az;
      const float ny = crg * ax + cgg * ay + cgb * az;
      const float nz = crb * ax + cgb * ay + cbb * az;
      const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
      if (len < 1e-6f) {
        break;  // flat (or near-flat) block: the axis is arbitrary, and lo == hi == mean is exact
      }
      ax = nx / len; ay = ny / len; az = nz / len;
    }

    // Project onto the axis and take the extremes as the endpoints.
    float tmin = 3.4e38f, tmax = -3.4e38f;
    for (uint32_t i = 0; i < 16; ++i) {
      if (skip_transparent && in[i].a < 128) continue;
      const float t = (float(in[i].r) - mean[0]) * ax + (float(in[i].g) - mean[1]) * ay +
                      (float(in[i].b) - mean[2]) * az;
      tmin = std::min(tmin, t);
      tmax = std::max(tmax, t);
    }
    if (tmin > tmax) {  // no participating texels (guarded above, but keep the fit total)
      tmin = tmax = 0.0f;
    }
    const auto at = [&](float t) {
      const auto ch = [](float v) {
        return uint8_t(std::lround(std::clamp(v, 0.0f, 255.0f)));
      };
      return Rgba8{ch(mean[0] + ax * t), ch(mean[1] + ay * t), ch(mean[2] + az * t), 255};
    };
    lo = at(tmin);
    hi = at(tmax);
  } else if (skip_transparent) {
    for (uint32_t i = 0; i < 16; ++i) {
      if (in[i].a < 128) continue;
      lo.r = std::min(lo.r, in[i].r); hi.r = std::max(hi.r, in[i].r);
      lo.g = std::min(lo.g, in[i].g); hi.g = std::max(hi.g, in[i].g);
      lo.b = std::min(lo.b, in[i].b); hi.b = std::max(hi.b, in[i].b);
    }
  } else {
    lo = in[0];
    hi = in[0];
    for (uint32_t i = 1; i < 16; ++i) {
      lo.r = std::min(lo.r, in[i].r); hi.r = std::max(hi.r, in[i].r);
      lo.g = std::min(lo.g, in[i].g); hi.g = std::max(hi.g, in[i].g);
      lo.b = std::min(lo.b, in[i].b); hi.b = std::max(hi.b, in[i].b);
    }
  }
  uint16_t c0 = Pack565(hi);
  uint16_t c1 = Pack565(lo);
  if (punchthrough) {
    // c0 <= c1 selects the punch-through palette. Equal endpoints are fine and stay in it.
    if (c0 > c1) std::swap(c0, c1);
  } else if (c0 < c1) {
    std::swap(c0, c1);
  }
  dst[0] = uint8_t(c0 & 0xFF);
  dst[1] = uint8_t(c0 >> 8);
  dst[2] = uint8_t(c1 & 0xFF);
  dst[3] = uint8_t(c1 >> 8);

  Rgba8 pal[4];
  Bc1Palette(dst, punchthrough, pal);
  // In punch-through mode index 3 IS the transparent slot, so opaque texels may only choose from
  // 0..2; letting them pick 3 would punch holes in solid pixels.
  const uint32_t usable = punchthrough ? 3u : 4u;
  uint32_t bits = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    if (punchthrough && in[i].a < 128) {
      bits |= 3u << (2 * i);
      continue;
    }
    uint32_t best = 0, best_err = ~0u;
    for (uint32_t p = 0; p < usable; ++p) {
      const int dr = int(in[i].r) - int(pal[p].r);
      const int dg = int(in[i].g) - int(pal[p].g);
      const int db = int(in[i].b) - int(pal[p].b);
      const uint32_t err = uint32_t(dr * dr + dg * dg + db * db);
      if (err < best_err) {
        best_err = err;
        best = p;
      }
    }
    bits |= best << (2 * i);
  }
  for (uint32_t i = 0; i < 4; ++i) dst[4 + i] = uint8_t((bits >> (8 * i)) & 0xFF);
}

/// Fit a BC4 block over one channel, selected by `channel` (0 = R, 1 = G, 3 = A).
void EncodeBc4(const Rgba8 in[16], uint32_t channel, uint8_t* dst) {
  uint8_t v[16];
  for (uint32_t i = 0; i < 16; ++i) {
    const uint8_t* c = &in[i].r;
    v[i] = c[channel];
  }
  uint8_t lo = v[0], hi = v[0];
  for (uint32_t i = 1; i < 16; ++i) {
    lo = std::min(lo, v[i]);
    hi = std::max(hi, v[i]);
  }
  dst[0] = hi;  // a0 > a1 selects the 8-value interpolated mode
  dst[1] = lo;
  uint8_t pal[8];
  Bc4Palette(dst, pal);
  uint64_t bits = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    uint32_t best = 0, best_err = ~0u;
    for (uint32_t p = 0; p < 8; ++p) {
      const int d = int(v[i]) - int(pal[p]);
      const uint32_t err = uint32_t(d * d);
      if (err < best_err) {
        best_err = err;
        best = p;
      }
    }
    bits |= uint64_t(best) << (3 * i);
  }
  for (uint32_t i = 0; i < 6; ++i) dst[2 + i] = uint8_t((bits >> (8 * i)) & 0xFF);
}

void EncodeBcBlock(D3DFORMAT fmt, const Rgba8 in[16], uint8_t* dst) {
  if (fmt == kFmtAti2) {
    EncodeBc4(in, 0, dst);      // X
    EncodeBc4(in, 1, dst + 8);  // Y
    return;
  }
  if (fmt == D3DFMT_DXT3) {
    for (uint32_t i = 0; i < 8; ++i) {
      dst[i] = uint8_t((in[i * 2 + 0].a >> 4) | (in[i * 2 + 1].a & 0xF0));
    }
  } else if (fmt == D3DFMT_DXT5) {
    EncodeBc4(in, 3, dst);
  }
  EncodeBc1Color(in, fmt == D3DFMT_DXT1 ? dst : dst + 8,
                 /*allow_punchthrough=*/fmt == D3DFMT_DXT1);
}

/// Encode an RGBA image back to BC. Texels past the edge of a partial block repeat the last
/// real one, so the fit is never dragged towards whatever the padding held.
void EncodeBcImage(D3DFORMAT fmt, const std::vector<Rgba8>& src, uint32_t width, uint32_t height,
                   uint8_t* dst, uint32_t row_bytes) {
  const uint32_t bpb = BcBlockBytes(fmt);
  const uint32_t bw = (width + 3) / 4;
  const uint32_t bh = (height + 3) / 4;
  Rgba8 texels[16];
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint32_t py = std::min(by * 4 + ty, height - 1);
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t px = std::min(bx * 4 + tx, width - 1);
          texels[ty * 4 + tx] = src[size_t(py) * width + px];
        }
      }
      EncodeBcBlock(fmt, texels, dst + size_t(by) * row_bytes + size_t(bx) * bpb);
    }
  }
}

/// Halve an RGBA image with a 2x2 box filter (odd dimensions repeat the last row/column).
/// Write an Rgba8 image as a 32-bit BMP. Debug-only, so it favours being obviously correct
/// over being fast: bottom-up rows, BGRA order, no compression.
/// Key for the decode-history diagnostics (DECODECHANGE / DECODEHIST): the address PLUS every
/// fetch-constant field that changes the decoded bytes. Keyed on address alone, the same
/// memory bound through two different VIEWS (a colormap and a broadcast-swizzle mask, or two
/// samplers with different swizzles) legitimately decodes to different host bytes -- and the
/// stamp ping-ponged between them, reporting "content changed with zero guest writes". That
/// produced 1359 phantom changes in one run and sent a whole evening chasing writes invisible
/// to page protection. The view is part of the identity, not noise.
uint64_t DecodeViewKey(const TextureFetchConstant& t) {
  uint64_t v = t.base_address;
  v = v * 0x9E3779B97F4A7C15ull ^ ((uint64_t(t.width) << 32) | t.height);
  v = v * 0x9E3779B97F4A7C15ull ^ ((uint64_t(t.format) << 32) | t.swizzle);
  v = v * 0x9E3779B97F4A7C15ull ^ ((uint64_t(t.endian) << 8) | (t.tiled ? 1u : 0u));
  v = v * 0x9E3779B97F4A7C15ull ^ t.pitch_pixels;
  return v;
}

void DumpRgbaBmp(const char* path, const std::vector<Rgba8>& px, uint32_t w, uint32_t h) {
  if (!w || !h || px.size() < size_t(w) * h) {
    return;
  }
  FILE* f = std::fopen(path, "wb");
  if (!f) {
    return;
  }
  const uint32_t data_bytes = w * h * 4;
  const uint32_t file_bytes = 54 + data_bytes;
  uint8_t hdr[54] = {};
  hdr[0] = 'B'; hdr[1] = 'M';
  std::memcpy(hdr + 2, &file_bytes, 4);
  const uint32_t pixel_offset = 54;
  std::memcpy(hdr + 10, &pixel_offset, 4);
  const uint32_t dib = 40;
  std::memcpy(hdr + 14, &dib, 4);
  std::memcpy(hdr + 18, &w, 4);
  std::memcpy(hdr + 22, &h, 4);
  const uint16_t planes = 1, bpp = 32;
  std::memcpy(hdr + 26, &planes, 2);
  std::memcpy(hdr + 28, &bpp, 2);
  std::memcpy(hdr + 34, &data_bytes, 4);
  std::fwrite(hdr, 1, sizeof(hdr), f);
  std::vector<uint8_t> row(size_t(w) * 4);
  for (uint32_t y = 0; y < h; ++y) {
    const Rgba8* srow = &px[size_t(h - 1 - y) * w];
    for (uint32_t x = 0; x < w; ++x) {
      row[x * 4 + 0] = srow[x].b;
      row[x * 4 + 1] = srow[x].g;
      row[x * 4 + 2] = srow[x].r;
      row[x * 4 + 3] = srow[x].a;
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
}

void BoxFilterHalf(const std::vector<Rgba8>& src, uint32_t sw, uint32_t sh,
                   std::vector<Rgba8>& dst, uint32_t dw, uint32_t dh) {
  dst.resize(size_t(dw) * dh);
  for (uint32_t y = 0; y < dh; ++y) {
    const uint32_t y0 = std::min(y * 2, sh - 1);
    const uint32_t y1 = std::min(y * 2 + 1, sh - 1);
    for (uint32_t x = 0; x < dw; ++x) {
      const uint32_t x0 = std::min(x * 2, sw - 1);
      const uint32_t x1 = std::min(x * 2 + 1, sw - 1);
      const Rgba8& a = src[size_t(y0) * sw + x0];
      const Rgba8& b = src[size_t(y0) * sw + x1];
      const Rgba8& c = src[size_t(y1) * sw + x0];
      const Rgba8& d = src[size_t(y1) * sw + x1];
      dst[size_t(y) * dw + x] = {
          uint8_t((uint32_t(a.r) + b.r + c.r + d.r + 2) / 4),
          uint8_t((uint32_t(a.g) + b.g + c.g + d.g + 2) / 4),
          uint8_t((uint32_t(a.b) + b.b + c.b + d.b + 2) / 4),
          uint8_t((uint32_t(a.a) + b.a + c.a + d.a + 2) / 4),
      };
    }
  }
}

/// The per-texel byte permutation a fetch constant's channel swizzle implies, for a
/// 32-bit format landing in D3DFMT_A8R8G8B8.
///
/// After the endian swap a Xenos texel's bytes are its components in order: [X][Y][Z][W].
/// D3DFMT_A8R8G8B8 reads that same memory as [B][G][R][A]. The swizzle says where each
/// output channel comes from -- R = comp[swz.x], G = comp[swz.y], B = comp[swz.z],
/// A = comp[swz.w] -- so the destination byte for R (index 2) takes source byte swz.x,
/// and so on. Component values 4 and 5 are the constants 0 and 1.
///
/// This is *not* a fixed red/blue swap. NX1's textures are overwhelmingly swizzle 0x60A
/// (R<-Z, G<-Y, B<-X), which is already exactly D3D's byte order and needs no work at
/// all; the identity swizzle 0x688 is the one that needs the swap. Applying a blanket
/// R<->B swap to every 8888 texture inverted red and blue on all of them -- including the
/// colour-grading LUT the composite runs the whole frame through, which is what turned
/// the sky orange.
struct Swizzle32 {
  uint8_t src[4];  ///< per destination byte (B,G,R,A): source byte 0-3, or 4=zero, 5=one
  bool identity;
};

Swizzle32 MakeSwizzle32(uint32_t swizzle) {
  Swizzle32 s{};
  const uint8_t comp[4] = {
      uint8_t(swizzle & 0x7),         // -> R (dst byte 2)
      uint8_t((swizzle >> 3) & 0x7),  // -> G (dst byte 1)
      uint8_t((swizzle >> 6) & 0x7),  // -> B (dst byte 0)
      uint8_t((swizzle >> 9) & 0x7),  // -> A (dst byte 3)
  };
  s.src[2] = comp[0];
  s.src[1] = comp[1];
  s.src[0] = comp[2];
  s.src[3] = comp[3];
  s.identity = s.src[0] == 0 && s.src[1] == 1 && s.src[2] == 2 && s.src[3] == 3;
  return s;
}

/// Apply `s` in place over a run of 32-bit texels.
///
/// Selector handling, per the XDK's GPUSWIZZLE: 0-3 pick a component, 4 is constant 0, 5 is
/// constant 1. 7 is KEEP, which the header marks "Fetch instructions only" -- it means "leave this
/// channel to the fetch INSTRUCTION", so it has no defined meaning in a fetch constant. It used to
/// fall into the same bucket as constant-0 and force the channel to BLACK, which is the worst
/// available guess: pass the component through instead, so an unexpected value degrades to
/// identity rather than to a missing channel.
inline uint8_t SwizzleSelect(uint8_t sel, const uint8_t in[4], uint32_t dst_index) {
  if (sel < 4) {
    return in[sel];
  }
  if (sel == 4) {
    return 0x00;  // GPUSWIZZLE_0
  }
  if (sel == 5) {
    return 0xFF;  // GPUSWIZZLE_1
  }
  return in[dst_index < 4 ? dst_index : 0];  // KEEP / reserved -> pass through
}

void SwizzleRow32(uint8_t* row, uint32_t texels, const Swizzle32& s) {
  for (uint32_t i = 0; i < texels; ++i) {
    uint8_t* p = row + size_t(i) * 4;
    const uint8_t in[4] = {p[0], p[1], p[2], p[3]};
    for (uint32_t d = 0; d < 4; ++d) {
      p[d] = SwizzleSelect(s.src[d], in, d);
    }
  }
}

}  // namespace

// Declared in d3d9_resources.h. Defined here -- OUTSIDE the anonymous namespace above, so it has
// external linkage, but after it, so PhysicalAddress and TranslatePhysical are in scope.
uint64_t ResourceTracker::MirrorFingerprint(uint32_t base_address, uint32_t bytes) {
  const uint32_t phys = PhysicalAddress(base_address);
  constexpr uint64_t kPhysWindowBytes = 0x20000000ull;
  if (!phys || uint64_t(phys) + bytes > kPhysWindowBytes) {
    return 0;
  }
  // Through MirrorSnapshot on purpose: this must see exactly what the decode sees.
  const uint8_t* p = MirrorSnapshot(phys, bytes);
  if (!p) {
    return 0;
  }
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint32_t i = 0; i < bytes; ++i) {
    h = (h ^ p[i]) * 0x100000001B3ull;
  }
  return h ? h : 1;
}

void ResourceTracker::PrefetchTextureAtDraw(const TextureFetchConstant& t) {
  if (!t.valid || !t.base_address || !REXCVAR_GET(nx1_d3d9_texture_mirror)) {
    return;
  }
  const auto* fmt = rex::graphics::FormatInfo::Get(t.format);
  if (!fmt) {
    return;
  }
  // Same span the decode itself computes (see GetTexture), so the pages captured here are exactly
  // the pages it will read. Deriving it any other way would leave part of the texture unmirrored
  // and the window open for that part.
  const uint32_t pitch_pixels = t.pitch_pixels ? t.pitch_pixels : t.width;
  const rex::graphics::TextureExtent extent = rex::graphics::TextureExtent::Calculate(
      fmt, pitch_pixels, t.height, /*depth=*/1, t.tiled, /*is_guest=*/true);
  const uint32_t bpb = fmt->bytes_per_block();
  const size_t bytes =
      t.tiled ? TiledSurfaceExtentBytes(extent.block_pitch_h, extent.block_pitch_v, bpb)
              : size_t(extent.block_pitch_h) * extent.block_pitch_v * bpb;
  if (!bytes || bytes > (1u << 26)) {  // 64 MB sanity bound; an absurd span means a bad descriptor
    return;
  }
  MirrorSnapshot(t.base_address, uint32_t(bytes));
  ++prefetch_draws_;
}

uint64_t GuestTextureFingerprint(uint32_t base_address, uint32_t bytes) {
  const uint32_t phys = PhysicalAddress(base_address);
  // Same 512 MB physical window ResourceTracker::kMirrorPages covers, stated locally rather than
  // widening that class's access surface for a diagnostic. An unbounded read here crashed the game
  // once already (muzzle-flash sprites decode on the spot).
  constexpr uint64_t kPhysWindowBytes = 0x20000000ull;
  if (!phys || uint64_t(phys) + bytes > kPhysWindowBytes) {
    return 0;
  }
  const uint8_t* p = TranslatePhysical(phys);
  if (!p) {
    return 0;
  }
  // Deliberately LIVE memory, not MirrorSnapshot: the question is whether the guest's bytes changed
  // between the draw and the decode, and the mirror would serve a cached page and hide exactly that.
  // Cheap FNV over a short prefix -- this runs per draw on the guest thread, which PROF/bound shows
  // is the longer pole, so it has to stay small.
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint32_t i = 0; i < bytes; ++i) {
    h = (h ^ p[i]) * 0x100000001B3ull;
  }
  return h ? h : 1;  // 0 is reserved for "unreadable"
}

//=============================================================================

D3DPRIMITIVETYPE HostPrimitiveType(uint32_t xenos_primitive_type) {
  switch (xenos_primitive_type) {
    case 1: return D3DPT_POINTLIST;
    case 2: return D3DPT_LINELIST;
    case 3: return D3DPT_LINESTRIP;
    case 4: return D3DPT_TRIANGLELIST;
    case 5: return D3DPT_TRIANGLEFAN;
    case 6: return D3DPT_TRIANGLESTRIP;
    default: return D3DPRIMITIVETYPE(0);
  }
}

uint32_t HostPrimitiveCount(uint32_t xenos_primitive_type, uint32_t index_count) {
  switch (xenos_primitive_type) {
    case 1: return index_count;
    case 2: return index_count / 2;
    case 3: return index_count ? index_count - 1 : 0;
    case 4: return index_count / 3;
    case 5:
    case 6: return index_count >= 3 ? index_count - 2 : 0;
    default: return 0;
  }
}

//=============================================================================

ResourceTracker& ResourceTracker::Get() {
  static ResourceTracker instance;
  return instance;
}

void ResourceTracker::Initialize(IDirect3DDevice9Ex* device) {
  Shutdown();
  device_ = device;
  layouts_ = new LayoutMap();
  vertex_buffers_ = new VertexBufferMap();
  vertex_hashes_ = new VertexHashMap();
  index_buffers_ = new IndexBufferMap();
  index_ranges_ = new IndexRangeMap();
  textures_ = new TextureMap();
  best_textures_ = new BestTextureMap();
  resolves_ = new ResolveMap();
  render_targets_ = new TargetMap();

  // INTZ is what makes depth sampleable on D3D9. Without it the shadow maps and the
  // scene depth can still be *rendered*, but never read back, and the lighting that
  // depends on them stays wrong.
  intz_supported_ = false;
  IDirect3D9* d3d = nullptr;
  if (SUCCEEDED(device_->GetDirect3D(&d3d)) && d3d) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    D3DDISPLAYMODE mode{};
    if (SUCCEEDED(device_->GetCreationParameters(&cp)) &&
        SUCCEEDED(d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode))) {
      intz_supported_ =
          SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                           D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, D3DFMT_INTZ));
    }
    d3d->Release();
  }
  REXGPU_INFO("nx1_d3d9: INTZ sampleable depth {}", intz_supported_ ? "supported" : "UNAVAILABLE");

  // The neutral stand-in for textures we cannot produce yet (see white_).
  IDirect3DTexture9* staging = nullptr;
  if (SUCCEEDED(device_->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging,
                                       nullptr))) {
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(staging->LockRect(0, &locked, nullptr, 0))) {
      *static_cast<uint32_t*>(locked.pBits) = 0xFFFFFFFFu;
      staging->UnlockRect(0);
      if (SUCCEEDED(device_->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &white_,
                                           nullptr))) {
        device_->UpdateTexture(staging, white_);
      }
    }
    staging->Release();
  }
  if (!white_) {
    REXGPU_WARN("nx1_d3d9: could not create the white fallback texture");
  }

  // Register the physical-memory write-watch. It invalidates mirror pages (below) only when the
  // guest rewrites their bytes -- the streaming pool leaves a non-resident texture's memory as
  // transient garbage, so we snapshot-and-hold rather than re-read per draw. Mirrors the reference.
  if (auto* mem = rex::system::kernel_state()->memory(); mem && !mem_watch_handle_) {
    mem_watch_handle_ = mem->RegisterPhysicalMemoryInvalidationCallback(&MemWatchThunk, this);
  }
  // Allocate the CPU snapshot mirror of guest physical memory (lazily committed) and record the
  // host pointer for guest physical 0, so MirrorSnapshot can copy pages out of live RAM.
  phys_base_ = TranslatePhysical(0);
  if (!mirror_) {
    mirror_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(kMirrorPages) << 12,
                                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    mirror_valid_.assign((kMirrorPages + 63) / 64, 0);
    mirror_sweep_page_ = 0;
  }
  // Register the GPU-write watch. The physical-memory callback above traps CPU stores; this one
  // is the only way we hear about memexport and resolve writes, which is how textures actually
  // arrive. Registering is deliberately unconditional -- the cvar is checked in the callback --
  // so it can be toggled at runtime for an A/B without a relaunch.
  //
  // May legitimately fail here: the reference's SharedMemory is created in SetupContext, and the
  // command processor may not have reached it yet. RegisterGpuWriteWatch is therefore idempotent
  // and retried from AdvanceFrame until it takes.
  RegisterGpuWriteWatch();
  REXGPU_INFO("nx1_d3d9: texture write-watch {}, GPU-write watch {}, mirror {}",
              mem_watch_handle_ ? "registered" : "FAILED",
              gpu_watch_handle_ ? "registered" : "deferred (reference not up yet)",
              (mirror_ && phys_base_) ? "allocated" : "FAILED");
}

void ResourceTracker::RegisterGpuWriteWatch() {
  if (gpu_watch_handle_) {
    return;
  }
  auto* gs = rex::graphics::GraphicsSystem::Nx1Current();
  auto* cp = gs ? gs->command_processor() : nullptr;
  auto* sm = cp ? cp->Nx1SharedMemory() : nullptr;
  if (!sm) {
    return;
  }
  gpu_watch_handle_ = sm->RegisterGlobalWatch(&GpuWriteWatchThunk, this);
  REXGPU_INFO("nx1_d3d9: GPU-write watch registered on the reference SharedMemory -- texture "
              "invalidation now covers memexport/resolve writes, which host page protection "
              "cannot trap");
}

/// Fires on the reference's GPU worker thread, INSIDE SharedMemory's global critical region.
///
/// Same discipline as MemWatchThunk and for the same reason: it may only queue. Touching the
/// texture map here would iterate an unordered_map while the render thread inserts into it, which
/// is exactly the UB that killed the game inside ImageCache_DmaCopyDelayed once the DMA call rate
/// rose. InvalidateGuestRange takes dirty_mu_ and appends; DrainMemoryWrites applies it on the
/// render thread at the frame boundary.
///
/// Lock order is safe: we take dirty_mu_ while holding the reference's global lock, and nothing
/// on our side ever calls into SharedMemory while holding dirty_mu_.
void ResourceTracker::GpuWriteWatchThunk(const std::unique_lock<std::recursive_mutex>& global_lock,
                                         void* ctx, uint32_t address_first, uint32_t address_last,
                                         bool invalidated_by_gpu) {
  (void)global_lock;
  // CPU-side invalidations arrive here too, but we already have those from our own physical-memory
  // watch, which reports them with tighter ranges. Taking both would double the queue for no new
  // information -- the whole point of this subscription is the GPU writes we CANNOT otherwise see.
  if (!invalidated_by_gpu) {
    return;
  }
  if (auto* self = static_cast<ResourceTracker*>(ctx)) {
    self->OnGpuWrite(address_first, address_last);
  }
}

void ResourceTracker::OnGpuWrite(uint32_t address_first, uint32_t address_last) {
  if (!REXCVAR_GET(nx1_d3d9_gpu_write_watch) || address_last < address_first) {
    return;
  }
  const uint64_t len = uint64_t(address_last) - address_first + 1;
  // The whole-buffer blast (destination unknown) is not information we can act on -- see the
  // cvar's comment. Counted separately so a run where blasts dominate is visible as such rather
  // than looking like the watch simply found nothing.
  if (const uint64_t max_mb = REXCVAR_GET(nx1_d3d9_gpu_write_watch_max_mb);
      max_mb && len > (max_mb << 20)) {
    if (len >= (uint64_t(kMirrorPages) << 12)) {
      ++gpu_write_blasts_;
    } else {
      ++gpu_write_dropped_;
    }
    return;
  }
  if (uint64_t(address_first) + len > (uint64_t(kMirrorPages) << 12)) {
    return;  // outside the window every other reader in this file respects
  }
  // WHERE DO THEY ACTUALLY POINT? DRAINHIT says GPU-write ranges reach the drain (55,629) but
  // match a texture's watch range 2 times. That is either an address-space mismatch or these
  // writes genuinely targeting non-texture memory (memexport mostly writes vertex/stream-out
  // buffers). Print a few real ranges beside a few live texture ranges; the two are directly
  // comparable and settle it without further inference.
  {
    static std::atomic<uint32_t> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 8) {
      REXGPU_WARN("nx1_d3d9: GPUWRITE-RANGE {:08X}..{:08X} ({} KB)", address_first, address_last,
                  (address_last - address_first + 1) / 1024);
    }
  }
  ++gpu_write_ranges_;
  gpu_write_pages_ += (address_last >> 12) - (address_first >> 12) + 1;
  InvalidateGuestRange(address_first, uint32_t(len), DirtySource::kGpuWrite);
}

void ResourceTracker::Shutdown() {
  if (mem_watch_handle_) {
    if (auto* mem = rex::system::kernel_state()->memory()) {
      mem->UnregisterPhysicalMemoryInvalidationCallback(mem_watch_handle_);
    }
    mem_watch_handle_ = nullptr;
  }
  // Before writes_pending_ is cleared below, and while the reference is still alive: the watch
  // list holds a raw `this`, so leaving it registered past our destruction is a use-after-free on
  // the next memexport draw.
  if (gpu_watch_handle_) {
    auto* gs = rex::graphics::GraphicsSystem::Nx1Current();
    auto* cp = gs ? gs->command_processor() : nullptr;
    if (auto* sm = cp ? cp->Nx1SharedMemory() : nullptr) {
      sm->UnregisterGlobalWatch(gpu_watch_handle_);
    }
    gpu_watch_handle_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(dirty_mu_);
    writes_pending_.clear();
  }
  if (writeback_staging_) {
    writeback_staging_->Release();
    writeback_staging_ = nullptr;
    writeback_staging_w_ = writeback_staging_h_ = 0;
  }
  writeback_counts_.clear();
  if (mirror_) {
    VirtualFree(mirror_, 0, MEM_RELEASE);
    mirror_ = nullptr;
  }
  mirror_valid_.clear();
  mirror_sweep_page_ = 0;
  phys_base_ = nullptr;
  if (depth_blit_ps_) {
    depth_blit_ps_->Release();
    depth_blit_ps_ = nullptr;
  }
  if (white_) {
    white_->Release();
    white_ = nullptr;
  }
  if (layouts_) {
    auto* map = static_cast<LayoutMap*>(layouts_);
    for (const auto& [key, layout] : *map) {
      if (layout.decl) layout.decl->Release();
    }
    delete map;
    layouts_ = nullptr;
    last_layout_ = nullptr;  // memo points into the map just deleted
    last_layout_key_ = 0;
  }
  if (vertex_buffers_) {
    auto* map = static_cast<VertexBufferMap*>(vertex_buffers_);
    for (auto [key, entry] : *map) {
      if (entry.vb) entry.vb->Release();
    }
    delete map;
    vertex_buffers_ = nullptr;
  }
  if (vertex_hashes_) {
    delete static_cast<VertexHashMap*>(vertex_hashes_);
    vertex_hashes_ = nullptr;
  }
  if (index_ranges_) {
    delete static_cast<IndexRangeMap*>(index_ranges_);
    index_ranges_ = nullptr;
  }
  if (index_buffers_) {
    auto* map = static_cast<IndexBufferMap*>(index_buffers_);
    for (auto [key, entry] : *map) {
      if (entry.ib) entry.ib->Release();
    }
    delete map;
    index_buffers_ = nullptr;
  }
  if (textures_) {
    auto* map = static_cast<TextureMap*>(textures_);
    for (auto [key, entry] : *map) {
      if (entry.tex) entry.tex->Release();
      if (entry.vol) entry.vol->Release();
      if (entry.cube) entry.cube->Release();
    }
    delete map;
    textures_ = nullptr;
  }
  if (best_textures_) {
    auto* map = static_cast<BestTextureMap*>(best_textures_);
    for (auto [key, b] : *map) {
      if (b.tex) b.tex->Release();
    }
    delete map;
    best_textures_ = nullptr;
  }
  if (resolves_) {
    auto* map = static_cast<ResolveMap*>(resolves_);
    for (auto [key, entry] : *map) {
      // Non-owned entries alias a depth target released with render_targets_ below.
      if (entry.tex && entry.owned) entry.tex->Release();
    }
    delete map;
    resolves_ = nullptr;
    resolve_flat_count_ = 0;
    resolve_flat_valid_ = false;
  }
  if (render_targets_) {
    auto* map = static_cast<TargetMap*>(render_targets_);
    for (const auto& [key, t] : *map) {
      if (t.surface && !t.is_backbuffer) t.surface->Release();
      if (t.tex) t.tex->Release();
    }
    delete map;
    render_targets_ = nullptr;
  }
  backbuffer_ = nullptr;
  device_ = nullptr;
}

const VertexLayout* ResourceTracker::GetVertexLayout(const uint8_t* base, uint32_t decl_object,
                                                     const uint32_t* stream_stride) {
  if (!device_ || !layouts_) {
    return nullptr;
  }
  if (!decl_object) {
    return nullptr;
  }

  // The declaration alone does not determine the host layout: SetStreamSource
  // supplies the strides, and two draws can share a declaration across streams
  // of different pitch.
  uint64_t key = MixKey(decl_object, VertexDeclarationUniqueness(base, decl_object));
  for (uint32_t s = 0; s < kMaxHostStreams; ++s) {
    key = MixKey(key, stream_stride[s]);
  }

  // Last-result memo. This runs once per draw (~5000 a frame) and consecutive draws
  // overwhelmingly share a layout, so the repeat case skips the map probe entirely. The
  // cached pointer stays valid because LayoutMap is a node-based std::unordered_map: entries
  // are never moved by later insertions, and nothing erases from it outside Shutdown. That is
  // load-bearing -- converting it to FlatMap would leave this dangling, as would an eviction
  // sweep, and the caller holds the returned pointer across its whole draw setup.
  if (key == last_layout_key_ && last_layout_) {
    return last_layout_->decl ? last_layout_ : nullptr;
  }

  auto* map = static_cast<LayoutMap*>(layouts_);
  if (auto it = map->find(key); it != map->end()) {
    last_layout_key_ = key;
    last_layout_ = &it->second;
    return it->second.decl ? &it->second : nullptr;
  }

  VertexLayout layout;
  layout.key = key;
  for (uint32_t s = 0; s < kMaxHostStreams; ++s) {
    layout.guest_stride[s] = stream_stride[s];
    layout.bulk_swap[s] = true;
  }

  const uint32_t count = VertexDeclarationCount(base, decl_object);
  D3DVERTEXELEMENT9 host_elements[MAXD3DDECLLENGTH + 1] = {};
  uint32_t host_count = 0;
  bool ok = count > 0 && count <= MAXD3DDECLLENGTH;

  for (uint32_t i = 0; ok && i < count; ++i) {
    const GuestVertexElement e = ReadVertexElement(base, decl_object, i);
    if (e.stream >= kMaxHostStreams) {
      REXGPU_WARN("nx1_d3d9: vertex element on stream {} (only {} supported)", e.stream,
                  kMaxHostStreams);
      ok = false;
      break;
    }
    HostFormat host_format;
    if (!PickHostFormat(e.format, e.is_signed, e.is_normalized, e.swizzle, &host_format)) {
      if ((unsupported_formats_++ % 100) == 0) {
        REXGPU_WARN("nx1_d3d9: unsupported Xenos vertex format {} (signed={}, normalized={})",
                    e.format, e.is_signed, e.is_normalized);
      }
      ok = false;
      break;
    }

    ConvertOp op{};
    op.src_offset = uint16_t(e.offset);
    op.dst_offset = uint16_t(layout.host_stride[e.stream]);
    op.stream = uint8_t(e.stream);
    op.format = uint8_t(e.format);
    op.src_size = host_format.src_size;
    op.dst_size = host_format.dst_size;
    op.is_signed = e.is_signed;
    op.is_normalized = e.is_normalized;
    op.expand = host_format.expand;
    layout.host_stride[e.stream] += host_format.dst_size;
    if (op.expand || op.dst_offset != op.src_offset) {
      layout.bulk_swap[e.stream] = false;
    }
    layout.ops.push_back(op);

    host_elements[host_count++] = D3DVERTEXELEMENT9{
        WORD(e.stream),           WORD(op.dst_offset), BYTE(host_format.type),
        D3DDECLMETHOD_DEFAULT, BYTE(e.usage),       BYTE(e.usage_index),
    };
    layout.stream_count = std::max(layout.stream_count, e.stream + 1);
  }

  if (ok) {
    for (uint32_t s = 0; s < kMaxHostStreams; ++s) {
      // A stream is only a bulk swap if the host vertex exactly covers the guest
      // one -- trailing guest padding would shift every subsequent vertex.
      if (layout.host_stride[s] != layout.guest_stride[s]) {
        layout.bulk_swap[s] = false;
      }
    }
    host_elements[host_count] = D3DVERTEXELEMENT9 D3DDECL_END();
    if (FAILED(device_->CreateVertexDeclaration(host_elements, &layout.decl))) {
      REXGPU_ERROR("nx1_d3d9: CreateVertexDeclaration failed ({} elements)", host_count);
      layout.decl = nullptr;
    }
  }

  // A failed layout is cached too: the same declaration will fail every draw,
  // and re-deriving it each time would be pure overhead.
  layout.content_key = LayoutContentKey(layout);
  auto& stored = map->emplace(key, std::move(layout)).first->second;
  return stored.decl ? &stored : nullptr;
}

const VertexLayout* ResourceTracker::GetShaderVertexLayout(const uint8_t* base, uint32_t vs_object,
                                                           uint32_t vs_pass,
                                                           uint32_t stream0_stride,
                                                           const uint32_t* stream_stride) {
  // stream0_stride == 0 is valid: it selects bound-buffer mode (strides come from
  // m_StreamStride). Only reject a genuinely unusable tracker here.
  if (!device_ || !layouts_) {
    return nullptr;
  }
  if (!vs_object) {
    return nullptr;
  }
  // The caller resolved the exact microcode pass the draw runs -- the same one
  // BindShadersAndConstants picked -- so the vfetch layout matches the shader the SM3 cache was
  // keyed on. Deriving it here a second time would be a second chance to disagree.
  const GuestUcode ucode = ReadGuestUcode(base, vs_object, /*pixel_shader=*/false, vs_pass);
  if (!ucode.valid()) {
    return nullptr;
  }

  // The vfetch layout is fixed by the microcode; for UP draws the host stride also
  // depends on the supplied stream-0 stride, so fold it into the key.
  const uint64_t key = MixKey(MixKey(ucode.physical_address, ucode.dword_count), stream0_stride);
  auto* map = static_cast<LayoutMap*>(layouts_);
  if (auto it = map->find(key); it != map->end()) {
    return it->second.decl ? &it->second : nullptr;
  }

  // Reuse rexglue's Xenos analyzer -- it produces exactly the vertex_bindings
  // XenosRecomp consumed to assign this shader's input semantics.
  const uint32_t* ucode_dwords =
      reinterpret_cast<const uint32_t*>(TranslatePhysical(ucode.physical_address));
  rex::graphics::Shader shader(rex::graphics::xenos::ShaderType::kVertex, ucode.physical_address,
                               ucode_dwords, ucode.dword_count, std::endian::big);
  rex::string::StringBuffer disasm;
  shader.AnalyzeUcode(disasm);

  // stream0_stride != 0 -> UP draw: one interleaved stream 0 with the supplied
  // stride. stream0_stride == 0 -> bound-buffer draw (BindStreams): each vfetch
  // binding is its own stream (95 - fetch_constant, matching VertexFetchSlotForStream)
  // fed by that stream's fetch constant, with the stride taken from the vfetch.
  const bool up_mode = stream0_stride != 0;

  VertexLayout layout;
  layout.key = key;
  for (uint32_t s = 0; s < kMaxHostStreams; ++s) {
    layout.bulk_swap[s] = true;
  }
  if (up_mode) {
    layout.guest_stride[0] = stream0_stride;
  }

  D3DVERTEXELEMENT9 host_elements[MAXD3DDECLLENGTH + 1] = {};
  uint32_t host_count = 0;
  bool ok = true;
  size_t ordinal = 0;
  uint32_t packed_count = 0, texcoord_count = 0, color_count = 0;

  for (const auto& binding : shader.vertex_bindings()) {
    const uint32_t stream = up_mode ? 0u : (95u - binding.fetch_constant);
    if (stream >= kMaxHostStreams) {
      ok = false;
      break;
    }
    if (!up_mode) {
      // The bound-buffer stride is not in the shader's vfetch (it reads 0 there);
      // the guest sets it via D3DDevice_SetStreamSource into m_StreamStride[].
      layout.guest_stride[stream] = stream_stride[stream];
    }
    for (const auto& attribute : binding.attributes) {
      if (host_count >= MAXD3DDECLLENGTH) {
        ok = false;
        break;
      }
      const auto& attrs = attribute.fetch_instr.attributes;
      const uint32_t format = uint32_t(attrs.data_format);
      const bool is_signed = attrs.is_signed;
      const bool is_normalized = !attrs.is_integer;
      // The usage ordinal spans ALL bindings/attributes in listing order -- exactly
      // as WriteNx1RawVertexFetchMetadata did, so semantics match the compiled shader.
      const RawVertexUsage usage =
          Nx1RawVertexUsage(ordinal++, format, packed_count, texcoord_count, color_count);

      HostFormat host_format;
      // The vfetch instruction carries its OWN swizzle (GPUVERTEX_FETCH_INSTRUCTION SwizzleX..W),
      // spliced in from the declaration by PatchVertexShaderToMatchVertexDeclaration -- but the
      // analyzer's attribute view does not surface it, so this path cannot tell D3DCOLOR from
      // UBYTE4N. Passing ABGR keeps the previous behaviour rather than guessing; a D3DCOLOR
      // element reaching a draw through the shader-derived layout (bound streams with no
      // CVertexDeclaration) still gets R and B exchanged. Fixing it needs the swizzle threaded
      // out of the fetch instruction.
      if (!PickHostFormat(format, is_signed, is_normalized, nx1::d3d9::kGpuSwizzleAbgr,
                          &host_format)) {
        if ((unsupported_formats_++ % 100) == 0) {
          REXGPU_WARN("nx1_d3d9: unsupported Xenos vfetch format {} (signed={}, normalized={})",
                      format, is_signed, is_normalized);
        }
        ok = false;
        break;
      }

      ConvertOp op{};
      op.src_offset = uint16_t(attrs.offset * 4);  // vfetch offset is in dwords
      op.dst_offset = uint16_t(layout.host_stride[stream]);
      op.stream = uint8_t(stream);
      op.format = uint8_t(format);
      op.src_size = host_format.src_size;
      op.dst_size = host_format.dst_size;
      op.is_signed = is_signed;
      op.is_normalized = is_normalized;
      op.expand = host_format.expand;
      layout.host_stride[stream] += host_format.dst_size;
      if (op.expand || op.dst_offset != op.src_offset) {
        layout.bulk_swap[stream] = false;
      }
      layout.ops.push_back(op);

      host_elements[host_count++] = D3DVERTEXELEMENT9{
          WORD(stream), WORD(op.dst_offset), BYTE(host_format.type), D3DDECLMETHOD_DEFAULT,
          usage.usage,  usage.usage_index,
      };
      layout.stream_count = std::max(layout.stream_count, stream + 1);
    }
    if (!ok) {
      break;
    }
  }

  ok = ok && host_count > 0;
  if (ok) {
    for (uint32_t s = 0; s < layout.stream_count; ++s) {
      // A stream is only a bulk swap if the host vertex exactly covers the guest one.
      if (layout.host_stride[s] != layout.guest_stride[s]) {
        layout.bulk_swap[s] = false;
      }
    }
    host_elements[host_count] = D3DVERTEXELEMENT9 D3DDECL_END();
    if (FAILED(device_->CreateVertexDeclaration(host_elements, &layout.decl))) {
      REXGPU_ERROR("nx1_d3d9: shader-derived CreateVertexDeclaration failed ({} elements)",
                   host_count);
      layout.decl = nullptr;
    }
  }

  layout.content_key = LayoutContentKey(layout);
  auto& stored = map->emplace(key, std::move(layout)).first->second;
  return stored.decl ? &stored : nullptr;
}

IDirect3DVertexBuffer9* ResourceTracker::GetVertexBuffer(const uint8_t* base,
                                                         const VertexFetchConstant& fetch,
                                                         uint32_t stream,
                                                         const VertexLayout& layout,
                                                         uint32_t needed_vertices,
                                                         uint32_t* vertex_count) {
  *vertex_count = 0;
  if (!device_ || !vertex_buffers_ || stream >= kMaxHostStreams) {
    return nullptr;
  }
  const auto vmark = [this] {
    return prof_enabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  };
  const auto vadd = [this](uint64_t& sink, std::chrono::steady_clock::time_point t0) {
    if (prof_enabled_) sink += uint64_t((std::chrono::steady_clock::now() - t0).count());
  };
  const auto t_fast = vmark();
  if (prof_enabled_) ++prof_vtx_.calls;

  const uint32_t guest_stride = layout.guest_stride[stream];
  const uint32_t host_stride = layout.host_stride[stream];
  if (!guest_stride || !host_stride) {
    return nullptr;
  }
  if (!fetch.base_address || fetch.size_bytes < guest_stride) {
    return nullptr;
  }

  // Keyed on the layout's *content*, not on the shader that asked for it: the same pooled
  // buffer read through the same vertex format converts to the same bytes no matter which
  // of the frame's ~120 vertex shaders is bound.
  const uint64_t key = MixKey(MixKey(fetch.base_address, layout.content_key), stream);
  auto* map = static_cast<VertexBufferMap*>(vertex_buffers_);
  auto& entry = (*map)[key];

  // The fetch constant's size is the distance to the end of the buffer, which for the
  // engine's shared pools is megabytes -- not this model's vertex count. Mirroring all of
  // it hashed and converted ~78k vertices for a draw that touches a few hundred. Bound it
  // by what the draw's indices actually reference, and only ever grow: several draws share
  // an entry, and re-creating the buffer whenever one of them needs fewer vertices than the
  // last would thrash it.
  const uint32_t pool_count = fetch.size_bytes / guest_stride;
  uint32_t count = needed_vertices ? std::min(needed_vertices, pool_count) : pool_count;
  count = std::max(count, entry.vertex_count);
  const uint32_t host_bytes = count * host_stride;
  *vertex_count = count;

  // Frame-coherent reuse: already validated this frame -> skip the content hash.
  if (entry.vb && entry.last_frame == frame_ && entry.bytes == host_bytes) {
    *vertex_count = entry.vertex_count;
    vadd(prof_vtx_.fast_ns, t_fast);
    return entry.vb;
  }
  vadd(prof_vtx_.fast_ns, t_fast);
  entry.last_frame = frame_;

  const uint8_t* src = TranslatePhysical(fetch.base_address);
  last_stream_addr_ = fetch.base_address;
  last_stream_bytes_ = uint32_t(size_t(count) * guest_stride);

  // Hash the guest bytes at most once per buffer per frame. The hash only depends on the
  // guest memory, so every layout reading the same buffer gets the same answer -- and the
  // buffers are large enough (a shared pool runs to megabytes) that re-hashing one per
  // reader was half a gigabyte of memory traffic a frame.
  const size_t guest_bytes = size_t(count) * guest_stride;
  uint64_t content_hash = 0;
  {
    const auto t_hash = vmark();
    auto* hashes = static_cast<VertexHashMap*>(vertex_hashes_);
    auto& cached = (*hashes)[MixKey(fetch.base_address, guest_bytes)];
    if (cached.frame != frame_ + 1) {  // +1: a default-constructed entry must not match frame 0
      cached.frame = frame_ + 1;
      cached.hash = XXH3_64bits(src, guest_bytes);
      if (prof_enabled_) {
        ++prof_vtx_.hashes;
        prof_vtx_.hash_bytes += guest_bytes;
      }
    }
    content_hash = cached.hash;
    vadd(prof_vtx_.hash_ns, t_hash);
  }
  if (entry.vb && entry.bytes == host_bytes && entry.content_hash == content_hash) {
    return entry.vb;
  }
  // Lock(.., 0) on a D3DPOOL_DEFAULT buffer blocks until the GPU has finished reading it.
  // Measured at ~23 us to rewrite ~8 KB -- 0.35 GB/s, which is a pipeline stall, not CPU
  // throughput. A buffer rewritten again and again is dynamic geometry, so give repeat
  // offenders a DYNAMIC buffer and let Lock DISCARD: the driver hands back fresh storage
  // instead of waiting. Static geometry deliberately stays in DEFAULT -- it is rebuilt once
  // and then read by thousands of draws, and DYNAMIC would park it in AGP memory where the
  // GPU reads it more slowly. That is the trade this threshold exists to avoid making blindly.
  ++entry.rebuilds;
  const bool want_dynamic = entry.rebuilds >= kDynamicRebuilds;
  if (entry.vb && (entry.bytes != host_bytes || want_dynamic != entry.dynamic)) {
    entry.vb->Release();
    entry.vb = nullptr;
  }
  if (!entry.vb) {
    const DWORD usage = D3DUSAGE_WRITEONLY | (want_dynamic ? D3DUSAGE_DYNAMIC : 0u);
    if (FAILED(device_->CreateVertexBuffer(host_bytes, usage, 0, D3DPOOL_DEFAULT, &entry.vb,
                                           nullptr))) {
      REXGPU_ERROR("nx1_d3d9: CreateVertexBuffer({} bytes, {}) failed", host_bytes,
                   want_dynamic ? "dynamic" : "default");
      entry.vb = nullptr;
      return nullptr;
    }
    entry.dynamic = want_dynamic;
  }

  const auto t_conv = vmark();
  if (prof_enabled_) {
    ++prof_vtx_.converts;
    prof_vtx_.convert_bytes += host_bytes;
    if (entry.dynamic) ++prof_vtx_.dynamic_converts;
  }
  void* mapped = nullptr;
  // DISCARD is only legal on a DYNAMIC buffer, and only for a whole-buffer lock -- which
  // Lock(0, 0, ..) is.
  if (FAILED(entry.vb->Lock(0, 0, &mapped, entry.dynamic ? D3DLOCK_DISCARD : 0))) {
    return nullptr;
  }
  auto* dst = static_cast<uint8_t*>(mapped);

  if (layout.bulk_swap[stream]) {
    SwapDwords(dst, src, size_t(count) * guest_stride);
  } else {
    // Select this stream's ops ONCE. The filter used to sit inside the per-vertex loop, so a
    // multi-stream layout re-scanned every other stream's ops for every vertex converted --
    // pure overhead multiplied by the vertex count, and the reason this path ran at 0.37 GB/s.
    const ConvertOp* stream_ops[MAXD3DDECLLENGTH];
    uint32_t stream_op_count = 0;
    for (const ConvertOp& op : layout.ops) {
      if (op.stream == stream && stream_op_count < MAXD3DDECLLENGTH) {
        stream_ops[stream_op_count++] = &op;
      }
    }
    for (uint32_t v = 0; v < count; ++v) {
      const uint8_t* sv = src + size_t(v) * guest_stride;
      uint8_t* dv = dst + size_t(v) * host_stride;
      for (uint32_t oi = 0; oi < stream_op_count; ++oi) {
        const ConvertOp& op = *stream_ops[oi];
        if (op.expand) {
          float decoded[4];
          Expand(sv + op.src_offset, op, decoded);
          std::memcpy(dv + op.dst_offset, decoded, op.dst_size);
        } else {
          SwapDwords(dv + op.dst_offset, sv + op.src_offset, op.src_size);
        }
      }
    }
  }
  entry.vb->Unlock();
  vadd(prof_vtx_.convert_ns, t_conv);

  entry.bytes = host_bytes;
  entry.vertex_count = count;
  entry.content_hash = content_hash;
  return entry.vb;
}

void ResourceTracker::LogCacheStats() {
  if (!device_) {
    return;
  }
  const size_t textures = textures_ ? static_cast<TextureMap*>(textures_)->size() : 0;
  // Must go through the VertexBufferMap alias like every other cache here. Spelling the
  // container out longhand meant the FlatMap swap missed this cast, and it read a FlatMap as
  // an unordered_map -- printing vb=2330591112192, which reads exactly like heap corruption.
  const size_t vbs = vertex_buffers_ ? static_cast<VertexBufferMap*>(vertex_buffers_)->size() : 0;
  const size_t ibs = index_buffers_ ? static_cast<IndexBufferMap*>(index_buffers_)->size() : 0;
  const size_t resolves = resolves_ ? static_cast<ResolveMap*>(resolves_)->size() : 0;
  NX1_LOGI_STATS(
      "nx1_d3d9: cache frame={} textures={} vb={} ib={} resolves={} | uploads={} rebuilds={} "
      "evicted={} failures={} unsupported={} | driver texmem={} MiB",
      frame_, textures, vbs, ibs, resolves, tex_uploads_, tex_rebuilds_, tex_evicted_,
      tex_failures_, unsupported_texture_formats_,
      device_->GetAvailableTextureMem() / (1024 * 1024));
  REXGPU_INFO("nx1_d3d9: DMAINVALIDATE {} mirrored-copy ranges queued for invalidation; "
              "DrainMemoryWrites applies them on the render thread and dirties the textures they "
              "overlap (zero here means the mirror landed no bytes at all)",
              dma_invalidations_);
  REXGPU_INFO("nx1_d3d9: TORNPAGES {} of {} page copies changed while being read -- non-zero "
              "means texture decodes are reading half-written guest memory",
              torn_pages_, torn_checked_);
  // Read this against DECODECHANGE's "page writes 0 -> 0". Every range counted here is a write to
  // guest texture memory that host page protection could not trap, so before this watch existed
  // the cache never heard about it. GPUWATCH ranges=0 while the watch is registered means the
  // reference is not reporting GPU writes at all in this configuration (check that memexport
  // draws still execute -- nx1_skip_reference_raster exempts them, but only while the reference
  // command processor is running); blasts dominating means the destinations are unknown and the
  // precise-range path is not where the texture traffic is.
  REXGPU_INFO("nx1_d3d9: GPUWATCH {} ({}) | {} ranges, {} pages queued | {} whole-buffer blasts "
              "dropped, {} over-wide dropped",
              gpu_watch_handle_ ? "registered" : "NOT REGISTERED",
              REXCVAR_GET(nx1_d3d9_gpu_write_watch) ? "on" : "off", gpu_write_ranges_,
              gpu_write_pages_, gpu_write_blasts_, gpu_write_dropped_);
  if (REXCVAR_GET(nx1_d3d9_dbg_dmacover)) {
    const uint64_t d = dmacover_decodes_ ? dmacover_decodes_ : 1;
    const uint64_t pg = dmacover_pg_never_ + dmacover_pg_skipped_ + dmacover_pg_written_;
    REXGPU_WARN("nx1_d3d9: DMACOVER {} decodes | UNDERCOPIED {} ({:.1f}%, other half would fit in "
                "{}) | covered {} ({:.1f}%) | no copy recorded {} ({:.1f}%)",
                dmacover_decodes_, dmacover_under_, 100.0 * double(dmacover_under_) / double(d),
                dmacover_alt_fits_, dmacover_covered_,
                100.0 * double(dmacover_covered_) / double(d), dmacover_nocopy_,
                100.0 * double(dmacover_nocopy_) / double(d));
    // READ THE PAGE LINE AGAINST THE DECODE LINE. "covered" only means a copy's SPAN reached the
    // page; the per-page skip still leaves empty-source pages holding the previous occupant, and
    // that shows up here as `spanned but skipped` while the decode line says covered.
    REXGPU_WARN("nx1_d3d9: DMACOVER pages {} | never targeted {} ({:.1f}%) | spanned but SKIPPED "
                "{} ({:.1f}%) | actually written {} ({:.1f}%)",
                pg, dmacover_pg_never_, 100.0 * double(dmacover_pg_never_) / double(pg ? pg : 1),
                dmacover_pg_skipped_,
                100.0 * double(dmacover_pg_skipped_) / double(pg ? pg : 1), dmacover_pg_written_,
                100.0 * double(dmacover_pg_written_) / double(pg ? pg : 1));
  }
  if (framecap_count_) {
    REXGPU_WARN("nx1_d3d9: FRAMECAP {} distinct textures captured to texdump/cap*", framecap_count_);
  }
  // CLAMP hits=0 with the cvar on means no decode ever overlapped a live neighbour, so the
  // over-read is not happening in this scene -- read that before concluding anything from the
  // picture. Non-zero says how much foreign memory we STOPPED decoding as texels.
  REXGPU_WARN("nx1_d3d9: CLAMP {} | {} decodes clamped to the next live texture base, {} KiB of "
              "the next allocation not decoded",
              REXCVAR_GET(nx1_d3d9_clamp_alloc) ? "on" : "off", clamp_hits_, clamp_bytes_ >> 10);
  REXGPU_INFO("nx1_d3d9: REARM {} ({} pages re-armed) -- pair with the DECODECHANGE no-write-delta "
              "fraction: if forcing the re-arm collapses it, our cached armed bit had gone stale "
              "against the protection the reference also manages",
              REXCVAR_GET(nx1_d3d9_rearm_watch) ? "on" : "off", rearm_calls_);
  REXGPU_INFO("nx1_d3d9: REDECODE forced={} rechecks (nx1_d3d9_redecode_delay={}) -- pair this "
              "with the DECODECHANGE count: rechecks that found DIFFERENT bytes are decodes that "
              "had been taken before the data landed",
              forced_rechecks_, REXCVAR_GET(nx1_d3d9_redecode_delay));
  // ARMING FOR THE GPU-WRITE WATCH. REBUILDSRC shows gpu_write=0/0, but that counts REBUILDS
  // ATTRIBUTED, so a zero there cannot distinguish "the reference never reports GPU writes",
  // "it reports them but they never overlap a texture" and "the subscription is dead". These
  // counters have existed all along and were never printed anywhere, which is why the mechanism
  // looked inert without anyone being able to say so.
  NX1_LOGI_STATS("nx1_d3d9: PROBEGATE {} probe-detected changes REFUSED for want of a "
                 "reported write (nx1_d3d9_probe_needs_write={})",
                 probe_refused_no_write_, REXCVAR_GET(nx1_d3d9_probe_needs_write) ? 1 : 0);
  REXGPU_WARN("nx1_d3d9: GPUWRITE ranges={} pages={} | blasts(whole-buffer, unusable)={} "
              "dropped(over {} MB cap)={} -- ranges=0 means the reference never reported a GPU "
              "write to us at all; ranges>0 with gpu_write=0/0 in REBUILDSRC means it reports them "
              "but they never overlap a live texture's watched range",
              gpu_write_ranges_, gpu_write_pages_, gpu_write_blasts_, 
              REXCVAR_GET(nx1_d3d9_gpu_write_watch_max_mb), gpu_write_dropped_);
  {
    // Unconditional, including zeros, with both columns: rebuilds and how many changed content.
    static const char* kSrc[] = {"cpu_write", "gpu_write", "dma",     "probe",
                                 "mip_reloc", "recheck",   "resolve", "partial", "debug"};
    std::string line;
    for (size_t i = 0; i < size_t(DirtySource::kCount); ++i) {
      line += fmt::format("{}={}/{} ", kSrc[i], changed_by_source_[i], dirty_by_source_[i]);
    }
    REXGPU_WARN("nx1_d3d9: REBUILDSRC changed/total per invalidation source: {}-- the source with "
                "the high CHANGED rate is the one adopting new bytes, which is where we diverge "
                "from the reference (it never re-reads at all)",
                line);
  }
  REXGPU_INFO("nx1_d3d9: CONTENTPROBE {} rebuilds forced by a changed slot occupant -- each one "
              "is a surface that would otherwise have rendered another texture's bytes",
              probe_rebuilds_);
  // Unconditional, including zero. Zero with the gate ON is a real result: it would mean re-decodes
  // are never poorer than what they replace, so the churn seen in the POLL trajectories is not
  // reaching the decode path and keep-best cannot be the answer.
  REXGPU_WARN("nx1_d3d9: KEEPBEST {} re-decodes refused as poorer than the decode already held "
              "(gate={}, drop>={}%, max {} consecutive per entry)",
              keepbest_refused_, REXCVAR_GET(nx1_d3d9_keep_best) ? "on" : "off",
              REXCVAR_GET(nx1_d3d9_keep_best_drop_pct), REXCVAR_GET(nx1_d3d9_keep_best_max));
  REXGPU_INFO("nx1_d3d9: PARTIALSRC {} of {} decodes had an incomplete source ({}%) | {} of {} "
              "source pages were still empty ({}%) -- the objective speckle baseline",
              partial_decodes_, decodes_total_,
              decodes_total_ ? partial_decodes_ * 100 / decodes_total_ : 0, partial_pages_sum_,
              decode_pages_sum_,
              decode_pages_sum_ ? partial_pages_sum_ * 100 / decode_pages_sum_ : 0);
  REXGPU_INFO("nx1_d3d9: RESOLVEMAP served={} rejected_stale={} (entries={})", resolve_served_,
              resolve_rejected_, resolves_ ? static_cast<ResolveMap*>(resolves_)->size() : 0);
  REXGPU_INFO("nx1_d3d9: SRCCLASS repeating(placeholder-like)={} high-entropy(garbage-like)={} "
              "-- two DIFFERENT failures; a fix that moves only one is not the whole bug",
              repeating_source_binds_, highentropy_source_binds_);
  REXGPU_INFO("nx1_d3d9: PLACEHOLDER binds={} (textures bound at the engine's not-resident "
              "buffer {:08X}); a config that streams properly drives this toward zero",
              placeholder_binds_, DefaultPixelsAddress());
  REXGPU_INFO("nx1_d3d9: mipgen guest={} built={} auto={} basemap(level0 only)={} "
              "skipped(no chain declared)={} skipped(fmt)={} mip_relocs={} | mipfill mode={} "
              "levels_overwritten={}",
              mips_guest_, mips_built_, mips_auto_, mips_basemap_, mips_skip_nochain_,
              mips_skip_unsupported_, mip_relocs_, REXCVAR_GET(nx1_d3d9_dbg_mipfill),
              mipfill_levels_);
  // MEMORY ACCOUNTING. A 33 GB runaway was observed twice; guessing at which container or
  // resource pool grows has already cost one wrong fix (the dump force-rebuild was a real
  // pathology but not the cause -- the leak reproduced at the same size without it). Report the
  // process totals beside every pool this tracker owns, so one run says which number moves.
  // Committed memory rather than working set: a leak of never-touched pages shows in commit
  // long before it shows in RSS, and D3D9Ex staging lives in the process heap.
  {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                         sizeof(pmc));
    size_t pending = 0;
    {
      std::lock_guard<std::mutex> lk(dirty_mu_);
      pending = writes_pending_.size();
    }
    size_t mirror_pages_valid = 0;
    for (uint64_t w : mirror_valid_) mirror_pages_valid += size_t(__popcnt64(w));
    // BYTES, not counts. Sum what each cache actually holds and report the largest single
    // entry beside it: a cache of 3000 buffers is unremarkable until one learns they are
    // pool-sized. GetVertexBuffer allocates the WHOLE guest pool when a draw cannot bound
    // itself by index reach (needed_vertices == 0), and the size is grow-only per entry, so
    // one unbounded draw pins an entry at pool size for the rest of the session.
    uint64_t tex_bytes = 0, vb_bytes = 0, vb_max = 0;
    // COUNTS, not only bytes. A 100 MB/s runaway was reported while this line showed a 1 MiB
    // texture cache against 4 GB of private bytes -- which is either a leak somewhere else
    // entirely, or host_bytes under-reporting what the entries actually hold (it is set from the
    // decode, so any path that grows a texture afterwards -- driver-generated mip chains, most
    // obviously -- is invisible here). A count beside the bytes separates those two cases in one
    // run instead of by inspection: many entries + few bytes means this number is lying.
    size_t tex_n = 0, vb_n = 0, rt_n = 0, rv_n = 0;
    uint64_t rt_bytes = 0, rv_bytes = 0;
    if (auto* tm = static_cast<TextureMap*>(textures_)) {
      tex_n = tm->size();
      for (auto [k, e] : *tm) tex_bytes += e.host_bytes;
    }
    if (auto* vm = static_cast<VertexBufferMap*>(vertex_buffers_)) {
      vb_n = vm->size();
      for (auto [k, e] : *vm) {
        vb_bytes += e.bytes;
        vb_max = std::max<uint64_t>(vb_max, e.bytes);
      }
    }
    // The two pools this census never covered. Both hold full-size D3DPOOL_DEFAULT surfaces and
    // neither has any eviction, so "it cannot be these, they are small" needed checking rather
    // than assuming -- resolves_ measured 8 entries, render_targets_ was never measured at all.
    if (auto* rm = static_cast<TargetMap*>(render_targets_)) {
      rt_n = rm->size();
      for (const auto& [k, t] : *rm) {
        if (t.tex) rt_bytes += uint64_t(t.width) * t.height * 4;
      }
    }
    if (auto* rvm = static_cast<ResolveMap*>(resolves_)) {
      rv_n = rvm->size();
      for (auto [k, e] : *rvm) {
        if (e.tex && e.owned) rv_bytes += uint64_t(e.width) * e.height * 4;
      }
    }
    NX1_LOGI_STATS("nx1_d3d9: MEMPOOLS textures={} ({} MiB) vbs={} ({} MiB) render_targets={} "
                "({} MiB) resolves={} ({} MiB)",
                tex_n, tex_bytes / (1024 * 1024), vb_n, vb_bytes / (1024 * 1024), rt_n,
                rt_bytes / (1024 * 1024), rv_n, rv_bytes / (1024 * 1024));
    NX1_LOGI_STATS("nx1_d3d9: MEM private={} MiB working={} MiB | tex_cache={} MiB vb_cache={} MiB "
                "(largest vb {} KiB) | writes_pending={} detile_scratch={} KiB mirror_valid={} MiB",
                pmc.PrivateUsage / (1024 * 1024), pmc.WorkingSetSize / (1024 * 1024),
                tex_bytes / (1024 * 1024), vb_bytes / (1024 * 1024), vb_max / 1024, pending,
                // The detile scratch is per-thread now (see DetileScratch) and no longer a single
                // member this thread can measure for all of them; report this thread's own.
                DetileScratchBytes() / 1024, (mirror_pages_valid * 4096) / (1024 * 1024));
  }
  // Was the shared detile buffer ever actually contended? threads>1 with overlap_max>1 is the
  // race observed directly rather than inferred from log thread ids. overlap_max==1 means the
  // buffer was never concurrently held and the thread_local change fixed nothing real.
  NX1_LOGW_STATS("nx1_d3d9: DETILESCRATCH {} threads used it, max {} held it at once ({})",
              DetileScratchThreads().load(std::memory_order_relaxed),
              DetileScratchOverlapMax().load(std::memory_order_relaxed),
              DetileScratchOverlapMax().load(std::memory_order_relaxed) > 1
                  ? "RACE CONFIRMED -- concurrent decodes shared one buffer before this fix"
                  : "no overlap seen; the shared buffer was not actually contended");
  REXGPU_INFO("nx1_d3d9: BORDER {} decodes declare a border -- the XDK untiler takes the border "
              "flag as a required argument and ours ignores it, so nonzero here is a real gap in "
              "our layout model (zero means it is theoretical)",
              border_decodes_);
  REXGPU_INFO("nx1_d3d9: packedmip enabled={} small_decodes={} packed_decodes={} offsets_applied={}"
              " ({})",
              REXCVAR_GET(nx1_d3d9_packed_mip_offset) ? 1 : 0, small_decodes_, packed_decodes_,
              packed_offsets_,
              packed_offsets_ ? "ARMED -- level 0 is being read from a shifted sub-tile origin"
              : packed_decodes_ ? "INERT -- packed textures seen but the offset resolved to (0,0)"
                                : "NOT ARMED -- no decode declared a packed mip tail");
  REXGPU_INFO("nx1_d3d9: commit freeze={} unfrozen_writes={} ({})",
              REXCVAR_GET(nx1_d3d9_commit_textures) ? "on" : "OFF", unfrozen_writes_,
              REXCVAR_GET(nx1_d3d9_commit_textures)
                  ? "expected 0 while frozen"
                  : (unfrozen_writes_ ? "ARMED -- frozen textures are honouring writes"
                                      : "NOT ARMED -- no frozen entry saw a write, test is void"));
}

// Physical-memory write-watch callback. Runs on whatever guest thread wrote the memory, under the
// memory system's global lock -- so it must not touch the texture cache or take the render lock.
// It only queues the written range; DrainMemoryWrites() applies it on the render thread. The
// returned pair is the (page-aligned) range the memory system may safely unprotect so the write
// proceeds; returning just the touched pages is correct (other pages stay watched).
std::pair<uint32_t, uint32_t> ResourceTracker::MemWatchThunk(void* ctx, uint32_t addr, uint32_t len,
                                                             bool exact) {
  return static_cast<ResourceTracker*>(ctx)->OnMemoryWrite(addr, len, exact);
}

std::pair<uint32_t, uint32_t> ResourceTracker::OnMemoryWrite(uint32_t addr, uint32_t len, bool) {
  {
    std::lock_guard<std::mutex> lk(dirty_mu_);
    writes_pending_.push_back({addr, len, DirtySource::kCpuWrite});
  }
  const uint32_t p0 = addr & ~0xFFFu;
  const uint32_t p1 = (addr + len + 0xFFFu) & ~0xFFFu;
  return std::make_pair(p0, p1 - p0);
}

// Ensure [phys_addr, phys_addr+len) is captured in the mirror; return a pointer into the mirror.
// A page is copied out of live guest RAM the first time it is requested while invalid, then held
// (and its write-watch armed) until the guest writes it. Textures decode through this so
// streaming-pool churn never reaches them between writes.
void ResourceTracker::RebuildResolveFlat() {
  resolve_flat_count_ = 0;
  resolve_flat_valid_ = false;
  if (!resolves_) {
    return;
  }
  auto* map = static_cast<ResolveMap*>(resolves_);
  if (map->size() > kResolveFlatMax) {
    return;  // fall back to the hash map
  }
  for (auto [key, entry] : *map) {
    if (!entry.tex) {
      continue;
    }
    resolve_flat_addr_[resolve_flat_count_] = uint32_t(key);
    resolve_flat_tex_[resolve_flat_count_] = entry.tex;
    resolve_flat_w_[resolve_flat_count_] = entry.width;
    resolve_flat_h_[resolve_flat_count_] = entry.height;
    ++resolve_flat_count_;
  }
  resolve_flat_valid_ = true;
}

const uint8_t* ResourceTracker::PhysicalPointer(uint32_t phys_addr) const {
  return TranslatePhysical(phys_addr);
}

uint32_t ResourceTracker::PageWriteCount(uint32_t phys) const {
  const size_t page = size_t(phys & 0x1FFFFFFF) >> 12;
  return page < page_writes_.size() ? page_writes_[page] : 0;
}

/// Record that WE wrote guest memory (mirroring an image-cache copy), so the affected textures
/// re-decode. Host writes bypass guest page protection, so the write-watch never fires for them.
///
/// QUEUE ONLY -- this runs on the guest DMA thread. It used to invalidate the mirror and walk the
/// texture map inline, which is exactly what the write-watch callback is documented never to do:
/// "it must NOT touch the texture cache -- it only queues the written range, which
/// DrainMemoryWrites() applies from AdvanceFrame on the render thread." Iterating an
/// unordered_map while the render/worker thread inserts into it is undefined behaviour, and it
/// killed the game inside ImageCache_DmaCopyDelayed (guest lr=824D3504, read fault 0x1B25) once
/// nx1_d3d9_dma_verbatim raised the call rate ~8x. The race was latent long before that.
///
/// Routing through writes_pending_ also picks up work this function never did: DrainMemoryWrites
/// dirties entries by their MIP watch range as well as the base range.
void ResourceTracker::InvalidateGuestRange(uint32_t phys, uint32_t bytes, DirtySource source) {
  if (!bytes) {
    return;
  }
  std::lock_guard<std::mutex> lk(dirty_mu_);
  writes_pending_.push_back({phys, bytes, source});
  // Counts RANGES QUEUED now, not entries dirtied -- the dirtying moved into DrainMemoryWrites,
  // which cannot separate a DMA-origin write from a write-watch one. Renamed in the log to match;
  // leaving it reading "textures re-decoded" would have made it report zero forever while
  // claiming that meant the mirror landed bytes nobody read.
  ++dma_invalidations_;
}

/// Drop the snapshot for a range WITHOUT claiming anyone wrote it.
///
/// InvalidateGuestRange also bumps page_writes_, which is right for our own mirrored copies (we
/// really did write those bytes) and WRONG here: the content probe only observed that a slot's
/// contents differ from what we decoded, and recording that as a write would corrupt the
/// write-history predicate that the partial-source test depends on -- the same class of error as
/// marking pages written for a copy whose source was empty.
///
/// This exists because the probe (which reads LIVE memory) forced a rebuild, while the rebuild
/// reads MirrorSnapshot -- so a slot whose occupant changed was re-decoded from the stale snapshot
/// and produced the same old image. 2,457 forced rebuilds in one run, every one a no-op in content
/// terms, which is why driving SLOTREUSE to zero never moved the speckle.
/// WHOSE BYTES ARE THESE? Trace each source page to the address it was first seen under.
///
/// Built because the bug is NOT reproducible per surface -- the wall that speckles this run will
/// not speckle next run -- so any instrument requiring the user to nominate a target cannot
/// converge. This one finds the corrupt texture itself: the corrupt images show rectangular blocks
/// of OTHER textures' content, and that is a checkable claim.
void ResourceTracker::NotePageOrigin(const uint8_t* src, size_t guest_bytes,
                                     const TextureFetchConstant& t, uint32_t height,
                                     uint32_t block_width, uint32_t block_height,
                                     uint32_t block_pitch_h, uint32_t bpb) {
  if (!src || !REXCVAR_GET(nx1_d3d9_dbg_pageorigin) || guest_bytes < 4096) {
    return;
  }
  // ONLY THE PAGES A FETCH ACTUALLY READS. The first version of this walked every page of the
  // DECLARED extent and reported 22.7% of decodes as holding another texture's content -- which
  // was true and meaningless, because the alignment gaps in a texture allocation are precisely
  // the offsets no fetch references, and the XDK documents the guest as packing other data into
  // them (XGAddress2DTiledExtent: "unused memory regions ... you might want to reclaim").
  //
  // The question that matters is narrower: of the bytes we ACTUALLY DECODE, does any come from
  // another texture? Walk the real block addresses -- the same ones DetileMip2D reads -- and
  // check only the pages they land in. Foreign content here is corruption, not packing.
  std::vector<uint32_t> touched;
  // HOW MUCH of each page we actually consume. A page can be shared between this texture's tail and
  // whatever the guest packed into the surrounding tile padding -- the XDK documents that packing
  // explicitly -- and hashing the WHOLE page then reports the neighbour's content as "foreign" even
  // when every byte we read is ours. Without this the detector cannot tell a genuinely mixed slot
  // from a page we barely touch, which is the same class of over-count its five previous revisions
  // each removed.
  std::vector<uint32_t> touched_blocks;
  {
    const uint32_t bw = block_width, bh = block_height;
    if (!bw || !bh) {
      return;
    }
    touched.reserve(64);
    touched_blocks.reserve(64);
    uint32_t last = UINT32_MAX, last_idx = 0;
    auto note = [&](size_t off) {
      const uint32_t pg = uint32_t(off >> 12);
      if (pg == last) {  // fast path: consecutive blocks usually share a page
        ++touched_blocks[last_idx];
        return;
      }
      const auto it = std::find(touched.begin(), touched.end(), pg);
      if (it == touched.end()) {
        touched.push_back(pg);
        touched_blocks.push_back(1);
        last_idx = uint32_t(touched.size() - 1);
      } else {
        last_idx = uint32_t(it - touched.begin());
        ++touched_blocks[last_idx];
      }
      last = pg;
    };
    if (t.tiled) {
      const uint32_t bpb_log2 = Log2Exact(bpb);
      for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
          note(size_t(uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
              int32_t(bx), int32_t(by), block_pitch_h, bpb_log2))));
        }
      }
    } else {
      const uint32_t src_row = block_pitch_h * bpb;
      for (uint32_t by = 0; by < bh; ++by) {
        const size_t row = size_t(by) * src_row;
        for (size_t o = row; o < row + size_t(bw) * bpb; o += 4096) note(o);
        note(row + size_t(bw) * bpb - 1);
      }
    }
  }
  if (touched.empty()) {
    return;
  }
  const uint32_t self = PhysicalAddress(t.base_address);
  uint32_t blank_this_decode = 0, blank_this_decode_total = 0;
  bool unwritten_this_decode = false;
  uint32_t pages = 0, foreign = 0, named = 0;
  uint32_t from_addr[8] = {};
  uint32_t from_page[8] = {};
  std::vector<uint32_t> foreign_pages;
  for (const uint32_t pg : touched) {
    const size_t off = size_t(pg) << 12;
    if (off + 4096 > guest_bytes) {
      continue;
    }
    // Strided hash: identifying a page's content does not need every byte, and hashing all of
    // them for every decode is real bandwidth.
    uint64_t h = 1469598103934665603ull;
    uint64_t first = 0;
    bool uniform = true;
    uint32_t zero_samples = 0, samples = 0;
    for (uint32_t i = 0; i < 4096; i += 64) {
      uint64_t v;
      std::memcpy(&v, src + off + i, sizeof(v));
      if (i == 0) {
        first = v;
      } else if (v != first) {
        uniform = false;
      }
      ++samples;
      if (v == 0) {
        ++zero_samples;
      }
      h = (h ^ v) * 1099511628211ull;
    }
    // A UNIFORM PAGE PROVES NOTHING ABOUT PROVENANCE. Solid-black and fully-transparent regions
    // are everywhere in this content, and two unrelated textures sharing one is coincidence, not
    // theft -- the same trap that made the convergence detector report tiny shared dummy maps as
    // corruption. The >3-address filter does not catch it either: a blank page present in exactly
    // two textures passes. Excluded, and counted so the filter's own impact is visible.
    // BLANK REGIONS. A uniform page whose value is ZERO is not "shared content we cannot
    // attribute" -- it is a hole, and the image dumps show holes are the DOMINANT corruption
    // (50-85% of several textures, against single-digit noise). The provenance filter below throws
    // these away, which is precisely why MIXED never moved: it is blind to the main failure.
    // Counted here, over pages a fetch actually reads, so tile padding cannot inflate it.
    // MEASURED AT SAMPLE GRANULARITY, not whole pages. The first cut of this counted only pages
    // that were ENTIRELY zero and reported 0.7% -- while the decoded-image dumps of the very same
    // textures were 50-85% black. Both were "right": a page holding a few real blocks among mostly
    // zeros is not uniform, so it did not count, yet its pixels are overwhelmingly black on screen.
    // The page-level test simply cannot express what the artifact looks like. Counting zero SAMPLES
    // (64 per page, the same ones the hash already reads) tracks the visible hole fraction.
    blank_read_pages_ += samples;
    blank_pages_ += zero_samples;
    blank_this_decode += zero_samples;
    blank_this_decode_total += samples;
    // ZERO CONTENT IS NOT EVIDENCE OF A HOLE. This project has inferred residency from texture
    // CONTENT and been wrong three times -- the entropy classifiers twice, then an all-zero-pages
    // test that counted a UI logo on a transparent background as missing and rendered the NX1 title
    // as a solid white box. Enormous amounts of legitimate art are zero: transparent atlas regions,
    // alpha masks, decal borders. The rule that survived is JUDGE WRITE HISTORY, NEVER CONTENT.
    //
    // So the only defensible hole is a page that is zero AND that nothing has ever written.
    // PageWriteCount includes our own mirror/DMA writes (InvalidateGuestRange records them, since
    // host writes into guest RAM bypass page protection), so a page reading 0 here really was
    // never filled by anyone.
    if (zero_samples * 2 >= samples) {  // predominantly zero
      ++unwritten_candidates_;
      if (PageWriteCount(uint32_t(self + off)) == 0) {
        ++unwritten_holes_;
        unwritten_this_decode = true;
      }
    }
    if (uniform) {
      ++pageorigin_uniform_skipped_;
      continue;
    }
    ++pages;
    auto& po = page_origin_[h];
    if (!po.first_addr) {
      po.first_addr = self;
      po.addr_count = 1;
    } else if (po.first_addr != self) {
      if (po.addr_count < 255) ++po.addr_count;
      // A page reached by many addresses is legitimately shared (blank/uniform), not stolen.
      if (po.addr_count <= kPageOriginMaxAddrs) {
        ++foreign;
        foreign_pages.push_back(pg);
        if (named < 8) {
          from_addr[named] = po.first_addr;
          from_page[named] = uint32_t(off >> 12);
          ++named;
        }
      }
    }
  }
  // RELOCATION IS NOT CORRUPTION. The dominant pattern is ALL pages foreign from ONE donor, which
  // is a texture the image cache MOVED between pool slots -- exactly what ImageCache_DmaCopy does.
  // The content at the new address really is that image; it only looks foreign because we recorded
  // its hash at the old address first. (The kPageOriginMaxAddrs filter misses this: a relocation
  // touches exactly two addresses, so it passes.)
  //
  // A MIXTURE cannot be explained that way: some pages this texture's, some another's, or pages
  // from several different donors. That is a slot holding parts of more than one image, which is
  // the artifact. Count the two separately or the benign case buries the interesting one.
  uint32_t distinct = 0;
  for (uint32_t i = 0; i < named; ++i) {
    bool seen = false;
    for (uint32_t j = 0; j < i; ++j) seen |= from_addr[j] == from_addr[i];
    if (!seen) ++distinct;
  }
  const bool whole_texture_move = foreign == pages && distinct <= 1;
  if (whole_texture_move) {
    ++pageorigin_relocated_;
  }
  if (pages && foreign && !whole_texture_move &&
      foreign * 100 / pages >= REXCVAR_GET(nx1_d3d9_dbg_pageorigin_pct)) {
    ++pageorigin_flagged_;
    if (pageorigin_flagged_ <= 16) {
      std::string from;
      for (uint32_t i = 0; i < named; ++i) {
        from += fmt::format("p{}<-{:08X} ", from_page[i], from_addr[i]);
      }
      // These are FETCHED offsets, computed with the same GetTiledOffset2D calls DetileMip2D
      // makes -- not the declared extent. The alignment gaps, which the XDK says the guest packs
      // other data into, are excluded by construction. So foreign content counted here is data
      // reaching real texels, which is the artifact itself rather than a map of guest packing.
      REXGPU_WARN("nx1_d3d9: PAGEORIGIN MIXED {:08X} {}x{} fmt={} tiled={} pitch_px={} -- {} of {} "
                  "pages WE ACTUALLY READ come from {} other address(es) [{}]. Not a relocation "
                  "(that moves ALL pages from ONE donor) and not an alignment gap: this slot holds "
                  "parts of more than one image",
                  t.base_address, t.width, height, t.format, t.tiled ? 1 : 0, t.pitch_pixels,
                  foreign, pages, distinct, from);
      // DID THE COPY THAT FILLED THIS SLOT COVER THE WHOLE TEXTURE? The masks show half the pages
      // holding the previous occupant, which is what an UNDER-SIZED copy produces. The delayed
      // path takes its size from the low half of a packed ImageAllocInfo; the high half is the
      // other candidate, and the choice was verified against exactly one destination. If bytes <
      // the texture's extent here -- and especially if `alt` matches that extent -- the size
      // choice is wrong for this class of image and that is the whole bug.
      // WERE THE FOREIGN PAGES EVER WRITTEN FOR THIS TEXTURE? page_writes_ counts everything we
      // can see: guest CPU writes via the watch, plus our own mirrored copies. Comparing the
      // foreign pages against this texture's OWN pages separates the two remaining explanations,
      // which no other measurement here can tell apart:
      //   foreign pages ~0 writes, own pages > 0  -> that part of the texture NEVER ARRIVED. The
      //     slot still holds the previous occupant because nothing ever delivered the rest.
      //   foreign pages written as much as own    -> the data DID arrive and is simply wrong,
      //     which puts this back on identity rather than delivery.
      uint64_t fw = 0, ow = 0;
      uint32_t fn = 0, on = 0;
      for (const uint32_t pg : touched) {
        const uint32_t w = PageWriteCount(uint32_t(self + (size_t(pg) << 12)));
        const bool bad =
            std::find(foreign_pages.begin(), foreign_pages.end(), pg) != foreign_pages.end();
        if (bad) { fw += w; ++fn; } else { ow += w; ++on; }
      }
      // NOT A COUNT -- a saturated flag. The write-watch fires on a page's FIRST write and then
      // leaves the page writable, so every written page reports exactly 1 forever. Measured: these
      // read `1 over 1`, `18 over 18`, `16 over 16` -- foreign and own alike, always equal. The
      // earlier reading of this line ("foreign~0 with own>0 means that part NEVER ARRIVED") could
      // therefore never have discriminated anything, and is retracted. Kept only as a
      // has-this-page-ever-been-CPU-written bit, which is how the unexplained-content test below
      // uses it.
      REXGPU_WARN("nx1_d3d9:   ^ pages ever CPU-written (saturating flag, NOT a count): foreign "
                  "{} of {}, own {} of {}",
                  fw, fn, ow, on);
      // PER-PAGE COVERAGE, asked of the FOREIGN pages themselves.
      //
      // This previously looked up the texture's BASE page and printed the answer as if it were
      // about the foreign ones. For a texture whose foreign pages sit 12 pages in, that is simply a
      // different question, and it is where "no mirrored copy recorded" -- 14 of the 16 sampled
      // verdicts -- came from. Two other suspected defects were checked and cleared: the map's
      // 400,000-entry cap cannot bind (the destination histogram spans 16 x 16 MB regions, so the
      // map tops out near 65,536 pages, and it now warns if it ever does), and coverage being
      // recorded before MirrorDmaCopy's size gate is latent only, since DMAGATE measures zero.
      //
      // The three outcomes are genuinely different diagnoses:
      //   NEVER TARGETED -- no image-cache copy ever addressed this page. Expected and BENIGN for a
      //     streamer-loaded texture (DB_ReadStreamFile writes texels straight into the pool with a
      //     CPU copy; regions 06x/07x take zero DMA all run). Only meaningful for a moved image.
      //   SPANNED, NEVER WRITTEN -- a copy covered this page and our per-page empty-source skip
      //     declined it, leaving the previous occupant. That is a hole we create.
      //   WRITTEN -- we did land bytes here and they are still another texture's, which moves the
      //     fault upstream to what the SOURCE held.
      // COVERAGE OF THE FOREIGN PAGES. `bpb * blocks_read / 4096` is the fraction of the page our
      // decode actually consumes. A foreign verdict on a page we read 10% of is very weak evidence
      // of a mixed slot and strong evidence of ordinary tile-padding packing; a foreign verdict on
      // a page we read in full is the real thing. Reported as a distribution because the geometry
      // predicts the difference: on 06FD0000 (300x152, 38 block rows) every foreign page sat in
      // tile row 1, which spans block rows 32..63 and is therefore 82% padding.
      uint32_t low_cov = 0, full_cov = 0, min_pct = 100, max_pct = 0;
      for (const uint32_t pg : foreign_pages) {
        const auto it = std::find(touched.begin(), touched.end(), pg);
        if (it == touched.end()) {
          continue;
        }
        const uint32_t blocks = touched_blocks[size_t(it - touched.begin())];
        const uint32_t pct = std::min<uint32_t>(100, (blocks * bpb * 100) / 4096);
        if (pct < 50) ++low_cov; else ++full_cov;
        min_pct = std::min(min_pct, pct);
        max_pct = std::max(max_pct, pct);
      }
      if (!foreign_pages.empty()) {
        REXGPU_WARN("nx1_d3d9:   ^ foreign-page COVERAGE: {} of {} are pages we read LESS THAN HALF "
                    "of ({}%..{}%). Low coverage means the page is shared with the guest's tile-"
                    "padding packing and the bytes we actually consume may be entirely ours -- only "
                    "the fully-read ones ({}) are evidence of a genuinely mixed slot",
                    low_cov, foreign_pages.size(), min_pct, max_pct, full_cov);
      }
      uint32_t never = 0, spanned = 0, written = 0;
      for (const uint32_t pg : foreign_pages) {
        switch (DmaPageVerdictFor(uint32_t(self + (size_t(pg) << 12)))) {
          case 0: ++never; break;
          case 1: ++spanned; break;
          default: ++written; break;
        }
      }
      uint32_t own_written = 0, own_total = 0;
      for (const uint32_t pg : touched) {
        if (std::find(foreign_pages.begin(), foreign_pages.end(), pg) != foreign_pages.end()) {
          continue;
        }
        ++own_total;
        if (DmaPageVerdictFor(uint32_t(self + (size_t(pg) << 12))) == 2) ++own_written;
      }
      REXGPU_WARN("nx1_d3d9:   ^ FOREIGN pages by coverage: {} never targeted by any copy, {} "
                  "spanned but SKIPPED (empty source), {} actually written. Control -- this "
                  "texture's OWN pages: {} of {} written. Read the CONTROL first: own pages also "
                  "0 means this texture is not DMA-delivered at all (the streamer wrote it), so "
                  "'never targeted' is expected and benign",
                  never, spanned, written, own_written, own_total);
      // WHO WROTE THESE BYTES AT ALL? Three mechanisms can put content at a guest address on our
      // side: the guest's own CPU write (the write-watch sees it), our DMA mirror (the coverage
      // map's `wrote`), and a resolve we performed. A page that holds NON-ZERO content while none
      // of the three touched it was filled by something outside our model -- which on hardware
      // means the GPU, i.e. a render-to-texture we never observed. That is the one form of the
      // EDRAM hypothesis this side can test without the reference CP running.
      //
      // KNOWN FALSE POSITIVE, do not read a small count as proof: the write-watch is armed per
      // range, so a page the guest wrote BEFORE we armed it reads as never-written. Treat only a
      // large, persistent count as signal, and confirm with RangeWrittenByGpu before believing it.
      uint32_t unexplained = 0;
      for (const uint32_t pg : foreign_pages) {
        const uint32_t page_addr = uint32_t(self + (size_t(pg) << 12));
        if (PageWriteCount(page_addr) || DmaPageVerdictFor(page_addr) == 2 ||
            OverlapsResolveDest(page_addr, 4096)) {
          continue;
        }
        const size_t off = size_t(pg) << 12;
        if (off + 4096 > guest_bytes) {
          continue;
        }
        bool nonzero = false;
        for (uint32_t i = 0; i < 4096 && !nonzero; i += 8) {
          nonzero = src[off + i] != 0;
        }
        if (nonzero) {
          ++unexplained;
        }
      }
      if (unexplained) {
        REXGPU_WARN("nx1_d3d9:   ^ {} of {} foreign pages hold content NO KNOWN WRITER produced "
                    "(not CPU-written, not DMA-written, not a resolve destination). Candidate for "
                    "a render-to-texture we never observed -- but the write-watch arms late, so "
                    "confirm against the reference's RangeWrittenByGpu before believing it",
                    unexplained, foreign_pages.size());
      }
    }
    // THE SHAPE, not just the count. One pixel per block: red where that block's bytes came from
    // another texture's page, green where they came from this texture's own. Compare the shape
    // against the corruption on screen -- that is the step no amount of address arithmetic can
    // substitute for, and it is what decides whether this 8.8% is the artifact.
    // Arm the image dump for THIS decode, and force the mask out alongside it -- an image without
    // its mask cannot be read block for block, and a mask without its image is what this
    // investigation already has six revisions of.
    if (REXCVAR_GET(nx1_d3d9_dbg_dump_mixed) > 0) {
      pageorigin_dump_this_ = true;
    }
    if (const uint32_t budget = REXCVAR_GET(nx1_d3d9_dbg_pageorigin_dump);
        budget || pageorigin_dump_this_) {
      if (budget) {  // guard: the mask is also forced when dump_mixed armed it, and budget is 0 then
        REXCVAR_SET(nx1_d3d9_dbg_pageorigin_dump, budget - 1);
      }
      const uint32_t bw = block_width, bh = block_height;
      std::vector<Rgba8> mask(size_t(bw) * bh);
      const uint32_t bpb_log2 = Log2Exact(bpb);
      for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
          size_t off;
          if (t.tiled) {
            off = size_t(uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                int32_t(bx), int32_t(by), block_pitch_h, bpb_log2)));
          } else {
            off = size_t(by) * block_pitch_h * bpb + size_t(bx) * bpb;
          }
          const uint32_t pg = uint32_t(off >> 12);
          const bool bad =
              std::find(foreign_pages.begin(), foreign_pages.end(), pg) != foreign_pages.end();
          mask[size_t(by) * bw + bx] =
              bad ? Rgba8{255, 40, 40, 255} : Rgba8{40, 160, 40, 255};
        }
      }
      char p[256];
      std::snprintf(p, sizeof(p), "texdump/prov_%08X_f%llu.bmp", t.base_address,
                    static_cast<unsigned long long>(frame_));
      DumpRgbaBmp(p, mask, bw, bh);
      REXGPU_WARN("nx1_d3d9: PAGEORIGIN dumped provenance mask -> {} ({}x{} blocks, red = read "
                  "from another texture)",
                  p, bw, bh);
    }
  }
  // Hand the flagged pages to the detiler for this texture only. Whole-texture relocations are
  // deliberately excluded: painting those would light up half the world in magenta and drown the
  // case under test.
  if (REXCVAR_GET(nx1_d3d9_dbg_paint_foreign) && !whole_texture_move && foreign) {
    paint_pages_ = foreign_pages;
    // Name the textures being painted, so "no magenta" can be checked against what was actually
    // marked rather than guessed at -- a painted texture that is simply not on screen looks
    // identical to a paint path that never ran.
    if (g_painted_textures.fetch_add(1, std::memory_order_relaxed) < 12) {
      REXGPU_WARN("nx1_d3d9: PAINT marking {:08X} {}x{} fmt={} -- {} of {} pages foreign; look for "
                  "magenta on whatever surface uses this texture",
                  t.base_address, t.width, height, t.format, foreign, pages);
    }
  } else {
    paint_pages_.clear();
  }
  // Per-decode blank tally. `touched` is the set of pages a fetch really reads, so this fraction is
  // "how much of the image we are about to decode is a hole" -- the number the screen actually
  // shows, and the one MIXED cannot express.
  ++blank_decodes_total_;
  if (blank_this_decode_total && blank_this_decode * 4 >= blank_this_decode_total) {
    ++blank_decodes_;  // at least a quarter of the bytes we read are zero
  }
  if (unwritten_this_decode) {
    ++unwritten_decodes_;
  }
  ++pageorigin_decodes_;
  if ((pageorigin_decodes_ % 2000) == 0) {
    // THE METRIC THAT SHOULD TRACK THE ARTIFACT. Read this one, not MIXED: the decoded-image dumps
    // showed holes are the dominant corruption and MIXED is structurally blind to them. Unlike
    // MIXED -- pinned at 7-10% through every intervention -- this should MOVE when the underlying
    // delivery changes. If it does not move either, the fault is not in what reaches guest RAM.
    REXGPU_WARN("nx1_d3d9: BLANK {} of {} sampled bytes a fetch actually reads are ZERO ({:.1f}%), "
                "and {} of {} decodes are at least a QUARTER zero ({:.1f}%). "
                "These are holes in guest memory -- verified against LIVE RAM with the mirror off, "
                "so not a snapshot artifact",
                blank_pages_, blank_read_pages_,
                blank_read_pages_ ? 100.0 * double(blank_pages_) / double(blank_read_pages_) : 0.0,
                blank_decodes_, blank_decodes_total_,
                blank_decodes_total_ ? 100.0 * double(blank_decodes_) / double(blank_decodes_total_)
                                     : 0.0);
    // THE ONE TO ACT ON. The line above is CONTENT, and content has been wrong three times here --
    // transparent art reads identically to a hole. This crosses it with write history: of the pages
    // that are predominantly zero, how many did NOTHING ever write? Only those are undelivered.
    // If holes ~= 0 while candidates is large, the zeros are legitimate art and delivery is fine.
    REXGPU_WARN("nx1_d3d9: UNWRITTEN {} of {} predominantly-zero pages were NEVER WRITTEN by "
                "anything ({:.1f}%), across {} decodes. Holes ~0 with many candidates means the "
                "zeros are legitimate transparent/dark art and delivery is NOT the problem",
                unwritten_holes_, unwritten_candidates_,
                unwritten_candidates_
                    ? 100.0 * double(unwritten_holes_) / double(unwritten_candidates_)
                    : 0.0,
                unwritten_decodes_);
    REXGPU_WARN("nx1_d3d9: PAINT {} blocks marked across {} textures (enabled={}). Zero blocks "
                "means the marker never reached a decode -- the instrument, not the theory. "
                "Nonzero with no magenta on screen means those textures were not in view",
                g_painted_blocks.load(), g_painted_textures.load(),
                REXCVAR_GET(nx1_d3d9_dbg_paint_foreign) ? 1 : 0);
    REXGPU_WARN("nx1_d3d9: PAGEORIGIN {} of {} decodes are MIXED (pages from several images at "
                "fetched offsets) -- {} more are whole-texture RELOCATIONS, which are benign and "
                "expected. Mixed==0 means every slot we decode holds exactly one image and the "
                "fault lies downstream of the read ({} uniform pages excluded as unprovable)",
                pageorigin_flagged_, pageorigin_decodes_, pageorigin_relocated_,
                pageorigin_uniform_skipped_);
  }
}

void ResourceTracker::InvalidateMirrorPages(uint32_t phys, uint32_t bytes) {
  if (!bytes || !mirror_ || mirror_valid_.empty()) {
    return;
  }
  const uint32_t p0 = phys >> 12;
  const uint32_t p1 = (phys + bytes - 1) >> 12;
  for (uint32_t p = p0; p <= p1 && (p >> 6) < mirror_valid_.size(); ++p) {
    mirror_valid_[p >> 6] &= ~(uint64_t(1) << (p & 63));
    if ((p >> 6) < watch_armed_.size()) {
      watch_armed_[p >> 6] &= ~(uint64_t(1) << (p & 63));
    }
  }
}

void ResourceTracker::MirrorInvalidateAll() {
  // Without this the re-decode would re-read the SNAPSHOT, which is exactly the data under
  // suspicion. Dropping validity forces MirrorSnapshot to re-copy from guest memory.
  for (auto& w : mirror_valid_) w = 0;
  for (auto& w : watch_armed_) w = 0;
}

void ResourceTracker::ArmWriteWatch(uint32_t phys_addr, uint32_t len) {
  if (!phys_base_ || !len || uint64_t(phys_addr) + len > (uint64_t(kMirrorPages) << 12)) {
    return;
  }
  if (watch_armed_.size() != mirror_valid_.size()) {
    watch_armed_.assign(mirror_valid_.size(), 0);
  }
  auto* mem = rex::system::kernel_state()->memory();
  if (!mem) {
    return;
  }
  // WE ARE NOT THE ONLY OWNER OF THIS PAGE PROTECTION. The reference's SharedMemory arms the same
  // physical pages (shared_memory.cpp:412) and its own invalidation callback can unprotect them,
  // with bookkeeping entirely separate from watch_armed_. Our bit therefore records "we asked for
  // this page once", not "the OS is currently trapping writes to it" -- and when those diverge the
  // early-out below makes the divergence PERMANENT: we skip re-arming because we believe we are
  // armed, so no write is ever seen, so DrainMemoryWrites never clears the bit to make us re-arm.
  //
  // That deadlock is the shape of the measurement: with the counter finally fixed, 64.5% of
  // texture content changes show no write to their pages, 57.5% of those more than 60 frames
  // apart (so not frame-boundary lag) -- while the same pages carry nonzero cumulative counts,
  // i.e. they WERE trapped early on and then stopped being.
  //
  // Re-arming unconditionally costs a call per 4 KB page per decode (1024 for a 4 MB texture),
  // which is why it is a cvar rather than the default. If the blind fraction collapses with this
  // on, the stale-armed-bit deadlock is the mechanism.
  const bool force = REXCVAR_GET(nx1_d3d9_rearm_watch);
  const uint32_t p0 = phys_addr >> 12;
  const uint32_t p1 = (phys_addr + len - 1) >> 12;
  for (uint32_t p = p0; p <= p1; ++p) {
    const uint64_t bit = uint64_t(1) << (p & 63);
    if (force || !(watch_armed_[p >> 6] & bit)) {
      watch_armed_[p >> 6] |= bit;
      mem->EnablePhysicalMemoryAccessCallbacks(p << 12, 4096, true, false);
      rearm_calls_ += force ? 1 : 0;
    }
  }
}

const uint8_t* ResourceTracker::MirrorSnapshot(uint32_t phys_addr, uint32_t len) {
  if (!mirror_ || !phys_base_ || uint64_t(phys_addr) + len > (uint64_t(kMirrorPages) << 12)) {
    return TranslatePhysical(phys_addr);
  }
  const uint32_t p0 = phys_addr >> 12;
  const uint32_t p1 = (phys_addr + len - 1) >> 12;
  for (uint32_t p = p0; p <= p1; ++p) {
    if (!(mirror_valid_[p >> 6] & (uint64_t(1) << (p & 63)))) {
      // VALIDATE AND ARM THE WATCH **BEFORE** THE COPY, NOT AFTER.
      //
      // This ordering is the reference's, and its header states the reason outright
      // (rexglue-sdk/include/rex/graphics/shared_memory.h:135-140): "MakeRangeValid must be called
      // for each successfully uploaded range as early as possible, BEFORE the memcpy, to make sure
      // invalidation that happened during the CPU -> GPU memcpy isn't missed."
      //
      // We had it inverted, which opened two windows the reference does not have:
      //   1. During the memcpy the page was UNPROTECTED, so a guest write landing mid-copy raised
      //      no fault at all -- we captured a torn page and were not told.
      //   2. We then SET the valid bit, so the torn page was cached as good, and the watch was
      //      armed only afterwards.
      // The result is a torn page LOCKED IN as valid, wrong until something unrelated happens to
      // invalidate it. That is the artifact's exact shape: a surface that is wrong persistently and
      // then "resolves when you get close", which is content_probe forcing the rebuild.
      //
      // With this order a write racing the copy faults against the protection we just armed, and
      // the callback clears the bit we just set -- so the page is simply re-copied next request.
      // A torn read can no longer become permanent; it becomes self-healing, which is precisely
      // what makes the reference immune while reading the very same memory.
      //
      // Our own read below does not fault: the protection is read-only, and this is a read.
      mirror_valid_[p >> 6] |= (uint64_t(1) << (p & 63));
      if (auto* mem = rex::system::kernel_state()->memory()) {
        mem->EnablePhysicalMemoryAccessCallbacks(p << 12, 4096, true, false);
      }
      std::memcpy(mirror_ + (size_t(p) << 12), phys_base_ + (size_t(p) << 12), 4096);
      // TORN-PAGE TEST. This copy runs on the render thread with NOTHING synchronising it
      // against the guest thread, which may be part-way through writing this very page --
      // invalidations are queued and drained later, so nothing here waits for a write to finish.
      // A page captured mid-write is half old bytes and half new, which is spatially patterned
      // corruption that differs on every decode: exactly the observed flicker, where a surface
      // alternates between correct and speckled in BOTH directions. A timing race would heal one
      // way and stay healed, so tearing fits the symptom and a race no longer does.
      //
      // Copy twice and compare. Differing copies mean the page changed WHILE WE READ IT, which
      // is proof rather than inference -- and it is the one explanation left that predicts a
      // non-deterministic decode from a fixed address.
      if (REXCVAR_GET(nx1_d3d9_dbg_tornpages)) {
        alignas(16) static thread_local uint8_t second[4096];
        std::memcpy(second, phys_base_ + (size_t(p) << 12), 4096);
        ++torn_checked_;
        if (std::memcmp(mirror_ + (size_t(p) << 12), second, 4096) != 0) {
          ++torn_pages_;
          if (torn_pages_ <= 8) {
            REXGPU_WARN("nx1_d3d9: TORNPAGE physical page {:05X} ({:08X}) changed between two "
                        "back-to-back copies -- the guest is writing it as we snapshot it",
                        p, p << 12);
          }
        }
      }
      // NOTHING re-validates here. The bit was set before the copy on purpose: if the guest wrote
      // this page while we were copying it, the invalidation callback has already cleared that bit
      // and re-setting it now would reintroduce exactly the bug this ordering fixes.
      //
      // AND THAT IS MEASURABLE. If the bit is gone by the time the copy finishes, a guest write
      // raced us: under the OLD ordering that page was captured torn and then cached as valid --
      // wrong until something unrelated invalidated it. Under this ordering it is simply re-copied
      // on the next request. So this counter is the frequency of the bug that was just fixed, and
      // a zero means the race does not happen and the fix, while correct, changes nothing.
      {
        static std::atomic<uint64_t> copies{0}, raced{0};
        const uint64_t n = copies.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!(mirror_valid_[p >> 6] & (uint64_t(1) << (p & 63)))) {
          raced.fetch_add(1, std::memory_order_relaxed);
        }
        if ((n % 500000) == 1) {
          const uint64_t r = raced.load(std::memory_order_relaxed);
          REXGPU_WARN("nx1_d3d9: MIRRORRACE {} page copies, {} ({:.4f}%) were INVALIDATED WHILE "
                      "BEING COPIED -- the guest wrote the page mid-snapshot. Each of these was a "
                      "torn page that the old validate-after-copy ordering cached as good; they are "
                      "now re-read instead. Zero means the race does not occur and this fix, though "
                      "correct, changes nothing",
                      n, r, 100.0 * double(r) / double(n));
        }
      }
    }
  }
  return mirror_ + phys_addr;
}

// Apply queued guest writes on the render thread: invalidate the mirror pages they touched so the
// next MirrorSnapshot re-copies fresh bytes, and dirty any texture entry whose watched range
// overlaps so it re-decodes. This is how a texture that legitimately reloads (or whose slot is
// repurposed) picks up its new bytes, while an untouched texture is held from the churn.
namespace { std::atomic<uint64_t>* g_drain_matched = nullptr; }

void ResourceTracker::DrainMemoryWrites() {
  std::vector<PendingWrite> writes;
  {
    std::lock_guard<std::mutex> lk(dirty_mu_);
    writes.swap(writes_pending_);
  }
  if (writes.empty()) return;
  // THE GAP. gpu_write_ranges_ says the reference reported 49,379 GPU writes; REBUILDSRC says zero
  // rebuilds came from them. Everything between those two numbers was unmeasured, so count it:
  // how many ranges of each source reach the drain, and how many of them match ANY live texture.
  // A source that arrives but never matches is an address-space or watch-range problem, not a
  // "nothing to invalidate" one.
  {
    static std::atomic<uint64_t> drained[size_t(DirtySource::kCount)]{};
    static std::atomic<uint64_t> matched[size_t(DirtySource::kCount)]{};
    static std::atomic<uint64_t> reports{0};
    for (const PendingWrite& w : writes) {
      if (size_t(w.source) < size_t(DirtySource::kCount)) {
        drained[size_t(w.source)].fetch_add(1, std::memory_order_relaxed);
      }
    }
    g_drain_matched = matched;  // consumed by the match test below
    if ((reports.fetch_add(1, std::memory_order_relaxed) % 600) == 1) {
      static const char* kSrc[] = {"cpu", "gpu", "dma", "probe", "mip", "recheck", "resolve", "partial", "dbg"};
      std::string line;
      for (size_t i = 0; i < size_t(DirtySource::kCount); ++i) {
        line += fmt::format("{}={}/{} ", kSrc[i],
                            matched[i].load(std::memory_order_relaxed),
                            drained[i].load(std::memory_order_relaxed));
      }
      REXGPU_WARN("nx1_d3d9: DRAINHIT matched/drained ranges per source: {}-- a source with a large "
                  "drained count and zero matched is arriving but never overlapping a live "
                  "texture's watch range: an address-space or watch-range bug, not an absence of "
                  "work",
                  line);
    }
  }
  for (const PendingWrite& w : writes) {
    const uint32_t addr = w.addr, len = w.len;
    if (uint64_t(addr) + len > (uint64_t(kMirrorPages) << 12)) continue;
    const uint32_t p0 = addr >> 12;
    const uint32_t p1 = (addr + len - 1) >> 12;
    if (page_writes_.size() != size_t(kMirrorPages)) page_writes_.assign(kMirrorPages, 0);
    for (uint32_t p = p0; p <= p1; ++p) {
      if (p < page_writes_.size() && page_writes_[p] < 255) ++page_writes_[p];
      const uint64_t bit = uint64_t(1) << (p & 63);
      mirror_valid_[p >> 6] &= ~bit;  // invalidate: re-copy on next request
      // Disarm too, so the next read RE-ARMS the callback. The guest access callback fires
      // once and then leaves the page writable, so a watch that is armed only once catches
      // only the FIRST write of a multi-write upload -- we decode the partially-written
      // texture and never hear about the rest of it. MirrorSnapshot re-arms as a side effect
      // of re-copying; the live-read path has no copy, so it needs this.
      if ((p >> 6) < watch_armed_.size()) {
        watch_armed_[p >> 6] &= ~bit;
      }
    }
  }
  if (const uint32_t track = REXCVAR_GET(nx1_d3d9_dbg_track_addr); track) {
    // Span of the tracked texture, so writes to ANY of its pages show up -- the base page
    // alone tells us nothing about a texture that covers 32 of them.
    // One page is the FALLBACK when no entry claims this address, not a measurement. Say so in
    // the log: reading the placeholder as the texture's real size is what produced a phantom
    // "watch_size is far too small" defect that cost a round of investigation.
    uint32_t span = 4096;
    bool span_known = false;
    if (auto* tex_map = static_cast<TextureMap*>(textures_)) {
      for (auto [key, e] : *tex_map) {
        if (e.watch_addr == track && e.watch_size) {
          span = e.watch_size;
          span_known = true;
          break;
        }
      }
    }
    for (const PendingWrite& w : writes) {
      const uint32_t addr = w.addr, len = w.len;
      if (addr < track + span && addr + len > track) {
        REXGPU_INFO("nx1_d3d9: TRACK {:08X} WRITE frame={} range={:08X}+{} (+{} into a {} byte "
                    "window, {})",
                    track, frame_, addr, len, addr > track ? addr - track : 0, span,
                    span_known ? "the entry's real watch_size" : "ASSUMED 1 page, no entry found");
      }
    }
  }

  auto* textures = static_cast<TextureMap*>(textures_);
  if (!textures) return;
  if (const uint32_t track = REXCVAR_GET(nx1_d3d9_dbg_track_addr); track) {
    bool claimed = false;
    for (auto [key, e] : *textures) {
      if (e.watch_addr == track && e.watch_size) {
        claimed = true;
        break;
      }
    }
    if (!claimed) {
      for (const PendingWrite& w : writes) {
        const uint32_t addr = w.addr, len = w.len;
        if (addr <= track && track < addr + len) {
          REXGPU_INFO("nx1_d3d9: TRACK {:08X} WRITE with NO cache entry watching it (frame {})",
                      track, frame_);
          break;
        }
      }
    }
  }
  // One-shot global invalidate. Placed here so it uses the same path a guest write would.
  if (REXCVAR_GET(nx1_d3d9_dbg_redecode_all)) {
    REXCVAR_SET(nx1_d3d9_dbg_redecode_all, false);
    uint32_t n = 0;
    for (auto [key, entry] : *textures) {
      entry.dirty = true;
      entry.committed = false;
      entry.good_frames = 0;
      entry.zero_retries = 0;
      entry.retry_frame = 0;
      ++n;
    }
    MirrorInvalidateAll();
    REXGPU_INFO("nx1_d3d9: REDECODE forced {} cached textures to re-decode from current memory", n);
  }
  {
    static std::atomic<uint32_t> shown{0};
    if (shown.load(std::memory_order_relaxed) < 8) {
      for (auto [k2, e2] : *textures) {
        if (!e2.watch_size) continue;
        if (shown.fetch_add(1, std::memory_order_relaxed) >= 8) break;
        REXGPU_WARN("nx1_d3d9: TEXRANGE {:08X}..{:08X} ({} KB) -- compare against GPUWRITE-RANGE",
                    e2.watch_addr, e2.watch_addr + e2.watch_size, e2.watch_size / 1024);
      }
    }
  }
  for (auto [key, entry] : *textures) {
    if (!entry.watch_size) continue;
    const uint32_t a0 = entry.watch_addr;
    const uint32_t a1 = entry.watch_addr + entry.watch_size;
    // Guest-mip textures watch a second range: their levels were decoded from mip_address, and
    // the mip pool streams into that memory exactly as the base pool does into the base.
    const uint32_t m0 = entry.mip_watch_addr;
    const uint32_t m1 = entry.mip_watch_addr + entry.mip_watch_size;
    // Settled texture: frozen against the pool's later garbage. The cvar is honoured HERE, at the
    // point of use, not only at the commit site -- `committed` is one-way and survives the cvar
    // going false, so gating only the commit meant toggling it off mid-session left every
    // already-frozen texture frozen. That made the toggle look like a null result when it had
    // simply never reached the entries under investigation.
    // A guest write ALWAYS invalidates, exactly as the reference does -- it tracks per-range
    // validity and reloads on write, and has no notion of freezing a texture at all.
    //
    // The freeze was meant to protect a good decode from the pool recycling the slot underneath
    // it, but it cannot tell that case from the one it caused: a texture decoded from a slot the
    // pool had NOT yet filled is frozen as garbage for the session. Measured directly -- source
    // 0 -> 8151/8192 nonzero AFTER commit, then dirty=0 committed=1 forever, never re-decoded.
    // And it cannot be fixed by inspecting content: a recycled slot holds the PREVIOUS texture's
    // bytes, which are complete and non-zero, so "is the source populated" answers yes.
    if (entry.committed && REXCVAR_GET(nx1_d3d9_commit_textures) &&
        !REXCVAR_GET(nx1_d3d9_writes_always_invalidate)) {
      continue;
    }
    // Arming proof for the commit-freeze experiment. A frozen entry reaching the dirty test only
    // happens with the cvar off, so a non-zero count is direct evidence the toggle took effect on
    // real entries. Zero means the experiment did NOT run and a null visual result says nothing --
    // the failure mode that has already voided this test once.
    if (entry.committed) ++unfrozen_writes_;
    for (const PendingWrite& w : writes) {
      const uint32_t addr = w.addr, len = w.len;
      const bool hit_base = addr < a1 && addr + len > a0;
      const bool hit_mips = entry.mip_watch_size != 0 && addr < m1 && addr + len > m0;
      if (hit_base || hit_mips) {
        if (g_drain_matched && size_t(w.source) < size_t(DirtySource::kCount)) {
          g_drain_matched[size_t(w.source)].fetch_add(1, std::memory_order_relaxed);
        }
        entry.dirty = true;
        entry.dirty_source = w.source;
        entry.write_seen_since_decode = true;
        if (const uint32_t track = TrackedMatch(entry.watch_addr); track) {
          REXGPU_INFO("nx1_d3d9: TRACK {:08X} DIRTIED frame={} by write {:08X}+{} ({} {}+{})",
                      track, frame_, addr, len, hit_base ? "watch" : "MIP watch",
                      hit_base ? entry.watch_addr : entry.mip_watch_addr,
                      hit_base ? entry.watch_size : entry.mip_watch_size);
        }
        break;
      }
    }
  }
}

void ResourceTracker::ReportEffectCandidates() {
  std::vector<NewTex> v = baseline_new_;
  std::sort(v.begin(), v.end(),
            [](const NewTex& a, const NewTex& b) { return a.episodes > b.episodes; });
  NX1_LOGI_MISC("nx1_d3d9: EFFECTS {} newcomers since baseline, ranked by appearances",
              v.size());
  for (size_t i = 0; i < v.size() && i < 25; ++i) {
    const auto& n = v[i];
    NX1_LOGI_MISC("nx1_d3d9: EFFECTS {:08X} {}x{} fmt={} sampler={} appearances={} frames_bound={}",
                n.addr, n.width, n.height, n.format, n.sampler, n.episodes, n.frames_bound);
  }
}

void ResourceTracker::LearnTextureBaseline(uint32_t frames) {
  baseline_frames_ = frames;
  baseline_ready_ = false;
  baseline_addrs_.clear();
  baseline_new_.clear();
  NX1_LOGI_TEX("nx1_d3d9: NEWTEX learning baseline over {} frames -- keep the effect OFF screen",
              frames);
}

/// Re-read every watched address from LIVE guest memory and write out each new distinct version.
/// See AddrWatch: this answers whether the right bytes are EVER at that address.
void ResourceTracker::PollAddrWatch() {
  if (!addr_watch_frames_ || addr_watch_.empty()) {
    return;
  }
  --addr_watch_frames_;
  for (auto& w : addr_watch_) {
    if (uint64_t(PhysicalAddress(w.addr)) + w.bytes > (uint64_t(kMirrorPages) << 12)) {
      continue;
    }
    const uint8_t* live = TranslatePhysical(w.addr);
    if (!live) {
      continue;
    }
    const uint64_t h = XXH3_64bits(live, w.bytes);
    if (h == w.last_hash) {
      continue;
    }
    w.last_hash = h;
    if (w.versions >= 12) {
      continue;  // bounded: 12 distinct versions is plenty to find a clean one
    }
    char path[256];
    std::snprintf(path, sizeof(path), "texdump/watch_%08X_%ux%u_f%u_v%02u.bin", w.addr, w.w, w.h,
                  w.fmt, w.versions);
    if (FILE* f = std::fopen(path, "wb")) {
      std::fwrite(live, 1, w.bytes, f);
      std::fclose(f);
    }
    REXGPU_WARN("nx1_d3d9: ADDRWATCH {:08X} {}x{} fmt={} version {} written (content changed at "
                "this address)",
                w.addr, w.w, w.h, w.fmt, w.versions);
    ++w.versions;
  }
}

void ResourceTracker::ArmFrameCapture(uint32_t frames) {
  ++framecap_batch_;
  framecap_count_ = 0;
  framecap_seen_.clear();
  // Watch the same textures this capture is about to record, for far longer than the capture
  // itself, so their whole content history is on disk beside the one decode we took.
  //
  // DELIBERATELY NOT CLEARED on a re-arm. The intended workflow is: capture a surface while it is
  // speckled at distance, then WALK UP until it resolves and capture again. Clearing here would
  // drop the far-away texture's watch at exactly the moment its address is most interesting --
  // whether that address ever receives clean content is the whole question. Entries accumulate,
  // deduplicated by address, and every arm restarts the countdown so a long approach stays covered.
  addr_watch_frames_ = 3000;  // ~60 s at 50 fps: enough to cross a map on foot
  REXCVAR_SET(nx1_d3d9_dbg_dump_frame, frames);
  REXGPU_WARN("nx1_d3d9: FRAMECAP armed -- batch {}, capturing every distinct BC texture decoded "
              "over the next {} frames to texdump/c{:02}_*",
              framecap_batch_, frames, framecap_batch_);
}

void ResourceTracker::AdvanceFrame() {
  // The reference's SharedMemory is built in SetupContext, which may not have run when we
  // initialised. Cheap no-op once it has taken.
  RegisterGpuWriteWatch();
  PollAddrWatch();
  // Frame-capture countdown. Reports on expiry so an armed capture that caught nothing is
  // distinguishable from one that was never armed.
  if (const uint32_t fl = REXCVAR_GET(nx1_d3d9_dbg_dump_frame); fl) {
    REXCVAR_SET(nx1_d3d9_dbg_dump_frame, fl - 1);
    if (fl == 1) {
      // 0 here means every texture on screen was served from cache without re-decoding, which is
      // what a settled scene does -- press again after moving, or raise kCaptureFrames.
      REXGPU_WARN("nx1_d3d9: FRAMECAP batch {} finished -- {} distinct textures dumped to "
                  "texdump/c{:02}_*, {} addresses under watch",
                  framecap_batch_, framecap_count_, framecap_batch_, addr_watch_.size());
    }
  }
  // Apply any autotrack request posted by a decode. Main thread only -- see the member's note.
  if (const uint32_t req = autotrack_request_.exchange(0, std::memory_order_relaxed); req) {
    if (req == kAutotrackRelease) {
      REXCVAR_SET(nx1_d3d9_dbg_track_addr, 0u);
      autotrack_latched_ = 0;
      REXGPU_WARN("nx1_d3d9: AUTOTRACK released -- that texture decoded COMPLETE, so it healed "
                  "and is not the permanent case. Hunting the next candidate");
    } else {
      REXCVAR_SET(nx1_d3d9_dbg_track_addr, req);
      // Remember that AUTOTRACK owns this latch, so the frame poll may release it. A latch the
      // operator set by hand from the overlay must stay put.
      autotrack_latched_ = req;
      // Size the DMA watch window to THIS texture -- see autotrack_span_ for why neither a small
      // nor a large fixed span works.
      if (const uint32_t span = autotrack_span_.exchange(0, std::memory_order_relaxed)) {
        REXCVAR_SET(nx1_d3d9_dbg_track_bytes, span);
        NX1_LOGW_TEX("nx1_d3d9: AUTOTRACK watch window set to {} bytes to match the texture, so a "
                    "copy landing anywhere in its base is visible (a fixed 64 KB window covered "
                    "only the first quarter of a 512x512 BC base)",
                    span);
      }
    }
  }


  // Poll the tracked address every frame and report whenever its populated-byte count changes.
  // We only ever sampled it at bind time; if the guest fills this memory and clears it again
  // between binds, a snapshot at bind time cannot tell that apart from "never written", and
  // the reference backend -- which holds its own GPU-side copy of guest memory -- would keep
  // whatever it captured while the data was live.
  // Poll EVERY tracked texture, not just the primary. A surface binds up to 8 and there is no way
  // to tell from the screen which one speckles, so the set is polled together and the log says
  // which address moved. Per-address history is kept in a small ring rather than a single static,
  // which is what confined the old poll to one texture.
  TrackedTex tracked[kTrackSetMax + 1];
  const uint32_t tracked_n = TrackedList(tracked, kTrackSetMax + 1);
  for (uint32_t ti = 0; ti < tracked_n; ++ti) {
    const uint32_t track = tracked[ti].addr;
    if (const uint8_t* p = TranslatePhysical(track)) {
      // POLL THE WHOLE TRACKED TEXTURE, AND HASH IT.
      //
      // Two defects, both of which blunt the experiment this exists for -- track a surface while
      // it is CLEAN, walk up to it, and see what changes when it speckles:
      //   - it sampled a fixed 8192 bytes, 3% of a 262144-byte base, so a change anywhere else
      //     was invisible.
      //   - it keyed on the NONZERO COUNT alone, so an overwrite that swaps one texture's bytes
      //     for another's of similar density registers as no change at all. That is precisely the
      //     event we are hunting: content replaced, not content erased.
      // Hash as well as count, over the tracked span, and report on either changing.
      // Each entry carries its own span (its level-0 size), so a 32 KB DXT1 and a 256 KB DXT5
      // bound by the same surface are each polled over exactly their own extent.
      const uint32_t span = std::min(
          std::max(tracked[ti].span ? tracked[ti].span : REXCVAR_GET(nx1_d3d9_dbg_track_bytes),
                   4096u),
          1u << 20);
      uint32_t nz = 0, empty_pages = 0, pages = 0;
      uint64_t hash = 1469598103934665603ull;
      for (uint32_t off = 0; off < span; off += 4096) {
        const uint32_t end = std::min(off + 4096u, span);
        uint32_t page_nz = 0;
        for (uint32_t i = off; i < end; ++i) {
          page_nz += p[i] != 0 ? 1 : 0;
          hash = (hash ^ p[i]) * 1099511628211ull;
        }
        nz += page_nz;
        ++pages;
        empty_pages += page_nz == 0 ? 1 : 0;
      }
      // Per-ADDRESS history. A single static pair could only ever describe one texture, so with a
      // set every entry would look changed on every frame as the statics ping-ponged between them.
      struct PollHist {
        uint32_t addr = 0;
        uint32_t nz = 0xFFFFFFFFu;
        uint64_t hash = 0;
      };
      static PollHist hist[kTrackSetMax + 1];
      PollHist* h = nullptr;
      for (auto& e : hist) {
        if (e.addr == track) {
          h = &e;
          break;
        }
      }
      if (!h) {  // first sight of this address: claim a free or stale slot
        for (auto& e : hist) {
          if (!e.addr || !TrackedMatch(e.addr)) {
            e = PollHist{track, 0xFFFFFFFFu, 0};
            h = &e;
            break;
          }
        }
        if (!h) {
          continue;
        }
      }
      uint32_t& last_nz = h->nz;
      uint64_t& last_hash = h->hash;
      if (nz != last_nz || hash != last_hash) {
        // Same count with a different hash is the interesting case and is called out, because it
        // means the texture was REPLACED rather than filled -- invisible to the old poll.
        NX1_LOGW_TEX("nx1_d3d9: TRACK {:08X} POLL frame={} nonzero={}/{} ({} empty of {} pages) "
                    "hash={:016X}{}",
                    track, frame_, nz, span, empty_pages, pages, hash,
                    (nz == last_nz && last_nz != 0xFFFFFFFFu)
                        ? "  *** SAME nonzero count, DIFFERENT bytes -- this slot was REPLACED,"
                          " not filled ***"
                        : "");
        last_nz = nz;
        last_hash = hash;
      }
      // RELEASE AN AUTOTRACK LATCH THAT HAS HEALED. The decode-path release could not do this:
      // it only runs when the texture is DECODED again, and run 065's catch was served from cache
      // for its whole life (57 binds, 57 CACHED, zero decodes), so the latch stuck on a texture
      // that had long since filled.
      //
      // And it HAD filled -- 1A4C1000 polled 512/8192 nonzero when autotrack latched at frame
      // 1358 and 6977/8192 by frame 2023. It was caught MID-STREAM, not stuck. That is the real
      // lesson from that run: the guest lands a texture's MIP CHAIN BEFORE ITS BASE LEVEL, so
      // "starved base, delivered mips" is a normal transient of streaming and is only a fault when
      // it PERSISTS. Compare 16EE0000, which held the same decode across 580 frames.
      //
      // Polling is the right place for this because it observes the memory directly and needs no
      // decode. Releasing re-arms the hunt for the next candidate.
      // Threshold is against the SPAN now that the poll covers the whole texture; it used to be
      // against a fixed 8192 and would have fired ~32x too easily on a 256 KB base.
      if (autotrack_latched_ == track && uint64_t(nz) * 100 >= uint64_t(span) * 50u) {
        NX1_LOGW_TEX("nx1_d3d9: AUTOTRACK {:08X} RELEASED at frame {} -- it FILLED ({}/{} "
                    "nonzero), so it was caught mid-stream rather than stuck. The mip chain "
                    "landing before the base level is normal streaming order; only a texture "
                    "that STAYS starved is the specimen. Re-arming the hunt",
                    track, frame_, nz, span);
        autotrack_latched_ = 0;
        REXCVAR_SET(nx1_d3d9_dbg_track_addr, 0u);
      }
    }
  }

  if (baseline_frames_ > 0 && --baseline_frames_ == 0) {
    std::sort(baseline_addrs_.begin(), baseline_addrs_.end());
    baseline_addrs_.erase(std::unique(baseline_addrs_.begin(), baseline_addrs_.end()),
                          baseline_addrs_.end());
    baseline_ready_ = true;
    NX1_LOGI_TEX("nx1_d3d9: NEWTEX baseline is {} addresses -- now reporting newcomers",
                baseline_addrs_.size());
  }
  ++frame_;

  // DEBUG DUMP BUDGETS EXPIRE. The forced rebuild that makes a settled texture dump again is
  // armed by these cvars and used to terminate only when the budget decremented -- and that
  // decrement lives on the CPU block-compressed decode path alone. A material whose textures
  // take any other path (DXN, BC-alpha, uncompressed) never decremented it, so every matching
  // texture rebuilt EVERY FRAME forever, creating and destroying staging textures faster than
  // D3D9Ex reclaims them. Measured at 33 GB of process memory before the game was killed.
  // Termination must not depend on which decode path happens to run, so an armed dump now dies
  // after a fixed number of frames regardless.
  if (REXCVAR_GET(nx1_d3d9_dbg_mipdump) || REXCVAR_GET(nx1_d3d9_dbg_texdump)) {
    constexpr uint32_t kDumpArmFrames = 240;  // ~4s at 60 fps; dumps land in the first few
    if (++dump_arm_frames_ > kDumpArmFrames) {
      REXGPU_WARN("nx1_d3d9: dump budget expired after {} frames (mipdump {} texdump {} left) -- "
                  "disarming so the forced rebuild cannot run forever",
                  kDumpArmFrames, REXCVAR_GET(nx1_d3d9_dbg_mipdump),
                  REXCVAR_GET(nx1_d3d9_dbg_texdump));
      REXCVAR_SET(nx1_d3d9_dbg_mipdump, 0u);
      REXCVAR_SET(nx1_d3d9_dbg_texdump, 0u);
      REXCVAR_SET(nx1_d3d9_dbg_texdump_force, false);
      dump_arm_frames_ = 0;
    }
  } else {
    dump_arm_frames_ = 0;
  }

  DrainMemoryWrites();

  // Proactive early snapshot: capture physical memory into the mirror a chunk at a time over the
  // first frames, mirroring the reference's full-buffer memexport request that snapshots memory
  // early while textures are resident. On-demand MirrorSnapshot captures the rest; the write-watch
  // re-captures pages as the real image streams in. ~16 MB/frame -> full 512 MB in ~2s.
  if (mirror_ && phys_base_ && mirror_sweep_page_ < kMirrorPages) {
    const uint32_t end = std::min(mirror_sweep_page_ + 4096, kMirrorPages);  // 4096 pages = 16 MB
    auto* mem = rex::system::kernel_state()->memory();
    for (uint32_t p = mirror_sweep_page_; p < end; ++p) {
      if (!(mirror_valid_[p >> 6] & (uint64_t(1) << (p & 63)))) {
        std::memcpy(mirror_ + (size_t(p) << 12), phys_base_ + (size_t(p) << 12), 4096);
        mirror_valid_[p >> 6] |= (uint64_t(1) << (p & 63));
        if (mem) mem->EnablePhysicalMemoryAccessCallbacks(p << 12, 4096, true, false);
      }
    }
    mirror_sweep_page_ = end;
  }

  // Sweep periodically rather than every frame: walking the maps is O(entries), and an
  // entry only has to survive long enough that a resource used every few frames is not
  // thrashed. kKeepFrames is generous -- this is a leak guard, not a memory budget.
  constexpr uint64_t kSweepEvery = 300;
  constexpr uint64_t kKeepFrames = 600;
  if (frame_ % kSweepEvery || frame_ < kKeepFrames) {
    return;
  }
  const uint64_t cutoff = frame_ - kKeepFrames;

  if (auto* map = static_cast<VertexBufferMap*>(vertex_buffers_)) {
    for (auto it = map->begin(); it != map->end();) {
      if (it->second.last_frame < cutoff) {
        if (it->second.vb) it->second.vb->Release();
        it = map->erase(it);
      } else {
        ++it;
      }
    }
  }
  if (auto* map = static_cast<IndexBufferMap*>(index_buffers_)) {
    for (auto it = map->begin(); it != map->end();) {
      if (it->second.last_frame < cutoff) {
        if (it->second.ib) it->second.ib->Release();
        it = map->erase(it);
      } else {
        ++it;
      }
    }
  }
  // Only textures we uploaded live here; the resolve/depth aliases we do not own are in
  // resolves_, which is left alone.
  //
  // EVICTION IS NOT FREE HERE, and that is the point of the cvar. Re-decoding is only safe if
  // guest memory still holds what it held at the first decode -- and it demonstrably does not:
  // texture content changes between decodes with ZERO observed guest writes (DECODECHANGE),
  // so something modifies this memory through a path page protection cannot see. Evicting a
  // GOOD decode therefore gambles: the rebuild reads whatever is there now, and if that is the
  // pool's later garbage the texture is wrong until something rewrites it. The reference is not
  // reading better memory than we are -- it uploads once, never sees an invalidation either,
  // and so keeps its first (good) copy. Holding our decodes makes us behave the same way.
  if (auto* map = static_cast<TextureMap*>(textures_)) {
    for (auto it = map->begin(); it != map->end();) {
      if (!REXCVAR_GET(nx1_d3d9_evict_textures)) {
        ++it;
        continue;
      }
      if (it->second.last_frame < cutoff) {
        if (it->second.tex) it->second.tex->Release();
        if (it->second.vol) it->second.vol->Release();
        if (it->second.cube) it->second.cube->Release();
        ++tex_evicted_;
        it = map->erase(it);
      } else {
        ++it;
      }
    }
  }
  // Per-surface retained textures (PreferLargestForSurface). Each holds a reference on a texture that
  // may already be gone from textures_ above, so this is where those references are dropped.
  if (auto* map = static_cast<BestTextureMap*>(best_textures_)) {
    for (auto it = map->begin(); it != map->end();) {
      if (it->second.last_frame < cutoff) {
        if (it->second.tex) it->second.tex->Release();
        it = map->erase(it);
      } else {
        ++it;
      }
    }
  }
  if (auto* map = static_cast<VertexHashMap*>(vertex_hashes_)) {
    for (auto it = map->begin(); it != map->end();) {
      it = it->second.frame < cutoff ? map->erase(it) : std::next(it);
    }
  }
}

IDirect3DIndexBuffer9* ResourceTracker::GetIndexBuffer(const uint8_t* base,
                                                       const IndexBufferState& state,
                                                       uint32_t* index_size) {
  *index_size = 0;
  if (!device_ || !index_buffers_) {
    return nullptr;
  }
  if (!state.valid()) {
    return nullptr;
  }
  *index_size = state.index_size;

  const uint64_t key = MixKey(state.base_address, state.size_bytes);
  auto* map = static_cast<IndexBufferMap*>(index_buffers_);
  auto& entry = (*map)[key];
  // Frame-coherent reuse: already validated this frame -> skip the content hash.
  if (entry.ib && entry.last_frame == frame_ && entry.bytes == state.size_bytes) {
    *index_size = entry.index_size;
    return entry.ib;
  }
  entry.last_frame = frame_;

  const uint8_t* src = TranslatePhysical(state.base_address);
  const uint64_t content_hash = XXH3_64bits(src, state.size_bytes);
  if (entry.ib && entry.bytes == state.size_bytes && entry.content_hash == content_hash) {
    return entry.ib;
  }
  if (entry.ib && entry.bytes != state.size_bytes) {
    entry.ib->Release();
    entry.ib = nullptr;
  }
  if (!entry.ib &&
      FAILED(device_->CreateIndexBuffer(
          state.size_bytes, D3DUSAGE_WRITEONLY,
          state.index_size == 4 ? D3DFMT_INDEX32 : D3DFMT_INDEX16, D3DPOOL_DEFAULT, &entry.ib,
          nullptr))) {
    REXGPU_ERROR("nx1_d3d9: CreateIndexBuffer({} bytes) failed", state.size_bytes);
    entry.ib = nullptr;
    return nullptr;
  }

  // Byteswap once into the CPU copy, then hand that to D3D -- GetDrawMaxIndex reads it
  // back to bound each draw's vertex range.
  entry.cpu.resize(state.size_bytes);
  if (state.index_size == 4) {
    SwapDwords(entry.cpu.data(), src, state.size_bytes & ~3u);
  } else {
    auto* dst = reinterpret_cast<uint16_t*>(entry.cpu.data());
    const uint32_t n = state.size_bytes / 2;
    for (uint32_t i = 0; i < n; ++i) {
      uint16_t v;
      std::memcpy(&v, src + i * 2, sizeof(v));
      dst[i] = Swap16(v);
    }
  }

  void* mapped = nullptr;
  if (FAILED(entry.ib->Lock(0, 0, &mapped, 0))) {
    return nullptr;
  }
  std::memcpy(mapped, entry.cpu.data(), state.size_bytes);
  entry.ib->Unlock();

  entry.bytes = state.size_bytes;
  entry.index_size = state.index_size;
  entry.content_hash = content_hash;
  return entry.ib;
}

uint32_t ResourceTracker::GetDrawMaxIndex(const IndexBufferState& state, uint32_t start_index,
                                          uint32_t index_count) {
  if (!index_buffers_ || !index_ranges_ || !index_count) {
    return 0;
  }
  if (!state.valid()) {
    return 0;
  }
  auto* map = static_cast<IndexBufferMap*>(index_buffers_);
  auto it = map->find(MixKey(state.base_address, state.size_bytes));
  if (it == map->end() || it->second.cpu.empty()) {
    return 0;
  }
  const IndexBufferEntry& ib = it->second;

  // Keyed on the index buffer's *contents* as well as the draw range, so a rewritten
  // dynamic index buffer invalidates the memo instead of returning a stale range.
  auto* ranges = static_cast<IndexRangeMap*>(index_ranges_);
  const uint64_t key =
      MixKey(MixKey(ib.content_hash, start_index), (uint64_t(index_count) << 1) | ib.index_size);
  if (auto found = ranges->find(key); found != ranges->end()) {
    return found->second;
  }

  uint32_t max_index = 0;
  if (ib.index_size == 4) {
    const uint32_t n = uint32_t(ib.cpu.size() / 4);
    const auto* idx = reinterpret_cast<const uint32_t*>(ib.cpu.data());
    for (uint32_t i = start_index; i < start_index + index_count && i < n; ++i) {
      max_index = std::max(max_index, idx[i]);
    }
  } else {
    const uint32_t n = uint32_t(ib.cpu.size() / 2);
    const auto* idx = reinterpret_cast<const uint16_t*>(ib.cpu.data());
    for (uint32_t i = start_index; i < start_index + index_count && i < n; ++i) {
      max_index = std::max(max_index, uint32_t(idx[i]));
    }
  }
  (*ranges)[key] = max_index;
  return max_index;
}

uint32_t ResourceTracker::ConvertInlineVertices(uint32_t verts_addr, uint32_t count,
                                                uint32_t guest_stride, const VertexLayout& layout,
                                                std::vector<uint8_t>* out) {
  const uint32_t host_stride = layout.host_stride[0];
  if (!verts_addr || !count || !guest_stride || !host_stride) {
    return 0;
  }
  const uint8_t* src = TranslateGuestPointer(verts_addr);
  if (!src) {
    return 0;
  }
  out->resize(size_t(count) * host_stride);
  uint8_t* dst = out->data();

  // Bulk swap only when the host layout is byte-identical to the (passed) guest
  // stride; otherwise repack element by element, exactly as GetVertexBuffer does.
  if (layout.bulk_swap[0] && host_stride == guest_stride) {
    SwapDwords(dst, src, size_t(count) * guest_stride);
  } else {
    for (uint32_t v = 0; v < count; ++v) {
      const uint8_t* sv = src + size_t(v) * guest_stride;
      uint8_t* dv = dst + size_t(v) * host_stride;
      for (const ConvertOp& op : layout.ops) {
        if (op.stream != 0) {
          continue;
        }
        if (op.expand) {
          float decoded[4];
          Expand(sv + op.src_offset, op, decoded);
          std::memcpy(dv + op.dst_offset, decoded, op.dst_size);
        } else {
          SwapDwords(dv + op.dst_offset, sv + op.src_offset, op.src_size);
        }
      }
    }
  }
  return host_stride;
}

bool ResourceTracker::ConvertInlineIndices(uint32_t indices_addr, uint32_t index_count,
                                           uint32_t index_size, std::vector<uint8_t>* out) {
  if (!indices_addr || !index_count || (index_size != 2 && index_size != 4)) {
    return false;
  }
  const uint8_t* src = TranslateGuestPointer(indices_addr);
  if (!src) {
    return false;
  }
  out->resize(size_t(index_count) * index_size);
  if (index_size == 4) {
    SwapDwords(out->data(), src, size_t(index_count) * 4);
  } else {
    auto* dst = reinterpret_cast<uint16_t*>(out->data());
    for (uint32_t i = 0; i < index_count; ++i) {
      uint16_t v;
      std::memcpy(&v, src + size_t(i) * 2, sizeof(v));
      dst[i] = Swap16(v);
    }
  }
  return true;
}

IDirect3DBaseTexture9* ResourceTracker::GetTexture(const uint8_t* base,
                                                   const TextureFetchConstant& t,
                                                   uint32_t sampler) {
  if (!device_ || !textures_) {
    return nullptr;
  }
  // TEMP PROFILING: attribute this call to either "resolved from cache" or "rebuilt". Both
  // timers fire on whichever return is taken; the upload one stays disarmed until the rebuild
  // decision below actually commits to re-decoding.
  struct ScopeNs {
    bool on = false;
    std::chrono::steady_clock::time_point t0;
    uint64_t* sink = nullptr;
    ~ScopeNs() {
      if (on && sink) {
        *sink += uint64_t((std::chrono::steady_clock::now() - t0).count());
      }
    }
  };
  ScopeNs prof_lookup, prof_upload;
  if (prof_enabled_) {
    prof_lookup.on = true;
    prof_lookup.t0 = std::chrono::steady_clock::now();
    prof_lookup.sink = &prof_tex_.lookup_ns;
  }

  if (!t.valid || !t.base_address || !t.width || !t.height) {
    return nullptr;
  }

  // A texture whose address is a resolve destination is served by the rendered
  // image, not by untiling stale memory. This is what makes render-to-texture
  // (scene compositing, post effects) work.
  // Hypothesis check, no cvar needed: the reference discards base_address entirely when
  // mip_min_level != 0 and reads the texels from mip_address instead. If that is what these
  // sprites do, their base memory will be empty while mip_address holds real data. Report each
  // such texture once, with both populated-byte counts, so the claim stands or falls on
  // numbers rather than on one hand-picked address.
  if (t.valid && t.mip_min_level != 0 && t.base_address) {
    static std::mutex mm;
    static std::vector<uint32_t> seen_mip;
    std::lock_guard<std::mutex> lk(mm);
    if (std::find(seen_mip.begin(), seen_mip.end(), t.base_address) == seen_mip.end() &&
        seen_mip.size() < 40) {
      seen_mip.push_back(t.base_address);
      uint32_t base_nz = 0, mip_nz = 0;
      if (const uint8_t* bp = TranslatePhysical(t.base_address)) {
        for (uint32_t i = 0; i < 4096; ++i) base_nz += bp[i] != 0 ? 1 : 0;
      }
      if (const uint8_t* mp = t.mip_address ? TranslatePhysical(t.mip_address) : nullptr) {
        for (uint32_t i = 0; i < 4096; ++i) mip_nz += mp[i] != 0 ? 1 : 0;
      }
      REXGPU_INFO("nx1_d3d9: MIPBASE base={:08X} nz={}/4096 | mip={:08X} nz={}/4096 | {}x{} "
                  "fmt={} mip_min={} mip_max={} sampler={}",
                  t.base_address, base_nz, t.mip_address, mip_nz, t.width, t.height, t.format,
                  t.mip_min_level, t.mip_max_level, sampler);
    }
  }

  // Trace every bind of the tracked address BEFORE any early-out, so a texture served from
  // the resolve map -- which returns below, ahead of the cache and ahead of the paint -- is
  // still visible. Without this, a render-target-backed texture looks like it is never bound.
  if (t.valid && t.base_address) {
    if (baseline_frames_ > 0) {
      baseline_addrs_.push_back(t.base_address);
    } else if (baseline_ready_ && !std::binary_search(baseline_addrs_.begin(),
                                                      baseline_addrs_.end(), t.base_address)) {
      bool found = false;
      for (auto& n : baseline_new_) {
        if (n.addr == t.base_address) {
          if (n.last_frame + 2 < frame_) {
            ++n.episodes;  // was unbound for a while, so this is a fresh appearance
          }
          if (n.last_frame != frame_) {
            ++n.frames_bound;
          }
          n.last_frame = frame_;
          found = true;
          break;
        }
      }
      if (!found) {
        baseline_new_.push_back({t.base_address, t.width, t.height, t.format, sampler, frame_,
                                 frame_, false, 1, 1});
      }
    }
  }

  // OBJECTIVE SPECKLE METRIC. A bind whose pixels are the engine's placeholder buffer is a
  // texture the game itself considers not resident -- no judgement call, no screenshot
  // comparison. Counted per frame and reported with the cache stats so two configs (or two
  // backends) can be compared by number instead of by eye.
  if (const uint32_t dp = DefaultPixelsAddress();
      dp && t.base_address && PhysicalAddress(t.base_address) == PhysicalAddress(dp)) {
    ++placeholder_binds_;
  }

  const uint32_t dbg_track = REXCVAR_GET(nx1_d3d9_dbg_track_addr);
  if (dbg_track && t.base_address == dbg_track) {
    static uint64_t last_bind_frame = 0;
    if (frame_ != last_bind_frame) {
      last_bind_frame = frame_;
      // Poll BOTH allocations' live bytes every bound frame. This is the discriminator for the
      // "never resolves" textures: if base_nonzero stays garbage-shaped forever AND no TRACK
      // WRITE line ever appears, the guest never streams this memory in our session at all --
      // the failure is upstream of the texture cache (the game's streaming decision), not in
      // decode or invalidation. If a WRITE does appear and the texture stays wrong, the
      // invalidation chain lost it and the bug is ours.
      // SCAN THE WHOLE TEXTURE, not a fixed 8 KB prefix. The old fixed window silently reported
      // only the first 8192 bytes: fine for a 128x128 DXT1 (which IS 8192 bytes -- that is the
      // one case where the number meant what it said, and it read 1896/8192), but for a 512x512
      // DXN it covered 3% of the image. Every "this texture is ~99% full" reading taken while
      // hunting the speckle described an opening sliver and nothing else, which is precisely how
      // a texture that is half black blocks past the first pages reads as healthy.
      //
      // Reported as EMPTY PAGES rather than nonzero bytes: a hole is page-shaped (that is how the
      // data fails to arrive), and legitimately dark texels make a byte count look alarming for
      // no reason.
      const auto scan = [&](uint32_t addr, uint32_t bytes, uint32_t* empty_pages,
                            uint32_t* total_pages) {
        *empty_pages = *total_pages = 0;
        const uint8_t* p = addr ? TranslatePhysical(addr) : nullptr;
        if (!p || !bytes) {
          return;
        }
        for (uint32_t off = 0; off < bytes; off += 4096) {
          const uint32_t end = std::min(off + 4096u, bytes);
          uint32_t nz = 0;
          for (uint32_t i = off; i < end && !nz; ++i) nz += p[i] != 0 ? 1 : 0;
          ++*total_pages;
          *empty_pages += nz == 0 ? 1 : 0;
        }
      };
      // Guest size of level 0 from the format's own block geometry -- the same arithmetic the
      // decode uses, so the scan covers exactly what we would read.
      uint32_t base_bytes = 0;
      if (const auto* fi = rex::graphics::FormatInfo::Get(t.format)) {
        const uint32_t bw = (t.width + fi->block_width - 1) / fi->block_width;
        const uint32_t tex_h = t.dimension == 0 ? 1u : t.height;
        const uint32_t bh = (tex_h + fi->block_height - 1) / fi->block_height;
        base_bytes = bw * bh * fi->bytes_per_block();
      }
      uint32_t base_empty = 0, base_pages = 0, mip_empty = 0, mip_pages = 0;
      scan(t.base_address, base_bytes, &base_empty, &base_pages);
      scan(t.mip_address, base_bytes, &mip_empty, &mip_pages);
      NX1_LOGI_TEX("nx1_d3d9: TRACK {:08X} BIND frame={} sampler={} {}x{} fmt={} dim={} "
                  "mip_min={} mip_max={} mip_filter={} lod_bias={} packed={} | base {} bytes, "
                  "{} of {} pages EMPTY | mip_address={:08X} {} of {} pages EMPTY",
                  t.base_address, frame_, sampler, t.width, t.height, t.format, t.dimension,
                  t.mip_min_level, t.mip_max_level, t.mip_filter, t.lod_bias,
                  t.packed_mips ? 1 : 0, base_bytes, base_empty, base_pages, t.mip_address,
                  mip_empty, mip_pages);
    }
  }

  if (resolve_flat_valid_) {
    // Linear scan of at most a few entries, all in cache, instead of hashing into the map.
    const uint32_t phys = PhysicalAddress(t.base_address);
    for (uint32_t i = 0; i < resolve_flat_count_; ++i) {
      if (resolve_flat_addr_[i] != phys) {
        continue;
      }
      // The address matching is NOT sufficient on its own. Resolve entries are never
      // invalidated by a guest write and never aged out, so a recycled address keeps being
      // served its stale render target instead of the texture that now lives there. Require
      // the bind to actually describe that target: a 128x128 texture asking for memory that
      // once held a 1024x600 scene resolve is the pool reusing the address, not a
      // render-to-texture read.
      const bool size_ok =
          resolve_flat_w_[i] == t.width && resolve_flat_h_[i] == t.height;
      if (!size_ok && REXCVAR_GET(nx1_d3d9_resolve_size_check)) {
        ++resolve_rejected_;
        static std::mutex m;
        static std::vector<uint32_t> seen;
        std::lock_guard<std::mutex> lk(m);
        if (std::find(seen.begin(), seen.end(), phys) == seen.end() && seen.size() < 32) {
          seen.push_back(phys);
          REXGPU_WARN("nx1_d3d9: RESOLVESTALE {:08X} bind {}x{} fmt={} but resolve target is "
                      "{}x{} -- decoding guest memory instead of serving the stale target",
                      t.base_address, t.width, t.height, t.format, resolve_flat_w_[i],
                      resolve_flat_h_[i]);
        }
        break;  // fall through to the normal decode path
      }
      ++resolve_served_;
      NoteResolveTextureCensus(PhysicalAddress(t.base_address), 0, true);
      if (dbg_track && t.base_address == dbg_track) {
        REXGPU_INFO("nx1_d3d9: TRACK {:08X} SERVED FROM RESOLVE MAP frame={}", t.base_address,
                    frame_);
      }
      return resolve_flat_tex_[i];
    }
  } else if (resolves_) {
    auto* rmap = static_cast<ResolveMap*>(resolves_);
    if (auto it = rmap->find(PhysicalAddress(t.base_address));
        it != rmap->end() && it->second.tex) {
      if (dbg_track && t.base_address == dbg_track) {
        static uint64_t last_resolve_frame = 0;
        if (frame_ != last_resolve_frame) {
          last_resolve_frame = frame_;
          NX1_LOGI_TEX("nx1_d3d9: TRACK {:08X} SERVED FROM RESOLVE MAP frame={} ({}x{}) -- this is "
                      "a render target, not a CPU-decoded texture",
                      t.base_address, frame_, it->second.width, it->second.height);
        }
      }
      return it->second.tex;
    }
  }
  // xenos::DataDimension: 0 = 1D, 1 = 2D, 2 = 3D, 3 = cube.
  //
  // The composite's pixel shader ends with a 1D tone-map curve and a 3D colour-grading LUT:
  //   r0.x   = tfetch1D(s8, ..)
  //   r0.xyz = tfetch3D(s7, r0)
  //   oC0    = r0
  // so its entire output comes from those two. An unbound D3D9 sampler reads black, which
  // blacked out the whole composited frame while the scene texture feeding it was perfect.
  //
  // 1D needs no special case: on D3D9 a 1D texture *is* a 2D texture one row tall, which is
  // exactly what ps_3_0 compiles tex1D into. Fall through with height forced to 1.
  if (t.dimension == 2) {
    return GetVolumeTexture(t, sampler);
  }
  if (t.dimension == 3) {
    return GetCubeTexture(t, sampler);
  }
  const uint32_t height = t.dimension == 0 ? 1u : t.height;

  // The layout key is derived from the fetch constant alone, so it can be built -- and the
  // cache consulted -- before any of the format/extent work below. That matters: a scene is
  // ~5000 draws x 16 samplers, so this early-out runs ~80k times a frame and everything it
  // skips is multiplied by that.
  //
  // When the key changes, the texture object is rebuilt. The swizzle is part of the key: the
  // same memory bound with a different channel swizzle is a different texture. mip_max_level and
  // packed_mips are in too -- they decide our host chain's level count.
  //
  // mip_address is deliberately NOT in the key. The texture-streaming pool relocates a texture's
  // mip tail as it pages memory around, so mip_address churns through many values for one and the
  // same image (measured: a single 512x512 surface cycled its mip_address every few frames). We
  // build our own mip chain from level 0 and never read the guest's, so mip_address describes
  // nothing about the host texture -- folding it into the key only forced a re-decode every time
  // the pool moved, and re-reading base_address mid-stream is what dissolved the surface into
  // rainbow static. base_address (also the cache-map key) already identifies the texel data.
  // PITCH AND ENDIAN BELONG IN THIS KEY. DecodeViewKey -- the other "what makes a decode
  // different" key in this file -- includes both, and its comment records the evening lost to
  // treating a view difference as noise. This key omitted them, and they are not cosmetic:
  //
  //   pitch_pixels feeds TextureExtent::Calculate -> block_pitch_h -> GetTiledOffset2D(x, y,
  //   PITCH, bpb). A different pitch means completely different tiled addressing over the same
  //   bytes, so the cache would serve a decode built with the previous binding's stride --
  //   scattering blocks and reaching into the neighbouring allocations that the XDK documents
  //   the guest as packing data into (XGAddress2DTiledExtent: alignment gaps "you might want to
  //   reclaim for other purposes"). That reads as rectangular foreign blocks over a
  //   partly-correct surface, varying run to run with whichever binding cached first.
  //
  //   endian feeds CopySwapBlock inside DetileMip2D, so it changes the texel values outright.
  const uint64_t layout_key = MixKey(
      MixKey(MixKey(MixKey(t.base_address, (uint64_t(t.format) << 32) | t.width),
                    MixKey((uint64_t(height) << 1) | (t.tiled ? 1 : 0), t.swizzle)),
             (uint64_t(t.mip_max_level) << 1) | (t.packed_mips ? 1 : 0)),
      (uint64_t(t.pitch_pixels) << 8) | t.endian);

  // CACHE IDENTITY: ONE ENTRY PER DESCRIPTOR, WHICH IS WHAT THE REFERENCE DOES.
  //
  // The reference keys its texture cache on the whole descriptor -- TextureKey is base_page,
  // mip_page, dimension, width, height, depth, tiled, packed_mips, pitch, mip_max_level, format
  // and endianness (pipeline/texture/cache.h:134). Two descriptors that differ in any of those are
  // two different textures with two independent lifetimes, each holding the decode made when its
  // own bytes were resident.
  //
  // We keyed on (base_address, sampler) and used layout_key only to decide whether to REBUILD. So
  // when one address carries more than one descriptor -- measured: 051F4000 was bound as 256x256
  // f18, 512x128 f20 AND 256x256 f20 in a single session, and 11401000 alternated 256x256 /
  // 128x256 on consecutive frames -- they all collide in one entry and evict each other. Every
  // flip re-decodes, and a re-decode pairs THIS descriptor's geometry with whatever bytes are in
  // that memory NOW, which is the other texture's. That is a wrong (geometry, bytes) pairing, and
  // no amount of invalidation or re-reading can fix it: it is not stale data, it is mismatched
  // data. It also explains why every timing-based intervention this session failed.
  //
  // Including layout_key in the key gives each descriptor its own entry, as in the reference.
  const uint64_t key = REXCVAR_GET(nx1_d3d9_key_by_layout)
                           ? MixKey(MixKey(t.base_address, sampler), layout_key)
                           : MixKey(t.base_address, sampler);
  auto* map = static_cast<TextureMap*>(textures_);
  auto& entry = (*map)[key];
  if (const uint32_t track = TrackedMatch(t.base_address);
      track && entry.tex && entry.layout_key != layout_key) {
    REXGPU_INFO("nx1_d3d9: TRACK {:08X} LAYOUT CHANGED frame={} sampler={} {}x{} fmt={} -- the "
                "pool handed this address to a different texture",
                t.base_address, frame_, sampler, t.width, height, t.format);
  }
  // Count clean frames toward commit (once per new frame for a stable, already-decoded texture).
  // After kCommitFrames it is settled: DrainMemoryWrites stops honouring writes to it, so the
  // streaming pool's later garbage-recycle can't re-poison the decode (the confetti-on-backup).
  constexpr uint32_t kCommitFrames = 32;
  // NOT for a decode made from an incomplete source. "Drawn cleanly for 32 frames" only means
  // nothing crashed; a texture whose pool slot was still empty renders 32 clean frames of
  // garbage just as readily, and committing it makes that permanent.
  if (entry.tex && entry.last_frame != frame_ && entry.layout_key == layout_key &&
      !entry.committed && !entry.decoded_from_partial &&
      ++entry.good_frames >= kCommitFrames && REXCVAR_GET(nx1_d3d9_commit_textures)) {
    entry.committed = true;
  }
  // Debug: force ONE fresh decode of a texture matching the dump filter.
  //
  // The dump lives in the rebuild path, so a texture that is decoded once and thereafter served
  // from the early-out below can never be dumped -- which is exactly the case worth inspecting.
  // A texture whose guest memory is never written after our first decode is never dirtied, never
  // rebuilt, and we serve that first decode forever; if it was taken before the data landed, the
  // texture is permanently wrong and no amount of watching for writes will tell us. Dropping the
  // entry here makes the next bind take the full path and report the bytes as they are NOW.
  if (REXCVAR_GET(nx1_d3d9_dbg_texdump) && REXCVAR_GET(nx1_d3d9_dbg_texdump_force)) {
    const uint32_t fdim = REXCVAR_GET(nx1_d3d9_dbg_texdump_maxdim);
    const uint32_t ffmt1 = REXCVAR_GET(nx1_d3d9_dbg_texdump_fmt1);
    if (dump_filter_active_ ? dump_draw_
                            : ((!fdim || (t.width <= fdim && height <= fdim)) &&
                               (!ffmt1 || t.format == ffmt1 - 1))) {
      entry.dirty = true;
      entry.committed = false;
    }
  }

  // Valid-texture early-out. A built texture whose layout still matches and that the write-watch
  // has not dirtied IS the answer -- nothing below can change that, so none of it needs to run.
  //
  // This deliberately does NOT require last_frame == frame_. It used to, which meant the FIRST
  // bind of every distinct texture each frame fell through into the whole preamble below --
  // extent math, mip-level selection and above all MirrorSnapshot, which walks a page bit per
  // 4 KB (1024 iterations for a 4 MB surface) -- only to reach the identical `layout_key match &&
  // !dirty` test further down and return the same pointer. At ~1500 distinct textures a frame
  // that preamble was the single largest cost in the renderer (measured: the textures phase was
  // ~9 ms of a ~24 ms frame).
  //
  // Skipping MirrorSnapshot here cannot miss a guest write: the write-watch is armed when a page
  // is CAPTURED, not per call, and DrainMemoryWrites both invalidates the mirror page and sets
  // entry.dirty -- and a dirty entry fails this test and takes the full path, re-snapshotting then.
  // `frame_ < entry.retry_frame` holds only for a sprite that keeps decoding to nothing; it is
  // what stops the never-streamed ones re-decoding every frame in perpetuity.
  //
  // A guest-mip texture whose mip tail MOVED must re-decode: mip_address is deliberately not in
  // the cache key (the pool relocates tails; keying on it would mint a new entry per location),
  // so the move is caught here instead. mip_relocs_ counts these -- an earlier session measured
  // one surface cycling its mip_address every few frames, and if that churn is real this check
  // turns it into a rebuild storm; the counter in the periodic mipgen stats is how we would see.
  if (entry.tex && entry.mip_source == 3 && entry.mip_addr_seen != t.mip_address) {
    entry.dirty = true;
    entry.dirty_source = DirtySource::kMipReloc;
    ++mip_relocs_;
  }
  // A dump budget armed while a MATERIAL filter is active means "dump what this draw binds,
  // now". The rebuild has to be forced because a settled entry (dirty=0, committed=1) takes the
  // early-out below and never reaches the decode where dumping lives.
  //
  // Keyed on the DRAW, not on an address. Guest addresses are not stable identities: the
  // streaming pool reassigns them, and a dump aimed at a tracked address came back as a 64x64
  // DXT5 when the surface picked at that address had been a 512x512 DXT1 -- a different
  // texture entirely, silently. Acting on whatever the draw binds right now cannot go stale.
  if (entry.tex && REXCVAR_GET(nx1_d3d9_dbg_mipdump) && dump_filter_active_ && dump_draw_) {
    entry.dirty = true;
    entry.retry_frame = 0;  // the backoff must not defer the very rebuild we asked for
  }
  // FORCED RE-DECODE. If the corruption is a race -- the decode taken before the guest finished
  // landing the data -- then re-reading the same address later must produce different bytes, and
  // the texture must come good. That is testable directly, and no passive signal can test it:
  // every other suspect (mirror, eviction, resolve writeback, memexport readback, DmaCopy layout)
  // has been eliminated by measurement, and this one matches the symptom nothing else explained,
  // that speckle heals when you approach and that only SLOWING the whole pipeline has ever
  // produced a clean frame.
  //
  // Schedules on a geometric ladder rather than a single retry: whatever fills these slots is
  // asynchronous, so a fixed delay would answer only for that one latency. DECODECHANGE reports
  // which of these rechecks actually found different bytes -- that number is the measurement,
  // and the picture is the confirmation.
  if (entry.tex && entry.recheck_frame && frame_ >= entry.recheck_frame) {
    entry.dirty = true;
    entry.dirty_source = DirtySource::kRecheck;
    entry.retry_frame = 0;
    ++forced_rechecks_;
    if (entry.rechecks_left) {
      --entry.rechecks_left;
      const uint32_t base_delay = REXCVAR_GET(nx1_d3d9_redecode_delay);
      entry.recheck_frame = frame_ + uint64_t(base_delay) * (1u << (3 - std::min(3u, entry.rechecks_left)));
    } else {
      entry.recheck_frame = 0;
      entry.ladder_done = true;  // four rechecks is the whole test; do not re-arm
    }
  }
  // SLOT-OCCUPANT CHECK. Once per frame per texture, confirm the guest memory still holds what
  // we decoded. The pool relocates images between same-sized slots, so an address alone is not an
  // identity: measured 156 cases in one run where a cached address came to hold a DIFFERENT
  // texture of identical dimensions and format, and we served the old decode to the new binder.
  // Bounded to one probe per texture per frame -- the early-out below runs ~80k times a frame and
  // must stay cheap.
  if (entry.tex && entry.probe_hash && entry.probe_frame != frame_ &&
      REXCVAR_GET(nx1_d3d9_content_probe)) {
    entry.probe_frame = frame_;
    // PROBE THE WHOLE GUEST FOOTPRINT, NOT THE VISIBLE RECTANGLE.
    //
    // This used to size itself as ceil(w/bw) * ceil(h/bh) * bpb -- the UNPADDED block count. The
    // texture's actual guest extent is block_pitch_h * block_pitch_v * bpb, tile-aligned and
    // derived from the PITCH, which is routinely much larger: a 64x64 DXT1 at pitch 128 occupies
    // 8192 bytes while the old formula probed 2048. And because the probe reads linearly from the
    // base while the data is TILED, that 2048 was a scattered subset of tiles rather than a
    // quarter of the image -- so a change anywhere in the other 6 KB was invisible.
    //
    // That is why the probe fires constantly (3513 rebuilds in one run) and still never corrects
    // these surfaces: ADDRWATCH proves the content at the address really does change from garbage
    // to correct, and the probe simply was not looking at most of it.
    //
    // Uses the same Calculate() call the decode itself uses further down, so the probe and the
    // decode cover exactly the same bytes.
    uint32_t probe_bytes = 0;          // bounds check only; the hash walks tiled offsets
    rex::graphics::TextureExtent ex_p{};
    uint32_t bpb_p = 0;
    if (const auto* fi_p = rex::graphics::FormatInfo::Get(t.format)) {
      const uint32_t pitch_p = t.pitch_pixels ? t.pitch_pixels : t.width;
      ex_p = rex::graphics::TextureExtent::Calculate(fi_p, pitch_p, height, /*depth=*/1, t.tiled,
                                                     /*is_guest=*/true);
      bpb_p = fi_p->bytes_per_block();
      // Clamp the VISIBLE block grid to the created texture, exactly as the decode does -- a wider
      // pitch stays in block_pitch_h for addressing only.
      ex_p.block_width = std::min(ex_p.block_width,
                                  (t.width + fi_p->block_width - 1) / fi_p->block_width);
      ex_p.block_height = std::min(ex_p.block_height,
                                   (height + fi_p->block_height - 1) / fi_p->block_height);
      probe_bytes = ex_p.block_pitch_h * ex_p.block_pitch_v * bpb_p;
    }
    // BOUNDS. A texture's declared dimensions can imply more bytes than the mapped region
    // actually holds, and ProbeGuestContent reads to the end of what it is given -- firing a
    // weapon (muzzle-flash sprites) crashed the game on an out-of-bounds read here. Clamp to the
    // same physical window every other reader in this file respects.
    if (uint64_t(PhysicalAddress(t.base_address)) + probe_bytes > (uint64_t(kMirrorPages) << 12)) {
      probe_bytes = 0;
    }
    // BOUNDS. A texture's declared dimensions can imply more bytes than the mapped region
    // actually holds, and ProbeGuestContent reads to the end of whatever it is handed. A crash
    // was observed after firing a weapon (muzzle-flash sprites decode on the spot) and did not
    // reproduce -- an unbounded read is a good enough explanation to close regardless. Clamp to
    // the same physical window every other reader in this file respects.
    if (uint64_t(PhysicalAddress(t.base_address)) + probe_bytes > (uint64_t(kMirrorPages) << 12)) {
      probe_bytes = 0;
    }
    if (probe_bytes) {
      const uint64_t now_hash =
          ProbeTiledContent(TranslatePhysical(t.base_address), ex_p, bpb_p, t.tiled,
                            REXCVAR_GET(nx1_d3d9_probe_samples));
      if (now_hash && now_hash != entry.probe_hash) {
        // THE REFERENCE'S RULE, OPTIONALLY. It never polls: a texture is re-read only when a write
        // was actually reported for its memory. We poll, and REBUILDSRC shows the probe is
        // responsible for 1308 of 1405 content-changing rebuilds -- 93% of every time we adopt new
        // bytes -- while genuine guest writes account for 10. So the probe is where we diverge.
        //
        // With this on, a probe-detected change is adopted ONLY if a write was reported for this
        // entry since its last decode. A change nobody reported came through a path the reference
        // ignores, and it keeps its existing texture in exactly that case.
        //
        // THE RISK IS REAL AND THE OPPOSITE FAILURE IS LIKELY: the probe's changes are mostly
        // LEGITIMATE (a texture finishing streaming in), which is why enabling the probe was this
        // investigation's only confirmed improvement. If writes are rarely reported, this gate
        // blocks the good adoptions too and regresses toward the pre-probe state. Judge it on
        // REBUILDSRC (probe's changed count should fall) AND on the screen -- if the picture gets
        // worse, the rule is too strict and the discriminator has to come from somewhere else.
        if (REXCVAR_GET(nx1_d3d9_probe_needs_write) && !entry.write_seen_since_decode) {
          ++probe_refused_no_write_;
        } else {
        entry.dirty = true;
        entry.dirty_source = DirtySource::kProbe;
        entry.retry_frame = 0;
        ++probe_rebuilds_;
        // THE REBUILD MUST RE-READ THE SLOT. The probe reads LIVE memory; the rebuild reads
        // MirrorSnapshot, which serves a cached page until something drops its validity bit.
        // Without this the forced rebuild re-decoded the SAME STALE BYTES and produced the same
        // image -- which is exactly why the probe took SLOTREUSE 156 -> 0 while the speckle was
        // completely unmoved.
        InvalidateMirrorPages(PhysicalAddress(t.base_address), probe_bytes);
        }
      }
    }
  }
  // KEEP-BEST: is this rebuild an improvement, or are we about to adopt a worse moment of a
  // churning slot? Evaluated only when we ALREADY hold a decode of the SAME layout -- a layout
  // change is a different texture and always wins. See nx1_d3d9_keep_best.
  if (entry.tex && entry.dirty && entry.layout_key == layout_key && REXCVAR_GET(nx1_d3d9_keep_best) &&
      entry.best_sampled && entry.watch_size) {
    // entry.watch_size, not guest_bytes: the latter is computed further down and is not in scope
    // here. It is the byte span of the decode we are holding, which is the right basis anyway --
    // this branch only runs when layout_key is unchanged, so the span is the same texture's.
    if (const uint8_t* live = TranslatePhysical(t.base_address)) {
      const uint32_t sample_cap = std::min<uint32_t>(entry.watch_size, 1u << 20);
      uint32_t nz = 0, sampled = 0;
      for (uint32_t i = 0; i < sample_cap; i += 8) {  // sampled: a ratio, not a checksum
        nz += live[i] != 0 ? 1 : 0;
        ++sampled;
      }
      // Compare as a RATIO: guest_bytes can differ from the decode we hold if the pool re-aimed
      // the slot, and comparing raw counts across different sample sizes would be meaningless.
      // Against the HIGH-WATER MARK, not the last accepted decode -- see Entry::best_nonzero.
      const uint64_t have = uint64_t(entry.best_nonzero) * sampled;
      const uint64_t now = uint64_t(nz) * entry.best_sampled;
      const uint32_t drop = std::min(REXCVAR_GET(nx1_d3d9_keep_best_drop_pct), 100u);
      if (have && now * 100u < have * (100u - drop) &&
          entry.keepbest_refusals < REXCVAR_GET(nx1_d3d9_keep_best_max)) {
        ++entry.keepbest_refusals;
        ++keepbest_refused_;
        entry.dirty = false;  // keep what we have; a later, better write can dirty it again
        entry.last_frame = frame_;
        if (const uint32_t track = TrackedMatch(t.base_address); track) {
          REXGPU_WARN("nx1_d3d9: TRACK {:08X} KEEPBEST refused re-decode #{} frame={} -- source is "
                      "{}/{} populated where the decode we hold came from {}/{}. Adopting it would "
                      "show a poorer moment of a churning slot",
                      track, entry.keepbest_refusals, frame_, nz, sampled, entry.best_nonzero,
                      entry.best_sampled);
        }
        return entry.tex;
      }
    }
  }
  if (entry.tex && entry.layout_key == layout_key && (!entry.dirty || frame_ < entry.retry_frame)) {
    entry.last_frame = frame_;
    // Painting the tracked address white answers "which surface is this texture?" without a
    // capture tool: set the cvar, look for the white thing.
    if (const uint32_t track = TrackedMatch(t.base_address); track) {
      // Why a bind took the cache early-out. If this reports dirty=1 the entry was invalidated
      // and we served the stale decode anyway, which would be the bug outright.
      static uint64_t last_logged = 0;
      if (frame_ != last_logged) {
        last_logged = frame_;
        NX1_LOGI_TEX("nx1_d3d9: TRACK {:08X} CACHED frame={} dirty={} committed={} retry_frame={} "
                    "layout_ok=1 good_frames={} mip_source={} ({})",
                    t.base_address, frame_, entry.dirty ? 1 : 0, entry.committed ? 1 : 0,
                    entry.retry_frame, entry.good_frames, entry.mip_source,
                    entry.mip_source == 1   ? "cpu-built"
                    : entry.mip_source == 2 ? "driver auto"
                    : entry.mip_source == 3 ? "guest chain"
                                            : "NO CHAIN");
      }
      if (white_) {
        return white_;
      }
    }
    const uint32_t dbg_mip = REXCVAR_GET(nx1_d3d9_dbg_mipsrc);
    if (dbg_mip && white_ && entry.mip_source == (dbg_mip == 3 ? 0 : dbg_mip)) {
      return white_;
    }
    // A decode made from a holey source is NOT what this surface should look like. Show the
    // placeholder until the bytes arrive rather than the noise that is in the slot now.
    if (entry.decoded_from_partial && white_ && REXCVAR_GET(nx1_d3d9_hide_partial)) {
      return white_;
    }
    return entry.tex;
  }

  HostTextureFormat host = PickHostTextureFormat(t.format);
  // A block-compressed colour texture whose swizzle broadcasts one source channel to all of R,G,B
  // (a luminance/mask sprite -- the smoke and effect sprites are stored this way, e.g. swizzle
  // 0x200 = R,G,B<-red, A<-green) cannot be honoured by a compressed host texture, which samples a
  // fixed RGBA. Materialise it to A8R8G8B8 and apply the swizzle ourselves. Normal RGB textures map
  // R,G,B from distinct channels, so they stay compressed on the fast path.
  D3DFORMAT src_bc = D3DFMT_UNKNOWN;
  if (host.valid && host.decode == TexDecode::kNone &&
      (host.d3d == D3DFMT_DXT1 || host.d3d == D3DFMT_DXT3 || host.d3d == D3DFMT_DXT5)) {
    const uint32_t sx = t.swizzle & 7, sy = (t.swizzle >> 3) & 7, sz = (t.swizzle >> 6) & 7;
    if (sx == sy && sy == sz && sx < 4) {  // R,G,B all from the same data channel -> broadcast
      src_bc = host.d3d;
      host.d3d = D3DFMT_A8R8G8B8;
      host.decode = TexDecode::kColorSwizzle;
      host.opaque_block = false;
    } else if (t.swizzle != kIdentitySwizzle) {
      // TEMP DIAGNOSTIC: every other non-identity swizzle on a block-compressed texture is
      // currently ignored -- we bind the BC format and let the GPU sample RGBA straight
      // through. A sprite that sources its alpha from a colour channel would then come back
      // opaque, which is exactly a reticle rendering as a solid square and smoke as a solid
      // black one. Name them once each so we know whether that gap is real content or theory.
      static std::mutex m;
      static std::vector<uint32_t> seen;
      std::lock_guard<std::mutex> lk(m);
      if (std::find(seen.begin(), seen.end(), t.swizzle) == seen.end() && seen.size() < 24) {
        seen.push_back(t.swizzle);
        REXGPU_WARN("nx1_d3d9: BC texture with unhandled swizzle 0x{:03X} (x={} y={} z={} w={}) "
                    "fmt {} {}x{} addr {:08X} -- sampled as raw RGBA",
                    t.swizzle, sx, sy, sz, (t.swizzle >> 9) & 7, t.format, t.width, t.height,
                    t.base_address);
      }
    }
  }
  if (!host.valid) {
    if ((unsupported_texture_formats_++ % 100) == 0) {
      REXGPU_WARN("nx1_d3d9: unsupported Xenos texture format {} (sampler {})", t.format, sampler);
    }
    // The guest *expects* a texture here, so leaving the sampler unbound is worse than
    // wrong: a null sample reads back 0, and for the depth/shadow maps (fmt 23) that
    // means "fully occluded" -- the whole world shades black. White reads as "far /
    // unshadowed" and keeps lighting sane until real render targets land.
    return white_;
  }
  entry.last_frame = frame_;

  const auto* fmt = rex::graphics::FormatInfo::Get(t.format);
  if (!fmt) {
    return nullptr;
  }
  const uint32_t bpb = fmt->bytes_per_block();
  const uint32_t pitch_pixels = t.pitch_pixels ? t.pitch_pixels : t.width;
  rex::graphics::TextureExtent extent = rex::graphics::TextureExtent::Calculate(
      fmt, pitch_pixels, height, /*depth=*/1, t.tiled, /*is_guest=*/true);
  // Calculate() derives block_width from the pitch, but the host texture is created
  // at the *visible* width t.width. Cap the copy width to the visible blocks so
  // DetileMip2D fills exactly the created texture and never writes past its rows
  // (a wider pitch stays in block_pitch_h, used only for source addressing). Writing
  // the padded pitch width into a t.width texture corrupts the heap.
  extent.block_width = (t.width + fmt->block_width - 1) / fmt->block_width;

  // Mips are generated on the host, not read from the guest.
  //
  // NX1's fetch constants do declare a chain (mip_max_level 6..10) with the levels in a
  // separate allocation at mip_address -- but for most textures that memory does not hold
  // this texture's mips. Dumping the guest bytes and decoding them offline showed level 1 of
  // a trash decal resolving, cleanly and at exactly the pitch and tiling the layout predicts,
  // into an entirely different image; only textures whose mip_address happens to be
  // `base_address + base_size` (one contiguous allocation) carry their own chain. The rest
  // point into a shared pool that this build never fills -- it has no imagefile to stream
  // from -- so those levels are another image's pixels, permanently. Reading them is what
  // turned every minified surface into confetti.
  //
  // We only need *a* correct chain, not the artist's: the defect being fixed is aliasing.
  // Let the driver filter one down from level 0, which is data we know is right.
  //
  // This includes the CPU-decoded BC-alpha formats. They were excluded once, which left them
  // as the only textures in the game with no mip chain at all -- and the scope's noise masks
  // are exactly that: a DXT5A grain map, tiled hard and heavily minified. With no mips it
  // aliased its own noise, which is what filled the scope lens with coloured static. They
  // decode to A8R8G8B8, which mips like anything else.
  // Two ways to get a chain: let the driver filter one (uncompressed formats only), or build
  // it here (the block-compressed ones, which the driver refuses -- see the mip-gen section).
  //
  // Deliberately *not* gated on the guest's mip_max_level. A texture whose fetch constant
  // declares no chain is not a texture that must not be filtered -- it is one whose mips never
  // arrived, because this build has no imagefile to stream them from. On the console those same
  // surfaces have mips and are filtered; honouring the declaration reproduced a statement the
  // guest only makes because its data is missing, and left ~10% of the world aliasing. We
  // filter from level 0 regardless, so the guest's chain is not something we need.
  // GUEST MIP CHAIN. Plan the levels stored at mip_address, exactly as the reference's
  // GetMipLocation/GetMipExtent walk them: each level's storage is its pow2 dimensions padded
  // to 32x32 blocks (Calculate with is_guest), levels are laid out back to back, and once a
  // level's pow2 size drops to <=16 texels the whole remaining tail shares ONE 32x32-block
  // tile with per-level sub-tile offsets (GetPackedTileOffset). Decoding this chain instead of
  // generating one from level 0 is the distance-confetti fix -- see the cvar comment.
  struct GuestMip {
    uint32_t offset;  ///< byte offset of the level's storage from mip_address
    uint32_t ox, oy;  ///< packed-tail origin within that storage, in blocks
    uint32_t lw, lh;  ///< visible texel dimensions (what the host level holds)
    rex::graphics::TextureExtent ext;
  };
  std::vector<GuestMip> guest_plan;
  uint32_t guest_mip_bytes = 0;
  // Gate on the mip filter, UNCONDITIONALLY of any cvar: kBaseMap says the chain is not
  // resident, and decoding it painted nearly the whole world with the pool's garbage. A
  // texture that declares a SAMPLED chain (point/linear) is the opposite case -- hardware
  // reads its levels from mip_address, so that memory must be valid or the console itself
  // would show garbage.
  if (REXCVAR_GET(nx1_d3d9_guest_mips) && REXCVAR_GET(nx1_d3d9_mips) && t.mip_address &&
      t.mip_max_level > 0 && t.mip_filter != 2) {
    // USE THE REFERENCE'S OWN LAYOUT FUNCTION, NOT A PARALLEL ONE.
    //
    // We had a hand-rolled walk here and it disagreed with the hardware in at least two ways --
    // it missed the 4 KB per-level subresource alignment entirely, and after fixing that the scene
    // was still corrupt, so there was more. Since the set comparison proved we load exactly the
    // same textures from exactly the same addresses as the reference (0 divergences over 5834
    // addresses), there is no reason to keep a second implementation of the layout: call the one
    // that is known to render this game correctly.
    //
    // GetGuestTextureLayout returns per-level row pitch, z stride, extents and -- critically --
    // mip_offsets_bytes[], which already includes the subresource alignment and the packed-tail
    // rules that our version approximated.
    namespace tu2 = rex::graphics::texture_util;
    const auto gl = tu2::GetGuestTextureLayout(
        rex::graphics::xenos::DataDimension::k2DOrStacked,
        (t.pitch_pixels ? t.pitch_pixels : t.width) >> 5, t.width, height, /*depth*/ 1, t.tiled,
        static_cast<rex::graphics::xenos::TextureFormat>(t.format), t.packed_mips,
        /*has_base=*/true, t.mip_max_level);
    const uint32_t packed_level = gl.packed_level;
    const uint32_t last_stored = std::min(gl.max_level, packed_level);
    for (uint32_t m = 1; m <= last_stored; ++m) {
      const uint32_t lw = std::max(t.width >> m, 1u), lh = std::max(height >> m, 1u);
      if (lw < 4 || lh < 4) {
        break;  // stop where BcMipLevels stops: a 4x4 tail level is already one flat block
      }
      const auto& lvl = gl.mips[m];
      GuestMip gm;
      gm.offset = gl.mip_offsets_bytes[m];
      gm.ox = 0;
      gm.oy = 0;
      gm.lw = lw;
      gm.lh = lh;
      // Rebuild an extent whose block_pitch_h/v match the layout's strides, so the tiled address
      // maths downstream uses the reference's pitch rather than one we re-derived.
      gm.ext = rex::graphics::TextureExtent::Calculate(
          fmt, std::max(rex::next_pow2(t.width) >> m, 1u),
          std::max(rex::next_pow2(height) >> m, 1u), /*depth=*/1, t.tiled, /*is_guest=*/true);
      gm.ext.block_pitch_h = lvl.row_pitch_bytes / bpb;
      gm.ext.block_pitch_v = lvl.z_slice_stride_block_rows;
      if (m == packed_level && t.packed_mips) {
        rex::graphics::TextureInfo::GetPackedTileOffset(
            std::max(rex::next_pow2(t.width) >> m, 1u),
            std::max(rex::next_pow2(height) >> m, 1u), fmt, 0, &gm.ox, &gm.oy);
      }
      guest_mip_bytes = std::max(guest_mip_bytes, gm.offset + lvl.level_data_extent_bytes);
      gm.ext.block_width = (lw + fmt->block_width - 1) / fmt->block_width;
      gm.ext.block_height = (lh + fmt->block_height - 1) / fmt->block_height;
      guest_plan.push_back(gm);
    }
  }
  const bool guest_mips = !guest_plan.empty();

  // kBaseMap is honoured at the SAMPLER (MIPFILTER=NONE, see nx1_d3d9_basemap), never here.
  //
  // Skipping chain generation for kBaseMap textures looks like free decode time and is a trap:
  // mip_filter lives in the FETCH CONSTANT, which carries sampler state alongside the texture,
  // so the same texture bound by two materials -- one asking kBaseMap, one asking linear --
  // would flip the chain's existence between binds. With the level count (and mip_source) part
  // of what decides whether the host texture is recreated, that released and recreated the
  // texture on EVERY bind. Under D3D9Ex a DEFAULT-pool Release is deferred until the GPU is
  // done, so the destruction queue outran the driver: process memory climbed to 33 GB within
  // seconds of entering a match. The texture object must depend only on the texture.
  const bool auto_mips = SupportsAutoMips(device_, host.d3d);
  const bool bc_mips = IsBlockCompressed(host.d3d);
  const bool want_mips = REXCVAR_GET(nx1_d3d9_mips) && (guest_mips || auto_mips || bc_mips);
  const bool build_mips =
      want_mips && !guest_mips && !auto_mips && REXCVAR_GET(nx1_d3d9_bc_mips);
  const uint32_t levels = guest_mips ? uint32_t(1 + guest_plan.size())
                                     : (build_mips ? BcMipLevels(t.width, height) : 1);

  // Track how each texture's mip chain is provided (guest chain / built here / driver auto-gen /
  // skipped), so the periodic "mipgen" stats line can show the split across the whole run.
  const bool driver_mips = auto_mips && want_mips && !guest_mips;
  entry.mip_source = guest_mips ? 3 : build_mips ? 1 : (driver_mips ? 2 : 0);
  if (guest_mips) {
    ++mips_guest_;
  } else if (build_mips) {
    ++mips_built_;
  } else if (want_mips) {
    ++mips_auto_;
  } else if (t.mip_filter == 2) {
    // Counted for visibility only. The chain is still BUILT for these -- the sampler is what
    // refuses to read it -- so this must never feed back into the texture object.
    ++mips_basemap_;
  } else if (t.mip_max_level == 0) {
    ++mips_skip_nochain_;
  } else {
    ++mips_skip_unsupported_;
  }

  // NOTE: a per-frame REBUILD BUDGET was tried here and removed. The theory was that bursts of
  // 20-30 rebuilds in one frame caused the 19-30 ms drain spikes PROF/hitch attributes to the
  // worker. PROF/tex disproved it: steady state runs at UNDER ONE rebuild per frame
  // (new=0.1-0.9, dirty=0.0), so a budget never binds during play. The only place it did bind
  // was the map-load burst, where every texture is a first sight -- at 6/frame that needs
  // thousands of frames to catch up and left the screen black for three seconds. No effect where
  // it was aimed, real harm where it was not looked. If rebuild bursts ever do become the
  // problem, any budget needs a cold-start exemption: deferring a first sight means rendering
  // black, which is only tolerable when a stale texture already exists to show instead.
  // Sized by the TILED EXTENT, not the area -- for 1- and 2-byte blocks the tiled address reaches
  // past pitch*rows*bpb and this buffer is what DetileMip2D reads through. See
  // TiledSurfaceExtentBytes. Identical to the old value for every bpb >= 4, so BC and 32-bit
  // textures are unaffected.
  const size_t guest_bytes =
      t.tiled ? TiledSurfaceExtentBytes(extent.block_pitch_h, extent.block_pitch_v, bpb)
              : size_t(extent.block_pitch_h) * extent.block_pitch_v * bpb;
  // Decode from the CPU snapshot mirror, NOT live guest RAM. The mirror captured these bytes while
  // the texture was resident (early sweep / first touch) and holds them; live RAM would have the
  // streaming pool's transient fill by now, which is the confetti. entry.dirty (set by the write-
  // watch, via DrainMemoryWrites, when the guest rewrites this texture) is what forces a fresh
  // decode from the re-snapshotted mirror.
  // Reaching here means the early-out above declined: no texture yet, a changed layout, or the
  // write-watch dirtied this one. All three want fresh bytes, so the snapshot is never wasted.
  // Everything from here on is the rebuild; charge it to the upload timer instead of the lookup.
  if (prof_enabled_) {
    const auto now = std::chrono::steady_clock::now();
    prof_tex_.lookup_ns += uint64_t((now - prof_lookup.t0).count());
    prof_lookup.sink = nullptr;
    prof_upload.on = true;
    prof_upload.t0 = now;
    prof_upload.sink = &prof_tex_.upload_ns;
    ++prof_tex_.uploads;
    if (!entry.tex) {
      ++prof_tex_.why_new;
    } else if (entry.layout_key != layout_key) {
      ++prof_tex_.why_layout;
    } else if (entry.zero_retries) {
      ++prof_tex_.why_zero;  // never-resident sprite retrying; should now be rare, not every frame
    } else {
      ++prof_tex_.why_dirty;
    }
  }
  const uint8_t* src;
  if (REXCVAR_GET(nx1_d3d9_texture_mirror)) {
    src = MirrorSnapshot(t.base_address, uint32_t(guest_bytes));
    // IS THE SNAPSHOT STALE? Every census this investigation has run reads LIVE guest memory,
    // while the decode reads the mirror -- so a divergence between the two is precisely the blind
    // spot none of them could report. Compare what we are about to decode against what guest RAM
    // holds right now. Page-granular, because the corruption is: part of a texture correct, part
    // foreign, split on 4 KB boundaries.
    if (REXCVAR_GET(nx1_d3d9_dbg_mirrorstale) && guest_bytes) {
      if (const uint8_t* live = TranslatePhysical(t.base_address)) {
        uint32_t pages = 0, diff = 0;
        for (size_t off = 0; off < guest_bytes; off += 4096) {
          const size_t len = std::min<size_t>(4096, guest_bytes - off);
          ++pages;
          if (std::memcmp(src + off, live + off, len) != 0) {
            ++diff;
          }
        }
        ++mirrorstale_decodes_;
        mirrorstale_pages_ += pages;
        if (diff) {
          ++mirrorstale_stale_decodes_;
          mirrorstale_stale_pages_ += diff;
          if (mirrorstale_stale_decodes_ <= 12) {
            REXGPU_WARN("nx1_d3d9: MIRRORSTALE {:08X} {}x{} fmt={} -- {} of {} snapshot pages "
                        "DIFFER from live guest memory, so this decode is about to use bytes the "
                        "slot no longer holds",
                        t.base_address, t.width, height, t.format, diff, pages);
          }
        }
        if ((mirrorstale_decodes_ % 2000) == 0) {
          REXGPU_WARN("nx1_d3d9: MIRRORSTALE {} of {} decodes read a STALE snapshot ({} of {} "
                      "pages differed from live memory). Nonzero means the DECODE SOURCE, not "
                      "guest memory, is what holds the wrong texture",
                      mirrorstale_stale_decodes_, mirrorstale_decodes_, mirrorstale_stale_pages_,
                      mirrorstale_pages_);
        }
      }
    }
  } else {
    // Live read, but still watch the pages: the watch used to be armed only as a side effect
    // of snapshotting, so reading live left every texture unwatched and a decode taken before
    // the guest filled the memory could never be redone.
    ArmWriteWatch(t.base_address, uint32_t(guest_bytes));
    src = TranslatePhysical(t.base_address);
  }
  // HOW FAR MAY THIS DECODE READ? See nx1_d3d9_clamp_alloc. The fetch constant can declare more
  // bytes than the pool allocated, in which case the tail of our read is the NEXT texture and
  // decodes as rainbow noise. Stop at the next live base instead.
  //
  // "Live" is the whole point: the map accumulates every base ever bound, and an address the pool
  // has since freed still sits in it. Clamping against a dead neighbour would truncate a texture
  // that is perfectly fine, so only bases bound within the last few frames count.
  size_t src_limit = SIZE_MAX;
  live_bases_[t.base_address] = frame_;
  if (REXCVAR_GET(nx1_d3d9_clamp_alloc) && guest_bytes) {
    constexpr uint64_t kLiveFrames = 4;
    for (auto it = live_bases_.upper_bound(t.base_address);
         it != live_bases_.end() && uint64_t(it->first) < uint64_t(t.base_address) + guest_bytes;
         ++it) {
      if (frame_ - it->second <= kLiveFrames) {
        src_limit = size_t(it->first - t.base_address);
        ++clamp_hits_;
        clamp_bytes_ += guest_bytes - src_limit;
        break;
      }
    }
  }
  // AFTER the source is chosen, and deliberately outside the branch above: this census asks whose
  // bytes we are about to decode, which is the same question whether they came from the snapshot
  // or from live memory. It was originally written inside the mirror branch and therefore never
  // ran at all -- the config in use had nx1_d3d9_texture_mirror=false, so all 26,727 decodes of
  // that run took the else path and the detector reported nothing while appearing to work.
  // Walk the blocks that become VISIBLE TEXELS, not the padded block grid. extent.block_width is
  // derived from the PITCH (round_up(pitch, fmt->block_width) / fmt->block_width), so for a 398
  // wide texture with pitch 416 it spans 18 columns of padding per row -- memory that never
  // reaches a texel and legitimately holds other data. Walking it is how the previous revision
  // still counted padding after going to the trouble of excluding the alignment gaps.
  const uint32_t img_bw = fmt ? std::min((t.width + fmt->block_width - 1) / fmt->block_width,
                                         extent.block_width)
                              : extent.block_width;
  const uint32_t img_bh = fmt ? std::min((height + fmt->block_height - 1) / fmt->block_height,
                                         extent.block_height)
                              : extent.block_height;
  NoteHwCensus(t);
  // Reaching here means the resolve map did NOT serve this bind -- both serve paths return early.
  // So if this texture's memory overlaps something the guest resolved into, we are untiling guest
  // RAM where a rendered image belongs, which is the render-to-texture failure mode.
  NoteResolveTextureCensus(PhysicalAddress(t.base_address), guest_bytes, false);
  NotePageOrigin(src, guest_bytes, t, height, img_bw, img_bh, extent.block_pitch_h, bpb);
  // Scoped so the list can never leak into the NEXT texture's decode -- painting the wrong
  // texture would invalidate exactly the observation this test exists to make.
  struct PaintScope {
    ~PaintScope() { g_paint_pages = nullptr; }
  } paint_scope;
  g_paint_pages = paint_pages_.empty() ? nullptr : &paint_pages_;
  // ONLY the layout decides recreation. mip_source was added here too and had to come back out:
  // it is derived from state that can differ between binds of the same texture, so it turned
  // every such bind into a release + recreate (see the want_mips note above).
  if (entry.tex && entry.layout_key != layout_key) {
    entry.tex->Release();
    entry.tex = nullptr;
    ++tex_rebuilds_;
    // A different texture now occupies this key -- restart its warm-up before it re-commits.
    entry.good_frames = 0;
    entry.committed = false;
  }

  // Set when a broadcast-swizzle sprite decodes to a fully transparent (unstreamed) image, so it is
  // not committed and keeps re-decoding until its texel pool is resident.
  bool swizzle_all_zero = false;

  // TEMP PROFILING: stage boundaries inside the rebuild. A rebuild costs ~673 us; whether that
  // is the CPU BC encoder or the driver stalling at a saturated 4 GB of texture memory decides
  // which fix is the right one, so measure the four stages separately.
  auto stage_mark = [this] {
    return prof_enabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  };
  auto stage_add = [this](uint64_t& sink, std::chrono::steady_clock::time_point t0) {
    if (prof_enabled_) sink += uint64_t((std::chrono::steady_clock::now() - t0).count());
  };
  const auto t_stage = stage_mark();

  // IDirect3DDevice9Ex has no D3DPOOL_MANAGED. Fill a lockable SYSTEMMEM staging
  // texture with level 0 and UpdateTexture it into a DEFAULT texture that can be sampled.
  IDirect3DTexture9* staging = nullptr;
  if (FAILED(device_->CreateTexture(t.width, height, levels, 0, host.d3d, D3DPOOL_SYSTEMMEM,
                                    &staging, nullptr))) {
    REXGPU_ERROR("nx1_d3d9: staging CreateTexture({}x{}, fmt {}) failed", t.width, height,
                 t.format);
    return nullptr;
  }

  D3DLOCKED_RECT locked;
  if (FAILED(staging->LockRect(0, &locked, nullptr, 0))) {
    staging->Release();
    return nullptr;
  }
  // The runtime's pitch for a block format it does not recognise counts texels, not the block
  // row it actually has to hold -- 4x too small for BC5. The driver reads such a level as
  // tightly packed block rows, which is exactly what its `width * height` bytes hold.
  stage_add(prof_tex_.stage_ns, t_stage);
  if (prof_enabled_) prof_tex_.decode_bytes += guest_bytes;
  const auto t_decode = stage_mark();
  const uint32_t dst_row_bytes =
      host.opaque_block ? extent.block_width * bpb : uint32_t(locked.Pitch);
  auto* dst = static_cast<uint8_t*>(locked.Pitch >= 0 ? locked.pBits : nullptr);
  // PACKED MIP TAIL: level 0 is not necessarily at offset (0,0).
  //
  // Once a texture's levels shrink to 16 texels or less, Xenos stops giving each level its own
  // image and packs the whole tail into ONE 32x32-block tile, side by side. The SDK models this --
  // TextureInfo::GetMipLocation short-circuits mip 0 to base_address but STILL asks
  // GetPackedTileOffset for its offset when has_packed_mips is set -- and DetileMip2D has taken
  // offset_x/offset_y for exactly this reason all along. Every level-0 call site here passed the
  // (0,0) default, so for any packed texture we detiled from the wrong corner of the shared tile
  // and got whichever sibling level happened to sit there.
  //
  // This is why it hits SMALL textures specifically. The material behind the opaque-glass bug
  // samples 16x16, 8x8 and 64x64; large surfaces never take the packed path, which is why the
  // world looks right while these resolve to a flat block of a neighbour's texels -- white on the
  // glass, white on the sky, speckle at distance. Same defect, three long-standing symptoms.
  uint32_t packed_ox = 0, packed_oy = 0;
  if (t.packed_mips && REXCVAR_GET(nx1_d3d9_packed_mip_offset)) {
    rex::graphics::TextureInfo::GetPackedTileOffset(rex::next_pow2(t.width), rex::next_pow2(height),
                                                    fmt, /*packed_tile=*/0, &packed_ox, &packed_oy);
  }
  // Arming proof. Three counters, because "the glass is still white" has three different meanings
  // and they are indistinguishable without this: the flag is never set on the textures we care
  // about (small_decodes high, packed_decodes 0), the flag is set but the offset comes back (0,0)
  // so the change is a no-op (packed_decodes high, packed_offsets 0), or it really did relocate
  // the read and did not help (packed_offsets high). Only the third is evidence against the fix.
  if (t.width <= 16 || height <= 16) ++small_decodes_;
  if (t.packed_mips) ++packed_decodes_;
  if (packed_ox || packed_oy) ++packed_offsets_;
  // Bordered textures are laid out WITH their border texels, and XGUntileTextureLevel takes the
  // border flag as a required argument -- ours does not handle it at all. Count before building
  // anything: if NX1 never sets this, the gap costs nothing and needs only a comment.
  if (t.border) ++border_decodes_;

  /// Where the mip builder reads level 0 from. Defaults to the mapped surface, but the
  /// block-compressed path below redirects it to a normal-RAM copy -- see the comment there.
  const uint8_t* mip_source = dst;
  // THE CPU-DECODE PATHS BELOW MUST NOT LEAVE mip_source POINTING AT `dst`.
  //
  // `dst` is mapped D3D9 staging memory, which is routinely WRITE-COMBINED: fine to write,
  // unreliable to READ -- a read can bypass the pending write-combine buffers and return stale or
  // partial bytes. The plain block-compressed path below already detours through normal RAM for
  // exactly this reason, and its comment names the symptom: "That is the distance speckle on
  // block-compressed surfaces."
  //
  // But only that one path got the treatment. kColorSwizzle, kDXN and BC-alpha all wrote straight
  // into `dst` and left mip_source = dst, so every level of their chains was built from a
  // write-combined read-back. kDXN is the normal-map format, i.e. every world surface in this
  // game. Measured: with mips disabled entirely the scene is CLEAN, so level 0 is fine and the
  // corruption is produced by the chain -- which is what this fixes.
  uint8_t* argb_stage = nullptr;
  /// Bytes written into `dst` from a normal-RAM buffer, for the staging readback check below.
  /// Zero for the plain path, which detiles straight into `dst` and so has nothing to compare to.
  size_t staged_bytes = 0;
  const size_t argb_bytes = size_t(locked.Pitch) * height;
  if (dst) {
    if (host.decode == TexDecode::kColorSwizzle) {
      // Detile the compressed colour blocks, then decode to A8R8G8B8 applying the channel swizzle
      // (the compressed host format cannot honour a broadcast swizzle) -- see src_bc above.
      uint8_t* scratch = DetileScratch(size_t(extent.block_width) * extent.block_height * bpb);
      DetileMip2D(scratch, extent.block_width * bpb, src, extent, bpb, t.endian, t.tiled, packed_ox, packed_oy, src_limit);
      argb_stage = ArgbScratch(argb_bytes);
      DecodeBcColorSwizzledToArgb(argb_stage, uint32_t(locked.Pitch), scratch, extent.block_width,
                                  extent.block_height, t.width, height, src_bc, bpb, t.swizzle);
      // A broadcast-swizzle sprite that decodes to all zero is not resident yet: its texel pool has
      // not been streamed in (the same residency gap behind the distant-surface confetti). Caching
      // that as a settled texture would leave the effect permanently invisible even after the data
      // lands, so mark it dirty to force a re-decode until it carries real content.
      uint64_t sa = 0;
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* r = dst + size_t(y) * locked.Pitch;
        for (uint32_t x = 0; x < t.width; ++x) sa += r[x * 4 + 3];
      }
      swizzle_all_zero = sa == 0;
      std::memcpy(dst, argb_stage, argb_bytes);
      mip_source = argb_stage;
      staged_bytes = argb_bytes;
    } else if (host.decode == TexDecode::kDXN) {
      // CPU-decode DXN normal maps with the hardware RGGG swizzle (see PickHostTextureFormat).
      uint8_t* scratch = DetileScratch(size_t(extent.block_width) * extent.block_height * bpb);
      DetileMip2D(scratch, extent.block_width * bpb, src, extent, bpb, t.endian, t.tiled, packed_ox, packed_oy, src_limit);
      argb_stage = ArgbScratch(argb_bytes);
      DecodeDXNToArgb(argb_stage, uint32_t(locked.Pitch), scratch, extent.block_width,
                      extent.block_height, t.width, height);
      std::memcpy(dst, argb_stage, argb_bytes);
      mip_source = argb_stage;
      staged_bytes = argb_bytes;
    } else if (host.decode != TexDecode::kNone) {
      // CPU-decode single-channel BC-alpha: detile the compressed 8-byte blocks
      // into a linear scratch buffer, then expand each block to A8R8G8B8 (v,v,v,v).
      uint8_t* scratch = DetileScratch(size_t(extent.block_width) * extent.block_height * bpb);
      DetileMip2D(scratch, extent.block_width * bpb, src, extent, bpb, t.endian, t.tiled, packed_ox, packed_oy, src_limit);
      argb_stage = ArgbScratch(argb_bytes);
      DecodeBCAlphaToArgb(argb_stage, uint32_t(locked.Pitch), scratch, extent.block_width,
                          extent.block_height, t.width, height,
                          host.decode == TexDecode::kDXT5A, t.swizzle);
      std::memcpy(dst, argb_stage, argb_bytes);
      mip_source = argb_stage;
      staged_bytes = argb_bytes;
    } else if (build_mips) {
      // Detile into ordinary RAM and copy up, rather than writing straight into the mapped
      // surface. The mip chain below has to READ level 0 back, and `dst` is D3D9 staging
      // memory: mapped upload memory is routinely write-combined, which is fine to write and
      // unreliable to read -- a read can bypass the pending write-combine buffers and return
      // stale or partial bytes. Level 0 itself still looks right on screen because the GPU
      // receives what we wrote; only our read-back sees garbage, and every generated level is
      // built from it. That is the distance speckle on block-compressed surfaces.
      const size_t level0_bytes = size_t(extent.block_height) * dst_row_bytes;
      uint8_t* staged = DetileScratch(level0_bytes);
      DetileMip2D(staged, dst_row_bytes, src, extent, bpb, t.endian, t.tiled, packed_ox, packed_oy, src_limit);
      const Swizzle32 swz = MakeSwizzle32(t.swizzle);
      if (host.swizzle32 && !swz.identity) {
        for (uint32_t by = 0; by < extent.block_height; ++by) {
          SwizzleRow32(staged + size_t(by) * dst_row_bytes, extent.block_width, swz);
        }
      }
      std::memcpy(dst, staged, level0_bytes);
      mip_source = staged;
      staged_bytes = level0_bytes;
    } else {
      DetileMip2D(dst, dst_row_bytes, src, extent, bpb, t.endian, t.tiled, packed_ox, packed_oy, src_limit);
      const Swizzle32 swz = MakeSwizzle32(t.swizzle);
      if (host.swizzle32 && !swz.identity) {
        for (uint32_t by = 0; by < extent.block_height; ++by) {
          SwizzleRow32(dst + size_t(by) * dst_row_bytes, extent.block_width, swz);
        }
      }
    }
  }

  // DOES LEVEL 0 ACTUALLY LAND IN THE STAGING SURFACE?
  //
  // The one segment never verified IN THE LINE OF FIRE. The decode has been checked twice against
  // independent implementations -- but always against DUMP files, which are written from the
  // Rgba8/scratch buffers, not from the memory UpdateTexture reads. The synthetic-texture test
  // proved upload/sampler/level-selection sound, but it bypasses the decode entirely. So nothing
  // has ever confirmed that the bytes we decoded are the bytes sitting in level 0 when the upload
  // happens.
  //
  // It was assumed unverifiable: `dst` is mapped staging, believed write-combined and unreliable
  // to read back. That assumption looks wrong -- the ENC round-trip already reads back mip levels
  // of this same SYSTEMMEM texture and gets coherent images.
  //
  // THE CONTROL IS BUILT IN, because "readback differs from source" has two causes and the whole
  // point is to separate them. Read `dst` TWICE into separate buffers:
  //   A != B          -> the readback itself is unstable (write-combining is real). Says nothing
  //                      about what the GPU receives, and the check must be discarded.
  //   A == B, A != src-> the readback is stable and the bytes genuinely differ from what we
  //                      decoded. That is a write that did not land, in the line of fire.
  //   A == B == src   -> level 0 is exactly what we decoded. Segment closed for good.
  if (REXCVAR_GET(nx1_d3d9_dbg_stagecheck) && dst && staged_bytes && mip_source != dst) {
    static std::mutex m;
    static uint64_t checks = 0, unstable = 0, differing = 0, diff_bytes = 0;
    std::vector<uint8_t> a(staged_bytes), b(staged_bytes);
    std::memcpy(a.data(), dst, staged_bytes);
    std::memcpy(b.data(), dst, staged_bytes);
    const bool stable = std::memcmp(a.data(), b.data(), staged_bytes) == 0;
    const bool same = std::memcmp(a.data(), mip_source, staged_bytes) == 0;
    size_t nbad = 0;
    if (stable && !same) {
      for (size_t i = 0; i < staged_bytes; ++i) {
        if (a[i] != mip_source[i]) ++nbad;
      }
    }
    std::lock_guard<std::mutex> lk(m);
    ++checks;
    if (!stable) ++unstable;
    if (stable && !same) {
      ++differing;
      diff_bytes += nbad;
    }
    if ((checks % 2000) == 1) {
      REXGPU_WARN("nx1_d3d9: STAGECHECK {} level-0 uploads verified | readback UNSTABLE={} (a "
                  "nonzero here voids the rest -- write-combined memory) | stable-but-DIFFERENT "
                  "from the decode={} ({} bytes total). All zeros = level 0 reaches the GPU "
                  "exactly as decoded",
                  checks, unstable, differing, diff_bytes);
    }
  }

  // D9TEX -- our side of the REFTEX comparison. Same dedup key and same fields, so the two logs
  // diff directly. The question is whether the native renderer decodes textures the reference
  // never loads (we bind or resolve differently) or the same set (the difference is which
  // subresource, not which texture). Per-draw comparison cannot answer this -- the two backends
  // run unsynchronised, which is what made FETCHCMP report 27,616 bogus mismatches -- but SET
  // comparison needs no synchronisation at all.
  if (REXCVAR_GET(nx1_d3d9_dbg_texset)) {
    static std::mutex d9_m;
    static std::unordered_set<uint64_t> d9_seen;
    const uint64_t sig = (uint64_t(t.base_address >> 12) << 40) ^
                         (uint64_t(t.mip_address >> 12) << 20) ^
                         (uint64_t(t.width - 1) << 8) ^ uint64_t(t.format);
    std::lock_guard<std::mutex> lk(d9_m);
    if (d9_seen.size() < 60000 && d9_seen.insert(sig).second) {
      REXGPU_INFO("D9TEX base={:08X} mip={:08X} {}x{} fmt={} tiled={} packed={} mip_max={}",
                  t.base_address, t.mip_address, t.width, height, t.format, t.tiled ? 1 : 0,
                  t.packed_mips ? 1 : 0, t.mip_max_level);
    }
  }

  // CLASSIFY THE FAILURE, don't eyeball it. The capture shows TWO different looks -- a magenta
  // checker rim and rainbow noise -- and treating them as one bug is why no single-cause theory
  // has ever explained all the symptoms. They separate on a property that needs no engine
  // cooperation: a PLACEHOLDER is a small tile repeated across the surface, so its rows repeat
  // at a short period; unstreamed GARBAGE is high-entropy and repeats at no period at all. A
  // real texture is neither -- it varies, but smoothly.
  //
  // Measured on the guest bytes (pre-decode), so it is independent of every decode path.
  if (src && dst && guest_bytes >= 4096) {
    const uint8_t* g = src;
    const size_t span = std::min<size_t>(guest_bytes, 16384);
    // Row-period test: does the source repeat at 64/128/256/512 bytes?
    uint32_t best_period = 0;
    for (uint32_t period : {64u, 128u, 256u, 512u, 1024u}) {
      if (span < size_t(period) * 4) continue;
      size_t same = 0, total = 0;
      for (size_t i = period; i < span; i += 7) {  // sparse sample
        same += (g[i] == g[i - period]) ? 1 : 0;
        ++total;
      }
      if (total && (same * 100 / total) >= 92) {
        best_period = period;
        break;
      }
    }
    // Entropy proxy: distinct byte values in a sample. Garbage saturates; a placeholder tile
    // and a smooth texture both use few.
    bool seen[256] = {};
    uint32_t distinct = 0;
    for (size_t i = 0; i < span; i += 3) {
      if (!seen[g[i]]) { seen[g[i]] = true; ++distinct; }
    }
    if (best_period) {
      ++repeating_source_binds_;
    } else if (distinct >= 250) {
      ++highentropy_source_binds_;
    }
    if (const uint32_t budget = REXCVAR_GET(nx1_d3d9_dbg_classify); budget &&
        (best_period || distinct >= 250)) {
      REXCVAR_SET(nx1_d3d9_dbg_classify, budget - 1);
      REXGPU_WARN("nx1_d3d9: SRCCLASS {:08X} {}x{} fmt={} -> {} (period={} distinct={}/256)",
                  t.base_address, t.width, height, t.format,
                  best_period ? "REPEATING (placeholder-like)" : "HIGH-ENTROPY (garbage-like)",
                  best_period, distinct);
    }
  }

  uint32_t partial_pages = 0;
  uint32_t src_pages_total = 0;

  // DID THIS TEXTURE'S CONTENT CHANGE UNDER US? The corruption is image-space (top quarter,
  // proportional to height, sub-page on smaller textures), so no single contiguous write can
  // have produced it -- but PAGEWRITES shows every page written 2-4 times. That fits a texture
  // decoded correctly once and re-decoded later, after the guest wrote the slot again, with the
  // reference still showing what the FIRST upload produced. Hash every decode and report only
  // the transitions: an address whose hash changes tells us exactly when it went bad, and how
  // many writes had landed by then. Silence means each texture decodes the same way forever and
  // the "we re-read stale memory" model is wrong too.
  bool hash_dump_match = false;  ///< set when this decode matches nx1_d3d9_dbg_dump_hash_lo32
  if (src && dst) {
    uint64_t h = 1469598103934665603ull;
    const size_t hashed = size_t(dst_row_bytes) * extent.block_height;
    for (size_t i = 0; i < hashed; i += 257) h = (h ^ dst[i]) * 1099511628211ull;
    // DMA COVERAGE CENSUS -- see the cvar's comment. Runs on every decode so the sample is not
    // selected by the provenance detector, which is measured unreliable.
    if (REXCVAR_GET(nx1_d3d9_dbg_dmacover) && guest_bytes) {
      ++dmacover_decodes_;
      const uint32_t p_first = t.base_address >> 12;
      const uint32_t p_last = uint32_t((uint64_t(t.base_address) + guest_bytes - 1) >> 12);
      for (uint32_t p = p_first; p <= p_last; ++p) {
        switch (DmaPageVerdictFor(p << 12)) {
          case 0: ++dmacover_pg_never_; break;
          case 1: ++dmacover_pg_skipped_; break;
          default: ++dmacover_pg_written_; break;
        }
      }
      // Decode-level: did the copy covering our BASE reach the last byte we decode? Measured from
      // the copy's own base, since a texture can sit partway into a larger copy's destination.
      uint32_t cbase = 0, cbytes = 0, calt = 0;
      if (DmaCoverageFor(t.base_address, &cbase, &cbytes, &calt)) {
        const uint64_t need = (uint64_t(t.base_address) - cbase) + guest_bytes;
        if (cbytes < need) {
          ++dmacover_under_;
          // The discriminator for the lo-vs-hi ImageAllocInfo question: if the half we DIDN'T use
          // would have spanned the texture, the size choice is simply wrong.
          if (calt >= need) ++dmacover_alt_fits_;
        } else {
          ++dmacover_covered_;
        }
      } else {
        ++dmacover_nocopy_;
      }
    }
    // THIS COUNTER READ ZERO FOR ITS ENTIRE LIFE, and not because nothing wrote the pages.
    // `src_pages_total` is declared 0 at the top of this function and not assigned until ~130
    // lines BELOW here, so the loop bound was always 0, the body never ran, and every
    // DECODECHANGE line in every log has reported "page writes 0 -> 0" unconditionally. It was
    // read across sessions as proof that guest texture memory moves through paths the write-watch
    // cannot see, and a GPU-write watch was built on that reading. Derive the span locally from
    // the bytes this decode actually reads, so the number cannot depend on assignment order.
    uint32_t wsum = 0;
    if (guest_bytes) {
      const uint32_t p_first = t.base_address >> 12;
      const uint32_t p_last = uint32_t((uint64_t(t.base_address) + guest_bytes - 1) >> 12);
      for (uint32_t p = p_first; p <= p_last && p < page_writes_.size(); ++p) {
        wsum += page_writes_[p];
      }
    }
    auto& prev = decode_hashes_[DecodeViewKey(t)];
    // Index every decode's content by hash, and ask whether a CHANGED texture now holds content
    // first seen somewhere else. Registration happens for all decodes; the report only on change.
    if (REXCVAR_GET(nx1_d3d9_dbg_slotreuse)) {
      struct SeenTex {
        uint32_t addr, w, h, fmt, addr_count;
      };
      static std::mutex sm;
      static std::unordered_map<uint64_t, SeenTex> seen;
      std::lock_guard<std::mutex> lk(sm);
      auto it = seen.find(h);
      if (it == seen.end()) {
        if (seen.size() < 65536) {
          seen.emplace(h, SeenTex{t.base_address, t.width, height, t.format, 1});
        }
      } else {
        if (it->second.addr != t.base_address) {
          ++it->second.addr_count;
          // <= 3 distinct addresses: not one of the shared dummy maps that legitimately converge.
          if (prev.hash && prev.hash != h && it->second.addr_count <= 3) {
            REXGPU_WARN("nx1_d3d9: SLOTREUSE {:08X} ({}x{} fmt={}) changed INTO content first "
                        "decoded at {:08X} ({}x{} fmt={}) -- this surface is now showing another "
                        "texture's bytes, so the pool reassigned the slot under us",
                        t.base_address, t.width, height, t.format, it->second.addr, it->second.w,
                        it->second.h, it->second.fmt);
          }
        }
      }
    }
    // ATTRIBUTE THIS REBUILD TO THE SOURCE THAT CAUSED IT.
    //
    // The reference has been measured never re-reading a bad texture's memory (4.35M pages
    // uploaded, zero in its range) while we do -- so the open question is which of OUR
    // invalidation sources sends us back to guest RAM to adopt bytes it ignores. Counting rebuilds
    // per source is not enough on its own: a source that re-reads memory which turns out unchanged
    // costs time but cannot corrupt anything. What matters is the source whose rebuilds actually
    // CHANGE the decoded content, because that is the one adopting new bytes.
    {
      const size_t si = size_t(entry.dirty_source);
      if (si < size_t(DirtySource::kCount)) {
        ++dirty_by_source_[si];
        if (prev.hash && prev.hash != h) ++changed_by_source_[si];
      }
    }
    if (prev.hash && prev.hash != h) {
      ++prev.changes;
      // Rate-limited: 1058 lines in one session. The COUNT is what matters (it is the churn signal
      // compared across A/B runs); the individual lines were only ever useful when chasing a named
      // texture, and TRACK does that better.
      static std::atomic<uint64_t> dc_seen{0};
      const bool dc_print = (dc_seen.fetch_add(1, std::memory_order_relaxed) % 200) == 0;
      if (dc_print)
      NX1_LOGW_TEX("nx1_d3d9: DECODECHANGE {:08X} {}x{} fmt={} frame {} -> {} | hash {:016X} -> "
                  "{:016X} | page writes {} -> {}",
                  t.base_address, t.width, height, t.format, prev.frame, frame_, prev.hash, h,
                  prev.writes, wsum);
      // FLICKER DETECTOR. The artifact is now a surface showing the right texture, ONE bad frame,
      // then the right texture again -- i.e. a decode hash going X -> Y -> X. That is a MECHANISM
      // signal, not a content judgement, and it is the thing to capture.
      //
      // Content scoring failed at this: black-heavy UI and high-frequency detail art (wood grain
      // scored 58% "noise" while being perfectly clean) both trip it, so a corruption threshold
      // samples high-variance textures rather than broken ones. A hash reverting to its own
      // previous value cannot be explained by the texture merely being detailed.
      //
      // Dump the MIDDLE state Y: it is the frame that reached the screen wrong. `prev2` is the
      // hash before `prev`, so `h == prev2` with `prev.hash != h` is exactly the X -> Y -> X
      // return, and `prev` was the bad one. We dump on the RETURN, when the pattern is confirmed,
      // and record the offending hash so the earlier dump of Y can be matched up in the log.
      if (REXCVAR_GET(nx1_d3d9_dbg_dump_flicker) && prev.prev_hash && prev.prev_hash == h) {
        static std::atomic<uint32_t> flicks{0};
        const uint32_t n = flicks.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= REXCVAR_GET(nx1_d3d9_dbg_dump_flicker_max)) {
          char fp[256];
          std::snprintf(fp, sizeof(fp), "texdump/flick%02u_%08X_%ux%u_f%u.bin", n, t.base_address,
                        t.width, height, t.format);
          if (FILE* f = std::fopen(fp, "wb")) {
            std::fwrite(src, 1, guest_bytes, f);
            std::fclose(f);
          }
          REXGPU_WARN("nx1_d3d9: FLICKER #{} {:08X} {}x{} fmt={} RETURNED to hash {:016X} after one "
                      "frame at {:016X} (frames {} -> {}) | src {} of {} pages empty. The bad state "
                      "was {:016X}; this is a surface that was momentarily wrong and fixed itself, "
                      "which no content classifier can identify",
                      n, t.base_address, t.width, height, t.format, h, prev.hash, prev.frame,
                      frame_, partial_pages, src_pages_total, prev.hash);
        }
      }
      prev.prev_hash = prev.hash;
    }
    // Live convergence detector: how many DISTINCT addresses have produced this exact content?
    if (const uint32_t conv_n = REXCVAR_GET(nx1_d3d9_dbg_convergence_n); conv_n > 1) {
      static std::mutex cm;
      static std::unordered_map<uint64_t, std::vector<uint32_t>> conv;
      static std::vector<uint64_t> conv_dumped;
      std::lock_guard<std::mutex> lk(cm);
      if (conv.size() < 4096) {
        auto& addrs = conv[h];
        if (std::find(addrs.begin(), addrs.end(), t.base_address) == addrs.end()) {
          addrs.push_back(t.base_address);
          if (addrs.size() >= conv_n && conv_dumped.size() < 4 &&
              std::find(conv_dumped.begin(), conv_dumped.end(), h) == conv_dumped.end()) {
            conv_dumped.push_back(h);
            hash_dump_match = true;
            REXGPU_WARN("nx1_d3d9: CONVERGENCE hash {:016X} reached by {} DISTINCT addresses "
                        "(this one {:08X} {}x{} fmt={}) -- one image written over many slots; "
                        "dumping it",
                        h, addrs.size(), t.base_address, t.width, height, t.format);
          }
        }
      }
    }
    // Identify the image that many textures converge on.
    if (const uint32_t want = REXCVAR_GET(nx1_d3d9_dbg_dump_hash_lo32);
        want && uint32_t(h & 0xFFFFFFFFu) == want) {
      static std::mutex hm;
      static std::vector<uint32_t> dumped;
      std::lock_guard<std::mutex> lk(hm);
      if (std::find(dumped.begin(), dumped.end(), t.base_address) == dumped.end() &&
          dumped.size() < 4) {
        dumped.push_back(t.base_address);
        hash_dump_match = true;  // the existing dump path below handles BC and A8R8G8B8 alike
        REXGPU_WARN("nx1_d3d9: HASHDUMP {:08X} {}x{} fmt={} hash {:016X} -- dumping this decode",
                    t.base_address, t.width, height, t.format, h);
      }
    }
    if (!prev.decodes) {
      prev.first_hash = h;
      prev.first_frame = frame_;
    }
    ++prev.decodes;
    prev.hash = h;
    prev.frame = frame_;
    prev.writes = wsum;
  }
  // Blanket partial-source check: same measurement the tracked-address path makes, but for
  // EVERY texture, so a transiently-corrupt one does not have to be named in advance. The
  // decoded hash is logged alongside so successive decodes of the same address can be compared:
  // a hash that changes while the page coverage is incomplete is a decode of a half-written
  // source, which is exactly what "garbage only while it speckles" looks like.
  // ALWAYS measured, not just when a diagnostic is on: the commit gate below depends on it, and
  // gating the measurement behind a debug cvar would leave the fix inert in a normal run. Each
  // page stops at its first non-zero byte, so a populated texture costs one byte per page.
  const uint32_t budget = REXCVAR_GET(nx1_d3d9_dbg_partial_src);
  if (src && dst) {
    // SCAN ONLY THE BYTES THIS TEXTURE ACTUALLY USES. guest_bytes is the TILED, PADDED extent,
    // which for small images is far larger than the level: a 128x16 DXT1 is 1024 bytes but its
    // guest extent is 8192, so the padding and packed mip tail -- legitimately zero -- were
    // counted as missing data. That false "partial" verdict inflated the PARTIALSRC baseline to
    // 17% and, once partial decodes started being replaced by a placeholder, turned much of the
    // world white. A texture is incomplete only if the region it reads from is.
    size_t scan_bytes = guest_bytes;
    if (const auto* fi_scan = rex::graphics::FormatInfo::Get(t.format)) {
      const uint32_t bw_s = (t.width + fi_scan->block_width - 1) / fi_scan->block_width;
      const uint32_t bh_s = (height + fi_scan->block_height - 1) / fi_scan->block_height;
      const size_t level0 = size_t(bw_s) * bh_s * fi_scan->bytes_per_block();
      if (level0 && level0 < scan_bytes) {
        scan_bytes = level0;
      }
    }
    uint32_t pages_total = 0, pages_empty = 0;
    for (size_t off = 0; off < scan_bytes; off += 4096) {
      const size_t end = (off + 4096 < scan_bytes) ? off + 4096 : scan_bytes;
      uint32_t nz = 0;
      for (size_t i = off; i < end && nz == 0; ++i) nz += src[i] != 0 ? 1 : 0;
      ++pages_total;
      // NEVER WRITTEN, not merely zero. An all-zero page is not evidence of missing data: a UI
      // logo on a transparent background is genuinely zero over most of its area, and treating
      // that as absent flagged it partial and replaced the NX1 title logo with a white box. What
      // distinguishes absent from blank is whether anything ever wrote the page -- guest writes
      // via the write-watch, ours via InvalidateGuestRange above.
      //
      // Same class of error as the entropy classifiers this project has now been burned by three
      // times: judging texture CONTENT to infer residency. Judge the write history instead.
      if (nz == 0) {
        const uint32_t page = uint32_t((t.base_address + off) >> 12);
        const bool ever_written = page < page_writes_.size() && page_writes_[page] != 0;
        pages_empty += ever_written ? 0 : 1;
      }
    }
    partial_pages = pages_empty;
    src_pages_total = pages_total;
    if (pages_empty && budget) {
      uint64_t h = 1469598103934665603ull;
      const size_t hashed = size_t(dst_row_bytes) * extent.block_height;
      for (size_t i = 0; i < hashed; i += 64) {
        h = (h ^ dst[i]) * 1099511628211ull;
      }
      REXCVAR_SET(nx1_d3d9_dbg_partial_src, budget - 1);
      NX1_LOGW_STATS("nx1_d3d9: PARTIALSRC {:08X} fmt={} {}x{} EMPTY {}/{} pages -- decoded hash "
                  "{:016X} (frame {})",
                  t.base_address, t.format, t.width, height, pages_empty, pages_total, h, frame_);
    }
  }

  if (const uint32_t track = TrackedMatch(t.base_address); track) {
    // Whole-texture coverage, per 4 KB page. A texture can span 32 pages while the write
    // notification covers one, so "the first page has data" says nothing about the rest --
    // and a half-populated source is exactly what decodes to structured garbage.
    uint64_t nonzero = 0;
    uint32_t pages_total = 0, pages_empty = 0;
    if (src) {
      for (size_t off = 0; off < guest_bytes; off += 4096) {
        const size_t end = (off + 4096 < guest_bytes) ? off + 4096 : guest_bytes;
        uint32_t page_nz = 0;
        for (size_t i = off; i < end; ++i) {
          page_nz += src[i] != 0 ? 1 : 0;
        }
        nonzero += page_nz;
        ++pages_total;
        pages_empty += page_nz == 0 ? 1 : 0;
      }
    }
    NX1_LOGI_TEX("nx1_d3d9: TRACK {:08X} DECODE frame={} fmt={} {}x{} bytes={} nonzero={}/{} "
                "emptypages={}/{} dirty={} committed={}",
                t.base_address, frame_, t.format, t.width, height, guest_bytes, nonzero,
                guest_bytes, pages_empty, pages_total, entry.dirty ? 1 : 0,
                entry.committed ? 1 : 0);

    // If the declared range is empty, the texels are not missing -- the reference backend
    // renders this texture, so they exist. Sweep outwards to find where the data actually
    // lives: a hit at a fixed offset means our base address is wrong, and nothing within a
    // megabyte means we are looking in the wrong region entirely.
    if (nonzero == 0) {
      const uint8_t* window = TranslatePhysical(t.base_address);
      if (window) {
        constexpr int32_t kSweep = 1 << 20;  // +/- 1 MB
        int32_t first_hit = INT32_MIN;
        for (int32_t off = -kSweep; off < kSweep; off += 4096) {
          const int64_t abs_addr = int64_t(t.base_address) + off;
          if (abs_addr < 0 || uint64_t(abs_addr) + 4096 > (uint64_t(kMirrorPages) << 12)) {
            continue;
          }
          const uint8_t* page = window + off;
          uint32_t nz = 0;
          for (uint32_t i = 0; i < 4096 && nz < 64; ++i) {
            nz += page[i] != 0 ? 1 : 0;
          }
          if (nz >= 64) {
            first_hit = off;
            break;
          }
        }
        if (first_hit == INT32_MIN) {
          NX1_LOGI_TEX("nx1_d3d9: TRACK {:08X} SWEEP no populated page within +/-1 MB",
                      t.base_address);
        } else {
          NX1_LOGI_TEX("nx1_d3d9: TRACK {:08X} SWEEP nearest populated page at offset {} "
                      "({:08X})",
                      t.base_address, first_hit, uint32_t(int64_t(t.base_address) + first_hit));
        }
      }
    }
  }

  // Size gate. Dumping the "next N decodes" catches whichever textures happen to be rebuilt, and
  // at ~1500 bindings a frame that is never the one under investigation. The surfaces in question
  // (opaque glass, receding-LOD confetti) are the engine's SMALL swapped-in LODs -- the isolated
  // glass material samples 16x16 and 8x8 -- so bounding the dump by dimension aims it at that
  // population without needing an address that moves every launch.
  const uint32_t dump_maxdim = REXCVAR_GET(nx1_d3d9_dbg_texdump_maxdim);
  const uint32_t dump_fmt1 = REXCVAR_GET(nx1_d3d9_dbg_texdump_fmt1);
  const bool dump_size_ok =
      dump_filter_active_
          ? dump_draw_
          : ((!dump_maxdim || (t.width <= dump_maxdim && height <= dump_maxdim)) &&
             (!dump_fmt1 || t.format == dump_fmt1 - 1));
  // DUMP THE TEXTURES THE PROVENANCE DETECTOR ACTUALLY FLAGGED.
  //
  // Every instrument in this investigation measures memory PROVENANCE -- an indirect proxy for
  // "is this the right image". Six revisions of that detector later, MIXED still sits at 7-10% in
  // every run under every intervention, while the visible speckle varies run to run. A metric that
  // never moves while the artifact does is behaving like a background constant, and the benign
  // mechanism that would produce exactly that is two textures legitimately SHARING identical pages.
  //
  // The only thing that separates those is LOOKING at the decoded image:
  //   a coherent picture  -> the flag is benign page sharing, MIXED is not the speckle, and the
  //                          paint test's causality was correlation with a third variable
  //   a visible patchwork -> the slot really does hold parts of several images
  // Paired with the provenance mask (dbg_pageorigin_dump), which marks WHICH blocks were flagged,
  // so the two can be compared block for block instead of impressionistically.
  const bool dump_flagged = pageorigin_dump_this_ && REXCVAR_GET(nx1_d3d9_dbg_dump_mixed) > 0;
  if (dump_flagged) {
    REXCVAR_SET(nx1_d3d9_dbg_dump_mixed, REXCVAR_GET(nx1_d3d9_dbg_dump_mixed) - 1);
  }
  pageorigin_dump_this_ = false;

  // OPERATOR-AIMED FRAME CAPTURE -- deliberately classifies NOTHING.
  //
  // The previous version of this scored the decoded image and dumped what "looked corrupt". It
  // captured eight textures at 22-99% and every one was perfect: an Xbox controller on
  // transparency, litter and rubble decal atlases, dark grass. Alpha-masked decals are mostly
  // black with sparse high-contrast content, so they trip both a blackness test and a variance
  // test. That is the FOURTH time this project has judged texture CONTENT to infer badness and
  // been wrong, and content simply cannot separate corruption from legitimately dark or detailed
  // art -- no threshold fixes that, so no threshold is used here.
  //
  // Instead: when armed, dump every distinct texture bound over the next N frames. The operator
  // can SEE the speckle; point at it, arm this, and the corrupt texture is necessarily in the
  // capture. Picking it out afterwards is a human looking at pictures, which is the one step in
  // this investigation that has never produced a false result.
  // NO GATE BEYOND THE OPERATOR'S FINGER. A scene-resolve gate was tried and does not work: the
  // in-game menus (class select) draw over a live scene, so they pass it, and those are exactly
  // the textures that keep getting captured. F6 while looking at the speckle is the whole filter.
  if (const uint32_t frames_left = REXCVAR_GET(nx1_d3d9_dbg_dump_frame);
      frames_left && dst && src && guest_bytes && IsBlockCompressed(host.d3d) &&
      framecap_count_ < REXCVAR_GET(nx1_d3d9_dbg_dump_frame_max) && t.width >= 64 && height >= 64) {
    const uint64_t id = MixKey(t.base_address, (uint64_t(t.format) << 32) | (t.width << 16) | height);
    if (framecap_seen_.insert(id).second) {
      ++framecap_count_;
      bool watched = false;
      for (const auto& e : addr_watch_) watched |= e.addr == t.base_address;
      if (!watched && addr_watch_.size() < 16) {
        AddrWatch w;
        w.addr = t.base_address; w.bytes = uint32_t(guest_bytes);
        w.w = t.width; w.h = height; w.fmt = t.format; w.pitch = t.pitch_pixels;
        addr_watch_.push_back(w);
      }
      std::vector<Rgba8> fimg;
      DecodeBcImage(host.d3d, mip_source, dst_row_bytes, t.width, height, fimg);
      char fp[256];
      if (!fimg.empty()) {
        std::snprintf(fp, sizeof(fp), "texdump/c%02u_%03u_%08X_%ux%u_f%u.bmp", framecap_batch_, framecap_count_,
                      t.base_address, t.width, height, t.format);
        DumpRgbaBmp(fp, fimg, t.width, height);
      }
      std::snprintf(fp, sizeof(fp), "texdump/c%02u_%03u_%08X_%ux%u_f%u.bin", framecap_batch_, framecap_count_,
                    t.base_address, t.width, height, t.format);
      if (FILE* f = std::fopen(fp, "wb")) {
        std::fwrite(src, 1, guest_bytes, f);
        std::fclose(f);
      }
      std::snprintf(fp, sizeof(fp), "texdump/c%02u_%03u_%08X_%ux%u_f%u.txt", framecap_batch_, framecap_count_,
                    t.base_address, t.width, height, t.format);
      if (FILE* f = std::fopen(fp, "wb")) {
        std::fprintf(f,
                     "base=%08X width=%u height=%u format=%u bytes=%zu\n"
                     "pitch_pixels=%u tiled=%u endian=%u swizzle=0x%03X\n"
                     "block_width=%u block_height=%u block_pitch_h=%u block_pitch_v=%u bpb=%u\n"
                     "mip_max_level=%u packed_mips=%u mip_address=%08X dimension=%u depth=%u\n"
                     "host_d3dfmt=0x%08X frame=%llu\n"
                     // The SOURCE dwords, so the geometry above can be re-derived by hand against
                     // GPUTEXTURE_FETCH_CONSTANT instead of taken on trust. Size is in raw[2]:
                     // width_minus_1 = bits 0..12, height_minus_1 = bits 13..25. This is what
                     // separates "the guest asked for 256x256" from "we misread the size field",
                     // which is the open question on the proven 11401000 over-read.
                     "raw=%08X:%08X:%08X:%08X:%08X:%08X\n",
                     t.base_address, t.width, height, t.format, guest_bytes, t.pitch_pixels,
                     t.tiled ? 1u : 0u, uint32_t(t.endian), t.swizzle, extent.block_width,
                     extent.block_height, extent.block_pitch_h, extent.block_pitch_v, bpb,
                     t.mip_max_level, t.packed_mips ? 1u : 0u, t.mip_address, t.dimension, t.depth,
                     uint32_t(host.d3d), static_cast<unsigned long long>(frame_),
                     t.raw[0], t.raw[1], t.raw[2], t.raw[3], t.raw[4], t.raw[5]);
        std::fclose(f);
      }
    }
  }

  if (const uint32_t tex_dump_left = REXCVAR_GET(nx1_d3d9_dbg_texdump);
      dst && ((tex_dump_left && dump_size_ok) || hash_dump_match || dump_flagged)) {
    if (tex_dump_left && !hash_dump_match && !dump_flagged) {
      REXCVAR_SET(nx1_d3d9_dbg_texdump, tex_dump_left - 1);
    }
    // The raw guest bytes first: all-zero says the data never arrived, anything else says it
    // is present and we are decoding it wrong. That single distinction is what separates a
    // content gap from a renderer bug, and it cannot be read off the screen.
    char hex[3 * 32 + 1] = {};
    if (src) {
      for (uint32_t i = 0; i < 32; ++i) {
        std::snprintf(hex + i * 3, 4, "%02X ", src[i]);
      }
    }
    // The SAMPLER is the whole point when a shader names its inputs by slot (tf0, tf6...): a
    // dump that does not say which slot it filled cannot be matched to the shader that reads it.
    REXGPU_INFO("nx1_d3d9: TEXDUMP sampler={} addr={:08X} fmt={} {}x{} dim={} depth={} pitch={} tiled={} "
                "endian={} swizzle=0x{:03X} sign={}{}{}{} mips={} packed={} host=0x{:08X} "
                "attribution={} src[0..31]= {}",
                sampler, t.base_address, t.format, t.width, height, t.dimension, t.depth,
                t.pitch_pixels,
                t.tiled ? 1 : 0, t.endian, t.swizzle,
                t.sign[0], t.sign[1], t.sign[2], t.sign[3],
                t.mip_max_level, t.packed_mips ? 1 : 0,
                uint32_t(host.d3d),
                dump_draw_ ? "MATERIAL-FILTERED" : "UNFILTERED-next-rebuild",
                src ? hex : "<null>");

    std::vector<Rgba8> img;
    if (IsBlockCompressed(host.d3d)) {
      DecodeBcImage(host.d3d, mip_source, dst_row_bytes, t.width, height, img);
    } else if (host.d3d == D3DFMT_A8R8G8B8) {
      img.resize(size_t(t.width) * height);
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* r = dst + size_t(y) * dst_row_bytes;
        for (uint32_t x = 0; x < t.width; ++x) {
          img[size_t(y) * t.width + x] = {r[x * 4 + 2], r[x * 4 + 1], r[x * 4 + 0], r[x * 4 + 3]};
        }
      }
    }
    if (!img.empty()) {
      char path[256];
      // Flagged dumps get their own prefix so the pairing is unambiguous on disk: every
      // MIXED_<addr> image has a prov_<addr> mask from the same decode.
      std::snprintf(path, sizeof(path), "texdump/%s_%08X_%ux%u_f%u.bmp",
                    dump_flagged ? "MIXED" : "tex", t.base_address, t.width, height, t.format);
      DumpRgbaBmp(path, img, t.width, height);

      // THE FORK THIS INVESTIGATION HAS NEVER RESOLVED: are the BYTES wrong, or is our DECODE
      // wrong? Every instrument so far has measured provenance -- where bytes came from -- which
      // answers neither, and eleven of them have misled. Dumping the COMPLETE guest source
      // alongside the image we produced from it makes the question answerable offline by decoding
      // the same bytes with an independent implementation:
      //   independent decode CLEAN  -> the bytes are fine and our untile/BC path is wrong
      //   independent decode BROKEN -> the bytes really are another asset's, and no amount of
      //                                decode work will help; it is delivery or identity
      // The existing raw_/rawok_ dumps only cover the first tile row and a control row, which
      // cannot answer this for a texture whose corruption is spread over the whole image.
      //
      // The sidecar is not optional: a raw dump without pitch/endian/swizzle/tiled cannot be
      // decoded by anything but this build, and a dump aimed at an address goes stale because the
      // pool reassigns addresses between launches.
      if (src && guest_bytes) {
        char rp[256];
        std::snprintf(rp, sizeof(rp), "texdump/full_%08X_%ux%u_f%u.bin", t.base_address, t.width,
                      height, t.format);
        if (FILE* f = std::fopen(rp, "wb")) {
          std::fwrite(src, 1, guest_bytes, f);
          std::fclose(f);
        }
        std::snprintf(rp, sizeof(rp), "texdump/full_%08X_%ux%u_f%u.txt", t.base_address, t.width,
                      height, t.format);
        if (FILE* f = std::fopen(rp, "wb")) {
          std::fprintf(f,
                       "base=%08X width=%u height=%u format=%u bytes=%zu\n"
                       "pitch_pixels=%u tiled=%u endian=%u swizzle=0x%03X\n"
                       "block_width=%u block_height=%u block_pitch_h=%u block_pitch_v=%u bpb=%u\n"
                       "mip_max_level=%u packed_mips=%u mip_address=%08X dimension=%u depth=%u\n"
                       "host_d3dfmt=0x%08X frame=%llu\n",
                       t.base_address, t.width, height, t.format, guest_bytes, t.pitch_pixels,
                       t.tiled ? 1u : 0u, uint32_t(t.endian), t.swizzle, extent.block_width,
                       extent.block_height, extent.block_pitch_h, extent.block_pitch_v, bpb,
                       t.mip_max_level, t.packed_mips ? 1u : 0u, t.mip_address, t.dimension,
                       t.depth, uint32_t(host.d3d),
                       static_cast<unsigned long long>(frame_));
          std::fclose(f);
        }
      }
    }

    // ALSO dump mip level 1, decoded from mip_address. Xenos keeps level 0 at base_address and
    // levels 1..N at mip_address, and the streaming system fills them independently -- so a
    // texture whose mip chain arrived but whose base did not leaves base_address holding a stale
    // pool slot while the real image sits at mip_address. We build our mip chain from level 0,
    // so a garbage base then poisons every distance. Dumping both is the only way to see which
    // one actually holds the picture.
    if (t.mip_address && t.mip_address != t.base_address && t.width > 1 && height > 1) {
      const uint32_t mw = t.width >> 1, mh = height >> 1;
      const rex::graphics::TextureExtent mx = rex::graphics::TextureExtent::Calculate(
          fmt, mw, mh, /*depth=*/1, t.tiled, /*is_guest=*/true);
      const size_t mbytes = size_t(mx.block_pitch_h) * mx.block_pitch_v * bpb;
      if (const uint8_t* msrc = MirrorSnapshot(t.mip_address, uint32_t(mbytes))) {
        const uint32_t mrow = mx.block_width * bpb;
        uint8_t* mscratch = DetileScratch(size_t(mx.block_width) * mx.block_height * bpb);
        DetileMip2D(mscratch, mrow, msrc, mx, bpb, t.endian, t.tiled, 0, 0);
        std::vector<Rgba8> mimg;
        if (IsBlockCompressed(host.d3d)) {
          DecodeBcImage(host.d3d, mscratch, mrow, mw, mh, mimg);
        }
        if (!mimg.empty()) {
          char mpath[256];
          std::snprintf(mpath, sizeof(mpath), "texdump/mip1_%08X_%ux%u_f%u.bmp", t.mip_address, mw,
                        mh, t.format);
          DumpRgbaBmp(mpath, mimg, mw, mh);
          REXGPU_INFO("nx1_d3d9: MIPDUMP base={:08X} -> mip1 from {:08X} ({}x{})", t.base_address,
                      t.mip_address, mw, mh);
        }
      }
    }
  }

  // Filter the chain down from the level 0 we just wrote, while it is still mapped: decode it
  // once, then halve-and-re-encode into each level below. UpdateTexture carries the whole
  // staging chain across, so the levels have to be in place before the unlock.
  stage_add(prof_tex_.decode_ns, t_decode);
  const auto t_mip = stage_mark();
  if (build_mips && dst) {
    std::vector<Rgba8> cur, next;
    DecodeBcImage(host.d3d, mip_source, dst_row_bytes, t.width, height, cur);
    // CATCH THE ONE-FRAME FLICKER. The artifact is now a bad texture appearing for a single frame
    // at random, which cannot be captured by hand -- by the time the picker is aimed it is gone.
    //
    // WHY A CONTENT SCORE IS ACCEPTABLE HERE, given this file records being burned by content
    // classifiers three times: those classifiers DECIDED things (hide this texture, keep the older
    // decode, flag a page foreign) and a false positive silently corrupted the result. This one
    // only decides WHETHER TO WRITE A DUMP. A false positive costs a few files; it cannot change
    // what is rendered and cannot bias a conclusion, because the conclusion is drawn from the
    // dumped image afterwards. Trigger, not verdict.
    //
    // Metric matches tools/texture_analysis/indep.py's score(): fraction of 4x4 blocks that are
    // black (mean < 8) or noise (std > 40), so an auto-dump and an offline score are comparable.
    if (const uint32_t thr = REXCVAR_GET(nx1_d3d9_dbg_autodump_corrupt);
        thr && !cur.empty() && t.width >= 4 && height >= 4) {
      uint32_t bad = 0, total = 0;
      for (uint32_t by = 0; by + 4 <= height; by += 4) {
        for (uint32_t bx = 0; bx + 4 <= t.width; bx += 4) {
          float sum = 0.0f, sum2 = 0.0f;
          for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
              const Rgba8& p = cur[size_t(by + y) * t.width + (bx + x)];
              const float v = (float(p.r) + float(p.g) + float(p.b)) / 3.0f;
              sum += v;
              sum2 += v * v;
            }
          }
          const float mean = sum / 16.0f;
          const float var = std::max(0.0f, sum2 / 16.0f - mean * mean);
          if (mean < 8.0f || std::sqrt(var) > 40.0f) ++bad;
          ++total;
        }
      }
      const uint32_t pct = total ? uint32_t(100ull * bad / total) : 0;
      static std::atomic<uint32_t> auto_dumps{0};
      if (pct >= thr && auto_dumps.load(std::memory_order_relaxed) <
                            REXCVAR_GET(nx1_d3d9_dbg_autodump_max)) {
        const uint32_t n = auto_dumps.fetch_add(1, std::memory_order_relaxed) + 1;
        char p[256];
        std::snprintf(p, sizeof(p), "texdump/auto%02u_%08X_f%llu_L0.bmp", n, t.base_address,
                      static_cast<unsigned long long>(frame_));
        DumpRgbaBmp(p, cur, t.width, height);
        std::snprintf(p, sizeof(p), "texdump/auto%02u_%08X_%ux%u_f%u.bin", n, t.base_address,
                      t.width, height, t.format);
        if (FILE* f = std::fopen(p, "wb")) {
          std::fwrite(src, 1, guest_bytes, f);
          std::fclose(f);
        }
        // Aim the reference capture at it too, so the pair is available for this specimen.
        if (sampler == 0 && REXCVAR_GET(nx1_d3d9_dbg_dump_aims_refupload)) {
          REXCVAR_SET(nx1_refupload_addr, PhysicalAddress(t.base_address));
          REXCVAR_SET(nx1_refupload_bytes, guest_bytes);
        }
        REXGPU_WARN("nx1_d3d9: AUTODUMP #{} {:08X} s{} {}x{} fmt={} frame={} scored {}% corrupt "
                    "blocks (threshold {}) | src {} of {} pages empty -> texdump/auto{:02}_* . "
                    "This is a decode that reached the screen; the score is only the trigger, "
                    "judge it from the image",
                    n, t.base_address, sampler, t.width, height, t.format, frame_, pct, thr,
                    partial_pages, src_pages_total, n);
      }
    }
    // Spend the budget on the MATERIAL being investigated, not on whichever textures happen to
    // rebuild first -- four clean chains once "proved" the mip builder sound while the
    // speckling surfaces went unexamined. Use the picker's DUMP MIPS button, which filters to
    // the draw. An address filter was tried and is wrong: the pool reassigns addresses, so the
    // dump silently captured a different texture than the one picked.
    const uint32_t dump_left =
        (dump_filter_active_ && !dump_draw_) ? 0 : REXCVAR_GET(nx1_d3d9_dbg_mipdump);
    char dump_path[256];
    if (dump_left) {
      REXCVAR_SET(nx1_d3d9_dbg_mipdump, dump_left - 1);
      // FRAME-STAMPED. Repeated dumps of the same texture used to overwrite each other, which
      // made the one question everything now hinges on unanswerable: does a stale-prefix
      // texture HEAL? Stamping the frame lets the same surface be dumped twice, seconds apart,
      // and the two images compared. Heals => we are displaying a transient mid-refill state
      // and the fix is to not commit one. Never heals => the refill genuinely never completes.
      std::snprintf(dump_path, sizeof(dump_path), "texdump/mip_%08X_f%llu_L0.bmp", t.base_address,
                    static_cast<unsigned long long>(frame_));
      DumpRgbaBmp(dump_path, cur, t.width, height);
      // Source-state alongside the picture, because "level 0 is garbage" has two very different
      // causes and the image alone cannot separate them: empty pages mean the data has not
      // streamed in yet (residency), while a FULL source that decodes to noise means the slot
      // holds foreign bytes -- a recycled pool entry -- and no amount of waiting will fix it.
      // The SAMPLER SLOT matters as much as the picture: this dump captures every texture the
      // material binds, and a material can bind slots its shader never reads. A noise texture
      // on an unused high slot says nothing about what is on screen, while noise on slot 0 (the
      // colormap) is the artifact itself.
      // EVERY layout-bearing field, printed rather than assumed. The corruption is systematic
      // (a leading run of rows, stable, on data the guest demonstrably wrote), which is what a
      // misread layout field looks like -- and tiled/pitch/endian/dimension have never once
      // been logged for a corrupt texture. pitch especially: it is a 9-bit field scaled by 32,
      // so a texture whose pitch exceeds its width has rows we are reading at the wrong stride.
      // AIM THE REFERENCE-SIDE CAPTURE AT THIS SAME TEXTURE, so the two halves cannot drift apart.
      // Slot 0 only: the dump fires for every slot a material binds, and letting a high slot
      // re-aim it would move the watch off the colormap -- which is the surface that is actually
      // on screen -- between one dump and the next.
      if (sampler == 0 && REXCVAR_GET(nx1_d3d9_dbg_dump_aims_refupload)) {
        const uint32_t phys = PhysicalAddress(t.base_address);
        REXCVAR_SET(nx1_refupload_addr, phys);
        REXCVAR_SET(nx1_refupload_bytes, guest_bytes);
        REXGPU_WARN("nx1_d3d9: REFAIM reference upload capture -> {:08X}+{} bytes (the texture "
                    "this dump just wrote). Pair texdump/refup_* with mip_{:08X}_f{}_L0.bmp; a "
                    "capture with ZERO refup files for this range means the reference never "
                    "re-uploaded it, which is itself the retention answer",
                    phys, guest_bytes, t.base_address, frame_);
      }
      REXGPU_INFO("nx1_d3d9: mip dump s{} {}x{} fmt {} levels={} mip_filter={} aniso={} "
                  "src_empty_pages={}/{} mip_address={:08X} | tiled={} pitch={} dim={} "
                  "endian={} swizzle={:#05x} sign={},{},{},{} blocks={}x{} pitchblocks={}x{} "
                  "-> texdump/mip_{:08X}_f{}_L*.bmp",
                  sampler, t.width, height, t.format, levels, t.mip_filter, t.aniso_filter,
                  partial_pages, src_pages_total, t.mip_address, t.tiled ? 1 : 0, t.pitch_pixels,
                  t.dimension, t.endian, t.swizzle, t.sign[0], t.sign[1], t.sign[2], t.sign[3],
                  extent.block_width, extent.block_height, extent.block_pitch_h,
                  extent.block_pitch_v, t.base_address, frame_);

      // WAS IT EVER GOOD? If a corrupt texture shows decodes=1 (or changes=0), it has looked
      // exactly like this since the first time we ever read that memory -- so no caching,
      // eviction or invalidation policy could have produced it, and the whole "we re-read good
      // memory and lost" family of explanations is wrong. If instead first_hash differs from
      // the current one, it WAS good once and we replaced it, and the frames tell us when.
      if (auto it = decode_hashes_.find(DecodeViewKey(t)); it != decode_hashes_.end()) {
        const auto& st = it->second;
        NX1_LOGW_TEX("nx1_d3d9: DECODEHIST {:08X} decodes={} changes={} | first frame {} hash "
                    "{:016X} | now frame {} hash {:016X} | {}",
                    t.base_address, st.decodes, st.changes, st.first_frame, st.first_hash,
                    st.frame, st.hash,
                    st.changes ? "CHANGED since first decode" : "IDENTICAL since first decode");
      }

      // WAS THE BAD REGION EVER WRITTEN? The decisive question, and the one every previous
      // diagnostic talked around: they all established that we read this memory faithfully,
      // which is not in doubt. Print the per-page guest write count across the texture. If the
      // stale prefix reads 0 while the correct tail reads >0, the guest never wrote those bytes
      // while we were watching -- so it is not a race we lose, and the reference must be
      // showing a copy it took earlier. If the prefix reads >0, the writes DID arrive and we
      // failed to act on them, which is our bug and a very different fix.
      {
        std::string wb;
        const uint32_t np = uint32_t((guest_bytes + 4095) / 4096);
        uint32_t zero_pages = 0;
        for (uint32_t i = 0; i < np; ++i) {
          const uint32_t p = (t.base_address >> 12) + i;
          const uint32_t n = p < page_writes_.size() ? page_writes_[p] : 0;
          if (n == 0) ++zero_pages;
          if (i < 48) {
            if (i) wb += ',';
            wb += std::to_string(n);
          }
        }
        NX1_LOGW_TEX("nx1_d3d9: PAGEWRITES {:08X} {} pages, {} never written | per-page: {}",
                    t.base_address, np, zero_pages, wb);
      }

      // WHAT IS THE BAD REGION, ACTUALLY? The corruption is confined to the first macro-tile
      // row, is byte-identical across 26 seconds, and decodes to STRUCTURE (green/black
      // striping), not noise -- so those bytes are real data written by someone. Dump the raw
      // guest bytes of the first tile row so the pattern can be identified offline: a short
      // repeating period means a fill/clear, a 4 KB period means page-granular reuse, and
      // recognisable block structure means another texture's pixels.
      {
        const size_t tile_row_bytes =
            std::min<size_t>(guest_bytes, size_t(extent.block_pitch_h) * 32 * bpb);
        char raw_path[256];
        std::snprintf(raw_path, sizeof(raw_path), "texdump/raw_%08X_f%llu.bin", t.base_address,
                      static_cast<unsigned long long>(frame_));
        if (FILE* f = std::fopen(raw_path, "wb")) {
          std::fwrite(src, 1, tile_row_bytes, f);
          std::fclose(f);
        }
        // The same span from the KNOWN-GOOD second tile row, as a control: whatever analysis
        // says about the bad bytes has to say something different about these.
        //
        // `>=`, not `>`. A texture exactly two tile rows tall -- 256x256, the size of every
        // specimen this dump has been aimed at -- has guest_bytes == tile_row_bytes * 2, so the
        // strict test was false and the control was NEVER WRITTEN. Every capture taken with this
        // instrument shipped the bad row with nothing to compare it against, which is the whole
        // point of the pairing. Twelfth instrument to fail silently; the arming rule applies to
        // dumps too, not just counters.
        if (guest_bytes >= tile_row_bytes * 2) {
          std::snprintf(raw_path, sizeof(raw_path), "texdump/rawok_%08X_f%llu.bin",
                        t.base_address, static_cast<unsigned long long>(frame_));
          if (FILE* f = std::fopen(raw_path, "wb")) {
            std::fwrite(src + tile_row_bytes, 1, tile_row_bytes, f);
            std::fclose(f);
          }
        }
      }

      // OUR LAYOUT vs THE REFERENCE'S, on the same texture. Both backends run over the same
      // recompiled game and the same memory -- the reference renders these correctly, so the
      // bytes are there and any disagreement about WHERE they are is ours. GetGuestTextureLayout
      // is the exact function its texture cache uses, so a mismatch in row pitch or extent is
      // the bug outright, and a match rules the whole addressing question out for good.
      {
        const auto ref = rex::graphics::texture_util::GetGuestTextureLayout(
            static_cast<rex::graphics::xenos::DataDimension>(t.dimension),
            t.pitch_pixels >> 5, t.width, height, /*depth_or_array_size=*/1, t.tiled,
            static_cast<rex::graphics::xenos::TextureFormat>(t.format), t.packed_mips,
            /*has_base=*/true, t.mip_max_level);
        const uint32_t our_row = extent.block_pitch_h * bpb;
        const uint32_t ref_row = ref.base.row_pitch_bytes;
        const bool agree = our_row == ref_row &&
                           uint32_t(guest_bytes) >= ref.base.level_data_extent_bytes;
        REXGPU_WARN("nx1_d3d9: LAYOUTCMP {:08X} {} -- ours row={} bytes={} blocks={}x{} (pitch "
                    "{}x{}) | ref row={} extent={} packed_level={}",
                    t.base_address, agree ? "AGREE" : "*** MISMATCH ***", our_row,
                    uint32_t(guest_bytes), extent.block_width, extent.block_height,
                    extent.block_pitch_h, extent.block_pitch_v, ref_row,
                    ref.base.level_data_extent_bytes, ref.packed_level);
      }

      // THE OTHER ALLOCATION. NX1 keeps a permanent low-res image and STREAMS the high-res
      // level 0 (see the HUD's "tex perm" vs "tex stm" counters), so when base_address decodes
      // to noise the real picture may be sitting in the mip allocation right now. Decode the
      // first guest mip level and dump it beside the base: if it is a clean half-size image
      // while the base is noise, the two allocations stream INDEPENDENTLY and picking the one
      // that actually holds data is the fix -- which is also why applying guest mips to every
      // texture failed, since for other textures it is the base that is good and the mips noise.
      if (t.mip_address && t.mip_address != t.base_address && t.width > 4 && height > 4) {
        const uint32_t mw = std::max(1u, rex::next_pow2(t.width) >> 1);
        const uint32_t mh = std::max(1u, rex::next_pow2(height) >> 1);
        rex::graphics::TextureExtent mx = rex::graphics::TextureExtent::Calculate(
            fmt, mw, mh, /*depth=*/1, t.tiled, /*is_guest=*/true);
        const size_t mbytes = size_t(mx.block_pitch_h) * mx.block_pitch_v * bpb;
        mx.block_width = (std::max(1u, t.width >> 1) + fmt->block_width - 1) / fmt->block_width;
        mx.block_height = (std::max(1u, height >> 1) + fmt->block_height - 1) / fmt->block_height;
        if (const uint8_t* ms = MirrorSnapshot(t.mip_address, uint32_t(mbytes))) {
          uint32_t mnz = 0;
          for (size_t i = 0; i < mbytes; i += 64) mnz += ms[i] != 0 ? 1 : 0;
          const uint32_t mrow = mx.block_width * bpb;
          uint8_t* msc = DetileScratch(size_t(mx.block_height) * mrow);
          DetileMip2D(msc, mrow, ms, mx, bpb, t.endian, t.tiled, 0, 0);
          std::vector<Rgba8> mimg;
          DecodeBcImage(host.d3d, msc, mrow, std::max(1u, t.width >> 1),
                        std::max(1u, height >> 1), mimg);
          std::snprintf(dump_path, sizeof(dump_path), "texdump/mip_%08X_GUEST1.bmp",
                        t.base_address);
          DumpRgbaBmp(dump_path, mimg, std::max(1u, t.width >> 1), std::max(1u, height >> 1));
          REXGPU_WARN("nx1_d3d9: ALTSRC {:08X} guest mip1 from {:08X} ({}x{}) nonzero={}/{} "
                      "-> texdump/mip_{:08X}_GUEST1.bmp",
                      t.base_address, t.mip_address, std::max(1u, t.width >> 1),
                      std::max(1u, height >> 1), mnz, mbytes / 64, t.base_address);
        }
      }

      // THE MIRROR TEST. Decode the same texture straight from live guest memory and dump it
      // beside the mirror's version, plus a per-page diff. The corruption we are chasing is
      // page-granular -- part of a texture correct, part foreign, split on 4 KB boundaries --
      // which is the signature of a per-page cache holding pages captured at the wrong moment.
      // If LIVE is clean where MIRROR is noise, the mirror is serving stale pages and that is
      // the whole bug. If both are identically wrong, the mirror is innocent and the guest
      // memory really does hold this, which sends us back upstream to streaming.
      if (REXCVAR_GET(nx1_d3d9_texture_mirror)) {
        if (const uint8_t* live = TranslatePhysical(t.base_address)) {
          uint32_t diff_pages = 0;
          for (uint32_t p = 0; p * 4096 < guest_bytes; ++p) {
            const size_t off = size_t(p) * 4096;
            const size_t len = std::min<size_t>(4096, guest_bytes - off);
            if (std::memcmp(src + off, live + off, len) != 0) ++diff_pages;
          }
          const size_t l0 = size_t(extent.block_height) * dst_row_bytes;
          uint8_t* lscratch = DetileScratch(l0);
          DetileMip2D(lscratch, dst_row_bytes, live, extent, bpb, t.endian, t.tiled, packed_ox,
                      packed_oy);
          std::vector<Rgba8> limg;
          DecodeBcImage(host.d3d, lscratch, dst_row_bytes, t.width, height, limg);
          std::snprintf(dump_path, sizeof(dump_path), "texdump/mip_%08X_LIVE.bmp",
                        t.base_address);
          DumpRgbaBmp(dump_path, limg, t.width, height);
          REXGPU_WARN("nx1_d3d9: MIRRORDIFF {:08X} {} of {} pages differ from live guest memory "
                      "-> texdump/mip_{:08X}_LIVE.bmp",
                      t.base_address, diff_pages, src_pages_total, t.base_address);
        }
      }
    }
    if (REXCVAR_GET(nx1_d3d9_dbg_mipfill) == 2) {
      // Overwrite the decoded source with a smooth gradient. BoxFilterHalf and EncodeBcImage
      // then do their real work on real varying data -- the case a flat fill cannot exercise,
      // because a flat block collapses the BC endpoints and makes every index equivalent.
      // Clean gradient bands at distance therefore acquit both, leaving DecodeBcImage.
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < t.width; ++x) {
          cur[size_t(y) * t.width + x] = Rgba8{uint8_t(x * 255 / (t.width ? t.width : 1)),
                                               uint8_t(y * 255 / (height ? height : 1)), 128, 255};
        }
      }
      // Counted on the same line as mode 1 so either mode's arming is visible in one place.
      ++mipfill_levels_;
    }
    for (uint32_t level = 1; level < levels; ++level) {
      const uint32_t lw = std::max(1u, t.width >> level);
      const uint32_t lh = std::max(1u, height >> level);
      BoxFilterHalf(cur, std::max(1u, t.width >> (level - 1)),
                    std::max(1u, height >> (level - 1)), next, lw, lh);
      if (REXCVAR_GET(nx1_d3d9_dbg_mipfill) == 1) {
        // Overwrite the filtered result with a flat colour for this level. Everything else --
        // level count, encode, lock pitch, UpdateTexture -- runs exactly as it normally does,
        // so what reaches the screen tests the plumbing alone.
        static constexpr Rgba8 kLevelColors[8] = {
            {255, 0, 0, 255},   {0, 255, 0, 255},   {0, 0, 255, 255},   {255, 255, 0, 255},
            {255, 0, 255, 255}, {0, 255, 255, 255}, {255, 128, 0, 255}, {255, 255, 255, 255},
        };
        const Rgba8 c = kLevelColors[(level - 1) & 7];
        for (auto& px : next) {
          px = c;
        }
        ++mipfill_levels_;
      }
      if (dump_left) {
        std::snprintf(dump_path, sizeof(dump_path), "texdump/mip_%08X_f%llu_L%u.bmp",
                      t.base_address, static_cast<unsigned long long>(frame_), level);
        DumpRgbaBmp(dump_path, next, lw, lh);
      }

      D3DLOCKED_RECT mip;
      const HRESULT mip_hr = staging->LockRect(level, &mip, nullptr, 0);
      if (FAILED(mip_hr) || !mip.pBits) {
        // Silently skipping left this level as whatever the allocation happened to contain --
        // uninitialised memory sampled as a mip, which is indistinguishable from the decode
        // bugs we have been chasing. Say so rather than shipping garbage quietly.
        REXGPU_ERROR("nx1_d3d9: mip level {} of {}x{} (fmt {}) failed to lock ({:#x}); that "
                     "level holds uninitialised data",
                     level, t.width, height, t.format, static_cast<uint32_t>(mip_hr));
        continue;
      }
      {
        const uint32_t mip_row_bytes = host.opaque_block
                                           ? ((lw + 3) / 4) * BcBlockBytes(host.d3d)
                                           : uint32_t(mip.Pitch);
        EncodeBcImage(host.d3d, next, lw, lh, static_cast<uint8_t*>(mip.pBits), mip_row_bytes);
        // ROUND TRIP. Decode the blocks we just wrote straight back out and dump THAT. The
        // ordinary mip dump above captures `next` -- the encoder's INPUT -- so it proves the box
        // filter correct and says nothing about what the GPU receives. Level 0 never re-encodes
        // (it is the guest's own blocks), which is why every dump so far has looked perfect
        // while the screen speckled. If ENC is garbage where the plain dump is clean, the fault
        // is EncodeBcImage or mip_row_bytes, and it is invisible to every other diagnostic.
        if (dump_left) {
          std::vector<Rgba8> back;
          DecodeBcImage(host.d3d, static_cast<const uint8_t*>(mip.pBits), mip_row_bytes, lw, lh,
                        back);
          char enc_path[256];
          std::snprintf(enc_path, sizeof(enc_path), "texdump/mip_%08X_ENC%u.bmp", t.base_address,
                        level);
          DumpRgbaBmp(enc_path, back, lw, lh);
          if (level == 1) {
            REXGPU_WARN("nx1_d3d9: ENCTRIP {:08X} L1 {}x{} row={} (lock pitch {}, opaque_block={})"
                        " -> texdump/mip_{:08X}_ENC*.bmp",
                        t.base_address, lw, lh, mip_row_bytes, int(mip.Pitch),
                        host.opaque_block ? 1 : 0, t.base_address);
          }
        }
        staging->UnlockRect(level);
      }
      cur.swap(next);
    }
  }
  // Decode the guest's own mip levels from mip_address. Same source handling as the base:
  // through the mirror when it is on (snapshot-and-hold, write-watch re-arms on capture), or a
  // live read with the watch armed explicitly. Either way a guest write to the mip range lands
  // in writes_pending_ and DrainMemoryWrites dirties this entry via mip_watch_addr/size.
  if (!guest_plan.empty() && dst) {
    const uint8_t* mip_base_src;
    if (REXCVAR_GET(nx1_d3d9_texture_mirror)) {
      mip_base_src = MirrorSnapshot(t.mip_address, guest_mip_bytes);
    } else {
      ArmWriteWatch(t.mip_address, guest_mip_bytes);
      mip_base_src = TranslatePhysical(t.mip_address);
    }
    const Swizzle32 mip_swz = MakeSwizzle32(t.swizzle);
    // Visual dump of the guest chain, BC formats only: decode L0 and every guest level to BMPs
    // (texdump/gmip_<addr>_L*.bmp) from fresh scratch decodes -- NOT from the mapped staging,
    // which is write-combined and unreliable to read back. L1 looking like a half-size L0 says
    // the chain is real and the layout is right; a coherent DIFFERENT image says the pool slot
    // belongs to another texture; scrambled noise says the layout/addressing is wrong.
    bool gdump = false;
    if (const uint32_t gd = REXCVAR_GET(nx1_d3d9_dbg_mipdump);
        gd && mip_base_src && host.decode == TexDecode::kNone && IsBlockCompressed(host.d3d)) {
      REXCVAR_SET(nx1_d3d9_dbg_mipdump, gd - 1);
      gdump = true;
      const size_t l0_bytes = size_t(extent.block_height) * extent.block_width * bpb;
      uint8_t* scratch = DetileScratch(l0_bytes);
      DetileMip2D(scratch, extent.block_width * bpb, src, extent, bpb, t.endian, t.tiled,
                  packed_ox, packed_oy);
      std::vector<Rgba8> img;
      DecodeBcImage(host.d3d, scratch, extent.block_width * bpb, t.width, height, img);
      char path[256];
      std::snprintf(path, sizeof(path), "texdump/gmip_%08X_L0.bmp", t.base_address);
      DumpRgbaBmp(path, img, t.width, height);
      REXGPU_INFO("nx1_d3d9: guest mip dump {:08X} {}x{} fmt={} mip_address={:08X} levels={} "
                  "mip_filter={} mip_min={} mip_max={} aniso={} lod_bias={} ({:.2f}) "
                  "grad_exp={}/{} -> texdump/gmip_{:08X}_L*.bmp",
                  t.base_address, t.width, height, t.format, t.mip_address, guest_plan.size(),
                  t.mip_filter, t.mip_min_level, t.mip_max_level, t.aniso_filter, t.lod_bias,
                  double(t.lod_bias) / 32.0, t.grad_exp_h, t.grad_exp_v, t.base_address);
    }
    for (size_t li = 0; mip_base_src && li < guest_plan.size(); ++li) {
      const GuestMip& gm = guest_plan[li];
      const uint32_t level = uint32_t(li + 1);
      D3DLOCKED_RECT mip;
      const HRESULT mip_hr = staging->LockRect(level, &mip, nullptr, 0);
      if (FAILED(mip_hr) || !mip.pBits) {
        REXGPU_ERROR("nx1_d3d9: guest mip level {} of {}x{} (fmt {}) failed to lock ({:#x}); "
                     "that level holds uninitialised data",
                     level, t.width, height, t.format, static_cast<uint32_t>(mip_hr));
        continue;
      }
      auto* mdst = static_cast<uint8_t*>(mip.pBits);
      // Same pitch rule as level 0: for a block format the runtime does not recognise, its
      // reported pitch counts texels, not the block row it actually has to hold.
      const uint32_t mrow = host.opaque_block ? gm.ext.block_width * bpb : uint32_t(mip.Pitch);
      const uint8_t* msrc = mip_base_src + gm.offset;
      if (host.decode == TexDecode::kColorSwizzle) {
        uint8_t* scratch = DetileScratch(size_t(gm.ext.block_width) * gm.ext.block_height * bpb);
        DetileMip2D(scratch, gm.ext.block_width * bpb, msrc, gm.ext, bpb, t.endian, t.tiled,
                    gm.ox, gm.oy);
        DecodeBcColorSwizzledToArgb(mdst, uint32_t(mip.Pitch), scratch, gm.ext.block_width,
                                    gm.ext.block_height, gm.lw, gm.lh, src_bc, bpb, t.swizzle);
      } else if (host.decode == TexDecode::kDXN) {
        uint8_t* scratch = DetileScratch(size_t(gm.ext.block_width) * gm.ext.block_height * bpb);
        DetileMip2D(scratch, gm.ext.block_width * bpb, msrc, gm.ext, bpb, t.endian, t.tiled,
                    gm.ox, gm.oy);
        DecodeDXNToArgb(mdst, uint32_t(mip.Pitch), scratch, gm.ext.block_width,
                        gm.ext.block_height, gm.lw, gm.lh);
      } else if (host.decode != TexDecode::kNone) {
        uint8_t* scratch = DetileScratch(size_t(gm.ext.block_width) * gm.ext.block_height * bpb);
        DetileMip2D(scratch, gm.ext.block_width * bpb, msrc, gm.ext, bpb, t.endian, t.tiled,
                    gm.ox, gm.oy);
        DecodeBCAlphaToArgb(mdst, uint32_t(mip.Pitch), scratch, gm.ext.block_width,
                            gm.ext.block_height, gm.lw, gm.lh,
                            host.decode == TexDecode::kDXT5A, t.swizzle);
      } else {
        DetileMip2D(mdst, mrow, msrc, gm.ext, bpb, t.endian, t.tiled, gm.ox, gm.oy);
        if (host.swizzle32 && !mip_swz.identity) {
          for (uint32_t by = 0; by < gm.ext.block_height; ++by) {
            SwizzleRow32(mdst + size_t(by) * mrow, gm.ext.block_width, mip_swz);
          }
        }
        if (gdump) {
          const uint32_t srow = gm.ext.block_width * bpb;
          uint8_t* scratch = DetileScratch(size_t(gm.ext.block_height) * srow);
          DetileMip2D(scratch, srow, msrc, gm.ext, bpb, t.endian, t.tiled, gm.ox, gm.oy);
          std::vector<Rgba8> img;
          DecodeBcImage(host.d3d, scratch, srow, gm.lw, gm.lh, img);
          char path[256];
          std::snprintf(path, sizeof(path), "texdump/gmip_%08X_L%u.bmp", t.base_address, level);
          DumpRgbaBmp(path, img, gm.lw, gm.lh);
        }
      }
      staging->UnlockRect(level);
    }
  }
  staging->UnlockRect(0);
  stage_add(prof_tex_.mipgen_ns, t_mip);
  const auto t_commit = stage_mark();

  // D3DUSAGE_AUTOGENMIPMAP reports a single level and keeps its sub-levels to itself: the
  // driver filters them down from the level 0 UpdateTexture writes. A chain we built ourselves
  // is the ordinary case instead -- real levels on both textures, copied across as they are.
  if (!entry.tex &&
      FAILED(device_->CreateTexture(t.width, height, driver_mips ? 0 : levels,
                                    driver_mips ? D3DUSAGE_AUTOGENMIPMAP : 0, host.d3d,
                                    D3DPOOL_DEFAULT, &entry.tex, nullptr))) {
    REXGPU_ERROR("nx1_d3d9: CreateTexture({}x{}, fmt {}) failed", t.width, height, t.format);
    staging->Release();
    entry.tex = nullptr;
    ++tex_failures_;
    return nullptr;
  }
  // Known pattern instead of the decode, when armed. Placed here so it overwrites whatever the
  // decode produced but still travels the ENTIRE remaining path -- UpdateTexture, the DEFAULT-pool
  // texture, the sampler and the shader -- which is exactly the segment under test.
  if (REXCVAR_GET(nx1_d3d9_dbg_synthetic_tex)) {
    FillSyntheticPattern(staging, t.width, height, host.d3d, t.base_address);
  }
  if (FAILED(device_->UpdateTexture(staging, entry.tex))) {
    REXGPU_ERROR("nx1_d3d9: UpdateTexture({}x{}, fmt {}) failed", t.width, height, t.format);
    staging->Release();
    ++tex_failures_;
    return nullptr;
  }
  staging->Release();
  ++tex_uploads_;
  if (driver_mips) {
    entry.tex->SetAutoGenFilterType(D3DTEXF_LINEAR);
    entry.tex->GenerateMipSubLevels();
  }
  stage_add(prof_tex_.commit_ns, t_commit);

  // Approximate host footprint: the level-0 surface plus a full chain is ~4/3 of level 0.
  entry.host_bytes =
      uint32_t(size_t(dst_row_bytes) * extent.block_height * (levels > 1 ? 4 : 3) / 3);
  // A layout change means a DIFFERENT texture in this slot, so the high-water mark from the
  // previous occupant must not gate it -- otherwise a small or sparse successor could never
  // satisfy a mark set by a large dense predecessor and would be frozen out permanently.
  if (entry.layout_key != layout_key) {
    entry.best_nonzero = 0;
    entry.best_sampled = 0;
    entry.keepbest_refusals = 0;
  }
  entry.layout_key = layout_key;
  entry.dirty = false;
  {
    // SEED AND CHECK MUST USE THE SAME FUNCTION OVER THE SAME BYTES.
    //
    // They did not, briefly, and the cost was immediate: the seed hashed a linear range while the
    // bind-time check hashed a different span, so every probe mismatched, every texture rebuilt
    // every frame, and CONTENTPROBE went 3,513 -> 1,211,952 (~224 rebuilds/frame) with the frame
    // rate collapsing. Any future change here has to move both sites together.
    //
    // `extent` is already clamped to the VISIBLE block grid above (a wider pitch stays in
    // block_pitch_h for addressing), which is exactly what ProbeTiledContent wants.
    const uint32_t probe_bytes = extent.block_pitch_h * extent.block_pitch_v * bpb;
    const bool probe_in_range =
        uint64_t(PhysicalAddress(t.base_address)) + probe_bytes <= (uint64_t(kMirrorPages) << 12);
  // The decode has now consumed whatever write was reported; require a NEW one before a
  // probe-detected change is adopted again (see nx1_d3d9_probe_needs_write).
  entry.write_seen_since_decode = false;
    entry.probe_hash = probe_in_range
                           ? ProbeTiledContent(TranslatePhysical(t.base_address), extent, bpb,
                                               t.tiled, REXCVAR_GET(nx1_d3d9_probe_samples))
                           : 0;
    entry.probe_frame = frame_;
  }
  // Arm the premature-decode ladder HERE, at the actual end of a decode. It was first placed at
  // the `committed = true` further up, which sits above the cache lookup and never runs on this
  // path -- the run reported "forced=0" with the cvar plainly set, i.e. the instrument measured
  // nothing and the zero meant nothing.
  //
  // `ladder_done` is what bounds this. Arming unconditionally here re-armed the ladder inside the
  // very decode it triggered, so rechecks_left reset every time and EVERY texture re-decoded on a
  // 30-frame cycle forever -- a permanent rebuild storm across the whole cache rather than the
  // four-shot ladder intended. Arm once per entry; a genuine guest write still re-decodes through
  // the write-watch, independently of this.
  if (const uint32_t redecode_delay = REXCVAR_GET(nx1_d3d9_redecode_delay);
      redecode_delay && !entry.ladder_done) {
    entry.recheck_frame = frame_ + redecode_delay;
    entry.rechecks_left = 3;
  }
  // A broadcast-swizzle sprite that came out fully transparent is not resident yet: keep it dirty
  // (re-decode next bind) and never let it commit, so it recovers the moment its pool streams in.
  // A decode made from a source with holes is provisional for the same reason a fully-empty one
  // is: the pages have not all arrived. Keeping it caches a permanently speckled texture.
  // Independent of the retry cvar: even without retrying, a partial decode must not be frozen.
  entry.decoded_from_partial = partial_pages != 0;
  // Record what this decode was made from, so a later rebuild can be judged against it. Sampled
  // the same way keep-best samples, or the two are not comparable.
  if (src && guest_bytes) {
    const uint32_t sample_cap = std::min<uint32_t>(uint32_t(guest_bytes), 1u << 20);
    uint32_t nz = 0, sampled = 0;
    for (uint32_t i = 0; i < sample_cap; i += 8) {
      nz += src[i] != 0 ? 1 : 0;
      ++sampled;
    }
    entry.decoded_nonzero = nz;
    entry.decoded_sampled = sampled;
    // Keyed by ADDRESS for the population gate -- see ResourceTracker::src_permille_.
    if (sampled) {
      src_permille_[t.base_address] = uint32_t(uint64_t(nz) * 1000u / sampled);
      // *** GUEST RESIDENCY CLASSIFIER ***
      //
      // Read straight off the fetch constant, using the guest's OWN address arithmetic -- not a
      // content heuristic. Load_Texture (recomp nx1_mp_recomp.36.cpp:10707) computes one boolean
      // from ImageCache_GetImageBasePixels and uses it to pick between two address fixups:
      //
      //   RESIDENT     XGOffsetBaseTextureAddress -> base = basePixels (image-cache memory),
      //                                              mip  = pixels (fastfile blob)
      //                                              set INDEPENDENTLY, so the delta is arbitrary
      //   NOT RESIDENT XGOffsetResourceAddress    -> base = pixels,
      //                                              mip  = pixels + align(baseSize, 4096)
      //
      // So `mip == base + align(base_size, 4096)` identifies the non-resident path exactly. There
      // the base level lives in the FASTFILE BLOB rather than the cache, which is fine once the
      // blob has been read from disk and empty before that -- which is what a 12-23% populated
      // source looks like.
      //
      // The guest never clamps mips for either case: SetSamplerState_MaxMipLevel has ZERO call
      // sites in the recomp, SetTextureHeader_D3D writes only mip_max_level, and Load_Texture
      // always passes the full levelCount. So residency changes ADDRESSES ONLY, and a host that
      // samples level 0 of a non-resident image reads whatever that blob region currently holds.
      const uint32_t aligned_base = (uint32_t(guest_bytes) + 4095u) & ~4095u;
      const bool nonresident =
          t.mip_address && aligned_base && t.mip_address == t.base_address + aligned_base;
      const uint32_t permille = uint32_t(uint64_t(nz) * 1000u / sampled);
      {
        static std::atomic<uint64_t> res_n{0}, res_pm{0}, non_n{0}, non_pm{0}, nomip_n{0};
        if (!t.mip_address) {
          nomip_n.fetch_add(1, std::memory_order_relaxed);
        } else if (nonresident) {
          non_n.fetch_add(1, std::memory_order_relaxed);
          non_pm.fetch_add(permille, std::memory_order_relaxed);
        } else {
          res_n.fetch_add(1, std::memory_order_relaxed);
          res_pm.fetch_add(permille, std::memory_order_relaxed);
        }
        const uint64_t total = res_n.load(std::memory_order_relaxed) +
                               non_n.load(std::memory_order_relaxed) +
                               nomip_n.load(std::memory_order_relaxed);
        // Every 2000, not 20000. At the wider cadence the FIRST report landed at decode #1 with
        // every counter still zero, and the second needed 20,001 decodes -- a run doing 13,758
        // produced exactly one useless line reading "resident=0 NOT-resident=0". A report whose
        // first sample is size one is worse than none: it looks like a measured zero.
        if ((total % 2000) == 1) {
          const uint64_t r = res_n.load(std::memory_order_relaxed);
          const uint64_t nr = non_n.load(std::memory_order_relaxed);
          // Unconditional, including zeros: if one class never appears the classifier is wrong,
          // and a "non-resident textures are the bad ones" reading would be unfalsifiable.
          NX1_LOGW_STATS("nx1_d3d9: RESIDENCY resident={} decodes (mean {} permille populated) | "
                      "NOT-resident={} decodes (mean {} permille) | no-mip-chain={}. "
                      "Non-resident means base_address points into the FASTFILE BLOB "
                      "(mip == base + align(baseSize,4096)); a low mean there is a blob that has "
                      "not been read from disk yet",
                      r, r ? res_pm.load(std::memory_order_relaxed) / r : 0, nr,
                      nr ? non_pm.load(std::memory_order_relaxed) / nr : 0,
                      nomip_n.load(std::memory_order_relaxed));
        }
      }
    }
    // RAISE ONLY. A forced accept (escape hatch) must not drag the mark down to the trough it just
    // let through -- that is the exact defect that made the first keep-best inert.
    if (uint64_t(nz) * std::max(entry.best_sampled, 1u) >
        uint64_t(entry.best_nonzero) * std::max(sampled, 1u)) {
      entry.best_nonzero = nz;
      entry.best_sampled = sampled;
    }
  }
  entry.keepbest_refusals = 0;
  // RELEASE the latch when the tracked texture decodes CLEANLY, so the hunt moves on. The first
  // catch healed: data arrived progressively (nonzero 0 -> 1359 -> 7239 -> 7380), the guest wrote
  // it, DIRTIED fired correctly. Partial-at-first-decode is NORMAL -- a texture caught mid-stream
  // is not the bug. What matters is the one that never fills, so hold the latch only while the
  // decode is still holey and re-arm on the next candidate otherwise.
  // NOTE: only ever REQUEST a change here. Decodes run on the async translation worker
  // (nx1_d3d9_async), and writing a cvar from that thread crashed the game -- a read fault at
  // 0x27, i.e. a near-null deref inside the cvar machinery. The main thread applies it in
  // AdvanceFrame.
  if (!partial_pages && REXCVAR_GET(nx1_d3d9_dbg_autotrack_partial) &&
      REXCVAR_GET(nx1_d3d9_dbg_track_addr) == t.base_address) {
    autotrack_request_.store(kAutotrackRelease, std::memory_order_relaxed);
  }
  // THE FILTER USED TO BE `t.format == 18 && t.width <= 128 && height <= 128`, which made this
  // structurally incapable of catching anything we were actually chasing: every specimen in the
  // 2026-07-21 investigation is 256x256 (0505C000, 11BF9000, 10FAD000, 16EE0000, 12D18000). It
  // never fired for them, which read as "no partial decodes worth tracking" and meant only "none
  // at or below 128x128". Bounded by a cvar now instead of a literal.
  // NOT GATED ON `partial_pages` -- that was the defect that made two runs find nothing.
  //
  // partial_pages counts a page only when it is ENTIRELY zero AND was never written (:5702). The
  // specimen does not look like that: 16EE0000's base pages were 76-89% zero, so nz != 0, so
  // partial_pages was 0 and the texture could never become a candidate. Run 064 confirmed it --
  // zero specimens, and the only candidate all run was one mip-less texture. A gate that cannot
  // select the thing being hunted reports absence indistinguishably from success.
  //
  // Candidacy is now a per-page zero FRACTION on both regions, which is what "starved" actually
  // looks like in these captures.
  if (REXCVAR_GET(nx1_d3d9_dbg_autotrack_partial) && !REXCVAR_GET(nx1_d3d9_dbg_track_addr) &&
      t.width <= REXCVAR_GET(nx1_d3d9_dbg_autotrack_max_dim) &&
      height <= REXCVAR_GET(nx1_d3d9_dbg_autotrack_max_dim)) {
    // IS THIS THE "EMPTY BASE, GOOD MIPS" SPECIMEN? That pairing is the sharpest open question
    // left: 16EE0000's base was 76-89% zero while its guest mip 1 decoded as a perfect, complete,
    // 100%-nonzero image. Same texture, same allocation, one region delivered and the other not.
    // Scanning the mip region here says so at the moment of decode, while the address is still
    // live -- which is the only time it is useful, because the pool reassigns addresses every
    // launch and a specimen identified from an offline dump can no longer be tracked.
    //
    // This is a RELATIVE test within one texture, not a content judgement. A legitimately dark or
    // sparse texture has an equally dark mip chain, because a mip is a downsample of the base and
    // downsampling preserves the mean. A base that is mostly zero while its OWN half-size copy is
    // mostly non-zero is an internal inconsistency, not art. That distinction is why this is not
    // the sixth repeat of "inferred badness from texture content" -- but it is close enough to it
    // that the log prints both figures, so the call can be checked rather than trusted.
    // A page counts as STARVED when it is mostly zeros -- not when it is entirely zero. The
    // threshold is a cvar because it is the one number here that is a judgement rather than a
    // measurement; 75% separates the observed specimens (76-89% zero) from delivered art
    // (0-3% zero) with a wide margin either side.
    const uint32_t zero_pct = std::min(100u, REXCVAR_GET(nx1_d3d9_dbg_autotrack_zero_pct));
    // UNDELIVERED MEMORY IS EXACTLY ZERO. SPARSE ART IS PARTIALLY ZERO.
    //
    // Counting a page as starved at >=75% zero conflates the two, and run 066 proved it: this
    // latched 07B7B000 claiming "16 of 16 base pages starved", while that texture's own BIND line
    // reported `0 of 64 pages EMPTY` for the base and `15 of 64` for the MIP -- the opposite
    // reading. Both were true. Mine counted mostly-zero pages, BIND counted entirely-zero ones,
    // and a mostly-zero page is what transparent art looks like.
    //
    // Two compounding errors, both mine:
    //   - the scan capped at 64 KB, the first QUARTER of a 512x512 base, and generalised from it.
    //     In tiled layout that quarter is a specific spatial region, so a texture with a
    //     transparent top -- a sign, a window, a decal -- trips it by construction.
    //   - >=75% zero was calibrated against 16EE0000's 76-89% figures without asking what ELSE
    //     produces them. That is the sixth time this investigation would have inferred badness
    //     from texture content.
    //
    // So count both, over the WHOLE extent, and make the latch depend on the exactly-zero count.
    const uint32_t kScanCap = 1u << 20;
    const auto starved_scan = [zero_pct, kScanCap](const uint8_t* p, uint32_t bytes,
                                                   uint32_t* empty, uint32_t* starved,
                                                   uint32_t* total) {
      *empty = *starved = *total = 0;
      if (!p || !bytes) {
        return;
      }
      const uint32_t scan_bytes = std::min<uint32_t>(bytes, kScanCap);
      for (uint32_t off = 0; off < scan_bytes; off += 4096) {
        const uint32_t end = std::min(off + 4096u, scan_bytes);
        uint32_t zeros = 0;
        for (uint32_t i = off; i < end; ++i) zeros += p[i] == 0 ? 1 : 0;
        ++*total;
        *empty += (zeros == end - off) ? 1 : 0;
        *starved += (zeros * 100 >= (end - off) * zero_pct) ? 1 : 0;
      }
    };
    // Base is scanned from `src` -- the bytes this decode actually used -- while the mip region is
    // read live. They can differ in principle; MIRRORSTALE measured that divergence at 0.29% of
    // pages, so it does not move this comparison.
    uint32_t base_empty = 0, base_starved = 0, base_total = 0;
    uint32_t mip_empty = 0, mip_starved = 0, mip_total_pages = 0;
    starved_scan(src, uint32_t(guest_bytes), &base_empty, &base_starved, &base_total);
    if (t.mip_address && guest_bytes) {
      starved_scan(TranslatePhysical(t.mip_address), uint32_t(guest_bytes), &mip_empty,
                   &mip_starved, &mip_total_pages);
    }
    const uint32_t mip_nonzero_pages = mip_total_pages - mip_empty;
    // THE CONTRAST, which is the whole test: this texture's base is mostly starved while its own
    // mip chain is mostly delivered. Both halves are required -- a starved base alone is ordinary
    // mid-stream partial delivery, and it is the DISAGREEMENT between a texture and its own
    // downsample that cannot be explained by dark art.
    // Latch on EXACTLY-ZERO pages, not mostly-zero ones: undelivered memory reads as exact zeros,
    // whereas transparent art is merely mostly zero. `base_starved` is still computed and logged
    // beside it, because the gap between the two numbers is what tells sparse art apart from a
    // delivery hole -- and a future reader must be able to see that rather than take it on trust.
    const bool base_starved_mostly = base_total && base_empty * 2 > base_total;
    const bool mips_look_delivered =
        mip_total_pages && mip_nonzero_pages * 2 > mip_total_pages && base_starved_mostly;

    // LATCH ONLY ON THE SPECIMEN. The first version latched on ANY partial decode, and the very
    // first run proved why that fails: it caught 06248000, a 256x128 fmt=10 texture with
    // mip_address=0 -- no mip chain at all, so it cannot be the empty-base/delivered-mips case --
    // and then held the latch for the ENTIRE run (2745 TRACK lines, 1359 binds, never released,
    // zero DMACOPY). One ineligible candidate blocks the hunt completely, because arming requires
    // track_addr == 0 and the release only fires when THAT texture decodes cleanly.
    //
    // So the latch condition is now the specimen definition itself. Everything else is reported
    // and passed over. Rejections are logged (bounded) rather than dropped silently: "autotrack
    // never fired" has to be distinguishable from "autotrack fired and rejected 400 candidates",
    // which is the ambiguity that made the old <=128x128 filter look like an absence of specimens.
    if (!mips_look_delivered) {
      static std::atomic<uint64_t> passed{0};
      const uint64_t n = passed.fetch_add(1, std::memory_order_relaxed) + 1;
      // Only report a pass-over that was ACTUALLY starved. Every decode reaches this branch now
      // that the partial_pages gate is gone, so logging them all would bury the signal under
      // every healthy texture in the scene.
      if (base_starved_mostly && (n <= 8 || (n % 200) == 0)) {
        NX1_LOGW_TEX("nx1_d3d9: AUTOTRACK passed over {:08X} {}x{} fmt={} ({} of {} base pages "
                    "EMPTY ({} also >={}% zero), mip_address={:08X} {} of {} pages delivered) "
                    "-- {}. {} starved candidates passed over so far; still hunting",
                    t.base_address, t.width, height, t.format, base_empty, base_total,
                    base_starved, zero_pct, t.mip_address, mip_nonzero_pages, mip_total_pages,
                    t.mip_address ? "its mip chain is as starved as its base, so this is ordinary "
                                    "mid-stream partial delivery"
                                  : "it has NO mip chain, so the base/mip contrast cannot apply",
                    n);
      }
    } else {
      autotrack_span_.store(uint32_t(guest_bytes), std::memory_order_relaxed);
      autotrack_request_.store(t.base_address, std::memory_order_relaxed);
      NX1_LOGW_TEX("nx1_d3d9: AUTOTRACK {:08X} {}x{} fmt={} -- *** STARVED BASE, DELIVERED MIPS: "
                  "{} of {} base pages ENTIRELY zero ({} also >={}% zero) while mip_address={:08X} "
                  "has {} of {} pages delivered. *** Tracking from here. Watch for a DMACOPY "
                  "landing on the BASE range (and what its source held), and for a WRITE arriving "
                  "AFTER this decode",
                  t.base_address, t.width, height, t.format, base_empty, base_total, base_starved,
                  zero_pct, t.mip_address, mip_nonzero_pages, mip_total_pages);
    }
  }
  // BASELINE METRIC for the speckle. partial_pages counts source pages that were entirely zero
  // at decode time -- texels the guest had not landed yet -- so a texture decoded with any of
  // them is a speckle candidate by construction, and this is objective where looking at the
  // screen is not. Tracked as a rate because absolute counts move with how much is streaming.
  //
  // Worth measuring precisely now: three separate paths were found copying UNWRITTEN memory over
  // good texels (the mirror from an empty source, memexport readback with the reference not
  // rasterising, and forced re-decode from recycled slots). Each looked like a fix for missing
  // data and was in fact destroying data that had arrived. This number says what is left.
  ++decodes_total_;
  decode_pages_sum_ += src_pages_total;
  if (partial_pages) {
    ++partial_decodes_;
    partial_pages_sum_ += partial_pages;
  }
  const bool partial = partial_pages != 0 && REXCVAR_GET(nx1_d3d9_retry_partial);
  if (swizzle_all_zero || partial) {
    entry.dirty = true;
    entry.dirty_source = DirtySource::kPartial;
    entry.good_frames = 0;
    entry.committed = false;
    // 2, 4, 8 ... 64 frames between attempts. Deliberately not reset by DrainMemoryWrites: the
    // streaming pool rewrites these pages with zeros every frame, so "the guest wrote it" is not
    // evidence that content arrived, and honouring it would defeat the backoff entirely.
    ++entry.zero_retries;
    entry.retry_frame = frame_ + (1ull << (entry.zero_retries < 6 ? entry.zero_retries : 6));
  } else {
    entry.zero_retries = 0;
    entry.retry_frame = 0;
  }
  // Record the guest byte range so DrainMemoryWrites can dirty this entry when the guest rewrites
  // it. The mirror itself armed the write-watch when it captured these pages in MirrorSnapshot.
  entry.watch_addr = t.base_address;
  entry.watch_size = uint32_t(guest_bytes);
  // The mip range participates in invalidation only when the levels actually came from it.
  entry.mip_watch_addr = guest_plan.empty() ? 0 : t.mip_address;
  entry.mip_watch_size = guest_plan.empty() ? 0 : guest_mip_bytes;
  entry.mip_addr_seen = t.mip_address;

  // Checkerboard hunt. The artifact is intermittent ACROSS LAUNCHES, which points at something
  // that varies with where the guest happened to allocate the texture -- a tiling/pitch
  // assumption that holds for some base addresses and not others. Guest allocations move between
  // runs, so the only way to see it is to log each large texture's full decode geometry and diff
  // a good run against a bad one. Everything that feeds the detile is here; the alignment columns
  // are the point, since Xenos tiling is defined in 32x32 blocks over a 4 KB-page layout.
  if (REXCVAR_GET(nx1_d3d9_dbg_decode_log) && t.width >= 256 && t.height >= 256) {
    static std::mutex dm;
    static std::vector<uint32_t> decoded_seen;
    std::lock_guard<std::mutex> dlk(dm);
    if (std::find(decoded_seen.begin(), decoded_seen.end(), t.base_address) == decoded_seen.end()) {
      decoded_seen.push_back(t.base_address);
      REXGPU_INFO("nx1_d3d9: DECODEGEO {:08X} align4k={} align32k={} {}x{} fmt={} tiled={} "
                  "pitchpx={} blockpitch={}x{} swizzle={:#05X} endian={} mip={}..{} packed={} "
                  "bytes={} mipaddr={:08X}",
                  t.base_address, t.base_address & 0xFFFu, t.base_address & 0x7FFFu, t.width,
                  height, t.format, t.tiled ? 1 : 0, t.pitch_pixels, extent.block_pitch_h,
                  extent.block_pitch_v, t.swizzle, t.endian, t.mip_min_level, t.mip_max_level,
                  t.packed_mips ? 1 : 0, guest_bytes, t.mip_address);
    }
  }
  if (const uint32_t track = TrackedMatch(t.base_address); track && white_) {
    return white_;
  }
  return entry.tex;
}

uint32_t ResourceTracker::SourcePermilleFor(uint32_t base_address) {
  const auto it = src_permille_.find(base_address);
  return it == src_permille_.end() ? 0u : it->second;
}

IDirect3DBaseTexture9* ResourceTracker::PreferLargestForSurface(uint64_t surface_key,
                                                                uint32_t sampler, uint32_t format,
                                                                IDirect3DBaseTexture9* tex,
                                                                uint32_t width, uint32_t height,
                                                                uint32_t base_address,
                                                                uint32_t src_permille) {
  if (prof_enabled_) ++prof_lod_.calls;
  // POPULATION RETENTION RUNS FIRST, ABOVE the prefer_largest early-out below.
  //
  // It was originally placed after that check and was therefore DEAD: nx1_d3d9_prefer_largest is
  // false (memory keeps it off, because it substitutes textures after the dump and makes dumps
  // lie), so the function returned before ever reaching the gate -- not even its arming counter
  // ran. The two features are independent; entangling them made a "no holds" reading meaningless.
  if (REXCVAR_GET(nx1_d3d9_prefer_populated) && surface_key && tex && best_textures_) {
    auto& bp = (*static_cast<BestTextureMap*>(best_textures_))[
        MixKey(MixKey(surface_key, sampler), format)];
    static std::atomic<uint64_t> evaluated{0}, had_both{0}, held{0};
    const uint64_t e = evaluated.fetch_add(1, std::memory_order_relaxed) + 1;
    if (src_permille && bp.src_permille) {
      had_both.fetch_add(1, std::memory_order_relaxed);
    }
    if ((e % 500000) == 1) {
      REXGPU_WARN("nx1_d3d9: POPHOLD evaluated {} times, {} with BOTH populations known, {} held. "
                  "If the second number is 0 the gate cannot fire and any 'no holds' reading "
                  "is void",
                  e, had_both.load(std::memory_order_relaxed),
                  held.load(std::memory_order_relaxed));
    }
    if (bp.tex && src_permille && bp.src_permille &&
        src_permille + REXCVAR_GET(nx1_d3d9_prefer_populated_drop) < bp.src_permille) {
      const uint64_t h = held.fetch_add(1, std::memory_order_relaxed) + 1;
      if (h <= 8 || (h % 5000) == 0) {
        REXGPU_WARN("nx1_d3d9: POPHOLD {} -- surface {:016X} s{} offered {:08X} at {} permille, "
                    "holding retained {:08X} at {} permille instead",
                    h, surface_key, sampler, base_address, src_permille, bp.base_address,
                    bp.src_permille);
      }
      return bp.tex;
    }
    // Ratchet upward and retain the better-populated texture, independently of the area rule.
    if (src_permille > bp.src_permille) {
      if (bp.tex != tex) {
        if (bp.tex) bp.tex->Release();
        tex->AddRef();
        bp.tex = tex;
      }
      bp.src_permille = src_permille;
      bp.base_address = base_address;
    }
    bp.last_frame = frame_;
  }
  // Master off switch. This function SUBSTITUTES a different texture than the one GetTexture
  // resolved, and it does so after the texture dump has already run -- so every dump taken to
  // investigate a surface shows the texture we looked up, not necessarily the one the GPU is
  // handed. Being able to take it out of the path is the only way to tell those apart.
  if (!REXCVAR_GET(nx1_d3d9_prefer_largest)) {
    return tex;
  }
  if (!surface_key || !tex || !best_textures_) {
    if (prof_enabled_ && !surface_key) ++prof_lod_.no_surface;
    return tex;
  }
  auto* map = static_cast<BestTextureMap*>(best_textures_);
  // Key on format as well as surface+sampler: the same geometry is drawn in several passes (shadow,
  // depth pre-pass, colour) that each bind a DIFFERENT texture to the same sampler, so conflating
  // them substituted, e.g., a normal map for the albedo. Separating by format keeps each pass's
  // texture in its own lineage.
  auto& b = (*map)[MixKey(MixKey(surface_key, sampler), format)];
  b.last_frame = frame_;
  // POPULATION RETENTION -- runs before the area logic, because a surface swapping to a barely
  // streamed allocation is a different failure from a receding LOD and the area rule cannot see it.
  //
  // Measured (run 023): every real surface->texture swap went from a 12-23% populated source to an
  // 83-99% one, i.e. the surface had been rendering an incomplete texture. Holding the better
  // populated one until the new allocation fills is the direct answer to that.
  if (REXCVAR_GET(nx1_d3d9_prefer_populated)) {
    // ARMING. "POPHOLD 0" was previously unreadable: the first version looked the population up in
    // a map keyed by MixKey(MixKey(base, sampler), layout_key) using base_address alone, so it
    // always missed and the gate could never fire. Report how often it was EVALUATED and how often
    // it had both figures, so a zero says which of those it is.
    static std::atomic<uint64_t> evaluated{0}, had_both{0};
    const uint64_t e = evaluated.fetch_add(1, std::memory_order_relaxed) + 1;
    if (src_permille && b.src_permille) {
      had_both.fetch_add(1, std::memory_order_relaxed);
    }
    if ((e % 500000) == 1) {
      REXGPU_WARN("nx1_d3d9: POPHOLD evaluated {} times, {} with BOTH populations known. If the "
                  "second number is 0 the gate cannot fire and any 'no holds' reading is void",
                  e, had_both.load(std::memory_order_relaxed));
    }
  }
  if (REXCVAR_GET(nx1_d3d9_prefer_populated) && b.tex && src_permille && b.src_permille) {
    const uint32_t drop = REXCVAR_GET(nx1_d3d9_prefer_populated_drop);
    if (src_permille + drop < b.src_permille) {
      static std::atomic<uint64_t> held{0};
      const uint64_t h = held.fetch_add(1, std::memory_order_relaxed) + 1;
      if (h <= 8 || (h % 5000) == 0) {
        REXGPU_WARN("nx1_d3d9: POPHOLD {} -- surface {:016X} s{} offered {:08X} at {} permille "
                    "populated, holding its retained {:08X} at {} permille instead",
                    h, surface_key, sampler, base_address, src_permille, b.base_address,
                    b.src_permille);
      }
      return b.tex;
    }
  }
  if (src_permille > b.src_permille) {
    b.src_permille = src_permille;  // ratchet upward only
  }
  const uint32_t area = width * height;
  const uint32_t dbg = REXCVAR_GET(nx1_d3d9_dbg_lod);
  if (prof_enabled_ && !b.tex) ++prof_lod_.fresh;
  if (area > b.area) {
    if (prof_enabled_) ++prof_lod_.adopt;
    // STRICTLY larger -> the new best. Adopt it, holding a reference so it survives the base-keyed
    // cache evicting the entry it came from. Strictly-larger (not >=) is deliberate: a same-size
    // re-read of a texture whose slot was recycled to garbage must NOT replace the clean retained one.
    if (b.tex) b.tex->Release();
    tex->AddRef();
    b.tex = tex;
    b.area = area;
    b.base_address = base_address;
    b.src_permille = src_permille ? src_permille : b.src_permille;
    return dbg == 3 && white_ ? white_ : tex;
  }
  // Same size: a fresh binding at the largest resolution (a render target, or updated content). Keep
  // the retained texture but show the current one -- nothing is frozen stale at full resolution.
  if (area == b.area) {
    if (prof_enabled_) {
      ++prof_lod_.equal;
      if (b.tex == tex) {
        ++prof_lod_.equal_same;
      } else {
        ++prof_lod_.equal_diff;
      }
    }
    // Mode 4 narrows mode 2 to only the suspicious case: same area but a different texture
    // object, i.e. the one binding where this branch can serve a fresh (possibly unstreamed)
    // allocation in place of the retained one. Mode 2 painted ~68% of the scene white, which
    // made the speckled surface impossible to pick out.
    if (dbg == 4 && b.tex && b.tex != tex && white_) return white_;
    // NOTE: serving the retained texture here was tried, keyed first on base_address and then
    // on the texture object, to stop this branch handing back an unstreamed allocation. Painting
    // the branch white (dbg_lod 4) DOES turn the speckled surfaces white, so the garbage is
    // served through here -- but substituting the retained texture did not reduce the speckle
    // at all, which means the retained texture carries the same bad texels. The fault is
    // upstream of this function; do not re-attempt the substitution without new evidence.
    b.base_address = base_address;
    return dbg == 2 && white_ ? white_ : tex;
  }
  if (prof_enabled_ && b.tex) ++prof_lod_.substitute;
  if (dbg == 1 && b.tex && white_) return white_;
  // Smaller than what this surface has shown before: a receding LOD. Serve the retained full-res,
  // whose host-built mip chain the driver minifies to clean pixels instead of the recycled garbage.
  return b.tex ? b.tex : tex;
}

void ResourceTracker::SetBackbuffer(IDirect3DSurface9* surface, uint32_t width, uint32_t height) {
  backbuffer_ = surface;
  backbuffer_width_ = width;
  backbuffer_height_ = height;
}

IDirect3DSurface9* ResourceTracker::GetRenderTargetSurface(const uint8_t* base,
                                                           uint32_t guest_surface) {
  if (!device_ || !render_targets_ || !guest_surface) {
    return nullptr;
  }
  SurfaceSize s{};
  if (!ReadSurfaceSize(base, guest_surface, &s.width, &s.height)) {
    return nullptr;
  }
  auto* map = static_cast<TargetMap*>(render_targets_);
  auto& t = (*map)[ReadSurfaceEdramKey(base, guest_surface)];
  if (t.surface && t.width == s.width && t.height == s.height && !t.is_depth) {
    return t.surface;
  }
  if (t.surface && !t.is_backbuffer) {
    t.surface->Release();
  }
  if (t.tex) {
    t.tex->Release();
  }
  t = HostTarget{};

  // The guest's final composite goes to a display-sized target; render it straight
  // into the backbuffer so Present shows it without an extra copy.
  if (backbuffer_ && s.width == backbuffer_width_ && s.height == backbuffer_height_) {
    t.surface = backbuffer_;
    t.width = s.width;
    t.height = s.height;
    t.is_backbuffer = true;
    return t.surface;
  }

  if (FAILED(device_->CreateTexture(s.width, s.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, &t.tex, nullptr)) ||
      !t.tex) {
    REXGPU_ERROR("nx1_d3d9: render target CreateTexture({}x{}) failed", s.width, s.height);
    t = HostTarget{};
    return nullptr;
  }
  if (FAILED(t.tex->GetSurfaceLevel(0, &t.surface))) {
    t.tex->Release();
    t = HostTarget{};
    return nullptr;
  }
  t.width = s.width;
  t.height = s.height;
  REXGPU_INFO("nx1_d3d9: created colour target {}x{} for guest surface 0x{:08X}", s.width, s.height,
              guest_surface);
  return t.surface;
}

IDirect3DSurface9* ResourceTracker::GetDepthSurface(const uint8_t* base, uint32_t guest_surface,
                                                    uint32_t min_width, uint32_t min_height) {
  if (!device_ || !render_targets_ || !guest_surface) {
    return nullptr;
  }
  SurfaceSize s{};
  if (!ReadSurfaceSize(base, guest_surface, &s.width, &s.height)) {
    return nullptr;
  }
  // D3D9 drops every draw whose depth-stencil is smaller than its render target, and the
  // shadow depth surfaces really are smaller than what they render: they decode to
  // 1024x1024 / 512x512 but the pass targets (and resolves) 1024x2048 / 512x2048, because
  // the guest stacks cascades into an atlas. So grow the depth to cover the bound colour
  // target -- but only in *height*. Growing the width too let a stale 1024- or 1920-wide
  // target from a previous pass drag the narrow shadow depths up with it, and every grow
  // reallocates, throwing away the contents mid-frame.
  if (min_width <= s.width) {
    s.height = std::max(s.height, min_height);
  }
  auto* map = static_cast<TargetMap*>(render_targets_);
  auto& t = (*map)[ReadSurfaceEdramKey(base, guest_surface)];
  // Reuse while it still covers what is being asked for; only ever grow.
  if (t.surface && t.is_depth && t.width >= s.width && t.height >= s.height) {
    return t.surface;
  }
  if (t.surface && t.is_depth) {
    s.width = std::max(s.width, t.width);
    s.height = std::max(s.height, t.height);
  }
  if (t.surface && !t.is_backbuffer) {
    t.surface->Release();
  }
  if (t.tex) {
    t.tex->Release();
  }
  t = HostTarget{};
  t.is_depth = true;

  // INTZ keeps the depth buffer sampleable, which is the whole point: the guest
  // resolves its shadow maps and scene depth and the lighting shaders read them back.
  // Fall back to a plain (unsampleable) depth-stencil if the driver lacks INTZ.
  if (intz_supported_ &&
      SUCCEEDED(device_->CreateTexture(s.width, s.height, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_INTZ,
                                       D3DPOOL_DEFAULT, &t.tex, nullptr)) &&
      t.tex && SUCCEEDED(t.tex->GetSurfaceLevel(0, &t.surface))) {
    t.width = s.width;
    t.height = s.height;
    REXGPU_INFO("nx1_d3d9: created INTZ depth target {}x{} for guest surface 0x{:08X}", s.width,
                s.height, guest_surface);
    return t.surface;
  }
  if (t.tex) {
    t.tex->Release();
    t.tex = nullptr;
  }
  if (FAILED(device_->CreateDepthStencilSurface(s.width, s.height, D3DFMT_D24S8,
                                                D3DMULTISAMPLE_NONE, 0, TRUE, &t.surface,
                                                nullptr))) {
    REXGPU_ERROR("nx1_d3d9: depth target {}x{} failed", s.width, s.height);
    t = HostTarget{};
    return nullptr;
  }
  t.width = s.width;
  t.height = s.height;
  return t.surface;
}

IDirect3DTexture9* ResourceTracker::GetDepthTexture(const uint8_t* base, uint32_t guest_surface) {
  if (!render_targets_ || !guest_surface) {
    return nullptr;
  }
  auto* map = static_cast<TargetMap*>(render_targets_);
  auto it = map->find(ReadSurfaceEdramKey(base, guest_surface));
  return (it != map->end() && it->second.is_depth) ? it->second.tex : nullptr;
}

IDirect3DBaseTexture9* ResourceTracker::GetVolumeTexture(const TextureFetchConstant& t,
                                                         uint32_t sampler) {
  const HostTextureFormat host = PickHostTextureFormat(t.format);
  if (!host.valid || host.decode != TexDecode::kNone) {
    return nullptr;  // no compressed/decoded volume formats in the corpus
  }
  const auto* fmt = rex::graphics::FormatInfo::Get(t.format);
  if (!fmt || !t.depth) {
    return nullptr;
  }
  const uint32_t bpb = fmt->bytes_per_block();
  const uint32_t pitch_pixels = t.pitch_pixels ? t.pitch_pixels : t.width;
  const rex::graphics::TextureExtent extent = rex::graphics::TextureExtent::Calculate(
      fmt, pitch_pixels, t.height, t.depth, t.tiled, /*is_guest=*/true);
  const uint32_t blocks_wide = (t.width + fmt->block_width - 1) / fmt->block_width;
  const uint32_t blocks_high = (t.height + fmt->block_height - 1) / fmt->block_height;

  // pitch/endian included for the same reason as the 2D path: both change the decode, and a key
  // that ignores them serves the previous binding's image.
  const uint64_t layout_key = MixKey(
      MixKey(MixKey(t.base_address, (uint64_t(t.format) << 32) | t.width),
             MixKey((uint64_t(t.height) << 16) | t.depth, (uint64_t(t.swizzle) << 1) | t.tiled)),
      (uint64_t(t.pitch_pixels) << 8) | t.endian);
  const uint64_t key = MixKey(t.base_address, sampler | 0x8000u);  // distinct from the 2D key
  auto* map = static_cast<TextureMap*>(textures_);
  auto& entry = (*map)[key];
  if (entry.vol && entry.last_frame == frame_ && entry.layout_key == layout_key) {
    return entry.vol;
  }
  entry.last_frame = frame_;

  const size_t guest_bytes =
      size_t(extent.block_pitch_h) * extent.block_pitch_v * std::max(1u, t.depth) * bpb;
  const uint8_t* src;
  if (REXCVAR_GET(nx1_d3d9_texture_mirror)) {
    src = MirrorSnapshot(t.base_address, uint32_t(guest_bytes));
  } else {
    // Live read, but still watch the pages: the watch used to be armed only as a side effect
    // of snapshotting, so reading live left every texture unwatched and a decode taken before
    // the guest filled the memory could never be redone.
    ArmWriteWatch(t.base_address, uint32_t(guest_bytes));
    src = TranslatePhysical(t.base_address);
  }
  const uint64_t content_hash = XXH3_64bits(src, guest_bytes);

  if (entry.vol && entry.layout_key == layout_key && entry.content_hash == content_hash) {
    return entry.vol;
  }
  if (entry.vol && entry.layout_key != layout_key) {
    entry.vol->Release();
    entry.vol = nullptr;
  }

  IDirect3DVolumeTexture9* staging = nullptr;
  if (FAILED(device_->CreateVolumeTexture(t.width, t.height, t.depth, 1, 0, host.d3d,
                                          D3DPOOL_SYSTEMMEM, &staging, nullptr))) {
    REXGPU_ERROR("nx1_d3d9: staging CreateVolumeTexture({}x{}x{}, fmt {}) failed", t.width,
                 t.height, t.depth, t.format);
    return nullptr;
  }
  D3DLOCKED_BOX box;
  if (FAILED(staging->LockBox(0, &box, nullptr, 0))) {
    staging->Release();
    return nullptr;
  }
  // block_pitch_v, not blocks_high: the slice stride is the tile-padded height. guest_bytes below
  // already uses the padded value, so passing the visible one here made the two disagree.
  DetileMip3D(static_cast<uint8_t*>(box.pBits), uint32_t(box.RowPitch), uint32_t(box.SlicePitch),
              src, blocks_wide, blocks_high, t.depth, extent.block_pitch_h, extent.block_pitch_v,
              bpb, t.endian, t.tiled);
  const Swizzle32 swz = MakeSwizzle32(t.swizzle);
  if (host.swizzle32 && !swz.identity) {
    for (uint32_t z = 0; z < t.depth; ++z) {
      auto* slice = static_cast<uint8_t*>(box.pBits) + size_t(z) * box.SlicePitch;
      for (uint32_t y = 0; y < blocks_high; ++y) {
        SwizzleRow32(slice + size_t(y) * box.RowPitch, blocks_wide, swz);
      }
    }
  }

  staging->UnlockBox(0);

  if (!entry.vol &&
      FAILED(device_->CreateVolumeTexture(t.width, t.height, t.depth, 1, 0, host.d3d,
                                          D3DPOOL_DEFAULT, &entry.vol, nullptr))) {
    REXGPU_ERROR("nx1_d3d9: CreateVolumeTexture({}x{}x{}, fmt {}) failed", t.width, t.height,
                 t.depth, t.format);
    staging->Release();
    entry.vol = nullptr;
    return nullptr;
  }
  const HRESULT hr = device_->UpdateTexture(staging, entry.vol);
  staging->Release();
  if (FAILED(hr)) {
    REXGPU_ERROR("nx1_d3d9: UpdateTexture(volume {}x{}x{}) failed", t.width, t.height, t.depth);
    return nullptr;
  }
  entry.layout_key = layout_key;
  entry.content_hash = content_hash;
  return entry.vol;
}

IDirect3DBaseTexture9* ResourceTracker::GetCubeTexture(const TextureFetchConstant& t,
                                                       uint32_t sampler) {
  const HostTextureFormat host = PickHostTextureFormat(t.format);
  // The CPU BC-alpha decode is a 2D-only path; NX1's cube maps are all DXT1/DXN, so it
  // never comes up. Skip rather than upload garbage.
  if (!host.valid || host.decode != TexDecode::kNone) {
    return nullptr;
  }
  const auto* fmt = rex::graphics::FormatInfo::Get(t.format);
  if (!fmt || !t.width || t.width != t.height) {
    return nullptr;
  }
  const uint32_t bpb = fmt->bytes_per_block();
  const uint32_t pitch_pixels = t.pitch_pixels ? t.pitch_pixels : t.width;
  rex::graphics::TextureExtent extent = rex::graphics::TextureExtent::Calculate(
      fmt, pitch_pixels, t.height, /*depth=*/1, t.tiled, /*is_guest=*/true);
  // As in the 2D path: copy only the visible blocks, so the detile fills exactly the
  // texture we create at t.width and never runs past its rows.
  extent.block_width = (t.width + fmt->block_width - 1) / fmt->block_width;

  // The six faces are consecutive array slices, and Xenos aligns every array slice to
  // 4 KB -- so the face stride is the padded face size, not the visible one.
  const uint32_t align =
      1u << rex::graphics::xenos::kTextureSubresourceAlignmentBytesLog2;
  const uint32_t face_bytes = extent.block_pitch_h * extent.block_pitch_v * bpb;
  const uint32_t face_stride = (face_bytes + align - 1) & ~(align - 1);

  // pitch/endian included for the same reason as the 2D path.
  const uint64_t layout_key = MixKey(
      MixKey(MixKey(t.base_address, (uint64_t(t.format) << 32) | t.width),
             MixKey((uint64_t(t.height) << 1) | (t.tiled ? 1 : 0), t.swizzle)),
      (uint64_t(t.pitch_pixels) << 8) | t.endian);
  const uint64_t key = MixKey(t.base_address, sampler | 0x4000u);  // distinct from 2D and 3D
  auto* map = static_cast<TextureMap*>(textures_);
  auto& entry = (*map)[key];
  if (entry.cube && entry.last_frame == frame_ && entry.layout_key == layout_key) {
    return entry.cube;
  }
  entry.last_frame = frame_;

  const uint8_t* src =
      REXCVAR_GET(nx1_d3d9_texture_mirror)
          ? MirrorSnapshot(t.base_address, uint32_t(size_t(face_stride) * 6))
          : TranslatePhysical(t.base_address);
  const uint64_t content_hash = XXH3_64bits(src, size_t(face_stride) * 6);
  if (entry.cube && entry.layout_key == layout_key && entry.content_hash == content_hash) {
    return entry.cube;
  }
  if (entry.cube && entry.layout_key != layout_key) {
    entry.cube->Release();
    entry.cube = nullptr;
  }

  // A cube map needs its chain as much as any 2D texture -- more, in fact. These are the
  // environment reflections: the vector into them swings across most of a face from one pixel
  // to the next on a surface seen at a distance, so an unfiltered cube map returns a different
  // corner of the sky per pixel. That is the coloured sparkle, and no 2D mip chain touches it.
  const bool auto_mips = SupportsAutoMips(device_, host.d3d);
  const bool build_mips = REXCVAR_GET(nx1_d3d9_mips) && !auto_mips && IsBlockCompressed(host.d3d);
  const bool want_mips = REXCVAR_GET(nx1_d3d9_mips) && (auto_mips || build_mips);
  const uint32_t levels = build_mips ? BcMipLevels(t.width, t.width) : 1;

  IDirect3DCubeTexture9* staging = nullptr;
  if (FAILED(device_->CreateCubeTexture(t.width, levels, 0, host.d3d, D3DPOOL_SYSTEMMEM, &staging,
                                        nullptr))) {
    REXGPU_ERROR("nx1_d3d9: staging CreateCubeTexture({}, fmt {}) failed", t.width, t.format);
    return nullptr;
  }
  // Xenos face order is D3D's: +X, -X, +Y, -Y, +Z, -Z. Each face is an ordinary tiled 2D
  // image, so the 2D detiler handles it -- only the base pointer moves.
  for (uint32_t face = 0; face < 6; ++face) {
    const D3DCUBEMAP_FACES d3d_face = D3DCUBEMAP_FACES(D3DCUBEMAP_FACE_POSITIVE_X + face);
    D3DLOCKED_RECT locked;
    if (FAILED(staging->LockRect(d3d_face, 0, &locked, nullptr, 0))) {
      staging->Release();
      return nullptr;
    }
    auto* dst = static_cast<uint8_t*>(locked.pBits);
    // As in the 2D path, an unrecognised block FourCC (DXN) reports a pitch of one byte per
    // texel rather than the block row it has to hold.
    const uint32_t dst_row_bytes =
        host.opaque_block ? extent.block_width * bpb : uint32_t(locked.Pitch);
    DetileMip2D(dst, dst_row_bytes, src + size_t(face) * face_stride, extent, bpb, t.endian,
                t.tiled);
    const Swizzle32 swz = MakeSwizzle32(t.swizzle);
    if (host.swizzle32 && !swz.identity) {
      for (uint32_t by = 0; by < extent.block_height; ++by) {
        SwizzleRow32(dst + size_t(by) * dst_row_bytes, extent.block_width, swz);
      }
    }

    if (build_mips) {
      std::vector<Rgba8> cur, next;
      DecodeBcImage(host.d3d, dst, dst_row_bytes, t.width, t.width, cur);
      for (uint32_t level = 1; level < levels; ++level) {
        const uint32_t ls = std::max(1u, t.width >> level);
        BoxFilterHalf(cur, std::max(1u, t.width >> (level - 1)),
                      std::max(1u, t.width >> (level - 1)), next, ls, ls);
        D3DLOCKED_RECT mip;
        if (SUCCEEDED(staging->LockRect(d3d_face, level, &mip, nullptr, 0)) && mip.pBits) {
          const uint32_t mip_row_bytes = host.opaque_block
                                             ? ((ls + 3) / 4) * BcBlockBytes(host.d3d)
                                             : uint32_t(mip.Pitch);
          EncodeBcImage(host.d3d, next, ls, ls, static_cast<uint8_t*>(mip.pBits), mip_row_bytes);
          staging->UnlockRect(d3d_face, level);
        }
        cur.swap(next);
      }
    }
    staging->UnlockRect(d3d_face, 0);
  }

  if (!entry.cube &&
      FAILED(device_->CreateCubeTexture(t.width, auto_mips && want_mips ? 0 : levels,
                                        auto_mips && want_mips ? D3DUSAGE_AUTOGENMIPMAP : 0,
                                        host.d3d, D3DPOOL_DEFAULT, &entry.cube, nullptr))) {
    REXGPU_ERROR("nx1_d3d9: CreateCubeTexture({}, fmt {}) failed", t.width, t.format);
    staging->Release();
    entry.cube = nullptr;
    return nullptr;
  }
  const HRESULT hr = device_->UpdateTexture(staging, entry.cube);
  staging->Release();
  if (FAILED(hr)) {
    REXGPU_ERROR("nx1_d3d9: UpdateTexture(cube {}, fmt {}) failed", t.width, t.format);
    return nullptr;
  }
  if (want_mips && auto_mips) {
    entry.cube->SetAutoGenFilterType(D3DTEXF_LINEAR);
    entry.cube->GenerateMipSubLevels();
  }
  entry.layout_key = layout_key;
  entry.content_hash = content_hash;
  return entry.cube;
}

IDirect3DTexture9* ResourceTracker::GetResolvedTexture(uint32_t address) {
  if (!resolves_ || !address) {
    return nullptr;
  }
  auto* map = static_cast<ResolveMap*>(resolves_);
  auto it = map->find(PhysicalAddress(address));
  return it != map->end() ? it->second.tex : nullptr;
}

bool ResourceTracker::EnsureDepthBlit() {
  if (depth_blit_ps_) {
    return true;
  }
  if (depth_blit_failed_ || !device_) {
    return false;
  }
  depth_blit_failed_ = true;  // cleared on success

  // Reads the INTZ depth surface and writes it as a plain float. ps_2_0, not 3_0: the
  // quad is drawn with pre-transformed (XYZRHW) vertices through the fixed-function
  // vertex pipeline, and D3D9 refuses to pair that with a 3_0 pixel shader.
  static const char kSource[] =
      "sampler2D depth : register(s0);\n"
      "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
      "  return tex2D(depth, uv).rrrr;\n"
      "}\n";

  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  const HRESULT hr = D3DCompile(kSource, sizeof(kSource) - 1, "depth_blit", nullptr, nullptr,
                                "main", "ps_2_0", 0, 0, &code, &errors);
  if (FAILED(hr) || !code) {
    REXGPU_ERROR("nx1_d3d9: depth-blit shader failed to compile: {}",
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
  } else if (SUCCEEDED(device_->CreatePixelShader(
                 static_cast<const DWORD*>(code->GetBufferPointer()), &depth_blit_ps_))) {
    depth_blit_failed_ = false;
  }
  if (code) {
    code->Release();
  }
  if (errors) {
    errors->Release();
  }
  return !depth_blit_failed_;
}

void ResourceTracker::ResolveDepth(uint32_t dest_address, uint32_t width, uint32_t height,
                                   IDirect3DTexture9* src_depth, const RECT& src_rect,
                                   const POINT& dest_point) {
  // Recorded ahead of the guard for the same reason as ResolveColor: a dropped destination is
  // still a destination the guest resolved into.
  NoteResolveDestSeen(dest_address, uint64_t(width) * height * 4);
  if (!device_ || !resolves_ || !dest_address || !width || !height || !src_depth) {
    return;
  }
  D3DSURFACE_DESC sd{};
  if (FAILED(src_depth->GetLevelDesc(0, &sd)) || !sd.Width || !sd.Height || !EnsureDepthBlit()) {
    return;
  }

  auto* map = static_cast<ResolveMap*>(resolves_);
  if (const uint32_t track = TrackedMatch(PhysicalAddress(dest_address)); track) {
    NX1_LOGI_TEX("nx1_d3d9: TRACK {:08X} RESOLVE dest={:08X} {}x{}", track, dest_address, width,
                height);
  }
  auto& entry = (*map)[PhysicalAddress(dest_address)];
  if (entry.tex && (!entry.owned || entry.width != width || entry.height != height)) {
    if (entry.owned) {
      entry.tex->Release();
    }
    entry.tex = nullptr;
  }
  const bool fresh = entry.tex == nullptr;
  if (fresh) {
    if (FAILED(device_->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                      D3DPOOL_DEFAULT, &entry.tex, nullptr))) {
      REXGPU_ERROR("nx1_d3d9: depth resolve target R32F {}x{} failed", width, height);
      entry.tex = nullptr;
      return;
    }
    entry.width = width;
    entry.height = height;
    entry.owned = true;
    REXGPU_INFO("nx1_d3d9: depth resolve atlas R32F {}x{} for 0x{:08X} (src surface {}x{})", width,
                height, PhysicalAddress(dest_address), sd.Width, sd.Height);
  }
  IDirect3DSurface9* dst = nullptr;
  if (FAILED(entry.tex->GetSurfaceLevel(0, &dst)) || !dst) {
    return;
  }

  RECT sr = src_rect;
  if (sr.right <= sr.left || sr.bottom <= sr.top) {  // empty => whole surface
    sr = RECT{0, 0, LONG(sd.Width), LONG(sd.Height)};
  }
  sr.right = std::min<LONG>(sr.right, LONG(sd.Width));
  sr.bottom = std::min<LONG>(sr.bottom, LONG(sd.Height));
  RECT dr{dest_point.x, dest_point.y, dest_point.x + (sr.right - sr.left),
          dest_point.y + (sr.bottom - sr.top)};
  dr.right = std::min<LONG>(dr.right, LONG(width));
  dr.bottom = std::min<LONG>(dr.bottom, LONG(height));
  if (dr.right <= dr.left || dr.bottom <= dr.top) {
    dst->Release();
    return;
  }

  // The render target and depth-stencil are the only device state the per-draw path does
  // not rewrite, so they are all that has to be put back.
  IDirect3DSurface9* old_rt = nullptr;
  IDirect3DSurface9* old_ds = nullptr;
  device_->GetRenderTarget(0, &old_rt);
  device_->GetDepthStencilSurface(&old_ds);

  device_->SetDepthStencilSurface(nullptr);  // before the target: depth must cover the RT
  device_->SetRenderTarget(0, dst);
  if (fresh) {
    // 0 is "far" under this engine's reverse-Z, i.e. nothing occluding. A slot the guest
    // has not resolved into yet then reads as unshadowed instead of fully shadowed.
    device_->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
  }
  const D3DVIEWPORT9 vp{0, 0, width, height, 0.0f, 1.0f};
  device_->SetViewport(&vp);

  device_->SetVertexShader(nullptr);
  device_->SetVertexDeclaration(nullptr);
  device_->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
  device_->SetPixelShader(depth_blit_ps_);
  device_->SetTexture(0, src_depth);
  device_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  device_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  device_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  device_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  device_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  device_->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
  // The blit must copy raw depth values: the guest RT bound before this resolve is
  // usually a gamma surface, which leaves sRGB *write* conversion enabled on the device.
  // Save it across the blit -- the per-draw path does not reissue it (it is set at
  // SetRenderTarget time), so leaking FALSE back out would undo the gamma encode of
  // every draw until the next target switch.
  DWORD old_srgb_write = FALSE;
  device_->GetRenderState(D3DRS_SRGBWRITEENABLE, &old_srgb_write);
  device_->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  device_->SetRenderState(D3DRS_ZENABLE, FALSE);
  device_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  device_->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  device_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  device_->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  device_->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);

  // Pre-transformed quad, half-pixel biased so texel centres land on pixel centres.
  struct BlitVertex {
    float x, y, z, rhw, u, v;
  };
  const float su = 1.0f / float(sd.Width);
  const float sv = 1.0f / float(sd.Height);
  const float u0 = float(sr.left) * su, u1 = float(sr.right) * su;
  const float v0 = float(sr.top) * sv, v1 = float(sr.bottom) * sv;
  const float x0 = float(dr.left) - 0.5f, x1 = float(dr.right) - 0.5f;
  const float y0 = float(dr.top) - 0.5f, y1 = float(dr.bottom) - 0.5f;
  const BlitVertex quad[4] = {{x0, y0, 0.0f, 1.0f, u0, v0},
                              {x1, y0, 0.0f, 1.0f, u1, v0},
                              {x0, y1, 0.0f, 1.0f, u0, v1},
                              {x1, y1, 0.0f, 1.0f, u1, v1}};
  device_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(BlitVertex));

  device_->SetTexture(0, nullptr);
  device_->SetPixelShader(nullptr);
  device_->SetRenderState(D3DRS_SRGBWRITEENABLE, old_srgb_write);
  device_->SetRenderTarget(0, old_rt);
  device_->SetDepthStencilSurface(old_ds);
  if (old_rt) {
    old_rt->Release();
  }
  if (old_ds) {
    old_ds->Release();
  }
  dst->Release();
  // The bind path scans a flat mirror of this map; keep it in step.
  RebuildResolveFlat();
}

/// Write the resolved pixels back into guest RAM, so the game's CPU-side reads see them.
///
/// This is the native half of the root-cause fix for the texture speckle: NX1 bakes textures by
/// rendering, resolving, then READING THE RESOLVE DESTINATION ON THE CPU (compressing imposters
/// to DXT, retiling, and so on). On hardware the resolve lands in unified memory; here our
/// renderer resolved into a host texture and guest RAM kept the previous occupant's bytes, so
/// every bake compressed garbage. The reference proved the mechanism: readback_resolve="full"
/// (plus real rasterization) made the scene nearly clean until its readback path hit memory
/// pressure. This writeback makes the D3D9 renderer self-sufficient instead -- no reference
/// raster, no readback cvars.
REXCVAR_DEFINE_BOOL(nx1_d3d9_resolve_invalidates, false, "GPU",
                    "Report the resolve writeback's host-side guest-memory write to the texture "
                    "cache. Correct, but measured WORSE on screen: it forces a re-decode that "
                    "reads resolve output through the sampling texture's format, not the "
                    "resolve's. Off until that mismatch is handled");

REXCVAR_DEFINE_BOOL(nx1_d3d9_resolve_writeback, true, "GPU",
                    "Write resolve results back into guest memory so CPU-side texture bakes "
                    "read real pixels. The fix for baked-texture speckle (imposters, decals)");
/// Per-destination budget, because writeback is a GPU sync (GetRenderTargetData). Bake
/// destinations are resolved once or twice and are exactly what the CPU reads; the scene and
/// shadow targets are resolved every frame and never CPU-read, so each destination gets a few
/// writebacks and then stops. Predicated tiling resolves a destination in BANDS, each its own
/// call -- the budget must comfortably exceed the band count so the final band still writes.
REXCVAR_DEFINE_UINT32(nx1_d3d9_writeback_max, 8, "GPU",
                      "Writebacks per resolve destination before it is assumed to be a "
                      "per-frame render loop target and skipped");
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_writeback_all, false, "GPU",
                    "Debug: ignore the per-destination budget and write back EVERY resolve. "
                    "Costs a GPU sync per resolve; correctness reference, not a mode to play");

/// Record what writeback decided for every page this resolve destination covers.
///
/// The DMA-origin census can see that a staging buffer is empty in CPU RAM but not WHY. If the
/// buffer is a resolve destination we declined -- an unsupported format, a non-2D dimension --
/// then the "GPU-filled origin" is not outside our observation at all: it is a writeback we
/// chose not to perform, and the fix is to perform it rather than to hunt a new mechanism.
void ResourceTracker::MarkResolveDest(const TextureFetchConstant& dest, WritebackVerdict verdict) {
  if (!dest.base_address) {
    return;
  }
  // ARE WE IN GAMEPLAY, OR IN A MENU? The scene renders to a ~1024x600 target, so a resolve of
  // that shape is the frame's scene resolve and nothing in the front end produces one. This is
  // what gates the frame capture: an earlier capture ran in the menus, which have rendered
  // correctly for months, and dumped 8 pristine front-end textures. Recorded as a FRAME NUMBER
  // rather than a sticky flag so it self-clears -- "the scene resolved within the last couple of
  // frames" means we are in gameplay right now, not that we passed through it once at startup.
  if (dest.height >= 500 && dest.height <= 720 && dest.width >= 800) {
    scene_resolve_frame_ = frame_;
  }
  // Conservative extent: the real tiled extent when the format is known, else a plain 32bpp
  // estimate. Over-marking a page or two only widens the census's reach; under-marking would
  // make a destination look like it was never resolved.
  uint64_t bytes = uint64_t(dest.width ? dest.width : 1) * (dest.height ? dest.height : 1) * 4;
  if (const auto* fmt = rex::graphics::FormatInfo::Get(dest.format)) {
    const uint32_t pitch_px = dest.pitch_pixels ? dest.pitch_pixels : dest.width;
    if (pitch_px && dest.height) {
      const rex::graphics::TextureExtent ex = rex::graphics::TextureExtent::Calculate(
          fmt, pitch_px, dest.height, /*depth=*/1, dest.tiled, /*is_guest=*/true);
      bytes = uint64_t(ex.block_pitch_h) * ex.block_pitch_v * fmt->bytes_per_block();
    }
  }
  bytes = std::min<uint64_t>(bytes, 64u << 20);
  const uint32_t first = PhysicalAddress(dest.base_address) >> 12;
  const uint32_t last = uint32_t((uint64_t(PhysicalAddress(dest.base_address)) + bytes - 1) >> 12);
  std::lock_guard<std::mutex> lk(writeback_marks_m_);
  if (writeback_marks_.size() > 400000) {
    return;
  }
  for (uint32_t p = first; p <= last; ++p) {
    // A page that was EVER written back is reported as written back: one declined resolve does
    // not make the data absent if another landed it.
    auto& m = writeback_marks_[p];
    if (m.verdict != WritebackVerdict::kWrote) {
      m.verdict = verdict;
      m.format = dest.format;
    }
  }
}

ResourceTracker::WritebackVerdict ResourceTracker::ResolveDestVerdict(uint32_t phys,
                                                                     uint32_t* out_format) const {
  std::lock_guard<std::mutex> lk(writeback_marks_m_);
  const auto it = writeback_marks_.find(phys >> 12);
  if (it == writeback_marks_.end()) {
    return WritebackVerdict::kNever;
  }
  if (out_format) {
    *out_format = it->second.format;
  }
  return it->second.verdict;
}

bool ResourceTracker::WantResolveWriteback(const TextureFetchConstant& dest) {
  if (!REXCVAR_GET(nx1_d3d9_resolve_writeback)) {
    return false;
  }
  if (!dest.base_address || !dest.width || !dest.height || dest.dimension != 1) {
    // Was a SILENT reject; a whole class of bake destinations could have been dropped here
    // without a trace, indistinguishable from "no such resolves happen".
    static std::mutex m;
    static std::vector<uint32_t> seen;
    std::lock_guard<std::mutex> lk(m);
    if (dest.base_address &&
        std::find(seen.begin(), seen.end(), dest.base_address) == seen.end() &&
        seen.size() < 16) {
      seen.push_back(dest.base_address);
      REXGPU_WARN("nx1_d3d9: resolve writeback skipping {:08X} -- {}x{} dim={} (not a 2D "
                  "image destination)",
                  dest.base_address, dest.width, dest.height, dest.dimension);
    }
    MarkResolveDest(dest, WritebackVerdict::kNotImage);
    return false;
  }
  // Depth formats are published as sampleable host depth textures instead (ResolveDepth), and
  // the CPU does not read shadow maps back. Colour: k_8_8_8_8 (6) covers the bakes; name any
  // other format once so support can be added deliberately rather than guessed at.
  if (dest.format != 6) {
    static std::mutex m;
    static std::vector<uint32_t> seen;
    std::lock_guard<std::mutex> lk(m);
    if (std::find(seen.begin(), seen.end(), dest.format) == seen.end() && seen.size() < 16) {
      seen.push_back(dest.format);
      REXGPU_WARN("nx1_d3d9: resolve writeback skipping unsupported dest format {} ({}x{} at "
                  "{:08X}) -- add if its content is CPU-read",
                  dest.format, dest.width, dest.height, dest.base_address);
    }
    MarkResolveDest(dest, WritebackVerdict::kBadFormat);
    return false;
  }
  uint32_t& count = writeback_counts_[PhysicalAddress(dest.base_address)];
  if (!REXCVAR_GET(nx1_d3d9_dbg_writeback_all) &&
      count >= REXCVAR_GET(nx1_d3d9_writeback_max)) {
    MarkResolveDest(dest, WritebackVerdict::kOverBudget);
    return false;
  }
  ++count;
  MarkResolveDest(dest, WritebackVerdict::kWrote);
  return true;
}

/// THE PIXEL DIFF. Every theory tonight has hinged on one unanswered question: do we RENDER
/// the same image as the reference, or a different one? Fetch constants, memory contents and
/// streaming rates all failed to settle it. Pixels will.
///
/// Run with our resolve writeback OFF and the reference's readback_resolve="full" ON (plus
/// nx1_skip_reference_raster=false so its EDRAM is real). Then at the same resolve, guest RAM
/// holds the REFERENCE's pixels while our host target holds OURS -- same address, same frame.
///   ours matches ref      -> we render the same image; the fault is texture SAMPLING.
///   ours missing content  -> we genuinely do not draw it; unrendered-content proven in pixels.
void ResourceTracker::DumpResolveComparison(IDirect3DTexture9* tex, uint32_t width,
                                            uint32_t height, const TextureFetchConstant& dest) {
  const uint32_t budget = REXCVAR_GET(nx1_d3d9_dbg_framediff);
  if (!budget || !tex || !device_) {
    return;
  }
  REXCVAR_SET(nx1_d3d9_dbg_framediff, budget - 1);
  char path[256];

  IDirect3DSurface9* rt = nullptr;
  IDirect3DSurface9* stage = nullptr;
  if (SUCCEEDED(tex->GetSurfaceLevel(0, &rt)) && rt &&
      SUCCEEDED(device_->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8,
                                                     D3DPOOL_SYSTEMMEM, &stage, nullptr)) &&
      SUCCEEDED(device_->GetRenderTargetData(rt, stage))) {
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(stage->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
      std::vector<Rgba8> img(size_t(width) * height);
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
        for (uint32_t x = 0; x < width; ++x) {
          const uint8_t* px = row + size_t(x) * 4;
          img[size_t(y) * width + x] = {px[2], px[1], px[0], px[3]};
        }
      }
      stage->UnlockRect();
      std::snprintf(path, sizeof(path), "texdump/ours_%08X_f%llu.bmp", dest.base_address,
                    static_cast<unsigned long long>(frame_));
      DumpRgbaBmp(path, img, width, height);
    }
  }
  if (stage) stage->Release();
  if (rt) rt->Release();

  if (const auto* fmt = rex::graphics::FormatInfo::Get(dest.format)) {
    const uint32_t bpb = fmt->bytes_per_block();
    const uint32_t pitch_px = dest.pitch_pixels ? dest.pitch_pixels : width;
    rex::graphics::TextureExtent ex =
        rex::graphics::TextureExtent::Calculate(fmt, pitch_px, height, 1, dest.tiled, true);
    ex.block_width = (width + fmt->block_width - 1) / fmt->block_width;
    ex.block_height = (height + fmt->block_height - 1) / fmt->block_height;
    const size_t need = size_t(ex.block_height) * ex.block_width * bpb;
    if (const uint8_t* g = TranslatePhysical(PhysicalAddress(dest.base_address))) {
      uint8_t* scratch = DetileScratch(need);
      DetileMip2D(scratch, ex.block_width * bpb, g, ex, bpb, dest.endian, dest.tiled, 0, 0);
      std::vector<Rgba8> img(size_t(width) * height);
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = scratch + size_t(y) * ex.block_width * bpb;
        for (uint32_t x = 0; x < width; ++x) {
          const uint8_t* px = row + size_t(x) * 4;
          img[size_t(y) * width + x] = {px[0], px[1], px[2], px[3]};
        }
      }
      std::snprintf(path, sizeof(path), "texdump/ref_%08X_f%llu.bmp", dest.base_address,
                    static_cast<unsigned long long>(frame_));
      DumpRgbaBmp(path, img, width, height);
    }
  }
  REXGPU_WARN("nx1_d3d9: FRAMEDIFF {:08X} {}x{} -> texdump/ours_* and texdump/ref_*",
              dest.base_address, width, height);
}

void ResourceTracker::ResolveWriteback(IDirect3DTexture9* tex, uint32_t width, uint32_t height,
                                       const TextureFetchConstant& dest) {
  namespace tu = rex::graphics::texture_util;
  const auto t0 = std::chrono::steady_clock::now();

  const auto* fmt = rex::graphics::FormatInfo::Get(dest.format);
  if (!fmt) {
    return;
  }
  const uint32_t bpb = fmt->bytes_per_block();  // 4 for k_8_8_8_8
  const uint32_t pitch_px = dest.pitch_pixels ? dest.pitch_pixels : width;
  const rex::graphics::TextureExtent extent = rex::graphics::TextureExtent::Calculate(
      fmt, pitch_px, height, /*depth=*/1, dest.tiled, /*is_guest=*/true);
  const uint32_t pitch_blocks = extent.block_pitch_h;
  const uint64_t guest_bytes = uint64_t(extent.block_pitch_h) * extent.block_pitch_v * bpb;
  const uint32_t phys = PhysicalAddress(dest.base_address);
  if (uint64_t(phys) + guest_bytes > (uint64_t(kMirrorPages) << 12)) {
    return;
  }
  uint8_t* gdst = const_cast<uint8_t*>(TranslatePhysical(phys));
  if (!gdst) {
    return;
  }

  // Staging surface, reused while the size repeats (bakes come in bursts of one size).
  if (writeback_staging_ &&
      (writeback_staging_w_ != width || writeback_staging_h_ != height)) {
    writeback_staging_->Release();
    writeback_staging_ = nullptr;
  }
  if (!writeback_staging_) {
    if (FAILED(device_->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8,
                                                    D3DPOOL_SYSTEMMEM, &writeback_staging_,
                                                    nullptr))) {
      REXGPU_ERROR("nx1_d3d9: writeback staging ({}x{}) failed", width, height);
      writeback_staging_ = nullptr;
      return;
    }
    writeback_staging_w_ = width;
    writeback_staging_h_ = height;
  }
  IDirect3DSurface9* rt = nullptr;
  if (FAILED(tex->GetSurfaceLevel(0, &rt)) || !rt) {
    return;
  }
  // Synchronous by design: the caller (the guest thread, via the drain in Renderer::Resolve)
  // is waiting on this before letting the game's CPU read the destination.
  const HRESULT hr = device_->GetRenderTargetData(rt, writeback_staging_);
  rt->Release();
  if (FAILED(hr)) {
    REXGPU_ERROR("nx1_d3d9: writeback GetRenderTargetData({}x{}) failed {:#010x}", width, height,
                 uint32_t(hr));
    return;
  }
  D3DLOCKED_RECT lr{};
  if (FAILED(writeback_staging_->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    return;
  }

  // Byte order. Post-endian-swap, a k_8_8_8_8 texel's guest bytes are its components in order
  // [R][G][B][A] (see the Swizzle32 note); the host surface is [B][G][R][A]. So guest MEMORY
  // must hold the pre-swap form: reversed for 8-in-32 (the usual case), identity for none.
  // 8-in-16 on a 32bpp texel is nonsensical; treat as 8-in-32 and say so once.
  if (dest.endian == 1) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXGPU_WARN("nx1_d3d9: writeback dest {:08X} declares endian 1 (8in16) on 32bpp -- "
                  "treating as 8in32",
                  dest.base_address);
    }
  }
  const bool swap32 = dest.endian != 0;
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* srow = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* h = srow + size_t(x) * 4;  // B,G,R,A
      uint8_t out[4];
      if (swap32) {
        out[0] = h[3];  // A
        out[1] = h[0];  // B
        out[2] = h[1];  // G
        out[3] = h[2];  // R
      } else {
        out[0] = h[2];  // R
        out[1] = h[1];  // G
        out[2] = h[0];  // B
        out[3] = h[3];  // A
      }
      const size_t off =
          dest.tiled
              ? size_t(tu::GetTiledOffset2D(int32_t(x), int32_t(y), pitch_blocks, 2))
              : (size_t(y) * pitch_blocks + x) * 4;
      std::memcpy(gdst + off, out, 4);
    }
  }
  writeback_staging_->UnlockRect();

  ++writebacks_done_;
  writeback_bytes_ += guest_bytes;
  const double us =
      double((std::chrono::steady_clock::now() - t0).count()) / 1000.0;
  // TELL THE TEXTURE CACHE WE JUST WROTE THIS MEMORY.
  //
  // This function writes resolved render-target data into guest RAM through TranslatePhysical --
  // a HOST-side write. Host page protection only traps GUEST stores, so it does not fault, and
  // until now nothing was notified: not our texture cache, not the mirror. A texture sampling
  // this memory (render-to-texture, which is exactly what a resolve destination is for) kept
  // serving its previous decode.
  //
  // What covered for it was nx1_d3d9_content_probe, which POLLS live memory and rebuilds when the
  // content differs -- and REBUILDSRC measured the probe responsible for 1308 of 1405
  // content-changing rebuilds, 93% of every time we adopt new bytes, against 10 from real guest
  // writes. So the probe has largely been cleaning up after THIS. That also explains why enabling
  // it was the largest single improvement of the investigation.
  //
  // Polling is a poor substitute for a notification we can simply make: ProbeTiledContent is
  // SAMPLED (nx1_d3d9_probe_samples, 512 by default), so it can miss the change entirely and leave
  // a resolved-into texture stale indefinitely. Reporting the write explicitly is precise, cannot
  // miss, and is what the reference does for the same event (RangeWrittenByGpu on its resolve
  // path). kResolve so REBUILDSRC shows how much of the probe's work this takes over.
  // MEASURED WORSE ON SCREEN, SO DEFAULT OFF -- but kept, because the hole it closes is real.
  //
  // Reporting the write is correct: this function writes guest memory host-side and nothing was
  // told. But forcing the re-decode it implies makes the picture worse (user, 2026-07-22), and the
  // likely reason is that the two events are not the same thing. We write format 6 (8_8_8_8) pixels
  // here; a texture bound at this address with a DIFFERENT descriptor -- DXT1, say -- then decodes
  // resolve output through the wrong format and produces noise. Previously the SAMPLED content
  // probe often missed the change, so that texture kept its older and correct decode.
  //
  // So the invalidation is honest and the decode it triggers is wrong for an unrelated reason.
  // Turning it on is the right thing once a re-decode after a resolve uses the resolve's own
  // format rather than the fetch constant's; until then it trades a stale texture for a garbled
  // one. Also note this is a narrow path: format 6 only, dimension 1, and budgeted to
  // nx1_d3d9_writeback_max per destination -- it cannot account for the probe's 1308 content
  // changes, and an earlier claim in this file that it could was wrong.
  if (REXCVAR_GET(nx1_d3d9_resolve_invalidates)) {
    InvalidateGuestRange(phys, uint32_t(guest_bytes), DirtySource::kResolve);
  }
  NX1_LOGI_MISC("nx1_d3d9: WRITEBACK {:08X} {}x{} tiled={} endian={} {} KiB in {:.0f} us "
              "(#{} total)",
              dest.base_address, width, height, dest.tiled ? 1 : 0, dest.endian,
              guest_bytes / 1024, us, writebacks_done_);
}

namespace {
/// The engine's not-resident placeholder buffer, learned from ImageCache_GetDefaultPixels.
std::atomic<uint32_t> g_default_pixels{0};
}  // namespace

bool ResourceTracker::SetDefaultPixelsAddress(uint32_t addr) {
  uint32_t expected = 0;
  return g_default_pixels.compare_exchange_strong(expected, addr, std::memory_order_relaxed);
}

uint32_t ResourceTracker::DefaultPixelsAddress() {
  return g_default_pixels.load(std::memory_order_relaxed);
}

void ResourceTracker::ResolveColor(uint32_t dest_address, uint32_t width, uint32_t height,
                                   const RECT& src_rect, const POINT& dest_point,
                                   const TextureFetchConstant& dest, bool writeback) {
  // BEFORE the early-out, deliberately. A destination dropped here (no device yet, zero extent)
  // is one the guest DID resolve into and we did not record -- exactly the case the render-to-
  // texture census exists to catch, and it would be invisible if noted after the guard.
  NoteResolveDestSeen(dest_address, uint64_t(width) * height * 4);
  if (!device_ || !resolves_ || !dest_address || !width || !height) {
    return;
  }
  auto* map = static_cast<ResolveMap*>(resolves_);
  if (const uint32_t track = TrackedMatch(PhysicalAddress(dest_address)); track) {
    NX1_LOGI_TEX("nx1_d3d9: TRACK {:08X} RESOLVE dest={:08X} {}x{}", track, dest_address, width,
                height);
  }
  auto& entry = (*map)[PhysicalAddress(dest_address)];

  if (entry.tex && (!entry.owned || entry.width != width || entry.height != height)) {
    if (entry.owned) {
      entry.tex->Release();
    }
    entry.tex = nullptr;
  }
  if (!entry.tex) {
    // Render-target texture so StretchRect can write it and shaders can sample it.
    if (FAILED(device_->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                      D3DPOOL_DEFAULT, &entry.tex, nullptr))) {
      REXGPU_ERROR("nx1_d3d9: resolve target CreateTexture({}x{}) failed", width, height);
      entry.tex = nullptr;
      return;
    }
    entry.width = width;
    entry.height = height;
    entry.owned = true;
  }

  IDirect3DSurface9* src = nullptr;
  IDirect3DSurface9* dst = nullptr;
  if (SUCCEEDED(device_->GetRenderTarget(0, &src)) && src &&
      SUCCEEDED(entry.tex->GetSurfaceLevel(0, &dst)) && dst) {
    // Copy the resolved region out of whichever host target the guest is currently
    // rendering into. Under predicated tiling this is one *band* of the frame, and it
    // must land at dest_point -- blitting it over the whole destination smears the last
    // band across everything.
    const bool whole = src_rect.right <= src_rect.left || src_rect.bottom <= src_rect.top;
    D3DSURFACE_DESC sd{};
    src->GetDesc(&sd);
    RECT sr = whole ? RECT{0, 0, LONG(sd.Width), LONG(sd.Height)} : src_rect;
    // Clamp the source to the target we actually have.
    sr.right = std::min<LONG>(sr.right, LONG(sd.Width));
    sr.bottom = std::min<LONG>(sr.bottom, LONG(sd.Height));
    if (sr.right > sr.left && sr.bottom > sr.top) {
      RECT dr{dest_point.x, dest_point.y, dest_point.x + (sr.right - sr.left),
              dest_point.y + (sr.bottom - sr.top)};
      // THE HARDWARE RESOLVE CANNOT STRETCH -- the XDK states it outright ("It cannot do any
      // stretching"), and for a null source rect it defines the behaviour as copying the UPPER
      // LEFT CORNER of the surface, not a downscale of the whole thing. Clamping the destination
      // without shrinking the source in lockstep turned every oversized copy into a point-filtered
      // downscale: a small destination (a bloom/luminance reduction step) received a squashed whole
      // frame instead of the corner tile its shader expects, so the reduction converged on the
      // wrong average. Shrink both edges together and the blit stays 1:1, as on hardware.
      const LONG clamped_w = std::min<LONG>(dr.right, LONG(width)) - dr.left;
      const LONG clamped_h = std::min<LONG>(dr.bottom, LONG(height)) - dr.top;
      if (clamped_w > 0 && clamped_h > 0) {
        sr.right = sr.left + clamped_w;
        sr.bottom = sr.top + clamped_h;
        dr.right = dr.left + clamped_w;
        dr.bottom = dr.top + clamped_h;
        device_->StretchRect(src, &sr, dst, &dr, D3DTEXF_POINT);
      }
    }
  }
  if (dst) dst->Release();
  if (src) src->Release();
  // The bind path scans a flat mirror of this map; keep it in step.
  RebuildResolveFlat();

  // After the band has landed in the host copy. Bands before the last write a partially
  // updated image; the final band's writeback leaves the destination complete, and the guest
  // thread is drained behind ALL of them before the game can read.
  if (writeback && entry.tex) {
    ResolveWriteback(entry.tex, width, height, dest);
  }
  if (entry.tex) {
    DumpResolveComparison(entry.tex, width, height, dest);
  }
}

}  // namespace nx1::d3d9

#endif  // _WIN32
