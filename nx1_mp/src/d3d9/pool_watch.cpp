/// Pool watch -- observe guest texture-pool memory over time, INDEPENDENT of which renderer runs.
///
/// WHY THIS EXISTS, and why it is not part of the D3D9 tracker.
///
/// The speckle investigation is stuck on one fork that no instrument could reach, because every
/// instrument we own lives inside the native D3D9 renderer and therefore cannot observe the
/// configuration we most need to compare against:
///
///   Xenia renders this scene correctly and has NEVER speckled ([[nx1-d3d9-reference-is-ground-truth]],
///   settled by the user, not a hypothesis). Xenia uploads textures from
///   `memory().TranslatePhysical(...)` -- the SAME CPU RAM the native renderer decodes from
///   (d3d12/shared_memory.cpp:393). Yet the native renderer measurably reads a wall texture that is
///   only 21% populated while drawing it (run 071, 10CBA000: 6944/32768 nonzero, constant across
///   2600 binds, filling to 99% only after the last bind).
///
/// Those cannot both be true of the same bytes at the same moment. Either the pool degrades for
/// everyone and Xenia survives on a snapshot it took while the data was intact (it uploads a page
/// only when not already valid and re-reads only on invalidation), or the pool degrades ONLY when
/// our renderer is active -- in which case we are corrupting it, and `CopyLiveSourcePages` writing
/// into `GuestPointer(base, dst + off)` is the obvious suspect.
///
/// Hooking Xenia's own upload path cannot answer it: with nx1_skip_reference_raster on, IssueDraw
/// returns at d3d12/command_processor.cpp:3851, BEFORE texture_cache_->RequestTextures() at :4034,
/// so in the native configuration Xenia never requests or uploads textures at all. Its copy of the
/// texture simply does not exist in the runs being debugged.
///
/// So watch the MEMORY instead of either renderer. This runs on its own thread, gated only on its
/// own cvar, so the same measurement can be taken in pure-Xenia mode and in native mode and the
/// two trajectories compared directly.
///
/// WHAT IT REPORTS, and why these three events: a page filling is normal streaming; a page LOSING
/// content is the event this whole investigation has been unable to catch; and a page whose bytes
/// change while its density does not is a slot being reused. Only the second and third can explain
/// a texture that is 21% present while being drawn.
///
/// SAMPLING. Bytes are read every 8th offset. This is a statistical watch over tens of megabytes on
/// a timer, not a correctness check -- full hashing would cost ~16x for no change in whether a page
/// went from populated to empty. Reads race the guest by construction (no lock, no fence); a torn
/// read can at worst misreport one page on one tick, and the events we care about persist across
/// ticks.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#ifdef _WIN32
// NOMINMAX before windows.h: its min/max macros otherwise swallow every std::min/std::max below.
// guest_d3d.h documents this same trap ("drags std::min in behind <windows.h>'s min macro").
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // VirtualQuery -- see HostReadable
#endif
#include <algorithm>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <rex/system/kernel_state.h>

#include "guest_d3d.h"

namespace nx1::d3d9 {

REXCVAR_DEFINE_BOOL(nx1_poolwatch, false, "GPU",
                    "Watch guest texture-pool memory on a background thread and report pages that "
                    "LOSE content. Runs in pure-Xenia mode as well as native, so the same "
                    "measurement can be compared between the two");
/// Physical base of the watched window. The texture pool has lived around 0x0E000000-0x17000000 in
/// every capture this investigation has taken; the default starts there rather than at 0 so the
/// scan cost stays bounded.
REXCVAR_DEFINE_UINT32(nx1_poolwatch_addr, 0x0E000000, "GPU",
                      "Physical start address of the pool-watch window");
/// 160 MiB by default so per-launch pool relocation cannot move the pool out from under the
/// window. A fixed 64 MiB window made cross-run "empty page" counts unsafe to compare.
REXCVAR_DEFINE_UINT32(nx1_poolwatch_mb, 160, "GPU", "Size of the pool-watch window, in MiB");
REXCVAR_DEFINE_UINT32(nx1_poolwatch_ms, 250, "GPU", "Pool-watch sampling interval, milliseconds");
/// A page must lose at least this percent of its populated bytes to count as DEGRADED. Small
/// wobbles are normal for a live pool; the artifact is a page going from mostly-populated to
/// mostly-empty.
REXCVAR_DEFINE_UINT32(nx1_poolwatch_drop_pct, 25, "GPU",
                      "Percent of a page's populated bytes that must disappear to report a "
                      "DEGRADED event");

namespace {

struct PageState {
  uint32_t nz = 0;      ///< sampled populated bytes
  uint64_t hash = 0;    ///< sampled content hash
  bool seen = false;
};

std::atomic<bool> g_started{false};

/// IS THIS HOST PAGE ACTUALLY READABLE?
///
/// GuestTranslatePhysical is pure arithmetic -- TranslateVirtual folds in the heap's host offset
/// and returns a pointer whether or not anything is mapped there. A non-null return says NOTHING
/// about the page being committed, and this thread crashed the very first launch by trusting it:
///
///   Unhandled host access violation on non-guest thread, read fault 000000010E000000
///
/// which is host_base + guest 0x0E000000, the first page of the watch window, unmapped at the time.
/// So ask the OS. VirtualQuery hands back the whole enclosing region, which lets the caller skip an
/// unmapped span in one call instead of faulting page by page.
///
/// Guarded pages are skipped as well as uncommitted ones: touching a PAGE_GUARD page raises an
/// exception AND clears the guard bit, which would perturb whatever the guest is using it for.
/// A read on an ordinary write-watched (PAGE_READONLY) page is harmless and does not fire the
/// write callback -- this watch must observe the pool, never alter it.
bool HostReadable(const uint8_t* p, const uint8_t** region_end) {
  *region_end = p + 4096;
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) {
    return false;
  }
  *region_end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) {
    return false;
  }
  switch (mbi.Protect & 0xFF) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;  // PAGE_NOACCESS and anything unexpected
  }
#else
  return true;
#endif
}

void PoolWatchThread() {
  std::vector<PageState> pages;
  uint32_t window_addr = 0, window_pages = 0;
  uint64_t fills = 0, degrades = 0, replaces = 0, ticks = 0;

  for (;;) {
    const uint32_t interval = std::max(REXCVAR_GET(nx1_poolwatch_ms), 16u);
    std::this_thread::sleep_for(std::chrono::milliseconds(interval));
    if (!REXCVAR_GET(nx1_poolwatch)) {
      continue;  // hot-reloadable: stay parked until switched on, then start clean
    }

    const uint32_t addr = REXCVAR_GET(nx1_poolwatch_addr) & ~0xFFFu;
    const uint32_t want_pages =
        std::min<uint32_t>(std::max(REXCVAR_GET(nx1_poolwatch_mb), 1u), 512u) * 256u;
    if (addr != window_addr || want_pages != window_pages) {
      // Window changed (or first run): reset the baseline rather than reporting every page as
      // changed. A "degradation" that is really a re-aim would be the fifteenth lying instrument.
      pages.assign(want_pages, PageState{});
      window_addr = addr;
      window_pages = want_pages;
      fills = degrades = replaces = 0;
      REXGPU_WARN("nx1: POOLWATCH armed on {:08X}+{} MiB, {} ms interval, drop>={}% -- baseline "
                  "reset, so the first tick reports nothing by construction",
                  window_addr, want_pages / 256, interval, REXCVAR_GET(nx1_poolwatch_drop_pct));
      continue;
    }

    const uint32_t drop_pct = std::min(REXCVAR_GET(nx1_poolwatch_drop_pct), 100u);
    uint32_t full = 0, partial = 0, empty = 0, unmapped = 0;
    uint32_t first_pop = 0xFFFFFFFFu, last_pop = 0;
    uint32_t tick_degrades = 0, tick_fills = 0, tick_replaces = 0;

    // Cache the end of the last region VirtualQuery reported, so a mapped run costs one query
    // rather than one per page, and an unmapped run is skipped wholesale.
    const uint8_t* readable_until = nullptr;
    for (uint32_t i = 0; i < window_pages; ++i) {
      const uint32_t page_addr = window_addr + i * 4096u;
      // TranslatePhysical, NOT GuestTranslatePhysical.
      //
      // GuestTranslatePhysical is TranslateVirtual underneath -- right for the 0xA/0xB/0xE window
      // EAs the DMA hooks deal in, WRONG for a raw physical page number like 0x0E000000. Using it
      // resolved every page of the window into unmapped space, so the first two runs reported
      // `unmapped=139264` -- the entire window -- and produced two identical all-zero totals that
      // looked like a clean comparison and measured nothing at all. The texture decoder reads pool
      // memory through TranslatePhysical (d3d9_resources.cpp:1179); match it exactly, or this
      // watch is not looking at the same bytes the decoder is.
      const uint8_t* p =
          rex::system::kernel_state()->memory()->TranslatePhysical<const uint8_t*>(page_addr);
      if (!p) {
        ++unmapped;
        continue;
      }
      if (p + 4096 > readable_until) {
        const uint8_t* region_end = nullptr;
        if (!HostReadable(p, &region_end)) {
          ++unmapped;
          // Nothing readable until the end of this region; if it covers further pages of the
          // window, skip them without another query.
          // CLAMP THE SKIP TO THE WINDOW. VirtualQuery reports the whole enclosing region, which
          // for a large reservation can be far bigger than the window -- the first version added
          // that raw region size to `unmapped` and reported 139264 unmapped pages out of a 16384
          // page window, a counter that cannot be believed even in principle.
          if (region_end > p) {
            const uint64_t region_pages = uint64_t(region_end - p) / 4096;
            const uint32_t remaining = window_pages - i;
            const uint32_t skip = uint32_t(std::min<uint64_t>(region_pages, remaining));
            if (skip > 1) {
              i += skip - 1;
              unmapped += skip - 1;
            }
          }
          readable_until = nullptr;
          continue;
        }
        readable_until = region_end;
      }
      uint32_t nz = 0;
      uint64_t hash = 1469598103934665603ull;
      for (uint32_t off = 0; off < 4096; off += 8) {
        const uint8_t b = p[off];
        nz += b != 0 ? 1 : 0;
        hash = (hash ^ b) * 1099511628211ull;
      }
      const uint32_t kSamples = 4096 / 8;
      if (nz) {
        if (page_addr < first_pop) first_pop = page_addr;
        if (page_addr > last_pop) last_pop = page_addr;
      }
      if (nz == 0) {
        ++empty;
      } else if (nz * 100 >= kSamples * 90u) {
        ++full;
      } else {
        ++partial;
      }

      PageState& s = pages[i];
      if (!s.seen) {
        s = PageState{nz, hash, true};
        continue;
      }
      if (nz == s.nz && hash == s.hash) {
        continue;
      }
      // DEGRADED: populated bytes disappeared. This is the event the investigation could not catch.
      if (s.nz && nz < s.nz && (s.nz - nz) * 100 >= s.nz * drop_pct) {
        ++degrades;
        if (++tick_degrades <= 8) {
          REXGPU_WARN("nx1: POOLWATCH DEGRADED page {:08X} populated {}->{} of {} samples "
                      "({}% lost) -- content that was there is now gone",
                      page_addr, s.nz, nz, kSamples, (s.nz - nz) * 100 / s.nz);
        }
      } else if (nz == s.nz) {
        // Same density, different bytes: the slot was reused rather than filled or emptied.
        ++replaces;
        if (++tick_replaces <= 4) {
          REXGPU_WARN("nx1: POOLWATCH REPLACED page {:08X} same populated count ({}) but different "
                      "bytes -- this slot's occupant changed",
                      page_addr, nz);
        }
      } else if (nz > s.nz) {
        ++fills;
        ++tick_fills;
      }
      s.nz = nz;
      s.hash = hash;
    }

    // WHERE WAS THE POOL, ACTUALLY? Report the observed extent -- first and last populated page.
    //
    // Pool base addresses are reassigned EVERY LAUNCH, so a fixed window can cover different
    // fractions of the real pool in different runs. Comparing "empty page" counts across runs is
    // then meaningless: a run whose pool sits outside the window reads as empty, which is
    // indistinguishable from a pool that genuinely did not fill. The mirror-off run showed
    // empty=8917 while textures visibly loaded, which is exactly that ambiguity.
    //
    // Printing the extent makes each run state where the pool actually was, so two runs can be
    // checked for comparability instead of assumed comparable.
    // Unconditional totals, every tick. A silent watch is indistinguishable from a dead one, and
    // the whole point is to compare these numbers between two runs -- so they must print even when
    // nothing happened.
    if ((++ticks % 8) == 1 || tick_degrades || tick_replaces) {
      REXGPU_WARN("nx1: POOLWATCH tick {} | pages full={} partial={} empty={} unmapped={} | "
                  "populated extent {:08X}..{:08X} ({} MiB of the {} MiB window) | this tick: "
                  "degraded={} replaced={} filled={} | totals: degraded={} replaced={} filled={}",
                  ticks, full, partial, empty, unmapped, first_pop, last_pop,
                  first_pop <= last_pop ? ((last_pop - first_pop) >> 20) + 1 : 0,
                  window_pages / 256, tick_degrades, tick_replaces, tick_fills, degrades, replaces,
                  fills);
    }
  }
}

}  // namespace

void EnsurePoolWatchStarted() {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }
  // Detached on purpose: this outlives any renderer and has no shutdown ordering requirements --
  // it only reads guest memory and logs.
  std::thread(PoolWatchThread).detach();
}

}  // namespace nx1::d3d9
