/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/graphics/d3d12/nx1_native_shader_cache.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_BOOL(d3d12_bindless, true, "GPU/D3D12", "Use bindless resources where available")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(d3d12_readback_memexport, false, "GPU/D3D12",
                    "Read data written by memory export in shaders on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(d3d12_readback_resolve, false, "GPU/D3D12",
                    "Read render-to-texture results on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(d3d12_submit_on_primary_buffer_end, true, "GPU/D3D12",
                    "Submit command list when PM4 primary buffer ends")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(d3d12_primary_buffer_submit_interval, 1, "GPU/D3D12",
                      "Submit command list after this many PM4 primary buffers when "
                      "d3d12_submit_on_primary_buffer_end is enabled")
    .range(1, 16)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_bypass_black_scene_composite, false, "GPU/D3D12",
                    "NX1 diagnostic: for the final composite shader, sample the valid scene "
                    "buffer instead of the black postprocess buffer")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_disable_scene_composite_depth, false, "GPU/D3D12",
                    "NX1 diagnostic: disable depth/stencil while drawing the scene composite "
                    "EDRAM target")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_disable_scene_composite_alpha_test, false, "GPU/D3D12",
                    "NX1 diagnostic: disable alpha test/alpha-to-mask while drawing the scene "
                    "composite EDRAM target")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_gpu_probe_logging, false, "GPU/D3D12",
                    "NX1 diagnostic: log high-frequency GPU probe details")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_gpu_hitch_profiling, false, "GPU/D3D12",
                    "NX1 diagnostic: log D3D12 renderer stages that exceed the hitch threshold")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_gpu_hitch_log_individual, false, "GPU/D3D12",
                    "NX1 diagnostic: log every renderer hitch immediately instead of summary-only")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(nx1_gpu_hitch_threshold_us, 4000, "GPU/D3D12",
                      "NX1 D3D12 renderer hitch logging threshold in microseconds")
    .range(100, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(nx1_gpu_hitch_summary_frames, 600, "GPU/D3D12",
                      "NX1 D3D12 renderer hitch summary interval in frames, or 0 to disable")
    .range(0, 60000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_native_present_direct_copy, false, "GPU/D3D12",
                    "NX1 legacy override: force direct-copy native present attempts")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(nx1_native_present_mode, "auto", "GPU/D3D12",
                      "NX1 experimental native present path: off, auto, direct_copy, shader_copy")
    .allowed({"off", "auto", "direct_copy", "shader_copy"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_native_present_bypass_gamma, true, "GPU/D3D12",
                    "NX1 diagnostic: allow raw native present copies that bypass the Xbox gamma "
                    "ramp. This is faster but can cause lighting flicker.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_native_present_gamma_descriptors, true, "GPU/D3D12",
                    "NX1 native present: use the lightweight native descriptor heap for the "
                    "gamma-preserving swap path.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_native_present_log, false, "GPU/D3D12",
                    "Log NX1 native present decisions and fallbacks")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_native_shader_cache, false, "GPU/D3D12",
                    "NX1 experimental: use XenosRecomp DXIL shader cache for eligible guest draws")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_native_shader_cache_vs_only, true, "GPU/D3D12",
                    "NX1 experimental: allow native XenosRecomp shaders for VS-only depth/no-color draws")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_native_shader_cache_color, false, "GPU/D3D12",
                    "NX1 experimental (WIP, currently renders black - keep OFF): also use native "
                    "XenosRecomp shaders for color (VS+PS) draws so they share the same native vertex "
                    "shader as the depth pre-pass. Configures eligible color draws synchronously "
                    "(bypassing async placeholder compilation) so the native path is selected. The "
                    "native VS+PS path is not yet functional; enable only for debugging.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nx1_native_shader_cache_log, false, "GPU/D3D12",
                    "Log NX1 native shader cache hits and fallbacks")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(nx1_dbg_fetch_seq, 0, "GPU",
                      "Diagnostic: log this many draws' texture fetch constants from BOTH "
                      "sides -- CPFETCH here, read from the PM4 register file at the moment "
                      "this command processor executes the draw (the one instant it is "
                      "authoritative), and D9FETCH in the native renderer at its own draw. "
                      "Aligning the two s0 streams offline is the only valid form of the "
                      "shadow-copy-vs-ring comparison: an instantaneous register-file read "
                      "from the other thread only measures ring lag");

REXCVAR_DEFINE_BOOL(nx1_suppress_reference_resolve, false, "GPU",
                    "Diagnostic: while nx1_d3d9 owns the output, do not let this backend resolve "
                    "EDRAM back into guest memory. Resolves are the only guest-memory WRITES it "
                    "performs, they are host-side (invisible to the guest write-watch), and they "
                    "run regardless of nx1_skip_reference_raster -- so this is the only way to "
                    "rule them out as the source of corrupted texture memory");

REXCVAR_DEFINE_BOOL(nx1_skip_reference_raster, true, "GPU",
                    "Skip the reference backend's raster when the native renderer "
                    "owns output (off = let it render, so render-to-texture "
                    "content still reaches guest memory)");

namespace rex::graphics::d3d12 {

// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_pwl_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_pwl_fxaa_luma_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_table_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_table_fxaa_luma_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fxaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fxaa_extreme_cs.h"
#include "../shaders/bytecode/d3d12_5_1/nx1_native_present_copy_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_downscale_cs.h"
}  // namespace shaders

namespace {

using Nx1HitchClock = std::chrono::steady_clock;

enum class Nx1NativePresentMode {
  kOff,
  kAuto,
  kDirectCopy,
  kShaderCopy,
};

Nx1NativePresentMode Nx1GetNativePresentMode() {
  if (REXCVAR_GET(nx1_native_present_direct_copy)) {
    return Nx1NativePresentMode::kDirectCopy;
  }

  const std::string& mode = REXCVAR_GET(nx1_native_present_mode);
  if (mode == "off") {
    return Nx1NativePresentMode::kOff;
  }
  if (mode == "direct_copy") {
    return Nx1NativePresentMode::kDirectCopy;
  }
  if (mode == "shader_copy") {
    return Nx1NativePresentMode::kShaderCopy;
  }
  return Nx1NativePresentMode::kAuto;
}

const char* Nx1NativePresentModeName(Nx1NativePresentMode mode) {
  switch (mode) {
    case Nx1NativePresentMode::kOff:
      return "off";
    case Nx1NativePresentMode::kAuto:
      return "auto";
    case Nx1NativePresentMode::kDirectCopy:
      return "direct_copy";
    case Nx1NativePresentMode::kShaderCopy:
      return "shader_copy";
  }
  return "unknown";
}

uint64_t Nx1ElapsedUs(Nx1HitchClock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      Nx1HitchClock::now() - start)
                      .count());
}

struct Nx1HitchSummaryEntry {
  const char* stage = nullptr;
  uint64_t count = 0;
  uint64_t total_us = 0;
  uint64_t max_us = 0;
  uint64_t max_frame = 0;
  uint64_t max_item = 0;
};

void Nx1ResetHitchSummary(Nx1HitchSummaryEntry* entries, size_t entry_count) {
  for (size_t i = 0; i < entry_count; ++i) {
    entries[i] = {};
  }
}

void Nx1LogHitchSummary(Nx1HitchSummaryEntry* entries, size_t entry_count,
                        uint64_t first_frame, uint64_t last_frame) {
  Nx1HitchSummaryEntry* ordered[64] = {};
  static_assert(rex::countof(ordered) >= 64);
  uint32_t ordered_count = 0;
  uint64_t total_count = 0;
  for (size_t i = 0; i < entry_count && ordered_count < rex::countof(ordered); ++i) {
    if (!entries[i].stage || !entries[i].count) {
      continue;
    }
    total_count += entries[i].count;
    ordered[ordered_count++] = &entries[i];
  }
  if (!ordered_count) {
    return;
  }

  std::sort(ordered, ordered + ordered_count,
            [](const Nx1HitchSummaryEntry* a, const Nx1HitchSummaryEntry* b) {
              return a->max_us > b->max_us;
            });
  REXGPU_WARN("[NX1 hitch summary] frames={}..{} hitches={}", first_frame, last_frame,
              total_count);
  const uint32_t top_count = std::min<uint32_t>(ordered_count, 6);
  for (uint32_t i = 0; i < top_count; ++i) {
    const Nx1HitchSummaryEntry& entry = *ordered[i];
    REXGPU_WARN(
        "[NX1 hitch summary] #{} stage={} count={} avg={:.3f}ms max={:.3f}ms max_frame={} "
        "max_item={}",
        i + 1, entry.stage, entry.count,
        double(entry.total_us) / double(entry.count) / 1000.0, double(entry.max_us) / 1000.0,
        entry.max_frame, entry.max_item);
  }
}

void Nx1RecordHitchSummary(const char* stage, uint64_t elapsed_us, uint64_t frame,
                           uint64_t item) {
  const uint32_t summary_frames = REXCVAR_GET(nx1_gpu_hitch_summary_frames);
  if (!summary_frames) {
    return;
  }

  static Nx1HitchSummaryEntry summaries[64];
  static uint64_t first_summary_frame = 0;
  static uint64_t next_summary_frame = 0;
  if (!first_summary_frame) {
    first_summary_frame = frame;
    next_summary_frame = frame + summary_frames;
  }

  Nx1HitchSummaryEntry* entry = nullptr;
  for (Nx1HitchSummaryEntry& candidate : summaries) {
    if (candidate.stage && std::strcmp(candidate.stage, stage) == 0) {
      entry = &candidate;
      break;
    }
    if (!candidate.stage && !entry) {
      entry = &candidate;
    }
  }
  if (entry) {
    if (!entry->stage) {
      entry->stage = stage;
    }
    ++entry->count;
    entry->total_us += elapsed_us;
    if (elapsed_us > entry->max_us) {
      entry->max_us = elapsed_us;
      entry->max_frame = frame;
      entry->max_item = item;
    }
  }

  if (frame >= next_summary_frame) {
    Nx1LogHitchSummary(summaries, rex::countof(summaries), first_summary_frame, frame);
    Nx1ResetHitchSummary(summaries, rex::countof(summaries));
    first_summary_frame = frame;
    next_summary_frame = frame + summary_frames;
  }
}

void Nx1LogHitch(const char* stage, uint64_t elapsed_us, uint64_t frame, uint64_t item) {
  if (!REXCVAR_GET(nx1_gpu_hitch_profiling)) {
    return;
  }
  const uint32_t threshold_us = std::max<uint32_t>(1, REXCVAR_GET(nx1_gpu_hitch_threshold_us));
  if (elapsed_us < threshold_us) {
    return;
  }
  Nx1RecordHitchSummary(stage, elapsed_us, frame, item);
  if (REXCVAR_GET(nx1_gpu_hitch_log_individual)) {
    REXGPU_WARN("[NX1 hitch] stage={} elapsed={:.3f}ms frame={} item={} threshold={:.3f}ms",
                stage, double(elapsed_us) / 1000.0, frame, item,
                double(threshold_us) / 1000.0);
  }
}

class Nx1ScopedHitchTimer {
 public:
  Nx1ScopedHitchTimer(const char* stage, uint64_t frame, uint64_t item)
      : enabled_(REXCVAR_GET(nx1_gpu_hitch_profiling)),
        stage_(stage),
        frame_(frame),
        item_(item),
        start_(enabled_ ? Nx1HitchClock::now() : Nx1HitchClock::time_point{}) {}

  ~Nx1ScopedHitchTimer() {
    if (enabled_) {
      Nx1LogHitch(stage_, Nx1ElapsedUs(start_), frame_, item_);
    }
  }

 private:
  bool enabled_;
  const char* stage_;
  uint64_t frame_;
  uint64_t item_;
  Nx1HitchClock::time_point start_;
};

struct Nx1DrawProbeStats {
  uint64_t calls = 0;
  uint64_t issued = 0;
  uint64_t skipped = 0;
  uint64_t dropped = 0;
  struct FrameStats {
    struct TextureSnapshot {
      uint32_t slot = 0;
      uint32_t base = 0;
      uint32_t width = 0;
      uint32_t height = 0;
      uint32_t format = 0;
      uint32_t dwords[6] = {};
    };
    struct ColorDrawSnapshot {
      uint64_t issue = 0;
      uint64_t call = 0;
      bool indexed = false;
      uint32_t rt_bits = 0;
      uint32_t color_mask = 0;
      uint32_t viewport_width = 0;
      uint32_t viewport_height = 0;
      uint32_t viewport_x = 0;
      uint32_t viewport_y = 0;
      uint32_t scissor_width = 0;
      uint32_t scissor_height = 0;
      uint32_t scissor_x = 0;
      uint32_t scissor_y = 0;
      uint32_t texture_mask = 0;
      uint32_t guest_indices = 0;
      uint32_t host_vertices = 0;
      uint64_t vs = 0;
      uint64_t ps = 0;
      uint32_t texture_count = 0;
      TextureSnapshot textures[4] = {};
    };

    uint64_t calls = 0;
    uint64_t issued = 0;
    uint64_t skipped = 0;
    uint64_t dropped = 0;
    uint64_t copy_packets = 0;
    uint64_t resolves = 0;
    uint64_t frontbuffer_resolves = 0;
    uint64_t indexed_draws = 0;
    uint64_t color_draws = 0;
    uint64_t large_color_draws = 0;
    uint64_t thin_color_draws = 0;
    uint64_t color_vertices = 0;
    uint64_t depth_only_draws = 0;
    uint64_t memexport_draws = 0;
    uint32_t resolve_log_count = 0;
    uint32_t resolve_addresses[8] = {};
    uint32_t resolve_lengths[8] = {};
    uint32_t last_resolve_address = 0;
    uint32_t last_resolve_length = 0;
    bool last_resolve_result = false;
    uint32_t last_color_rt_bits = 0;
    uint32_t last_color_mask = 0;
    uint32_t last_color_viewport_width = 0;
    uint32_t last_color_viewport_height = 0;
    uint32_t last_color_scissor_width = 0;
    uint32_t last_color_scissor_height = 0;
    uint32_t last_color_textures = 0;
    uint32_t last_color_fetch0_base = 0;
    uint32_t last_color_fetch0_width = 0;
    uint32_t last_color_fetch0_height = 0;
    uint32_t last_color_fetch0_format = 0;
    uint32_t last_color_fetch0_dwords[6] = {};
    uint64_t last_color_vs = 0;
    uint64_t last_color_ps = 0;
    uint32_t color_ring_next = 0;
    uint32_t color_ring_count = 0;
    ColorDrawSnapshot color_ring[32] = {};
  } frame;
};

Nx1DrawProbeStats& Nx1DrawProbe() {
  static Nx1DrawProbeStats stats;
  return stats;
}

bool Nx1ShouldLogDrawProbe(uint64_t count) {
  return REXCVAR_GET(nx1_gpu_probe_logging) && (count <= 128 || (count % 1000) == 0);
}

bool Nx1ShouldLogOcclusionProbe(uint64_t count) {
  return REXCVAR_GET(nx1_gpu_probe_logging) && (count <= 128 || (count % 1000) == 0);
}

bool Nx1IsKnownFrontbufferResolve(uint32_t address, uint32_t length) {
  constexpr uint32_t kFrontbufferLength = 0x398000;
  return length == kFrontbufferLength &&
         (address == 0x0706D000 || address == 0x07406000);
}

void Nx1RecordFrameResolve(Nx1DrawProbeStats::FrameStats& frame, bool resolved,
                           uint32_t written_address, uint32_t written_length) {
  frame.last_resolve_result = resolved;
  frame.last_resolve_address = written_address;
  frame.last_resolve_length = written_length;
  if (!resolved || !written_length) {
    return;
  }
  ++frame.resolves;
  if (frame.resolve_log_count < rex::countof(frame.resolve_addresses)) {
    frame.resolve_addresses[frame.resolve_log_count] = written_address;
    frame.resolve_lengths[frame.resolve_log_count] = written_length;
    ++frame.resolve_log_count;
  }
  if (Nx1IsKnownFrontbufferResolve(written_address, written_length)) {
    ++frame.frontbuffer_resolves;
  }
}

int Nx1TrackedResolveIndex(uint32_t address) {
  switch (address & 0x1FFFFFFF) {
    case 0x1E460000:
      return 0;
    case 0x1E560000:
      return 1;
    case 0x1E860000:
      return 2;
    case 0x1EC60000:
      return 3;
    case 0x1F6D8000:
      return 4;
    case 0x1F938000:
      return 5;
    case 0x0706D000:
      return 6;
    case 0x07406000:
      return 7;
    default:
      return -1;
  }
}

const char* Nx1TrackedResolveLabel(int index) {
  static constexpr const char* kLabels[] = {
      "rtt_1e46",        "rtt_1e56",      "rtt_1e86",      "rtt_1ec6",
      "scene_ping_depth", "scene_composite", "frontbuffer_a", "frontbuffer_b"};
  return (index >= 0 && index < int(rex::countof(kLabels))) ? kLabels[index] : "unknown";
}

void Nx1ResolveReadbackDimensions(uint32_t length, uint32_t& width_out,
                                  uint32_t& height_out) {
  switch (length) {
    case 0x100000:
      width_out = 512;
      height_out = 512;
      return;
    case 0x260000:
      width_out = 1024;
      height_out = 608;
      return;
    case 0x398000:
      width_out = 1280;
      height_out = 736;
      return;
    case 0x400000:
      width_out = 1024;
      height_out = 1024;
      return;
    default:
      width_out = 0;
      height_out = 0;
      return;
  }
}

void Nx1LogResolveReadbackSamples(uint32_t written_address, uint32_t written_length,
                                  const void* mapped_data) {
  const int tracked_index = Nx1TrackedResolveIndex(written_address);
  if (tracked_index < 0 || !mapped_data || written_length < sizeof(uint32_t)) {
    return;
  }

  // Only sample real in-level frames; the startup and menu resolves aren't the
  // black-screen bug we're chasing.
  if (Nx1DrawProbe().frame.calls < 1000) {
    return;
  }

  static uint64_t log_counts[8] = {};
  uint64_t& log_count = log_counts[tracked_index];
  ++log_count;
  if (log_count > 16 && (log_count % 120) != 0) {
    return;
  }

  const uint8_t* bytes = static_cast<const uint8_t*>(mapped_data);
  const uint32_t word_count = written_length / sizeof(uint32_t);
  uint64_t color_nonzero = 0;
  uint64_t rgb_gt8 = 0;
  uint64_t rgb_gt32 = 0;
  uint64_t rgb_gt64 = 0;
  uint32_t min_word = UINT32_MAX;
  uint32_t max_word = 0;
  uint32_t first_nonzero_word_index = UINT32_MAX;
  uint32_t first_nonzero_word = 0;
  auto read_word = [&](uint32_t word_index) -> uint32_t {
    if (word_index >= word_count) {
      return 0;
    }
    uint32_t word = 0;
    std::memcpy(&word, bytes + uint64_t(word_index) * sizeof(uint32_t), sizeof(word));
    return word;
  };
  auto max_rgb8 = [](uint32_t word) -> uint32_t {
    const uint32_t r = word & 0xFFu;
    const uint32_t g = (word >> 8) & 0xFFu;
    const uint32_t b = (word >> 16) & 0xFFu;
    return std::max(r, std::max(g, b));
  };

  for (uint32_t i = 0; i < word_count; ++i) {
    const uint32_t word = read_word(i);
    const uint32_t color = word & 0x00FFFFFFu;
    min_word = std::min(min_word, color);
    max_word = std::max(max_word, color);
    if (color) {
      ++color_nonzero;
      if (first_nonzero_word_index == UINT32_MAX) {
        first_nonzero_word_index = i;
        first_nonzero_word = word;
      }
    }
    const uint32_t brightness = max_rgb8(word);
    rgb_gt8 += brightness > 8;
    rgb_gt32 += brightness > 32;
    rgb_gt64 += brightness > 64;
  }

  uint32_t width = 0;
  uint32_t height = 0;
  Nx1ResolveReadbackDimensions(written_length, width, height);
  auto sample_xy = [&](uint32_t x, uint32_t y) -> uint32_t {
    if (!width || !height || x >= width || y >= height) {
      return 0;
    }
    return read_word(y * width + x);
  };
  const uint32_t center = (width && height) ? sample_xy(width / 2, height / 2)
                                           : read_word(word_count / 2);
  const uint32_t q1 = (width && height) ? sample_xy(width / 4, height / 4)
                                       : read_word(word_count / 4);
  const uint32_t q3 = (width && height) ? sample_xy((width * 3) / 4, (height * 3) / 4)
                                       : read_word((word_count * 3) / 4);

  REXGPU_WARN(
      "[NX1 resolve-readback] {} #{} addr=0x{:08X}+0x{:X} inferred={}x{} "
      "words={} color_nonzero={} rgb_gt8={} rgb_gt32={} rgb_gt64={} min=0x{:08X} "
      "max=0x{:08X} center=0x{:08X} q1=0x{:08X} q3=0x{:08X} first_word={} "
      "first=0x{:08X}",
      Nx1TrackedResolveLabel(tracked_index), log_count, written_address, written_length, width,
      height, word_count, color_nonzero, rgb_gt8, rgb_gt32, rgb_gt64,
      min_word == UINT32_MAX ? 0 : min_word, max_word, center, q1, q3,
      first_nonzero_word_index == UINT32_MAX ? 0 : first_nonzero_word_index,
      first_nonzero_word);
}

void Nx1WriteTextureFetchConstant(RegisterFile& regs, uint32_t index,
                                  const xenos::xe_gpu_texture_fetch_t& fetch) {
  assert_true(index < 32);
  std::memcpy(
      &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                   (sizeof(fetch) / sizeof(uint32_t)) * index],
      &fetch, sizeof(fetch));
}

struct Nx1NativeSharedConstants {
  uint32_t texture_1d_descriptor_indices[16];
  uint32_t texture_2d_descriptor_indices[16];
  uint32_t texture_3d_descriptor_indices[16];
  uint32_t texture_cube_descriptor_indices[16];
  uint32_t sampler_descriptor_indices[16];
  uint32_t booleans;
  uint32_t swapped_texcoords;
  float half_pixel_offset[2];
  float alpha_threshold;
  uint32_t spec_constants;
  uint32_t alpha_test_function;
  uint32_t vertex_index_endian;
  uint32_t integer_constants[32][4];
  // Guest viewport NDC transform applied by the native vertex shader
  // (position.xyz = position.xyz * ndc_scale + ndc_offset * position.w), at
  // c54/c55. Each is a float3 occupying a full 16-byte register.
  float ndc_scale[3];
  uint32_t pad_c54;
  float ndc_offset[3];
  uint32_t pad_c55;
};
constexpr uint32_t kNx1NativeSpecConstantR11G11B10Normal = 1u << 0;
constexpr uint32_t kNx1NativeSpecConstantAlphaTest = 1u << 1;
constexpr uint32_t kNx1NativeSupportedSpecConstants =
    kNx1NativeSpecConstantR11G11B10Normal | kNx1NativeSpecConstantAlphaTest;
static_assert(offsetof(Nx1NativeSharedConstants, booleans) == 320);
static_assert(offsetof(Nx1NativeSharedConstants, swapped_texcoords) == 324);
static_assert(offsetof(Nx1NativeSharedConstants, half_pixel_offset) == 328);
static_assert(offsetof(Nx1NativeSharedConstants, alpha_threshold) == 336);
static_assert(offsetof(Nx1NativeSharedConstants, spec_constants) == 340);
static_assert(offsetof(Nx1NativeSharedConstants, alpha_test_function) == 344);
static_assert(offsetof(Nx1NativeSharedConstants, vertex_index_endian) == 348);
static_assert(offsetof(Nx1NativeSharedConstants, integer_constants) == 352);
static_assert(offsetof(Nx1NativeSharedConstants, ndc_scale) == 864);
static_assert(offsetof(Nx1NativeSharedConstants, ndc_offset) == 880);
static_assert(sizeof(Nx1NativeSharedConstants) == 896);

}  // namespace

D3D12CommandProcessor::D3D12CommandProcessor(D3D12GraphicsSystem* graphics_system,
                                             system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state), deferred_command_list_(*this) {
  legacy_readback_memexport_cvar_name_ = "d3d12_readback_memexport";
}
D3D12CommandProcessor::~D3D12CommandProcessor() = default;

void D3D12CommandProcessor::UpdateDebugMarkersEnabled() {
  debug_markers_enabled_ = IsGpuDebugMarkersEnabled();
}

void D3D12CommandProcessor::PushDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.BeginDebugMarker(label);
}

void D3D12CommandProcessor::PopDebugMarker() {
  if (!debug_markers_enabled_) {
    return;
  }
  deferred_command_list_.EndDebugMarker();
}

void D3D12CommandProcessor::InsertDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.InsertDebugMarker(label);
}

void D3D12CommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  InvalidateAllVertexBufferResidency();
  cache_clear_requested_ = true;
}

void D3D12CommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void D3D12CommandProcessor::InvalidateAllVertexBufferResidency() {
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  for (VertexBufferState& state : vertex_buffer_states_) {
    state.address = UINT32_MAX;
    state.size = UINT32_MAX;
  }
}

void D3D12CommandProcessor::InvalidateVertexBufferResidency(uint32_t vfetch_index) {
  if (vfetch_index >= vertex_buffer_states_.size()) {
    return;
  }
  vertex_buffers_in_sync_[vfetch_index >> 6] &= ~(uint64_t(1) << (vfetch_index & 63));
}

void D3D12CommandProcessor::InvalidateVertexBufferResidencyRange(uint32_t first_vfetch,
                                                                 uint32_t last_vfetch) {
  if (first_vfetch > last_vfetch) {
    std::swap(first_vfetch, last_vfetch);
  }
  if (first_vfetch >= vertex_buffer_states_.size()) {
    return;
  }
  last_vfetch = std::min(last_vfetch, uint32_t(vertex_buffer_states_.size() - 1));
  for (uint32_t vfetch_index = first_vfetch; vfetch_index <= last_vfetch; ++vfetch_index) {
    InvalidateVertexBufferResidency(vfetch_index);
  }
}

void D3D12CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                    uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
}

void D3D12CommandProcessor::RequestFrameTrace(const std::filesystem::path& root_path) {
  // Capture with PIX if attached.
  if (GetD3D12Provider().GetGraphicsAnalysis() != nullptr) {
    pix_capture_requested_.store(true, std::memory_order_relaxed);
    return;
  }
  CommandProcessor::RequestFrameTrace(root_path);
}

void D3D12CommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) {
  shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
}

void D3D12CommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  // Starting a new frame because descriptors may be needed.
  if (!BeginSubmission(true)) {
    return;
  }
  render_target_cache_->RestoreEdramSnapshot(snapshot);
}

bool D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    static uint64_t nx1_occlusion_fallback_count = 0;
    ++nx1_occlusion_fallback_count;
    if (Nx1ShouldLogOcclusionProbe(nx1_occlusion_fallback_count)) {
      REXGPU_WARN("[NX1 occlusion-probe] fallback #{}: resources unavailable enabled={} available={}",
                  nx1_occlusion_fallback_count, REXCVAR_GET(occlusion_query_enable),
                  occlusion_query_resources_available_);
    }
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }

  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  uint32_t sample_count_addr = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  auto* sample_counts =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_addr);
  if (!sample_counts) {
    static uint64_t nx1_occlusion_bad_ptr_count = 0;
    ++nx1_occlusion_bad_ptr_count;
    if (Nx1ShouldLogOcclusionProbe(nx1_occlusion_bad_ptr_count)) {
      REXGPU_WARN("[NX1 occlusion-probe] disable #{}: invalid sample_count_addr=0x{:08X}",
                  nx1_occlusion_bad_ptr_count, sample_count_addr);
    }
    DisableHostOcclusionQueries();
    return true;
  }

  auto write_fallback_result = [this, sample_counts]() -> bool {
    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
    if (fake_sample_count < 0) {
      return true;
    }
    bool is_end_via_z_pass =
        sample_counts->ZPass_A == kQueryFinished && sample_counts->ZPass_B == kQueryFinished;
    bool is_end_via_z_fail =
        sample_counts->ZFail_A == kQueryFinished && sample_counts->ZFail_B == kQueryFinished;
    if (is_end_via_z_pass || is_end_via_z_fail) {
      occlusion_sample_accumulator_ += uint32_t(fake_sample_count);
    }
    WriteGuestOcclusionResult(sample_counts, occlusion_sample_accumulator_);
    return true;
  };

  bool is_end_via_z_pass =
      sample_counts->ZPass_A == kQueryFinished && sample_counts->ZPass_B == kQueryFinished;
  bool is_end_via_z_fail =
      sample_counts->ZFail_A == kQueryFinished && sample_counts->ZFail_B == kQueryFinished;
  bool is_end = is_end_via_z_pass || is_end_via_z_fail;
  static uint64_t nx1_occlusion_event_count = 0;
  ++nx1_occlusion_event_count;
  if (Nx1ShouldLogOcclusionProbe(nx1_occlusion_event_count)) {
    REXGPU_WARN(
        "[NX1 occlusion-probe] event #{} initiator={} sample_addr=0x{:08X} is_end={} "
        "end_zpass={} end_zfail={} active={} active_addr=0x{:08X} values={:08X} {:08X} "
        "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
        nx1_occlusion_event_count, initiator & 0x3F, sample_count_addr, is_end,
        is_end_via_z_pass, is_end_via_z_fail, active_occlusion_query_.valid,
        active_occlusion_query_.sample_count_address, uint32_t(sample_counts->Total_A),
        uint32_t(sample_counts->Total_B), uint32_t(sample_counts->ZFail_A),
        uint32_t(sample_counts->ZFail_B), uint32_t(sample_counts->ZPass_A),
        uint32_t(sample_counts->ZPass_B), uint32_t(sample_counts->StencilFail_A),
        uint32_t(sample_counts->StencilFail_B));
  }

  if (!is_end) {
    WriteGuestOcclusionResult(sample_counts, occlusion_sample_accumulator_);
    if (active_occlusion_query_.valid &&
        active_occlusion_query_.sample_count_address != sample_count_addr) {
      static uint64_t nx1_occlusion_mismatch_begin_count = 0;
      ++nx1_occlusion_mismatch_begin_count;
      if (Nx1ShouldLogOcclusionProbe(nx1_occlusion_mismatch_begin_count)) {
        REXGPU_WARN(
            "[NX1 occlusion-probe] disable #{}: begin address mismatch active=0x{:08X} "
            "new=0x{:08X}",
            nx1_occlusion_mismatch_begin_count, active_occlusion_query_.sample_count_address,
            sample_count_addr);
      }
      DisableHostOcclusionQueries();
      return write_fallback_result();
    }
    if (!BeginGuestOcclusionQuery(sample_count_addr)) {
      static uint64_t nx1_occlusion_begin_failed_count = 0;
      ++nx1_occlusion_begin_failed_count;
      if (Nx1ShouldLogOcclusionProbe(nx1_occlusion_begin_failed_count)) {
        REXGPU_WARN("[NX1 occlusion-probe] fallback #{}: begin failed sample_addr=0x{:08X}",
                    nx1_occlusion_begin_failed_count, sample_count_addr);
      }
      return write_fallback_result();
    }
    return true;
  }

  if (!active_occlusion_query_.valid) {
    static uint64_t nx1_occlusion_end_miss_count = 0;
    ++nx1_occlusion_end_miss_count;
    if (Nx1ShouldLogOcclusionProbe(nx1_occlusion_end_miss_count)) {
      REXGPU_WARN(
          "[NX1 occlusion-probe] fallback #{}: end without active query sample_addr=0x{:08X}",
          nx1_occlusion_end_miss_count, sample_count_addr);
    }
    return write_fallback_result();
  }

  if (active_occlusion_query_.sample_count_address != sample_count_addr) {
    static uint64_t nx1_occlusion_split_addr_count = 0;
    ++nx1_occlusion_split_addr_count;
    if (Nx1ShouldLogOcclusionProbe(nx1_occlusion_split_addr_count)) {
      REXGPU_WARN("[NX1 occlusion-probe] split query #{}: begin_addr=0x{:08X} end_addr=0x{:08X}",
                  nx1_occlusion_split_addr_count, active_occlusion_query_.sample_count_address,
                  sample_count_addr);
    }
  }

  if (!EndGuestOcclusionQuery(sample_count_addr, sample_counts)) {
    static uint64_t nx1_occlusion_end_failed_count = 0;
    ++nx1_occlusion_end_failed_count;
    if (Nx1ShouldLogOcclusionProbe(nx1_occlusion_end_failed_count)) {
      REXGPU_WARN("[NX1 occlusion-probe] fallback #{}: end failed sample_addr=0x{:08X}",
                  nx1_occlusion_end_failed_count, sample_count_addr);
    }
    return write_fallback_result();
  }

  return true;
}

bool D3D12CommandProcessor::PushTransitionBarrier(ID3D12Resource* resource,
                                                  D3D12_RESOURCE_STATES old_state,
                                                  D3D12_RESOURCE_STATES new_state,
                                                  UINT subresource) {
  if (old_state == new_state) {
    return false;
  }
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = subresource;
  barrier.Transition.StateBefore = old_state;
  barrier.Transition.StateAfter = new_state;
  barriers_.push_back(barrier);
  return true;
}

void D3D12CommandProcessor::PushAliasingBarrier(ID3D12Resource* old_resource,
                                                ID3D12Resource* new_resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Aliasing.pResourceBefore = old_resource;
  barrier.Aliasing.pResourceAfter = new_resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::PushUAVBarrier(ID3D12Resource* resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.UAV.pResource = resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::SubmitBarriers() {
  UINT barrier_count = UINT(barriers_.size());
  if (barrier_count != 0) {
    deferred_command_list_.D3DResourceBarrier(barrier_count, barriers_.data());
    barriers_.clear();
  }
}

ID3D12RootSignature* D3D12CommandProcessor::GetRootSignature(const DxbcShader* vertex_shader,
                                                             const DxbcShader* pixel_shader,
                                                             bool tessellated) {
  if (bindless_resources_used_) {
    return tessellated ? root_signature_bindless_ds_ : root_signature_bindless_vs_;
  }

  D3D12_SHADER_VISIBILITY vertex_visibility =
      tessellated ? D3D12_SHADER_VISIBILITY_DOMAIN : D3D12_SHADER_VISIBILITY_VERTEX;

  uint32_t texture_count_vertex =
      uint32_t(vertex_shader->GetTextureBindingsAfterTranslation().size());
  uint32_t sampler_count_vertex =
      uint32_t(vertex_shader->GetSamplerBindingsAfterTranslation().size());
  uint32_t texture_count_pixel =
      pixel_shader ? uint32_t(pixel_shader->GetTextureBindingsAfterTranslation().size()) : 0;
  uint32_t sampler_count_pixel =
      pixel_shader ? uint32_t(pixel_shader->GetSamplerBindingsAfterTranslation().size()) : 0;

  // Better put the pixel texture/sampler in the lower bits probably because it
  // changes often.
  uint32_t index = 0;
  uint32_t index_offset = 0;
  index |= texture_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= texture_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= uint32_t(vertex_visibility == D3D12_SHADER_VISIBILITY_DOMAIN) << index_offset;
  ++index_offset;
  assert_true(index_offset <= 32);

  // Try an existing root signature.
  auto it = root_signatures_bindful_.find(index);
  if (it != root_signatures_bindful_.end()) {
    return it->second;
  }

  // Create a new one.
  D3D12_ROOT_SIGNATURE_DESC desc;
  D3D12_ROOT_PARAMETER parameters[kRootParameter_Bindful_Count_Max];
  desc.NumParameters = kRootParameter_Bindful_Count_Base;
  desc.pParameters = parameters;
  desc.NumStaticSamplers = 0;
  desc.pStaticSamplers = nullptr;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  // Base parameters.

  // Fetch constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FetchConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Vertex float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsVertex];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = vertex_visibility;
  }

  // Pixel float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsPixel];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }

  // System constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_SystemConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Bool and loop constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_BoolLoopConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Shared memory and, if ROVs are used, EDRAM.
  D3D12_DESCRIPTOR_RANGE shared_memory_and_edram_ranges[3];
  {
    auto& parameter = parameters[kRootParameter_Bindful_SharedMemoryAndEdram];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 2;
    parameter.DescriptorTable.pDescriptorRanges = shared_memory_and_edram_ranges;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    shared_memory_and_edram_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shared_memory_and_edram_ranges[0].NumDescriptors = 1;
    shared_memory_and_edram_ranges[0].BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
    shared_memory_and_edram_ranges[0].RegisterSpace =
        uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    shared_memory_and_edram_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    shared_memory_and_edram_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    shared_memory_and_edram_ranges[1].NumDescriptors = 1;
    shared_memory_and_edram_ranges[1].BaseShaderRegister =
        UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
    shared_memory_and_edram_ranges[1].RegisterSpace = 0;
    shared_memory_and_edram_ranges[1].OffsetInDescriptorsFromTableStart = 1;
    if (render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock) {
      ++parameter.DescriptorTable.NumDescriptorRanges;
      shared_memory_and_edram_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      shared_memory_and_edram_ranges[2].NumDescriptors = 1;
      shared_memory_and_edram_ranges[2].BaseShaderRegister =
          UINT(DxbcShaderTranslator::UAVRegister::kEdram);
      shared_memory_and_edram_ranges[2].RegisterSpace = 0;
      shared_memory_and_edram_ranges[2].OffsetInDescriptorsFromTableStart = 2;
    }
  }

  // Extra parameters.

  // Pixel textures.
  D3D12_DESCRIPTOR_RANGE range_textures_pixel;
  if (texture_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_textures_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_pixel.NumDescriptors = texture_count_pixel;
    range_textures_pixel.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_pixel.RegisterSpace = uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Pixel samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_pixel;
  if (sampler_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_samplers_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_pixel.NumDescriptors = sampler_count_pixel;
    range_samplers_pixel.BaseShaderRegister = 0;
    range_samplers_pixel.RegisterSpace = 0;
    range_samplers_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex textures.
  D3D12_DESCRIPTOR_RANGE range_textures_vertex;
  if (texture_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_textures_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_vertex.NumDescriptors = texture_count_vertex;
    range_textures_vertex.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_vertex.RegisterSpace = uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_vertex;
  if (sampler_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_samplers_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_vertex.NumDescriptors = sampler_count_vertex;
    range_samplers_vertex.BaseShaderRegister = 0;
    range_samplers_vertex.RegisterSpace = 0;
    range_samplers_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  ID3D12RootSignature* root_signature =
      ui::d3d12::util::CreateRootSignature(GetD3D12Provider(), desc);
  if (root_signature == nullptr) {
    REXGPU_ERROR(
        "Failed to create a root signature with {} pixel textures, {} pixel "
        "samplers, {} vertex textures and {} vertex samplers",
        texture_count_pixel, sampler_count_pixel, texture_count_vertex, sampler_count_vertex);
    return nullptr;
  }
  root_signatures_bindful_.emplace(index, root_signature);
  return root_signature;
}

uint32_t D3D12CommandProcessor::GetRootBindfulExtraParameterIndices(
    const DxbcShader* vertex_shader, const DxbcShader* pixel_shader,
    RootBindfulExtraParameterIndices& indices_out) {
  uint32_t index = kRootParameter_Bindful_Count_Base;
  if (pixel_shader && !pixel_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_pixel = index++;
  } else {
    indices_out.textures_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (pixel_shader && !pixel_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_pixel = index++;
  } else {
    indices_out.samplers_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_vertex = index++;
  } else {
    indices_out.textures_vertex = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_vertex = index++;
  } else {
    indices_out.samplers_vertex = RootBindfulExtraParameterIndices::kUnavailable;
  }
  return index;
}

uint64_t D3D12CommandProcessor::RequestViewBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update, uint32_t count_for_full_update,
    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out, D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = view_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update, count_for_full_update,
      descriptor_index);
  if (current_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = view_bindful_heap_pool_->GetLastRequestHeap();
  if (view_bindful_heap_current_ != heap) {
    view_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

uint32_t D3D12CommandProcessor::RequestPersistentViewBindlessDescriptor() {
  assert_true(bindless_resources_used_);
  if (!view_bindless_heap_free_.empty()) {
    uint32_t descriptor_index = view_bindless_heap_free_.back();
    view_bindless_heap_free_.pop_back();
    return descriptor_index;
  }
  if (view_bindless_heap_allocated_ >= kViewBindlessHeapSize) {
    return UINT32_MAX;
  }
  return view_bindless_heap_allocated_++;
}

void D3D12CommandProcessor::ReleaseViewBindlessDescriptorImmediately(uint32_t descriptor_index) {
  assert_true(bindless_resources_used_);
  view_bindless_heap_free_.push_back(descriptor_index);
}

bool D3D12CommandProcessor::RequestOneUseSingleViewDescriptors(
    uint32_t count, ui::d3d12::util::DescriptorCpuGpuHandlePair* handles_out) {
  assert_true(submission_open_);
  if (!count) {
    return true;
  }
  assert_not_null(handles_out);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  if (bindless_resources_used_) {
    // Request separate bindless descriptors that will be freed when this
    // submission is completed by the GPU.
    if (count >
        kViewBindlessHeapSize - view_bindless_heap_allocated_ + view_bindless_heap_free_.size()) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t descriptor_index;
      if (!view_bindless_heap_free_.empty()) {
        descriptor_index = view_bindless_heap_free_.back();
        view_bindless_heap_free_.pop_back();
      } else {
        descriptor_index = view_bindless_heap_allocated_++;
      }
      view_bindless_one_use_descriptors_.push_back(
          std::make_pair(descriptor_index, submission_current_));
      handles_out[i] = std::make_pair(
          provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_, descriptor_index),
          provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, descriptor_index));
    }
  } else {
    // Request a range within the current heap for bindful resources path.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle_start;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_start;
    if (RequestViewBindfulDescriptors(ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid, count,
                                      count, cpu_handle_start, gpu_handle_start) ==
        ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      handles_out[i] = std::make_pair(provider.OffsetViewDescriptor(cpu_handle_start, i),
                                      provider.OffsetViewDescriptor(gpu_handle_start, i));
    }
  }
  return true;
}

ui::d3d12::util::DescriptorCpuGpuHandlePair D3D12CommandProcessor::GetSystemBindlessViewHandlePair(
    SystemBindlessView view) const {
  assert_true(bindless_resources_used_);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  return std::make_pair(
      provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_, uint32_t(view)),
      provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, uint32_t(view)));
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

uint64_t D3D12CommandProcessor::RequestSamplerBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update, uint32_t count_for_full_update,
    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out, D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = sampler_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update, count_for_full_update,
      descriptor_index);
  if (current_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = sampler_bindful_heap_pool_->GetLastRequestHeap();
  if (sampler_bindful_heap_current_ != heap) {
    sampler_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

ID3D12Resource* D3D12CommandProcessor::RequestScratchGPUBuffer(uint32_t size,
                                                               D3D12_RESOURCE_STATES state) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || size == 0) {
    return nullptr;
  }

  if (size <= scratch_buffer_size_) {
    PushTransitionBarrier(scratch_buffer_, scratch_buffer_state_, state);
    scratch_buffer_state_ = state;
    scratch_buffer_used_ = true;
    return scratch_buffer_;
  }

  size = rex::align(size, kScratchBufferSizeIncrement);

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ID3D12Resource* buffer;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                                             state, nullptr, IID_PPV_ARGS(&buffer)))) {
    REXGPU_ERROR("Failed to create a {} MB scratch GPU buffer", size >> 20);
    return nullptr;
  }
  if (scratch_buffer_ != nullptr) {
    resources_for_deletion_.emplace_back(submission_current_, scratch_buffer_);
  }
  scratch_buffer_ = buffer;
  scratch_buffer_size_ = size;
  scratch_buffer_state_ = state;
  scratch_buffer_used_ = true;
  return scratch_buffer_;
}

void D3D12CommandProcessor::ReleaseScratchGPUBuffer(ID3D12Resource* buffer,
                                                    D3D12_RESOURCE_STATES new_state) {
  assert_true(submission_open_);
  assert_true(scratch_buffer_used_);
  scratch_buffer_used_ = false;
  if (buffer == scratch_buffer_) {
    scratch_buffer_state_ = new_state;
  }
}

void D3D12CommandProcessor::SetExternalPipeline(ID3D12PipelineState* pipeline) {
  if (current_external_pipeline_ != pipeline) {
    current_external_pipeline_ = pipeline;
    current_guest_pipeline_ = nullptr;
    deferred_command_list_.D3DSetPipelineState(pipeline);
  }
}

void D3D12CommandProcessor::SetExternalGraphicsRootSignature(ID3D12RootSignature* root_signature) {
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }
  // Force-invalidate because setting a non-guest root signature.
  current_graphics_root_up_to_date_ = 0;
}

void D3D12CommandProcessor::SetViewport(const D3D12_VIEWPORT& viewport) {
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftX != viewport.TopLeftX;
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftY != viewport.TopLeftY;
  ff_viewport_update_needed_ |= ff_viewport_.Width != viewport.Width;
  ff_viewport_update_needed_ |= ff_viewport_.Height != viewport.Height;
  ff_viewport_update_needed_ |= ff_viewport_.MinDepth != viewport.MinDepth;
  ff_viewport_update_needed_ |= ff_viewport_.MaxDepth != viewport.MaxDepth;
  if (ff_viewport_update_needed_) {
    ff_viewport_ = viewport;
    deferred_command_list_.RSSetViewport(ff_viewport_);
    ff_viewport_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetScissorRect(const D3D12_RECT& scissor_rect) {
  ff_scissor_update_needed_ |= ff_scissor_.left != scissor_rect.left;
  ff_scissor_update_needed_ |= ff_scissor_.top != scissor_rect.top;
  ff_scissor_update_needed_ |= ff_scissor_.right != scissor_rect.right;
  ff_scissor_update_needed_ |= ff_scissor_.bottom != scissor_rect.bottom;
  if (ff_scissor_update_needed_) {
    ff_scissor_ = scissor_rect;
    deferred_command_list_.RSSetScissorRect(ff_scissor_);
    ff_scissor_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetStencilReference(uint32_t stencil_ref) {
  ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
  if (ff_stencil_ref_update_needed_) {
    ff_stencil_ref_ = stencil_ref;
    deferred_command_list_.D3DOMSetStencilRef(stencil_ref);
    ff_stencil_ref_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology) {
  if (primitive_topology_ != primitive_topology) {
    primitive_topology_ = primitive_topology;
    deferred_command_list_.D3DIASetPrimitiveTopology(primitive_topology);
  }
}

std::string D3D12CommandProcessor::GetWindowTitleText() const {
  std::ostringstream title;
  title << "Direct3D 12";
  if (render_target_cache_) {
    // Rasterizer-ordered views are a feature very rarely used as of 2020 and
    // that faces adoption complications (outside of Direct3D - on Vulkan - at
    // least), but crucial to Xenia - raise awareness of its usage.
    // https://github.com/KhronosGroup/Vulkan-Ecosystem/issues/27#issuecomment-455712319
    // "In Xenia's title bar "D3D12 ROV" can be seen, which was a surprise, as I
    //  wasn't aware that Xenia D3D12 backend was using Raster Order Views
    //  feature" - oscarbg in that issue.
    switch (render_target_cache_->GetPath()) {
      case RenderTargetCache::Path::kHostRenderTargets:
        title << " - RTV/DSV";
        break;
      case RenderTargetCache::Path::kPixelShaderInterlock:
        title << " - ROV";
        break;
      default:
        break;
    }
    uint32_t draw_resolution_scale_x =
        texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
    uint32_t draw_resolution_scale_y =
        texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
    if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
      title << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
    }
  }
  return title.str();
}

bool D3D12CommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    REXGPU_ERROR("Failed to initialize base command processor context");
    return false;
  }
  InvalidateAllVertexBufferResidency();
  UpdateDebugMarkersEnabled();

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  fence_completion_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (fence_completion_event_ == nullptr) {
    REXGPU_ERROR("Failed to create the fence completion event");
    return false;
  }
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&submission_fence_)))) {
    REXGPU_ERROR("Failed to create the submission fence");
    return false;
  }
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&queue_operations_since_submission_fence_)))) {
    REXGPU_ERROR(
        "Failed to create the fence for awaiting queue operations done since "
        "the latest submission");
    return false;
  }

  // Create the command list and one allocator because it's needed for a command
  // list.
  ID3D12CommandAllocator* command_allocator;
  if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&command_allocator)))) {
    REXGPU_ERROR("Failed to create a command allocator");
    return false;
  }
  command_allocator_writable_first_ = new CommandAllocator;
  command_allocator_writable_first_->command_allocator = command_allocator;
  command_allocator_writable_first_->last_usage_submission = 0;
  command_allocator_writable_first_->next = nullptr;
  command_allocator_writable_last_ = command_allocator_writable_first_;
  if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator,
                                       nullptr, IID_PPV_ARGS(&command_list_)))) {
    REXGPU_ERROR("Failed to create the graphics command list");
    return false;
  }
  // Initially in open state, wait until a deferred command list submission.
  command_list_->Close();
  // Optional - added in Creators Update (SDK 10.0.15063.0).
  command_list_->QueryInterface(IID_PPV_ARGS(&command_list_1_));

  bindless_resources_used_ = REXCVAR_GET(d3d12_bindless) &&
                             provider.GetResourceBindingTier() >= D3D12_RESOURCE_BINDING_TIER_2;

  // Get the draw resolution scale for the render target cache and the texture
  // cache.
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x, draw_resolution_scale_y);
  if (!D3D12TextureCache::ClampDrawResolutionScaleToMaxSupported(
          draw_resolution_scale_x, draw_resolution_scale_y, provider)) {
    draw_resolution_scale_not_clamped = false;
  }
  if (!draw_resolution_scale_not_clamped) {
    REXGPU_WARN(
        "The requested draw resolution scale is not supported by the device or "
        "the emulator, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }

  shared_memory_ = std::make_unique<D3D12SharedMemory>(*this, *memory_, trace_writer_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("Failed to initialize shared memory");
    return false;
  }

  // Initialize the render target cache before configuring binding - need to
  // know if using rasterizer-ordered views for the bindless root signature.
  render_target_cache_ = std::make_unique<D3D12RenderTargetCache>(
      *register_file_, *memory_, trace_writer_, draw_resolution_scale_x, draw_resolution_scale_y,
      *this, bindless_resources_used_);
  if (!render_target_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the render target cache");
    return false;
  }

  // Initialize resource binding.
  constant_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider, std::max(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                         sizeof(float) * 4 * D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT));
  if (bindless_resources_used_) {
    D3D12_DESCRIPTOR_HEAP_DESC view_bindless_heap_desc;
    view_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    view_bindless_heap_desc.NumDescriptors = kViewBindlessHeapSize;
    view_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    view_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&view_bindless_heap_desc,
                                            IID_PPV_ARGS(&view_bindless_heap_)))) {
      REXGPU_ERROR("Failed to create the bindless CBV/SRV/UAV descriptor heap");
      return false;
    }
    view_bindless_heap_cpu_start_ = view_bindless_heap_->GetCPUDescriptorHandleForHeapStart();
    view_bindless_heap_gpu_start_ = view_bindless_heap_->GetGPUDescriptorHandleForHeapStart();
    view_bindless_heap_allocated_ = uint32_t(SystemBindlessView::kCount);

    D3D12_DESCRIPTOR_HEAP_DESC sampler_bindless_heap_desc;
    sampler_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_bindless_heap_desc.NumDescriptors = kSamplerHeapSize;
    sampler_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    sampler_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&sampler_bindless_heap_desc,
                                            IID_PPV_ARGS(&sampler_bindless_heap_current_)))) {
      REXGPU_ERROR("Failed to create the bindless sampler descriptor heap");
      return false;
    }
    sampler_bindless_heap_cpu_start_ =
        sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_gpu_start_ =
        sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_allocated_ = 0;
  } else {
    view_bindful_heap_pool_ = std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewBindfulHeapSize);
    sampler_bindful_heap_pool_ = std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize);
  }

  if (bindless_resources_used_) {
    // Global bindless resource root signatures.
    // No CBV or UAV descriptor ranges with any descriptors to be allocated
    // dynamically (via RequestPersistentViewBindlessDescriptor or
    // RequestOneUseSingleViewDescriptors) should be here, because they would
    // overlap the unbounded SRV range, which is not allowed on Nvidia Fermi!
    D3D12_ROOT_SIGNATURE_DESC root_signature_bindless_desc;
    D3D12_ROOT_PARAMETER
    root_parameters_bindless[kRootParameter_Bindless_Count];
    root_signature_bindless_desc.NumParameters = kRootParameter_Bindless_Count;
    root_signature_bindless_desc.pParameters = root_parameters_bindless;
    root_signature_bindless_desc.NumStaticSamplers = 0;
    root_signature_bindless_desc.pStaticSamplers = nullptr;
    root_signature_bindless_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    // Fetch constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FetchConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Vertex float constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FloatConstantsVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // Pixel float constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FloatConstantsPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Pixel shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Vertex shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // System constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SystemConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Bool and loop constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_BoolLoopConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Shared memory SRV and UAV.
    D3D12_DESCRIPTOR_RANGE root_shared_memory_view_ranges[2];
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SharedMemory];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable.NumDescriptorRanges =
          uint32_t(rex::countof(root_shared_memory_view_ranges));
      parameter.DescriptorTable.pDescriptorRanges = root_shared_memory_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      {
        auto& range = root_shared_memory_view_ranges[0];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kMain);
        range.OffsetInDescriptorsFromTableStart = 0;
      }
      {
        auto& range = root_shared_memory_view_ranges[1];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 1;
      }
    }
    // Sampler heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_sampler_range;
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SamplerHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 1;
      parameter.DescriptorTable.pDescriptorRanges = &root_bindless_sampler_range;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      root_bindless_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      root_bindless_sampler_range.NumDescriptors = UINT_MAX;
      root_bindless_sampler_range.BaseShaderRegister = 0;
      root_bindless_sampler_range.RegisterSpace = 0;
      root_bindless_sampler_range.OffsetInDescriptorsFromTableStart = 0;
    }
    // View heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_view_ranges[4];
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_ViewHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 0;
      parameter.DescriptorTable.pDescriptorRanges = root_bindless_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // EDRAM.
      if (render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock) {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::UAVRegister::kEdram);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kEdramR32UintUAV);
      }
      // Used UAV and SRV ranges must not overlap on Nvidia Fermi, so textures
      // have OffsetInDescriptorsFromTableStart after all static descriptors of
      // other types.
      // 2D array textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures2DArray);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // 3D textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures3D);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // Cube textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTexturesCube);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
    }
    root_signature_bindless_vs_ =
        ui::d3d12::util::CreateRootSignature(provider, root_signature_bindless_desc);
    if (!root_signature_bindless_vs_) {
      REXGPU_ERROR(
          "Failed to create the global root signature for bindless resources, "
          "the version for use without tessellation");
      return false;
    }
    root_parameters_bindless[kRootParameter_Bindless_FloatConstantsVertex].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_DOMAIN;
    root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesVertex].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_DOMAIN;
    root_signature_bindless_ds_ =
        ui::d3d12::util::CreateRootSignature(provider, root_signature_bindless_desc);
    if (!root_signature_bindless_ds_) {
      REXGPU_ERROR(
          "Failed to create the global root signature for bindless resources, "
          "the version for use with tessellation");
      return false;
    }

    // XenosRecomp native D3D12 shaders use their own register spaces, so keep
    // this isolated from the DXBC root signatures.
    D3D12_ROOT_SIGNATURE_DESC nx1_native_root_signature_desc = {};
    D3D12_ROOT_PARAMETER
        nx1_native_root_parameters[UINT(Nx1NativeRootParameter::kCount)] = {};
    D3D12_DESCRIPTOR_RANGE nx1_native_sampler_range = {};
    D3D12_DESCRIPTOR_RANGE nx1_native_view_ranges[4] = {};
    nx1_native_root_signature_desc.NumParameters =
        UINT(Nx1NativeRootParameter::kCount);
    nx1_native_root_signature_desc.pParameters = nx1_native_root_parameters;
    nx1_native_root_signature_desc.NumStaticSamplers = 0;
    nx1_native_root_signature_desc.pStaticSamplers = nullptr;
    nx1_native_root_signature_desc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    {
      auto& parameter =
          nx1_native_root_parameters[UINT(Nx1NativeRootParameter::kVertexConstants)];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister = 0;
      parameter.Descriptor.RegisterSpace = 4;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    {
      auto& parameter =
          nx1_native_root_parameters[UINT(Nx1NativeRootParameter::kPixelConstants)];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister = 1;
      parameter.Descriptor.RegisterSpace = 4;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    {
      auto& parameter =
          nx1_native_root_parameters[UINT(Nx1NativeRootParameter::kSharedConstants)];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister = 2;
      parameter.Descriptor.RegisterSpace = 4;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    {
      auto& parameter =
          nx1_native_root_parameters[UINT(Nx1NativeRootParameter::kFetchConstants)];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister = 3;
      parameter.Descriptor.RegisterSpace = 4;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    {
      auto& parameter =
          nx1_native_root_parameters[UINT(Nx1NativeRootParameter::kSharedMemory)];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      parameter.Descriptor.ShaderRegister = 0;
      parameter.Descriptor.RegisterSpace = 4;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    {
      auto& parameter =
          nx1_native_root_parameters[UINT(Nx1NativeRootParameter::kSamplerHeap)];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable.NumDescriptorRanges = 1;
      parameter.DescriptorTable.pDescriptorRanges = &nx1_native_sampler_range;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      nx1_native_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      nx1_native_sampler_range.NumDescriptors = UINT_MAX;
      nx1_native_sampler_range.BaseShaderRegister = 0;
      nx1_native_sampler_range.RegisterSpace = 3;
      nx1_native_sampler_range.OffsetInDescriptorsFromTableStart = 0;
    }
    {
      auto& parameter =
          nx1_native_root_parameters[UINT(Nx1NativeRootParameter::kViewHeap)];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable.NumDescriptorRanges = UINT(rex::countof(nx1_native_view_ranges));
      parameter.DescriptorTable.pDescriptorRanges = nx1_native_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      for (uint32_t i = 0; i < rex::countof(nx1_native_view_ranges); ++i) {
        auto& range = nx1_native_view_ranges[i];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = i == 0 ? 3 : i - 1;
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
    }
    root_signature_nx1_native_shader_ =
        ui::d3d12::util::CreateRootSignature(provider, nx1_native_root_signature_desc);
    if (!root_signature_nx1_native_shader_) {
      REXGPU_ERROR("Failed to create the NX1 native shader root signature");
      return false;
    }
  }

  primitive_processor_ = std::make_unique<D3D12PrimitiveProcessor>(
      *register_file_, *memory_, trace_writer_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the geometric primitive processor");
    return false;
  }

  texture_cache_ =
      D3D12TextureCache::Create(*register_file_, *shared_memory_, draw_resolution_scale_x,
                                draw_resolution_scale_y, *this, bindless_resources_used_);
  if (!texture_cache_) {
    REXGPU_ERROR("Failed to initialize the texture cache");
    return false;
  }

  pipeline_cache_ = std::make_unique<PipelineCache>(
      *this, *register_file_, *render_target_cache_.get(), bindless_resources_used_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the graphics pipeline cache");
    return false;
  }

  D3D12_HEAP_FLAGS heap_flag_create_not_zeroed = provider.GetHeapFlagCreateNotZeroed();

  // Create gamma ramp resources.
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;
  D3D12_RESOURCE_DESC gamma_ramp_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(gamma_ramp_buffer_desc, (256 + 128 * 3) * 4,
                                          D3D12_RESOURCE_FLAG_NONE);
  // The first action will be uploading.
  gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             heap_flag_create_not_zeroed, &gamma_ramp_buffer_desc,
                                             gamma_ramp_buffer_state_, nullptr,
                                             IID_PPV_ARGS(&gamma_ramp_buffer_)))) {
    REXGPU_ERROR("Failed to create the gamma ramp buffer");
    return false;
  }
  // The upload buffer is frame-buffered.
  gamma_ramp_buffer_desc.Width *= kQueueFrames;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesUpload,
                                             heap_flag_create_not_zeroed, &gamma_ramp_buffer_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&gamma_ramp_upload_buffer_)))) {
    REXGPU_ERROR("Failed to create the gamma ramp upload buffer");
    return false;
  }
  if (FAILED(gamma_ramp_upload_buffer_->Map(
          0, nullptr, reinterpret_cast<void**>(&gamma_ramp_upload_buffer_mapping_)))) {
    REXGPU_ERROR("Failed to map the gamma ramp upload buffer");
    gamma_ramp_upload_buffer_mapping_ = nullptr;
    return false;
  }

  // Initialize compute pipelines for output with gamma ramp.
  D3D12_ROOT_PARAMETER
  apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_constants =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    apply_gamma_root_parameter_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    apply_gamma_root_parameter_constants.Constants.ShaderRegister = 0;
    apply_gamma_root_parameter_constants.Constants.RegisterSpace = 0;
    apply_gamma_root_parameter_constants.Constants.Num32BitValues =
        sizeof(ApplyGammaConstants) / sizeof(uint32_t);
    apply_gamma_root_parameter_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_dest;
  apply_gamma_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  apply_gamma_root_descriptor_range_dest.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_dest.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_dest.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_dest =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kDestination)];
    apply_gamma_root_parameter_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_dest.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_dest;
    apply_gamma_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_source;
  apply_gamma_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_source.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_source.BaseShaderRegister = 1;
  apply_gamma_root_descriptor_range_source.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_source =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kSource)];
    apply_gamma_root_parameter_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_source;
    apply_gamma_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_ramp;
  apply_gamma_root_descriptor_range_ramp.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_ramp.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_ramp.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_ramp.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_ramp.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_gamma_ramp =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kRamp)];
    apply_gamma_root_parameter_gamma_ramp.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_ramp;
    apply_gamma_root_parameter_gamma_ramp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC apply_gamma_root_signature_desc;
  apply_gamma_root_signature_desc.NumParameters = UINT(ApplyGammaRootParameter::kCount);
  apply_gamma_root_signature_desc.pParameters = apply_gamma_root_parameters;
  apply_gamma_root_signature_desc.NumStaticSamplers = 0;
  apply_gamma_root_signature_desc.pStaticSamplers = nullptr;
  apply_gamma_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(apply_gamma_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, apply_gamma_root_signature_desc);
  if (!apply_gamma_root_signature_) {
    REXGPU_ERROR("Failed to create the gamma ramp application root signature");
    return false;
  }
  *(apply_gamma_table_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::apply_gamma_table_cs, sizeof(shaders::apply_gamma_table_cs),
      apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline");
    return false;
  }
  *(apply_gamma_table_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(device, shaders::apply_gamma_table_fxaa_luma_cs,
                                             sizeof(shaders::apply_gamma_table_fxaa_luma_cs),
                                             apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_fxaa_luma_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline with perceptual luma output");
    return false;
  }
  *(apply_gamma_pwl_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::apply_gamma_pwl_cs, sizeof(shaders::apply_gamma_pwl_cs),
      apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_pipeline_) {
    REXGPU_ERROR("Failed to create the PWL gamma ramp application compute pipeline");
    return false;
  }
  *(apply_gamma_pwl_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(device, shaders::apply_gamma_pwl_fxaa_luma_cs,
                                             sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs),
                                             apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_fxaa_luma_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the PWL gamma ramp application compute pipeline with "
        "perceptual luma output");
    return false;
  }
  D3D12_ROOT_PARAMETER
  native_present_root_parameters[UINT(NativePresentRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& native_present_root_parameter_constants =
        native_present_root_parameters[UINT(NativePresentRootParameter::kConstants)];
    native_present_root_parameter_constants.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    native_present_root_parameter_constants.Constants.ShaderRegister = 0;
    native_present_root_parameter_constants.Constants.RegisterSpace = 0;
    native_present_root_parameter_constants.Constants.Num32BitValues =
        sizeof(NativePresentConstants) / sizeof(uint32_t);
    native_present_root_parameter_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE native_present_root_descriptor_range_dest;
  native_present_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  native_present_root_descriptor_range_dest.NumDescriptors = 1;
  native_present_root_descriptor_range_dest.BaseShaderRegister = 0;
  native_present_root_descriptor_range_dest.RegisterSpace = 0;
  native_present_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& native_present_root_parameter_dest =
        native_present_root_parameters[UINT(NativePresentRootParameter::kDestination)];
    native_present_root_parameter_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    native_present_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    native_present_root_parameter_dest.DescriptorTable.pDescriptorRanges =
        &native_present_root_descriptor_range_dest;
    native_present_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE native_present_root_descriptor_range_source;
  native_present_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  native_present_root_descriptor_range_source.NumDescriptors = 1;
  native_present_root_descriptor_range_source.BaseShaderRegister = 0;
  native_present_root_descriptor_range_source.RegisterSpace = 0;
  native_present_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& native_present_root_parameter_source =
        native_present_root_parameters[UINT(NativePresentRootParameter::kSource)];
    native_present_root_parameter_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    native_present_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    native_present_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &native_present_root_descriptor_range_source;
    native_present_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC native_present_root_signature_desc;
  native_present_root_signature_desc.NumParameters =
      UINT(NativePresentRootParameter::kCount);
  native_present_root_signature_desc.pParameters = native_present_root_parameters;
  native_present_root_signature_desc.NumStaticSamplers = 0;
  native_present_root_signature_desc.pStaticSamplers = nullptr;
  native_present_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(native_present_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, native_present_root_signature_desc);
  if (!native_present_root_signature_) {
    REXGPU_ERROR("Failed to create the NX1 native-present root signature");
    return false;
  }
  native_present_view_heap_pool_ = std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
      device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256);
  *(nx1_native_present_copy_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(device, shaders::nx1_native_present_copy_cs,
                                             sizeof(shaders::nx1_native_present_copy_cs),
                                             native_present_root_signature_.Get());
  if (!nx1_native_present_copy_pipeline_) {
    REXGPU_ERROR("Failed to create the NX1 native-present copy compute pipeline");
    return false;
  }

  // Initialize compute pipelines for post-processing anti-aliasing.
  D3D12_ROOT_PARAMETER fxaa_root_parameters[UINT(FxaaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_constants =
        fxaa_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    fxaa_root_parameter_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    fxaa_root_parameter_constants.Constants.ShaderRegister = 0;
    fxaa_root_parameter_constants.Constants.RegisterSpace = 0;
    fxaa_root_parameter_constants.Constants.Num32BitValues =
        sizeof(FxaaConstants) / sizeof(uint32_t);
    fxaa_root_parameter_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_dest;
  fxaa_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  fxaa_root_descriptor_range_dest.NumDescriptors = 1;
  fxaa_root_descriptor_range_dest.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_dest.RegisterSpace = 0;
  fxaa_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_dest =
        fxaa_root_parameters[UINT(FxaaRootParameter::kDestination)];
    fxaa_root_parameter_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_dest.DescriptorTable.pDescriptorRanges = &fxaa_root_descriptor_range_dest;
    fxaa_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_source;
  fxaa_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  fxaa_root_descriptor_range_source.NumDescriptors = 1;
  fxaa_root_descriptor_range_source.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_source.RegisterSpace = 0;
  fxaa_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_source =
        fxaa_root_parameters[UINT(FxaaRootParameter::kSource)];
    fxaa_root_parameter_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &fxaa_root_descriptor_range_source;
    fxaa_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_STATIC_SAMPLER_DESC fxaa_root_sampler;
  fxaa_root_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  fxaa_root_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.MipLODBias = 0.0f;
  fxaa_root_sampler.MaxAnisotropy = 1;
  fxaa_root_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  fxaa_root_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  fxaa_root_sampler.MinLOD = 0.0f;
  fxaa_root_sampler.MaxLOD = 0.0f;
  fxaa_root_sampler.ShaderRegister = 0;
  fxaa_root_sampler.RegisterSpace = 0;
  fxaa_root_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC fxaa_root_signature_desc;
  fxaa_root_signature_desc.NumParameters = UINT(FxaaRootParameter::kCount);
  fxaa_root_signature_desc.pParameters = fxaa_root_parameters;
  fxaa_root_signature_desc.NumStaticSamplers = 1;
  fxaa_root_signature_desc.pStaticSamplers = &fxaa_root_sampler;
  fxaa_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(fxaa_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, fxaa_root_signature_desc);
  if (!fxaa_root_signature_) {
    REXGPU_ERROR("Failed to create the FXAA root signature");
    return false;
  }
  *(fxaa_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::fxaa_cs, sizeof(shaders::fxaa_cs), fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    REXGPU_ERROR("Failed to create the FXAA compute pipeline");
    return false;
  }
  *(fxaa_extreme_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::fxaa_extreme_cs, sizeof(shaders::fxaa_extreme_cs),
      fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    REXGPU_ERROR("Failed to create the extreme-quality FXAA compute pipeline");
    return false;
  }

  // Resolve downscale compute pipeline for scaled readback resolve.
  D3D12_ROOT_PARAMETER
  resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& constants_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kConstants)];
    constants_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants_parameter.Constants.ShaderRegister = 0;
    constants_parameter.Constants.RegisterSpace = 0;
    constants_parameter.Constants.Num32BitValues =
        sizeof(ResolveDownscaleConstants) / sizeof(uint32_t);
    constants_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE resolve_downscale_source_range;
  resolve_downscale_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  resolve_downscale_source_range.NumDescriptors = 1;
  resolve_downscale_source_range.BaseShaderRegister = 0;
  resolve_downscale_source_range.RegisterSpace = 0;
  resolve_downscale_source_range.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& source_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kSource)];
    source_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    source_parameter.DescriptorTable.NumDescriptorRanges = 1;
    source_parameter.DescriptorTable.pDescriptorRanges = &resolve_downscale_source_range;
    source_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE resolve_downscale_destination_range;
  resolve_downscale_destination_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  resolve_downscale_destination_range.NumDescriptors = 1;
  resolve_downscale_destination_range.BaseShaderRegister = 0;
  resolve_downscale_destination_range.RegisterSpace = 0;
  resolve_downscale_destination_range.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& destination_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kDestination)];
    destination_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    destination_parameter.DescriptorTable.NumDescriptorRanges = 1;
    destination_parameter.DescriptorTable.pDescriptorRanges = &resolve_downscale_destination_range;
    destination_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC resolve_downscale_root_signature_desc;
  resolve_downscale_root_signature_desc.NumParameters = UINT(ResolveDownscaleRootParameter::kCount);
  resolve_downscale_root_signature_desc.pParameters = resolve_downscale_root_parameters;
  resolve_downscale_root_signature_desc.NumStaticSamplers = 0;
  resolve_downscale_root_signature_desc.pStaticSamplers = nullptr;
  resolve_downscale_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(resolve_downscale_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, resolve_downscale_root_signature_desc);
  if (resolve_downscale_root_signature_) {
    *(resolve_downscale_pipeline_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateComputePipeline(device, shaders::resolve_downscale_cs,
                                               sizeof(shaders::resolve_downscale_cs),
                                               resolve_downscale_root_signature_.Get());
  }
  if (!resolve_downscale_root_signature_ || !resolve_downscale_pipeline_) {
    resolve_downscale_pipeline_.Reset();
    resolve_downscale_root_signature_.Reset();
    REXGPU_WARN("Failed to initialize D3D12 resolve-downscale readback pipeline");
  }

  if (bindless_resources_used_) {
    // Create the system bindless descriptors once all resources are
    // initialized.
    // kNullRawSRV.
    ui::d3d12::util::CreateBufferRawSRV(
        device,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullRawSRV)),
        nullptr, 0);
    // kNullRawUAV.
    ui::d3d12::util::CreateBufferRawUAV(
        device,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullRawUAV)),
        nullptr, 0);
    // kNullTexture2DArray.
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc;
    null_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_srv_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    null_srv_desc.Texture2DArray.MostDetailedMip = 0;
    null_srv_desc.Texture2DArray.MipLevels = 1;
    null_srv_desc.Texture2DArray.FirstArraySlice = 0;
    null_srv_desc.Texture2DArray.ArraySize = 1;
    null_srv_desc.Texture2DArray.PlaneSlice = 0;
    null_srv_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTexture2DArray)));
    // kNullTexture3D.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    null_srv_desc.Texture3D.MostDetailedMip = 0;
    null_srv_desc.Texture3D.MipLevels = 1;
    null_srv_desc.Texture3D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTexture3D)));
    // kNullTextureCube.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    null_srv_desc.TextureCube.MostDetailedMip = 0;
    null_srv_desc.TextureCube.MipLevels = 1;
    null_srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTextureCube)));
    // kSharedMemoryRawSRV.
    shared_memory_->WriteRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kSharedMemoryRawSRV)));
    // kSharedMemoryR32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32UintSRV)),
        2);
    // kSharedMemoryR32G32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32G32UintSRV)),
        3);
    // kSharedMemoryR32G32B32A32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV)),
        4);
    // kSharedMemoryRawUAV.
    shared_memory_->WriteRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kSharedMemoryRawUAV)));
    // kSharedMemoryR32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32UintUAV)),
        2);
    // kSharedMemoryR32G32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32G32UintUAV)),
        3);
    // kSharedMemoryR32G32B32A32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV)),
        4);
    // kEdramRawSRV.
    render_target_cache_->WriteEdramRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawSRV)));
    // kEdramR32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32UintSRV)),
        2);
    // kEdramR32G32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32UintSRV)),
        3);
    // kEdramR32G32B32A32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32B32A32UintSRV)),
        4);
    // kEdramRawUAV.
    render_target_cache_->WriteEdramRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawUAV)));
    // kEdramR32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32UintUAV)),
        2);
    // kEdramR32G32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32UintUAV)),
        3);
    // kEdramR32G32B32A32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32B32A32UintUAV)),
        4);
    // kGammaRampTableSRV.
    WriteGammaRampSRV(
        false, provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                             uint32_t(SystemBindlessView::kGammaRampTableSRV)));
    // kGammaRampPWLSRV.
    WriteGammaRampSRV(
        true, provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                            uint32_t(SystemBindlessView::kGammaRampPWLSRV)));
  }

  pix_capture_requested_.store(false, std::memory_order_relaxed);
  pix_capturing_ = false;
  occlusion_query_resources_available_ = InitializeOcclusionQueryResources();

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));

  return true;
}

void D3D12CommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();
  InvalidateAllVertexBufferResidency();
  ShutdownOcclusionQueryResources();

  ui::d3d12::util::ReleaseAndNull(readback_buffer_);
  readback_buffer_size_ = 0;
  for (auto& resolve_readback_pair : readback_buffers_) {
    auto& readback = resolve_readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
          readback.mapped_data[i] = nullptr;
        }
        readback.buffers[i]->Release();
        readback.buffers[i] = nullptr;
      }
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  readback_buffers_.clear();
  for (auto& memexport_readback_pair : memexport_readback_buffers_) {
    auto& readback = memexport_readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
          readback.mapped_data[i] = nullptr;
        }
        readback.buffers[i]->Release();
        readback.buffers[i] = nullptr;
      }
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  memexport_readback_buffers_.clear();

  ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
  scratch_buffer_size_ = 0;
  resolve_downscale_buffer_size_ = 0;
  resolve_downscale_buffer_.Reset();

  for (const std::pair<uint64_t, ID3D12Resource*>& resource_for_deletion :
       resources_for_deletion_) {
    resource_for_deletion.second->Release();
  }
  resources_for_deletion_.clear();

  fxaa_source_texture_submission_ = 0;
  fxaa_source_texture_.Reset();

  fxaa_extreme_pipeline_.Reset();
  fxaa_pipeline_.Reset();
  fxaa_root_signature_.Reset();
  resolve_downscale_pipeline_.Reset();
  resolve_downscale_root_signature_.Reset();

  apply_gamma_pwl_fxaa_luma_pipeline_.Reset();
  apply_gamma_pwl_pipeline_.Reset();
  apply_gamma_table_fxaa_luma_pipeline_.Reset();
  apply_gamma_table_pipeline_.Reset();
  nx1_native_present_copy_pipeline_.Reset();
  native_present_view_heap_pool_.reset();
  native_present_root_signature_.Reset();
  apply_gamma_root_signature_.Reset();

  // Unmapping will be done implicitly by the destruction.
  gamma_ramp_upload_buffer_mapping_ = nullptr;
  gamma_ramp_upload_buffer_.Reset();
  gamma_ramp_buffer_.Reset();

  texture_cache_.reset();

  pipeline_cache_.reset();

  primitive_processor_.reset();

  // Shut down binding - bindless descriptors may be owned by subsystems like
  // the texture cache.

  // Root signatures are used by pipelines, thus freed after the pipelines.
  ui::d3d12::util::ReleaseAndNull(root_signature_nx1_native_shader_);
  ui::d3d12::util::ReleaseAndNull(root_signature_bindless_ds_);
  ui::d3d12::util::ReleaseAndNull(root_signature_bindless_vs_);
  for (auto it : root_signatures_bindful_) {
    it.second->Release();
  }
  root_signatures_bindful_.clear();

  if (bindless_resources_used_) {
    texture_cache_bindless_sampler_map_.clear();
    for (const auto& sampler_bindless_heap_overflowed : sampler_bindless_heaps_overflowed_) {
      sampler_bindless_heap_overflowed.first->Release();
    }
    sampler_bindless_heaps_overflowed_.clear();
    sampler_bindless_heap_allocated_ = 0;
    ui::d3d12::util::ReleaseAndNull(sampler_bindless_heap_current_);
    view_bindless_one_use_descriptors_.clear();
    view_bindless_heap_free_.clear();
    ui::d3d12::util::ReleaseAndNull(view_bindless_heap_);
  } else {
    sampler_bindful_heap_pool_.reset();
    view_bindful_heap_pool_.reset();
  }
  constant_buffer_pool_.reset();

  render_target_cache_.reset();

  shared_memory_.reset();

  deferred_command_list_.Reset();
  ui::d3d12::util::ReleaseAndNull(command_list_1_);
  ui::d3d12::util::ReleaseAndNull(command_list_);
  ClearCommandAllocatorCache();

  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));

  // First release the fences since they may reference fence_completion_event_.

  queue_operations_done_since_submission_signal_ = false;
  queue_operations_since_submission_fence_last_ = 0;
  ui::d3d12::util::ReleaseAndNull(queue_operations_since_submission_fence_);

  ui::d3d12::util::ReleaseAndNull(submission_fence_);
  submission_open_ = false;
  submission_current_ = 1;
  submission_completed_ = 0;

  if (fence_completion_event_) {
    CloseHandle(fence_completion_event_);
    fence_completion_event_ = nullptr;
  }

  device_removed_ = false;

  CommandProcessor::ShutdownContext();
}

void D3D12CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (frame_open_) {
      uint32_t float_constant_index = (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    cbuffer_binding_bool_loop_.up_to_date = false;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    cbuffer_binding_fetch_.up_to_date = false;
    if (texture_cache_ != nullptr) {
      texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                  6);
    }
    InvalidateVertexBufferResidency((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 2);
  }
}

void D3D12CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  uint32_t end_index = start_index + num_registers - 1;

  auto range_has_any_constant_usage = [](const uint64_t* usage_map, uint32_t first_constant,
                                         uint32_t last_constant) -> bool {
    if (first_constant > last_constant) {
      return false;
    }
    uint32_t first_word = first_constant >> 6;
    uint32_t last_word = last_constant >> 6;
    uint32_t first_bit = first_constant & 63;
    uint32_t last_bit = last_constant & 63;
    if (first_word == last_word) {
      uint32_t bit_count = last_bit - first_bit + 1;
      uint64_t mask = bit_count == 64 ? UINT64_MAX : ((UINT64_C(1) << bit_count) - 1) << first_bit;
      return (usage_map[first_word] & mask) != 0;
    }
    if (usage_map[first_word] & (UINT64_MAX << first_bit)) {
      return true;
    }
    for (uint32_t word = first_word + 1; word < last_word; ++word) {
      if (usage_map[word]) {
        return true;
      }
    }
    uint64_t last_mask = last_bit == 63 ? UINT64_MAX : ((UINT64_C(1) << (last_bit + 1)) - 1);
    return (usage_map[last_word] & last_mask) != 0;
  };

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    if (frame_open_) {
      uint32_t first_float_constant = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      uint32_t last_float_constant = (end_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (first_float_constant < 256) {
        uint32_t last_vertex_constant = std::min(last_float_constant, 255u);
        if (range_has_any_constant_usage(current_float_constant_map_vertex_, first_float_constant,
                                         last_vertex_constant)) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
      if (last_float_constant >= 256) {
        uint32_t first_pixel_constant =
            first_float_constant >= 256 ? first_float_constant - 256 : 0;
        uint32_t last_pixel_constant = last_float_constant - 256;
        if (range_has_any_constant_usage(current_float_constant_map_pixel_, first_pixel_constant,
                                         last_pixel_constant)) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_bool_loop_.up_to_date = false;
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_fetch_.up_to_date = false;
    uint32_t first_fetch_dword = start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    uint32_t last_fetch_dword = end_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(first_fetch_dword / 6, last_fetch_dword / 6);
    }
    InvalidateVertexBufferResidencyRange(first_fetch_dword / 2, last_fetch_dword / 2);
    return;
  }

  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void D3D12CommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void D3D12CommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_up_to_date_ = false;
}

void D3D12CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");
  static uint64_t nx1_swap_probe_count = 0;
  ++nx1_swap_probe_count;
  const uint64_t nx1_swap_probe_id = nx1_swap_probe_count;
  Nx1ScopedHitchTimer nx1_issue_swap_timer("D3D12.IssueSwap", frame_current_,
                                           nx1_swap_probe_id);
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;

  if (!graphics_system_)
    return;
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    REXGPU_ERROR("IssueSwap: presenter is null");
    return;
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    REXGPU_ERROR("IssueSwap: BeginSubmission failed");
    return;
  }

  // Obtain the actual swap source texture size (resolution-scaled if it's a
  // resolve destination, or not otherwise).
  D3D12_SHADER_RESOURCE_VIEW_DESC swap_texture_srv_desc;
  xenos::TextureFormat frontbuffer_format;
  uint32_t frontbuffer_width_unscaled = 0, frontbuffer_height_unscaled = 0;
  ID3D12Resource* swap_texture_resource = nullptr;
  {
    Nx1ScopedHitchTimer nx1_request_swap_texture_timer(
        "D3D12.IssueSwap.RequestSwapTexture", frame_current_, nx1_swap_probe_id);
    swap_texture_resource =
        texture_cache_->RequestSwapTexture(swap_texture_srv_desc, frontbuffer_format,
                                           &frontbuffer_width_unscaled,
                                           &frontbuffer_height_unscaled);
  }
  if (!swap_texture_resource) {
    // Dump texture fetch constant 0 for debugging
    const auto& regs = *register_file_;
    auto fetch = regs.GetTextureFetch(0);
    REXGPU_ERROR(
        "IssueSwap: RequestSwapTexture failed - fetch0: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
        fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
    return;
  }
  D3D12_RESOURCE_DESC swap_texture_desc = swap_texture_resource->GetDesc();
  auto& nx1_draw_probe = Nx1DrawProbe();
  Nx1DrawProbeStats::FrameStats nx1_frame_probe = nx1_draw_probe.frame;
  const bool nx1_log_swap_probe = REXCVAR_GET(nx1_gpu_probe_logging) &&
                                  (nx1_swap_probe_id <= 64 ||
                                   (nx1_swap_probe_id % 120) <= 1);
  auto fetch = register_file_->GetTextureFetch(0);
  if (nx1_log_swap_probe) {
    REXGPU_WARN(
        "[NX1 swap-probe] IssueSwap #{} packet_frontbuffer=0x{:08X} packet_size={}x{} "
        "fetch_base=0x{:08X} fetch_size={}x{} fetch={:08X} {:08X} {:08X} {:08X} {:08X} "
        "{:08X} resource={}x{} dxgi={} frontbuffer_format={} unscaled={}x{}",
        nx1_swap_probe_id, frontbuffer_ptr, frontbuffer_width, frontbuffer_height,
        fetch.base_address << 12, fetch.size_2d.width + 1, fetch.size_2d.height + 1,
        fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5,
        uint32_t(swap_texture_desc.Width), swap_texture_desc.Height,
        uint32_t(swap_texture_desc.Format), uint32_t(frontbuffer_format), frontbuffer_width_unscaled,
        frontbuffer_height_unscaled);
    REXGPU_WARN(
        "[NX1 frame-probe] IssueSwap #{} frame calls={} issued={} skipped={} dropped={} "
        "copy_packets={} resolves={} frontbuffer_resolves={} color_draws={} large_color_draws={} "
        "thin_color_draws={} depth_only={} memexport={} indexed={} color_vertices={} "
        "last_resolve=0x{:08X}+0x{:X}({}) "
        "resolve_list={}:[0x{:08X}+0x{:X},0x{:08X}+0x{:X},0x{:08X}+0x{:X},0x{:08X}+0x{:X},"
        "0x{:08X}+0x{:X},0x{:08X}+0x{:X},0x{:08X}+0x{:X},0x{:08X}+0x{:X}] "
        "last_color rt_bits=0x{:X} mask=0x{:X} viewport={}x{} scissor={}x{} textures=0x{:08X} "
        "fetch0=0x{:08X} {}x{} fmt={} fdw={:08X}:{:08X}:{:08X}:{:08X}:{:08X}:{:08X} "
        "vs={:016X} ps={:016X}",
        nx1_swap_probe_id, nx1_frame_probe.calls, nx1_frame_probe.issued,
        nx1_frame_probe.skipped, nx1_frame_probe.dropped, nx1_frame_probe.copy_packets,
        nx1_frame_probe.resolves, nx1_frame_probe.frontbuffer_resolves,
        nx1_frame_probe.color_draws, nx1_frame_probe.large_color_draws,
        nx1_frame_probe.thin_color_draws, nx1_frame_probe.depth_only_draws,
        nx1_frame_probe.memexport_draws,
        nx1_frame_probe.indexed_draws, nx1_frame_probe.color_vertices,
        nx1_frame_probe.last_resolve_address, nx1_frame_probe.last_resolve_length,
        nx1_frame_probe.last_resolve_result, nx1_frame_probe.resolve_log_count,
        nx1_frame_probe.resolve_addresses[0], nx1_frame_probe.resolve_lengths[0],
        nx1_frame_probe.resolve_addresses[1], nx1_frame_probe.resolve_lengths[1],
        nx1_frame_probe.resolve_addresses[2], nx1_frame_probe.resolve_lengths[2],
        nx1_frame_probe.resolve_addresses[3], nx1_frame_probe.resolve_lengths[3],
        nx1_frame_probe.resolve_addresses[4], nx1_frame_probe.resolve_lengths[4],
        nx1_frame_probe.resolve_addresses[5], nx1_frame_probe.resolve_lengths[5],
        nx1_frame_probe.resolve_addresses[6], nx1_frame_probe.resolve_lengths[6],
        nx1_frame_probe.resolve_addresses[7], nx1_frame_probe.resolve_lengths[7],
        nx1_frame_probe.last_color_rt_bits,
        nx1_frame_probe.last_color_mask, nx1_frame_probe.last_color_viewport_width,
        nx1_frame_probe.last_color_viewport_height, nx1_frame_probe.last_color_scissor_width,
        nx1_frame_probe.last_color_scissor_height, nx1_frame_probe.last_color_textures,
        nx1_frame_probe.last_color_fetch0_base, nx1_frame_probe.last_color_fetch0_width,
        nx1_frame_probe.last_color_fetch0_height, nx1_frame_probe.last_color_fetch0_format,
        nx1_frame_probe.last_color_fetch0_dwords[0],
        nx1_frame_probe.last_color_fetch0_dwords[1],
        nx1_frame_probe.last_color_fetch0_dwords[2],
        nx1_frame_probe.last_color_fetch0_dwords[3],
        nx1_frame_probe.last_color_fetch0_dwords[4],
        nx1_frame_probe.last_color_fetch0_dwords[5],
        nx1_frame_probe.last_color_vs, nx1_frame_probe.last_color_ps);
    for (uint32_t ring_offset = 0; ring_offset < nx1_frame_probe.color_ring_count;
         ++ring_offset) {
      const uint32_t ring_index =
          (nx1_frame_probe.color_ring_next + rex::countof(nx1_frame_probe.color_ring) -
           nx1_frame_probe.color_ring_count + ring_offset) %
          rex::countof(nx1_frame_probe.color_ring);
      const auto& draw = nx1_frame_probe.color_ring[ring_index];
      const auto& texture0 = draw.textures[0];
      const auto& texture1 = draw.textures[1];
      const auto& texture2 = draw.textures[2];
      const auto& texture3 = draw.textures[3];
      REXGPU_WARN(
          "[NX1 color-ring] IssueSwap #{} {}/{} issue={} call={} indexed={} guest_indices={} "
          "host_vertices={} rt_bits=0x{:X} mask=0x{:X} viewport={}x{}+{},{} "
          "scissor={}x{}+{},{} textures=0x{:08X} texture_count={} "
          "f0=s{} 0x{:08X} {}x{} fmt={} {:08X}:{:08X}:{:08X}:{:08X}:{:08X}:{:08X} "
          "f1=s{} 0x{:08X} {}x{} fmt={} {:08X}:{:08X}:{:08X}:{:08X}:{:08X}:{:08X} "
          "f2=s{} 0x{:08X} {}x{} fmt={} {:08X}:{:08X}:{:08X}:{:08X}:{:08X}:{:08X} "
          "f3=s{} 0x{:08X} {}x{} fmt={} {:08X}:{:08X}:{:08X}:{:08X}:{:08X}:{:08X} "
          "vs={:016X} ps={:016X}",
          nx1_swap_probe_id, ring_offset + 1, nx1_frame_probe.color_ring_count, draw.issue,
          draw.call, draw.indexed, draw.guest_indices, draw.host_vertices, draw.rt_bits,
          draw.color_mask, draw.viewport_width, draw.viewport_height, draw.viewport_x,
          draw.viewport_y, draw.scissor_width, draw.scissor_height, draw.scissor_x,
          draw.scissor_y, draw.texture_mask, draw.texture_count, texture0.slot, texture0.base,
          texture0.width, texture0.height, texture0.format, texture0.dwords[0],
          texture0.dwords[1], texture0.dwords[2], texture0.dwords[3], texture0.dwords[4],
          texture0.dwords[5], texture1.slot, texture1.base, texture1.width, texture1.height,
          texture1.format, texture1.dwords[0], texture1.dwords[1], texture1.dwords[2],
          texture1.dwords[3], texture1.dwords[4], texture1.dwords[5], texture2.slot,
          texture2.base, texture2.width, texture2.height, texture2.format, texture2.dwords[0],
          texture2.dwords[1], texture2.dwords[2], texture2.dwords[3], texture2.dwords[4],
          texture2.dwords[5], texture3.slot, texture3.base, texture3.width, texture3.height,
          texture3.format, texture3.dwords[0], texture3.dwords[1], texture3.dwords[2],
          texture3.dwords[3], texture3.dwords[4], texture3.dwords[5], draw.vs, draw.ps);
    }
  }
  // The swap gamma / FXAA pass samples source texels by pixel index, but swap
  // textures may be allocation-padded. Prefer the active frontbuffer region
  // from the swap packet, scaled proportionally to the actual source texture.
  uint32_t source_width_scaled = uint32_t(swap_texture_desc.Width);
  uint32_t source_height_scaled = uint32_t(swap_texture_desc.Height);
  auto get_active_swap_dimension = [](uint32_t packet_unscaled, uint32_t source_unscaled,
                                      uint32_t source_scaled) -> uint32_t {
    if (!source_scaled) {
      return 0;
    }
    uint32_t active_unscaled = packet_unscaled ? packet_unscaled : source_unscaled;
    if (!active_unscaled) {
      return source_scaled;
    }
    if (source_unscaled) {
      active_unscaled = std::min(active_unscaled, source_unscaled);
      uint64_t active_scaled =
          (uint64_t(active_unscaled) * source_scaled + (source_unscaled >> 1)) / source_unscaled;
      return uint32_t(std::clamp<uint64_t>(active_scaled, 1, source_scaled));
    }
    return std::min(active_unscaled, source_scaled);
  };
  uint32_t guest_output_width =
      get_active_swap_dimension(frontbuffer_width, frontbuffer_width_unscaled, source_width_scaled);
  uint32_t guest_output_height = get_active_swap_dimension(
      frontbuffer_height, frontbuffer_height_unscaled, source_height_scaled);
  if (!guest_output_width) {
    guest_output_width = source_width_scaled
                             ? source_width_scaled
                             : (frontbuffer_width ? frontbuffer_width : frontbuffer_width_unscaled);
  }
  if (!guest_output_height) {
    guest_output_height = source_height_scaled ? source_height_scaled
                                               : (frontbuffer_height ? frontbuffer_height
                                                                     : frontbuffer_height_unscaled);
  }
  bool swap_source_scaled = frontbuffer_width_unscaled && frontbuffer_height_unscaled &&
                            (source_width_scaled != frontbuffer_width_unscaled ||
                             source_height_scaled != frontbuffer_height_unscaled);
  if (texture_cache_->IsDrawResolutionScaled() && !swap_source_scaled) {
    static bool draw_scale_swap_unscaled_logged = false;
    if (!draw_scale_swap_unscaled_logged) {
      draw_scale_swap_unscaled_logged = true;
      REXGPU_WARN(
          "D3D12 draw resolution scaling is enabled, but the swap source is "
          "unscaled ({}x{}). This title may be presenting from an unscaled "
          "resolve path.",
          guest_output_width, guest_output_height);
    }
  }

  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));

  presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,
      [this, &swap_texture_srv_desc, frontbuffer_format, swap_texture_resource, guest_output_width,
       guest_output_height, nx1_swap_probe_id,
       nx1_log_swap_probe](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        Nx1ScopedHitchTimer nx1_refresh_timer("D3D12.IssueSwap.RefreshGuestOutput",
                                              frame_current_, nx1_swap_probe_id);
        const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
        ID3D12Device* device = provider.GetDevice();

        SwapPostEffect swap_post_effect = GetActualSwapPostEffect();
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;
        if (use_fxaa) {
          // Make sure the texture of the correct size is available for FXAA.
          if (fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc = fxaa_source_texture_->GetDesc();
            if (fxaa_source_texture_desc.Width != guest_output_width ||
                fxaa_source_texture_desc.Height != guest_output_height) {
              if (submission_completed_ < fxaa_source_texture_submission_) {
                fxaa_source_texture_->AddRef();
                resources_for_deletion_.emplace_back(fxaa_source_texture_submission_,
                                                     fxaa_source_texture_.Get());
              }
              fxaa_source_texture_.Reset();
              fxaa_source_texture_submission_ = 0;
            }
          }
          if (!fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc;
            fxaa_source_texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            fxaa_source_texture_desc.Alignment = 0;
            fxaa_source_texture_desc.Width = guest_output_width;
            fxaa_source_texture_desc.Height = guest_output_height;
            fxaa_source_texture_desc.DepthOrArraySize = 1;
            fxaa_source_texture_desc.MipLevels = 1;
            fxaa_source_texture_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_texture_desc.SampleDesc.Count = 1;
            fxaa_source_texture_desc.SampleDesc.Quality = 0;
            fxaa_source_texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            fxaa_source_texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(device->CreateCommittedResource(
                    &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
                    &fxaa_source_texture_desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    nullptr, IID_PPV_ARGS(&fxaa_source_texture_)))) {
              REXGPU_ERROR("Failed to create the FXAA input texture");
              swap_post_effect = SwapPostEffect::kNone;
              use_fxaa = false;
            }
          }
        }

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

        ID3D12Resource* guest_output_resource =
            static_cast<ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context)
                .resource_uav_capable();

        const Nx1NativePresentMode native_present_mode = Nx1GetNativePresentMode();
        if (native_present_mode != Nx1NativePresentMode::kOff &&
            REXCVAR_GET(nx1_native_present_bypass_gamma)) {
          D3D12_RESOURCE_DESC native_source_desc = swap_texture_resource->GetDesc();
          D3D12_RESOURCE_DESC native_dest_desc = guest_output_resource->GetDesc();
          const bool native_source_default_swizzle =
              swap_texture_srv_desc.Shader4ComponentMapping ==
              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          const bool native_direct_copy_compatible =
              !use_fxaa && native_source_default_swizzle &&
              native_source_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
              native_dest_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
              native_source_desc.SampleDesc.Count == 1 && native_dest_desc.SampleDesc.Count == 1 &&
              native_source_desc.Format == ui::d3d12::D3D12Presenter::kGuestOutputFormat &&
              native_dest_desc.Format == ui::d3d12::D3D12Presenter::kGuestOutputFormat &&
              guest_output_width <= native_source_desc.Width &&
              guest_output_height <= native_source_desc.Height &&
              guest_output_width <= native_dest_desc.Width && guest_output_height <= native_dest_desc.Height;
          if (native_direct_copy_compatible) {
            static uint64_t nx1_native_present_count = 0;
            ++nx1_native_present_count;
            if (REXCVAR_GET(nx1_native_present_log) &&
                (nx1_native_present_count <= 16 || (nx1_native_present_count % 300) == 0)) {
              REXGPU_INFO(
                  "[NX1 native-present] direct copy #{} mode={} bypass_gamma={} source={}x{} "
                  "fmt={} output={}x{} guest={}x{} swizzle=0x{:08X}",
                  nx1_native_present_count, Nx1NativePresentModeName(native_present_mode),
                  REXCVAR_GET(nx1_native_present_bypass_gamma), uint32_t(native_source_desc.Width),
                  native_source_desc.Height, uint32_t(native_source_desc.Format),
                  uint32_t(native_dest_desc.Width), native_dest_desc.Height, guest_output_width,
                  guest_output_height, swap_texture_srv_desc.Shader4ComponentMapping);
            }
            context.SetIs8bpc(false);
            {
              Nx1ScopedHitchTimer nx1_native_present_timer(
                  "D3D12.IssueSwap.NativePresentDirectCopyWork", frame_current_,
                  nx1_swap_probe_id);
              PushTransitionBarrier(swap_texture_resource,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
              PushTransitionBarrier(guest_output_resource,
                                    ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
              SubmitBarriers();

              D3D12_TEXTURE_COPY_LOCATION copy_source = {};
              copy_source.pResource = swap_texture_resource;
              copy_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
              copy_source.SubresourceIndex = 0;
              D3D12_TEXTURE_COPY_LOCATION copy_dest = {};
              copy_dest.pResource = guest_output_resource;
              copy_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
              copy_dest.SubresourceIndex = 0;
              D3D12_BOX copy_source_box = {0, 0, 0, guest_output_width, guest_output_height, 1};
              deferred_command_list_.D3DCopyTextureRegion(&copy_dest, 0, 0, 0, &copy_source,
                                                          &copy_source_box);

              PushTransitionBarrier(swap_texture_resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
              PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_COPY_DEST,
                                    ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
              SubmitBarriers();
            }
            {
              Nx1ScopedHitchTimer nx1_native_present_timer(
                  "D3D12.IssueSwap.NativePresentDirectCopyEndSubmission", frame_current_,
                  nx1_swap_probe_id);
              EndSubmission(true);
            }
            return true;
          }

          const bool native_shader_copy_compatible =
              (native_present_mode == Nx1NativePresentMode::kAuto ||
               native_present_mode == Nx1NativePresentMode::kShaderCopy) &&
              !use_fxaa &&
              nx1_native_present_copy_pipeline_ &&
              native_present_view_heap_pool_ &&
              native_source_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
              native_dest_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
              native_source_desc.SampleDesc.Count == 1 && native_dest_desc.SampleDesc.Count == 1 &&
              guest_output_width <= native_source_desc.Width &&
              guest_output_height <= native_source_desc.Height &&
              guest_output_width <= native_dest_desc.Width && guest_output_height <= native_dest_desc.Height;
          if (native_shader_copy_compatible) {
            constexpr uint32_t kNativePresentDescriptorCount = 2;
            uint32_t native_present_descriptor_base = 0;
            if (native_present_view_heap_pool_->Request(
                    submission_current_, ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid,
                    kNativePresentDescriptorCount, kNativePresentDescriptorCount,
                    native_present_descriptor_base) ==
                ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
              REXGPU_WARN("[NX1 native-present] shader copy descriptor allocation failed");
            } else {
              D3D12_CPU_DESCRIPTOR_HANDLE native_present_heap_cpu_start =
                  native_present_view_heap_pool_->GetLastRequestHeapCPUStart();
              D3D12_GPU_DESCRIPTOR_HANDLE native_present_heap_gpu_start =
                  native_present_view_heap_pool_->GetLastRequestHeapGPUStart();
              ui::d3d12::util::DescriptorCpuGpuHandlePair native_present_descriptors
                  [kNativePresentDescriptorCount];
              for (uint32_t i = 0; i < kNativePresentDescriptorCount; ++i) {
                native_present_descriptors[i] = std::make_pair(
                    provider.OffsetViewDescriptor(native_present_heap_cpu_start,
                                                  native_present_descriptor_base + i),
                    provider.OffsetViewDescriptor(native_present_heap_gpu_start,
                                                  native_present_descriptor_base + i));
              }

              static uint64_t nx1_native_present_shader_count = 0;
              ++nx1_native_present_shader_count;
              if (REXCVAR_GET(nx1_native_present_log) &&
                  (nx1_native_present_shader_count <= 16 ||
                   (nx1_native_present_shader_count % 300) == 0)) {
                REXGPU_INFO(
                    "[NX1 native-present] shader copy #{} mode={} bypass_gamma={} source={}x{} "
                    "resource_fmt={} srv_fmt={} output={}x{} fmt={} guest={}x{} swizzle=0x{:08X}",
                    nx1_native_present_shader_count, Nx1NativePresentModeName(native_present_mode),
                    REXCVAR_GET(nx1_native_present_bypass_gamma), uint32_t(native_source_desc.Width),
                    native_source_desc.Height, uint32_t(native_source_desc.Format),
                    uint32_t(swap_texture_srv_desc.Format), uint32_t(native_dest_desc.Width),
                    native_dest_desc.Height, uint32_t(native_dest_desc.Format), guest_output_width,
                    guest_output_height, swap_texture_srv_desc.Shader4ComponentMapping);
              }

              context.SetIs8bpc(false);
              {
                Nx1ScopedHitchTimer nx1_native_present_timer(
                    "D3D12.IssueSwap.NativePresentBarrierBefore", frame_current_,
                    nx1_swap_probe_id);
                PushTransitionBarrier(guest_output_resource,
                                      ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                SubmitBarriers();
              }

              {
                Nx1ScopedHitchTimer nx1_native_present_timer(
                    "D3D12.IssueSwap.NativePresentDescriptorWrite", frame_current_,
                    nx1_swap_probe_id);
                D3D12_UNORDERED_ACCESS_VIEW_DESC native_present_dest_uav_desc;
                native_present_dest_uav_desc.Format =
                    ui::d3d12::D3D12Presenter::kGuestOutputFormat;
                native_present_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                native_present_dest_uav_desc.Texture2D.MipSlice = 0;
                native_present_dest_uav_desc.Texture2D.PlaneSlice = 0;
                device->CreateUnorderedAccessView(guest_output_resource, nullptr,
                                                  &native_present_dest_uav_desc,
                                                  native_present_descriptors[0].first);
                device->CreateShaderResourceView(swap_texture_resource, &swap_texture_srv_desc,
                                                 native_present_descriptors[1].first);
              }

              deferred_command_list_.SetDescriptorHeaps(
                  native_present_view_heap_pool_->GetLastRequestHeap(), nullptr);
              deferred_command_list_.D3DSetComputeRootSignature(
                  native_present_root_signature_.Get());
              NativePresentConstants native_present_constants;
              native_present_constants.size[0] = guest_output_width;
              native_present_constants.size[1] = guest_output_height;
              deferred_command_list_.D3DSetComputeRoot32BitConstants(
                  UINT(NativePresentRootParameter::kConstants),
                  sizeof(native_present_constants) / sizeof(uint32_t), &native_present_constants,
                  0);
              deferred_command_list_.D3DSetComputeRootDescriptorTable(
                  UINT(NativePresentRootParameter::kDestination),
                  native_present_descriptors[0].second);
              deferred_command_list_.D3DSetComputeRootDescriptorTable(
                  UINT(NativePresentRootParameter::kSource), native_present_descriptors[1].second);
              {
                Nx1ScopedHitchTimer nx1_native_present_timer(
                    "D3D12.IssueSwap.NativePresentShaderDispatch", frame_current_,
                    nx1_swap_probe_id);
                SetExternalPipeline(nx1_native_present_copy_pipeline_.Get());
                deferred_command_list_.D3DDispatch((guest_output_width + 15) / 16,
                                                   (guest_output_height + 7) / 8, 1);
              }

              {
                Nx1ScopedHitchTimer nx1_native_present_timer(
                    "D3D12.IssueSwap.NativePresentBarrierAfter", frame_current_,
                    nx1_swap_probe_id);
                PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
                SubmitBarriers();
              }
              {
                Nx1ScopedHitchTimer nx1_native_present_timer(
                    "D3D12.IssueSwap.NativePresentEndSubmission", frame_current_,
                    nx1_swap_probe_id);
                EndSubmission(true);
              }
              return true;
            }
          }

          static uint64_t nx1_native_present_fallback_count = 0;
          ++nx1_native_present_fallback_count;
          if (REXCVAR_GET(nx1_native_present_log) &&
              (nx1_native_present_fallback_count <= 16 ||
               (nx1_native_present_fallback_count % 300) == 0)) {
            REXGPU_WARN(
                "[NX1 native-present] fallback #{} mode={} bypass_gamma={} fxaa={} "
                "swizzle_default={} swizzle=0x{:08X} source={}x{} fmt={} samples={} dest={}x{} "
                "fmt={} samples={} guest={}x{}",
                nx1_native_present_fallback_count, Nx1NativePresentModeName(native_present_mode),
                REXCVAR_GET(nx1_native_present_bypass_gamma), use_fxaa, native_source_default_swizzle,
                swap_texture_srv_desc.Shader4ComponentMapping, uint32_t(native_source_desc.Width),
                native_source_desc.Height, uint32_t(native_source_desc.Format),
                native_source_desc.SampleDesc.Count, uint32_t(native_dest_desc.Width),
                native_dest_desc.Height, uint32_t(native_dest_desc.Format),
                native_dest_desc.SampleDesc.Count, guest_output_width, guest_output_height);
          }
        }
        if (native_present_mode != Nx1NativePresentMode::kOff &&
            !REXCVAR_GET(nx1_native_present_bypass_gamma) &&
            REXCVAR_GET(nx1_native_present_log)) {
          static uint64_t nx1_native_present_gamma_path_count = 0;
          ++nx1_native_present_gamma_path_count;
          if (nx1_native_present_gamma_path_count <= 16 ||
              (nx1_native_present_gamma_path_count % 300) == 0) {
            REXGPU_INFO(
                "[NX1 native-present] gamma-preserving path #{} mode={} native_descriptors={} "
                "fxaa={} gamma={} guest={}x{}",
                nx1_native_present_gamma_path_count, Nx1NativePresentModeName(native_present_mode),
                REXCVAR_GET(nx1_native_present_gamma_descriptors), use_fxaa,
                use_pwl_gamma_ramp ? "pwl" : "table", guest_output_width, guest_output_height);
          }
        }

        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        // Upload the new gamma ramp, using the upload buffer for the current
        // frame (will close the frame after this anyway, so can't write
        // multiple times per frame).
        if (!(use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                                 : gamma_ramp_256_entry_table_up_to_date_)) {
          Nx1ScopedHitchTimer nx1_gamma_upload_timer("D3D12.IssueSwap.GammaRampUpload",
                                                     frame_current_, nx1_swap_probe_id);
          uint32_t gamma_ramp_offset_bytes = use_pwl_gamma_ramp ? 256 * 4 : 0;
          uint32_t gamma_ramp_upload_offset_bytes =
              uint32_t(frame_current_ % kQueueFrames) * ((256 + 128 * 3) * 4) +
              gamma_ramp_offset_bytes;
          uint32_t gamma_ramp_size_bytes = (use_pwl_gamma_ramp ? 128 * 3 : 256) * 4;
          if (std::endian::native != std::endian::little && use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload_buffer = reinterpret_cast<reg::DC_LUT_PWL_DATA*>(
                gamma_ramp_upload_buffer_mapping_ + gamma_ramp_upload_offset_bytes);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_buffer_entry =
                  gamma_ramp_pwl_upload_buffer[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_buffer_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_buffer_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(gamma_ramp_upload_buffer_mapping_ + gamma_ramp_upload_offset_bytes,
                        use_pwl_gamma_ramp ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                                           : static_cast<const void*>(gamma_ramp_256_entry_table()),
                        gamma_ramp_size_bytes);
          }
          PushTransitionBarrier(gamma_ramp_buffer_.Get(), gamma_ramp_buffer_state_,
                                D3D12_RESOURCE_STATE_COPY_DEST);
          gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
          SubmitBarriers();
          deferred_command_list_.D3DCopyBufferRegion(
              gamma_ramp_buffer_.Get(), gamma_ramp_offset_bytes, gamma_ramp_upload_buffer_.Get(),
              gamma_ramp_upload_offset_bytes, gamma_ramp_size_bytes);
          (use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                              : gamma_ramp_256_entry_table_up_to_date_) = true;
        }

        // Destination, source, and if bindful, gamma ramp.
        ui::d3d12::util::DescriptorCpuGpuHandlePair apply_gamma_descriptors[3];
        ui::d3d12::util::DescriptorCpuGpuHandlePair apply_gamma_descriptor_gamma_ramp;
        bool apply_gamma_uses_native_present_descriptors = false;
        {
          Nx1ScopedHitchTimer nx1_gamma_descriptors_timer(
              "D3D12.IssueSwap.GammaDescriptorAlloc", frame_current_, nx1_swap_probe_id);
          const bool can_use_native_present_descriptors =
              REXCVAR_GET(nx1_native_present_gamma_descriptors) &&
              native_present_mode != Nx1NativePresentMode::kOff && !use_fxaa &&
              native_present_view_heap_pool_;
          if (can_use_native_present_descriptors) {
            constexpr uint32_t kApplyGammaDescriptorCount = 3;
            uint32_t apply_gamma_descriptor_base = 0;
            if (native_present_view_heap_pool_->Request(
                    submission_current_, ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid,
                    kApplyGammaDescriptorCount, kApplyGammaDescriptorCount,
                    apply_gamma_descriptor_base) !=
                ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
              D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu_start =
                  native_present_view_heap_pool_->GetLastRequestHeapCPUStart();
              D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu_start =
                  native_present_view_heap_pool_->GetLastRequestHeapGPUStart();
              for (uint32_t i = 0; i < kApplyGammaDescriptorCount; ++i) {
                apply_gamma_descriptors[i] = std::make_pair(
                    provider.OffsetViewDescriptor(heap_cpu_start, apply_gamma_descriptor_base + i),
                    provider.OffsetViewDescriptor(heap_gpu_start, apply_gamma_descriptor_base + i));
              }
              apply_gamma_uses_native_present_descriptors = true;
            }
          }
          if (!apply_gamma_uses_native_present_descriptors &&
              !RequestOneUseSingleViewDescriptors(bindless_resources_used_ ? 2 : 3,
                                                  apply_gamma_descriptors)) {
            return false;
          }
        }
        if (apply_gamma_uses_native_present_descriptors &&
            REXCVAR_GET(nx1_native_present_log)) {
          static uint64_t nx1_native_present_gamma_descriptor_count = 0;
          ++nx1_native_present_gamma_descriptor_count;
          if (nx1_native_present_gamma_descriptor_count <= 16 ||
              (nx1_native_present_gamma_descriptor_count % 300) == 0) {
            REXGPU_INFO(
                "[NX1 native-present] gamma descriptor heap #{} mode={} gamma={} guest={}x{}",
                nx1_native_present_gamma_descriptor_count,
                Nx1NativePresentModeName(native_present_mode),
                use_pwl_gamma_ramp ? "pwl" : "table", guest_output_width, guest_output_height);
          }
        }
        // Must not call anything that can change the descriptor heap from now
        // on!
        if (bindless_resources_used_ && !apply_gamma_uses_native_present_descriptors) {
          apply_gamma_descriptor_gamma_ramp = GetSystemBindlessViewHandlePair(
              use_pwl_gamma_ramp ? SystemBindlessView::kGammaRampPWLSRV
                                 : SystemBindlessView::kGammaRampTableSRV);
        } else {
          apply_gamma_descriptor_gamma_ramp = apply_gamma_descriptors[2];
          WriteGammaRampSRV(use_pwl_gamma_ramp, apply_gamma_descriptor_gamma_ramp.first);
        }

        if (use_fxaa) {
          fxaa_source_texture_submission_ = submission_current_;
        }

        ID3D12Resource* apply_gamma_dest =
            use_fxaa ? fxaa_source_texture_.Get() : guest_output_resource;
        D3D12_RESOURCE_STATES apply_gamma_dest_initial_state =
            use_fxaa ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                     : ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
        PushTransitionBarrier(apply_gamma_dest, apply_gamma_dest_initial_state,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // From now on, even in case of failure, apply_gamma_dest must be
        // transitioned back to apply_gamma_dest_initial_state!
        D3D12_UNORDERED_ACCESS_VIEW_DESC apply_gamma_dest_uav_desc;
        apply_gamma_dest_uav_desc.Format =
            use_fxaa ? kFxaaSourceTextureFormat : ui::d3d12::D3D12Presenter::kGuestOutputFormat;
        apply_gamma_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        apply_gamma_dest_uav_desc.Texture2D.MipSlice = 0;
        apply_gamma_dest_uav_desc.Texture2D.PlaneSlice = 0;
        device->CreateUnorderedAccessView(apply_gamma_dest, nullptr, &apply_gamma_dest_uav_desc,
                                          apply_gamma_descriptors[0].first);

        device->CreateShaderResourceView(swap_texture_resource, &swap_texture_srv_desc,
                                         apply_gamma_descriptors[1].first);

        PushTransitionBarrier(gamma_ramp_buffer_.Get(), gamma_ramp_buffer_state_,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        uint32_t group_count_x = (guest_output_width + 15) / 16;
        uint32_t group_count_y = (guest_output_height + 7) / 8;
        {
          Nx1ScopedHitchTimer nx1_apply_gamma_timer("D3D12.IssueSwap.ApplyGammaDispatch",
                                                    frame_current_, nx1_swap_probe_id);
          if (apply_gamma_uses_native_present_descriptors) {
            deferred_command_list_.SetDescriptorHeaps(
                native_present_view_heap_pool_->GetLastRequestHeap(), nullptr);
          }
          deferred_command_list_.D3DSetComputeRootSignature(apply_gamma_root_signature_.Get());
          ApplyGammaConstants apply_gamma_constants;
          apply_gamma_constants.size[0] = guest_output_width;
          apply_gamma_constants.size[1] = guest_output_height;
          deferred_command_list_.D3DSetComputeRoot32BitConstants(
              UINT(ApplyGammaRootParameter::kConstants),
              sizeof(apply_gamma_constants) / sizeof(uint32_t), &apply_gamma_constants, 0);
          deferred_command_list_.D3DSetComputeRootDescriptorTable(
              UINT(ApplyGammaRootParameter::kDestination), apply_gamma_descriptors[0].second);
          deferred_command_list_.D3DSetComputeRootDescriptorTable(
              UINT(ApplyGammaRootParameter::kSource), apply_gamma_descriptors[1].second);
          deferred_command_list_.D3DSetComputeRootDescriptorTable(
              UINT(ApplyGammaRootParameter::kRamp), apply_gamma_descriptor_gamma_ramp.second);
          ID3D12PipelineState* apply_gamma_pipeline;
          if (use_pwl_gamma_ramp) {
            apply_gamma_pipeline = use_fxaa ? apply_gamma_pwl_fxaa_luma_pipeline_.Get()
                                            : apply_gamma_pwl_pipeline_.Get();
          } else {
            apply_gamma_pipeline = use_fxaa ? apply_gamma_table_fxaa_luma_pipeline_.Get()
                                            : apply_gamma_table_pipeline_.Get();
          }
          SetExternalPipeline(apply_gamma_pipeline);
          SubmitBarriers();
          deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);
        }

        // Apply FXAA.
        if (use_fxaa) {
          // Destination and source.
          ui::d3d12::util::DescriptorCpuGpuHandlePair fxaa_descriptors[2];
          if (!RequestOneUseSingleViewDescriptors(uint32_t(rex::countof(fxaa_descriptors)),
                                                  fxaa_descriptors)) {
            // Failed to obtain descriptors for FXAA - just copy after gamma
            // ramp application without applying FXAA.
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
            PushTransitionBarrier(guest_output_resource,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
            SubmitBarriers();
            deferred_command_list_.D3DCopyResource(guest_output_resource, apply_gamma_dest);
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_COPY_DEST,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
            return false;
          } else {
            Nx1ScopedHitchTimer nx1_fxaa_timer("D3D12.IssueSwap.FxaaDispatch", frame_current_,
                                               nx1_swap_probe_id);
            assert_true(apply_gamma_dest_initial_state ==
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(guest_output_resource,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // From now on, even in case of failure, guest_output_resource must
            // be transitioned back to kGuestOutputInternalState!
            deferred_command_list_.D3DSetComputeRootSignature(fxaa_root_signature_.Get());
            FxaaConstants fxaa_constants;
            fxaa_constants.size[0] = guest_output_width;
            fxaa_constants.size[1] = guest_output_height;
            fxaa_constants.size_inv[0] = 1.0f / float(fxaa_constants.size[0]);
            fxaa_constants.size_inv[1] = 1.0f / float(fxaa_constants.size[1]);
            deferred_command_list_.D3DSetComputeRoot32BitConstants(
                UINT(FxaaRootParameter::kConstants), sizeof(fxaa_constants) / sizeof(uint32_t),
                &fxaa_constants, 0);
            D3D12_UNORDERED_ACCESS_VIEW_DESC fxaa_dest_uav_desc;
            fxaa_dest_uav_desc.Format = ui::d3d12::D3D12Presenter::kGuestOutputFormat;
            fxaa_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            fxaa_dest_uav_desc.Texture2D.MipSlice = 0;
            fxaa_dest_uav_desc.Texture2D.PlaneSlice = 0;
            device->CreateUnorderedAccessView(guest_output_resource, nullptr, &fxaa_dest_uav_desc,
                                              fxaa_descriptors[0].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kDestination), fxaa_descriptors[0].second);
            D3D12_SHADER_RESOURCE_VIEW_DESC fxaa_source_srv_desc;
            fxaa_source_srv_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            fxaa_source_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            fxaa_source_srv_desc.Texture2D.MostDetailedMip = 0;
            fxaa_source_srv_desc.Texture2D.MipLevels = 1;
            fxaa_source_srv_desc.Texture2D.PlaneSlice = 0;
            fxaa_source_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
            device->CreateShaderResourceView(fxaa_source_texture_.Get(), &fxaa_source_srv_desc,
                                             fxaa_descriptors[1].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kSource), fxaa_descriptors[1].second);
            SetExternalPipeline(swap_post_effect == SwapPostEffect::kFxaaExtreme
                                    ? fxaa_extreme_pipeline_.Get()
                                    : fxaa_pipeline_.Get());
            SubmitBarriers();
            deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);
            PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
          }
        } else {
          assert_true(apply_gamma_dest_initial_state ==
                      ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
          PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                apply_gamma_dest_initial_state);
        }

        if (nx1_log_swap_probe) {
          struct Nx1SwapReadback {
            const char* label = nullptr;
            Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
            UINT num_rows = 0;
            UINT64 row_size = 0;
            UINT64 total_size = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
          };
          Nx1SwapReadback nx1_readbacks[2];
          uint32_t nx1_readback_count = 0;
          auto queue_nx1_readback = [&](ID3D12Resource* resource,
                                        D3D12_RESOURCE_STATES current_state,
                                        const char* label) {
            if (!resource || nx1_readback_count >= rex::countof(nx1_readbacks)) {
              return;
            }
            D3D12_RESOURCE_DESC resource_desc = resource->GetDesc();
            if (resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                resource_desc.SampleDesc.Count != 1 || resource_desc.Width == 0 ||
                resource_desc.Height == 0) {
              return;
            }
            Nx1SwapReadback& readback = nx1_readbacks[nx1_readback_count];
            readback.label = label;
            readback.width = uint32_t(resource_desc.Width);
            readback.height = resource_desc.Height;
            readback.format = resource_desc.Format;
            device->GetCopyableFootprints(&resource_desc, 0, 1, 0, &readback.footprint,
                                          &readback.num_rows, &readback.row_size,
                                          &readback.total_size);
            if (!readback.total_size || !readback.num_rows || !readback.row_size) {
              return;
            }
            D3D12_RESOURCE_DESC buffer_desc;
            ui::d3d12::util::FillBufferResourceDesc(buffer_desc, readback.total_size,
                                                    D3D12_RESOURCE_FLAG_NONE);
            if (FAILED(device->CreateCommittedResource(
                    &ui::d3d12::util::kHeapPropertiesReadback,
                    provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&readback.buffer)))) {
              REXGPU_WARN("[NX1 swap-pixels] IssueSwap #{} {} readback allocation failed",
                          nx1_swap_probe_id, label);
              return;
            }
            PushTransitionBarrier(resource, current_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            SubmitBarriers();
            D3D12_TEXTURE_COPY_LOCATION copy_dest = {};
            copy_dest.pResource = readback.buffer.Get();
            copy_dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            copy_dest.PlacedFootprint = readback.footprint;
            D3D12_TEXTURE_COPY_LOCATION copy_source = {};
            copy_source.pResource = resource;
            copy_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            copy_source.SubresourceIndex = 0;
            deferred_command_list_.D3DCopyTextureRegion(&copy_dest, 0, 0, 0, &copy_source,
                                                        nullptr);
            PushTransitionBarrier(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, current_state);
            ++nx1_readback_count;
          };

          queue_nx1_readback(swap_texture_resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             "swap_source");
          queue_nx1_readback(guest_output_resource,
                             ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                             "guest_output");

          // Need to submit all the commands before giving the image back to the
          // presenter so it can submit its own commands for displaying it to the
          // queue.
          {
            Nx1ScopedHitchTimer nx1_readback_submit_timer(
                "D3D12.IssueSwap.ReadbackSubmitBarriers", frame_current_, nx1_swap_probe_id);
            SubmitBarriers();
          }
          {
            Nx1ScopedHitchTimer nx1_readback_submit_timer(
                "D3D12.IssueSwap.ReadbackEndSubmission", frame_current_, nx1_swap_probe_id);
            EndSubmission(true);
          }
          if (nx1_readback_count && AwaitAllQueueOperationsCompletion()) {
            for (uint32_t readback_index = 0; readback_index < nx1_readback_count;
                 ++readback_index) {
              Nx1SwapReadback& readback = nx1_readbacks[readback_index];
              D3D12_RANGE read_range = {0, size_t(readback.total_size)};
              void* mapping = nullptr;
              if (FAILED(readback.buffer->Map(0, &read_range, &mapping))) {
                REXGPU_WARN("[NX1 swap-pixels] IssueSwap #{} {} map failed", nx1_swap_probe_id,
                            readback.label);
                continue;
              }
              const uint8_t* bytes = static_cast<const uint8_t*>(mapping);
              const uint32_t color_mask =
                  readback.format == DXGI_FORMAT_R10G10B10A2_UNORM ? 0x3FFFFFFFu : 0x00FFFFFFu;
              uint64_t color_nonzero = 0;
              uint64_t rgb_gt8 = 0;
              uint64_t rgb_gt32 = 0;
              uint64_t rgb_gt64 = 0;
              uint64_t pixel_count = uint64_t(readback.width) * readback.height;
              uint32_t min_color = UINT32_MAX;
              uint32_t max_color = 0;
              uint32_t first_nonzero_x = UINT32_MAX;
              uint32_t first_nonzero_y = UINT32_MAX;
              uint32_t first_nonzero_word = 0;
              auto sample_word = [&](uint32_t x, uint32_t y) -> uint32_t {
                if (x >= readback.width || y >= readback.height) {
                  return 0;
                }
                uint32_t word = 0;
                std::memcpy(&word, bytes + readback.footprint.Offset +
                                       uint64_t(y) * readback.footprint.Footprint.RowPitch +
                                       uint64_t(x) * sizeof(uint32_t),
                            sizeof(word));
                return word;
              };
              auto max_rgb8 = [&](uint32_t word) -> uint32_t {
                if (readback.format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                  const uint32_t r = word & 0x3FFu;
                  const uint32_t g = (word >> 10) & 0x3FFu;
                  const uint32_t b = (word >> 20) & 0x3FFu;
                  const uint32_t max_10 = std::max(r, std::max(g, b));
                  return (max_10 * 255u + 511u) / 1023u;
                }
                const uint32_t r = word & 0xFFu;
                const uint32_t g = (word >> 8) & 0xFFu;
                const uint32_t b = (word >> 16) & 0xFFu;
                return std::max(r, std::max(g, b));
              };
              for (uint32_t y = 0; y < readback.height; ++y) {
                const uint8_t* row =
                    bytes + readback.footprint.Offset +
                    uint64_t(y) * readback.footprint.Footprint.RowPitch;
                for (uint32_t x = 0; x < readback.width; ++x) {
                  uint32_t word = 0;
                  std::memcpy(&word, row + uint64_t(x) * sizeof(uint32_t), sizeof(word));
                  uint32_t color = word & color_mask;
                  min_color = std::min(min_color, color);
                  max_color = std::max(max_color, color);
                  if (color) {
                    ++color_nonzero;
                    if (first_nonzero_x == UINT32_MAX) {
                      first_nonzero_x = x;
                      first_nonzero_y = y;
                      first_nonzero_word = word;
                    }
                  }
                  const uint32_t brightness = max_rgb8(word);
                  rgb_gt8 += brightness > 8;
                  rgb_gt32 += brightness > 32;
                  rgb_gt64 += brightness > 64;
                }
              }
              D3D12_RANGE write_range = {};
              readback.buffer->Unmap(0, &write_range);
              REXGPU_WARN(
                  "[NX1 swap-pixels] IssueSwap #{} {} fmt={} size={}x{} row_pitch={} "
                  "color_nonzero={}/{} rgb_gt8={} rgb_gt32={} rgb_gt64={} min=0x{:08X} "
                  "max=0x{:08X} center=0x{:08X} q1=0x{:08X} q3=0x{:08X} "
                  "first=({}, {}) 0x{:08X}",
                  nx1_swap_probe_id, readback.label, uint32_t(readback.format), readback.width,
                  readback.height, readback.footprint.Footprint.RowPitch, color_nonzero,
                  pixel_count, rgb_gt8, rgb_gt32, rgb_gt64,
                  min_color == UINT32_MAX ? 0 : min_color, max_color,
                  sample_word(readback.width / 2, readback.height / 2),
                  sample_word(readback.width / 4, readback.height / 4),
                  sample_word((readback.width * 3) / 4, (readback.height * 3) / 4),
                  first_nonzero_x == UINT32_MAX ? 0 : first_nonzero_x,
                  first_nonzero_y == UINT32_MAX ? 0 : first_nonzero_y, first_nonzero_word);
            }
          }
          return true;
        }

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue.
        {
          Nx1ScopedHitchTimer nx1_final_submit_timer(
              "D3D12.IssueSwap.FinalSubmitBarriers", frame_current_, nx1_swap_probe_id);
          SubmitBarriers();
        }
        {
          Nx1ScopedHitchTimer nx1_final_submit_timer(
              "D3D12.IssueSwap.FinalEndSubmission", frame_current_, nx1_swap_probe_id);
          EndSubmission(true);
        }
        return true;
      });

  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  nx1_draw_probe.frame = {};
  EndSubmission(true);
}

void D3D12CommandProcessor::OnPrimaryBufferEnd() {
  if (REXCVAR_GET(d3d12_submit_on_primary_buffer_end) && submission_open_ &&
      !active_occlusion_query_.valid && CanEndSubmissionImmediately()) {
    const uint32_t submit_interval =
        std::max<uint32_t>(1, REXCVAR_GET(d3d12_primary_buffer_submit_interval));
    if (++primary_buffers_since_submission_ >= submit_interval) {
      EndSubmission(false);
    }
  }
}

Shader* D3D12CommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address, uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool D3D12CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  auto& nx1_draw_probe = Nx1DrawProbe();
  const uint64_t nx1_draw_probe_call = ++nx1_draw_probe.calls;
  Nx1ScopedHitchTimer nx1_issue_draw_timer("D3D12.IssueDraw", frame_current_,
                                           nx1_draw_probe_call);
  auto& nx1_frame_probe = nx1_draw_probe.frame;
  ++nx1_frame_probe.calls;
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  const RegisterFile& regs = *register_file_;

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  bool surface_pitch_is_zero = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0;

  auto nx1_log_draw_drop = [&](const char* reason) -> bool {
    const uint64_t dropped = ++nx1_draw_probe.dropped;
    ++nx1_frame_probe.dropped;
    if (Nx1ShouldLogDrawProbe(dropped)) {
      REXGPU_WARN(
          "[NX1 draw-probe] dropped draw #{} call #{}: {} prim={} index_count={} "
          "edram={} surface_pitch={} active_vs={} active_ps={}",
          dropped, nx1_draw_probe_call, reason, uint32_t(primitive_type), index_count,
          uint32_t(edram_mode), regs.Get<reg::RB_SURFACE_INFO>().surface_pitch,
          active_vertex_shader() != nullptr, active_pixel_shader() != nullptr);
    }
    return false;
  };

  auto nx1_log_draw_skip = [&](const char* reason) -> bool {
    const uint64_t skipped = ++nx1_draw_probe.skipped;
    ++nx1_frame_probe.skipped;
    if (Nx1ShouldLogDrawProbe(skipped)) {
      REXGPU_INFO(
          "[NX1 draw-probe] skipped draw #{} call #{}: {} prim={} index_count={} "
          "edram={} surface_pitch={} active_vs={} active_ps={}",
          skipped, nx1_draw_probe_call, reason, uint32_t(primitive_type), index_count,
          uint32_t(edram_mode), regs.Get<reg::RB_SURFACE_INFO>().surface_pitch,
          active_vertex_shader() != nullptr, active_pixel_shader() != nullptr);
    }
    return true;
  };

  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    ++nx1_frame_probe.copy_packets;
    // DIAGNOSTIC: suppress the resolve writeback entirely while another renderer owns the
    // output. A resolve is the one thing this backend does that WRITES guest memory, and it
    // does so host-side -- so it never trips the guest page protection the native renderer's
    // write-watch relies on. That matches the one measurement nothing else explains: texture
    // content changing between decodes with zero observed guest writes. Note that skipping the
    // raster (nx1_skip_reference_raster) does NOT test this: resolves return above that skip,
    // so they run either way, and with the raster skipped they write EDRAM that was never
    // drawn into. Off by default -- the native renderer serves textures from its own resolve
    // map, but the guest CPU may still read resolved data, so this is a probe, not a fix.
    if (gpu_disabled_.load(std::memory_order_relaxed) &&
        REXCVAR_GET(nx1_suppress_reference_resolve)) {
      static std::atomic<uint64_t> suppressed{0};
      const uint64_t n = ++suppressed;
      if ((n % 10000) == 1) {
        REXGPU_INFO("nx1_d3d9: reference RESOLVE suppressed (owned by D3D9): {} so far", n);
      }
      return true;
    }
    if (!IssueCopy()) {
      return nx1_log_draw_drop("IssueCopy failed");
    }
    return true;
  }

  // CPFETCH: this side of the draw-sequence fetch comparison (see D9FETCH in the native
  // renderer, and the nx1_dbg_fetch_seq cvar). Placed BEFORE any skip or shader-analysis
  // early-out so every real draw packet is logged, keeping the sequence alignable against the
  // native renderer's stream. This side owns the budget decrement; the native side only reads
  // the cvar, so both stop together.
  if (const uint32_t fetch_budget = REXCVAR_GET(nx1_dbg_fetch_seq); fetch_budget) {
    REXCVAR_SET(nx1_dbg_fetch_seq, fetch_budget - 1);
    static std::atomic<uint64_t> cp_fetch_seq{0};
    const auto f0 = register_file_->GetTextureFetch(0);
    const auto f1 = register_file_->GetTextureFetch(1);
    REXGPU_WARN("nx1_d3d9: CPFETCH #{} idx={} s0={:08X} s1={:08X}",
                cp_fetch_seq.fetch_add(1, std::memory_order_relaxed), index_count,
                uint32_t(f0.base_address) << 12, uint32_t(f1.base_address) << 12);
  }

  // Vertex shader analysis.
  auto vertex_shader = static_cast<D3D12Shader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return nx1_log_draw_drop("missing vertex shader");
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (surface_pitch_is_zero && is_rasterization_done) {
    // Doesn't actually draw.
    // Unlikely that zero would even really be legal though.
    return nx1_log_draw_skip("surface pitch is zero");
  }
  D3D12Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<D3D12Shader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      return nx1_log_draw_skip("rasterization disabled and no vertex memexport");
    }
  }
  bool memexport_used_pixel = pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;

  // When another renderer owns the output (nx1_d3d9), this backend's raster work is pure
  // waste: the frame it produces is never presented, yet every draw still costs a pipeline
  // lookup, descriptor/EDRAM setup and a D3D12 submit -- the game was rendering itself twice.
  // Skip geometry here, as late as possible so everything with guest-visible side effects is
  // already preserved:
  //   - resolves (EdramMode::kCopy -> IssueCopy) returned above, so EDRAM writebacks still land
  //   - MEMEXPORT draws fall through and execute: their shaders write guest memory the CPU reads
  //   - packet execution, registers, fences, interrupts and swap bookkeeping are untouched
  //     (this is inside IssueDraw only), so the guest never notices and cannot deadlock
  // Cvar-gated because this skip is not as side-effect-free as it looks: a draw that renders
  // INTO a texture (rendered to EDRAM, then resolved out to guest memory) leaves that memory
  // empty when the raster is skipped, and the native renderer then decodes zeros. Turn it off
  // to test whether a missing texture is really a skipped render-to-texture.
  if (gpu_disabled_.load(std::memory_order_relaxed) && !memexport_used &&
      REXCVAR_GET(nx1_skip_reference_raster)) {
    static std::atomic<uint64_t> nx1_skipped{0};
    const uint64_t n = ++nx1_skipped;
    if ((n % 100000) == 1) {
      REXGPU_INFO("nx1_d3d9: Xenia raster skipped (owned by D3D9): {} draws so far", n);
    }
    return true;
  }

  if (!BeginSubmission(true)) {
    return nx1_log_draw_drop("BeginSubmission failed");
  }

  // Process primitives.
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  {
    Nx1ScopedHitchTimer nx1_primitive_process_timer("D3D12.IssueDraw.PrimitiveProcess",
                                                    frame_current_, nx1_draw_probe_call);
    if (!primitive_processor_->Process(primitive_processing_result)) {
      return nx1_log_draw_drop("PrimitiveProcessor::Process failed");
    }
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    // Nothing to draw.
    return nx1_log_draw_skip("host draw vertex count is zero");
  }

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;

  if (REXCVAR_GET(nx1_disable_scene_composite_depth) && pixel_shader &&
      edram_mode == xenos::EdramMode::kColorDepth) {
    auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
    auto color_info0 =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
    bool nx1_scene_composite_target = surface_info.surface_pitch == 1040 &&
                                      color_info0.color_base == 98 &&
                                      color_info0.color_base_bit_11 == 0 &&
                                      (normalized_color_mask & 0x7) == 0x7;
    if (nx1_scene_composite_target &&
        (normalized_depth_control.z_enable || normalized_depth_control.stencil_enable)) {
      static uint64_t nx1_disable_depth_count = 0;
      ++nx1_disable_depth_count;
      if (nx1_disable_depth_count <= 16 || (nx1_disable_depth_count % 240) == 0) {
        REXGPU_WARN(
            "[NX1 disable-composite-depth] count={} call={} surface_pitch={} color0=0x{:08X} "
            "depth_raw=0x{:08X} depth_norm=0x{:08X}",
            nx1_disable_depth_count, nx1_draw_probe_call, uint32_t(surface_info.surface_pitch),
            color_info0.value, regs.Get<reg::RB_DEPTHCONTROL>().value,
            normalized_depth_control.value);
      }
      normalized_depth_control.z_enable = 0;
      normalized_depth_control.z_write_enable = 0;
      normalized_depth_control.stencil_enable = 0;
    }
  }

  // Shader modifications.
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;
  DxbcShaderTranslator::Modification vertex_shader_modification =
      pipeline_cache_->GetCurrentVertexShaderModification(
          *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask);
  DxbcShaderTranslator::Modification pixel_shader_modification =
      pixel_shader
          ? pipeline_cache_->GetCurrentPixelShaderModification(
                *pixel_shader, interpolator_mask, ps_param_gen_pos, normalized_depth_control)
          : DxbcShaderTranslator::Modification(0);

  // Set up the render targets - this may perform dispatches and draws.
  {
    Nx1ScopedHitchTimer nx1_rt_update_timer("D3D12.IssueDraw.RenderTargetUpdate",
                                            frame_current_, nx1_draw_probe_call);
    if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,
                                      normalized_color_mask, *vertex_shader)) {
      return nx1_log_draw_drop("render target cache update failed");
    }
  }

  // Create the pipeline (for this, need the actually used render target formats
  // from the render target cache), translating the shaders - doing this now to
  // obtain the used textures.
  D3D12Shader::D3D12Translation* vertex_shader_translation = nullptr;
  D3D12Shader::D3D12Translation* pixel_shader_translation = nullptr;
  {
    Nx1ScopedHitchTimer nx1_shader_translate_timer("D3D12.IssueDraw.ShaderTranslation",
                                                   frame_current_, nx1_draw_probe_call);
    vertex_shader_translation = static_cast<D3D12Shader::D3D12Translation*>(
        vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
    pixel_shader_translation =
        pixel_shader ? static_cast<D3D12Shader::D3D12Translation*>(
                           pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value))
                     : nullptr;
  }
  uint32_t bound_depth_and_color_render_target_bits;
  uint32_t bound_depth_and_color_render_target_formats[1 + xenos::kMaxColorRenderTargets];
  bool host_render_targets_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets;
  if (host_render_targets_used) {
    bound_depth_and_color_render_target_bits =
        render_target_cache_->GetLastUpdateBoundRenderTargets(
            bound_depth_and_color_render_target_formats);
  } else {
    bound_depth_and_color_render_target_bits = 0;
  }
  void* pipeline_handle;
  ID3D12RootSignature* root_signature;
  {
    Nx1ScopedHitchTimer nx1_pipeline_config_timer("D3D12.IssueDraw.PipelineConfig",
                                                  frame_current_, nx1_draw_probe_call);
    if (!pipeline_cache_->ConfigurePipeline(
            vertex_shader_translation, pixel_shader_translation, primitive_processing_result,
            normalized_depth_control, normalized_color_mask,
            bound_depth_and_color_render_target_bits, bound_depth_and_color_render_target_formats,
            &pipeline_handle, &root_signature)) {
      return nx1_log_draw_drop("pipeline configuration failed");
    }
  }
  if (REXCVAR_GET(async_shader_compilation) &&
      pipeline_cache_->GetD3D12PipelineByHandle(pipeline_handle) == nullptr) {
    return nx1_log_draw_skip("async pipeline compilation pending");
  }
  const bool nx1_native_shader = root_signature == root_signature_nx1_native_shader_;
  // Emit a RenderDoc/PIX marker before every native draw so they are trivially
  // findable in a capture (search the Event Browser for "NX1-NATIVE"). Markers
  // auto-enable when a capture tool is attached.
  if (nx1_native_shader) {
    // Tag 3D-world draws (depth test enabled) vs 2D UI/HUD/composite (no depth)
    // so they are easy to find in a capture: filter the Event Browser for
    // "NX1-3D" to land on world geometry.
    const char* kind = pixel_shader == nullptr
                           ? "DEPTHPREPASS"
                           : (normalized_depth_control.z_enable ? "3D" : "2D");
    InsertDebugMarker("NX1-%s VS=%016llX PS=%016llX ztest=%u zwrite=%u", kind,
                      static_cast<unsigned long long>(vertex_shader->ucode_data_hash()),
                      static_cast<unsigned long long>(
                          pixel_shader != nullptr ? pixel_shader->ucode_data_hash() : 0ull),
                      uint32_t(normalized_depth_control.z_enable),
                      uint32_t(normalized_depth_control.z_write_enable));
  }

  // Update the textures - this may bind pipelines.
  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);

  bool nx1_restore_fetch0 = false;
  xenos::xe_gpu_texture_fetch_t nx1_saved_fetch0 = {};
  if (REXCVAR_GET(nx1_bypass_black_scene_composite) && pixel_shader &&
      pixel_shader->ucode_data_hash() == 0xA3B77B5C4D9E1E4Aull && (used_texture_mask & 0x1)) {
    xenos::xe_gpu_texture_fetch_t fetch0 = regs.GetTextureFetch(0);
    if ((fetch0.base_address << 12) == 0x1F938000) {
      static uint64_t nx1_final_composite_bypass_count = 0;
      nx1_saved_fetch0 = fetch0;
      fetch0.dword_0 = 0x88004802;
      fetch0.dword_1 = 0x1F6D8097;
      fetch0.dword_2 = 0x004AE3FF;
      fetch0.dword_3 = 0x01000D11;
      fetch0.dword_4 = 0x00000000;
      fetch0.dword_5 = 0x00000200;
      Nx1WriteTextureFetchConstant(*register_file_, 0, fetch0);
      texture_cache_->TextureFetchConstantWritten(0);
      nx1_restore_fetch0 = true;
      ++nx1_final_composite_bypass_count;
      if (nx1_final_composite_bypass_count <= 16 ||
          (nx1_final_composite_bypass_count % 120) == 0) {
        REXGPU_WARN(
            "[NX1 composite-bypass] final composite fetch0 0x1F938000 -> 0x1F6D8000 "
            "count={} call={}",
            nx1_final_composite_bypass_count, nx1_draw_probe_call);
      }
    }
  }
  {
    Nx1ScopedHitchTimer nx1_texture_request_timer("D3D12.IssueDraw.RequestTextures",
                                                  frame_current_, nx1_draw_probe_call);
    texture_cache_->RequestTextures(used_texture_mask);
  }

  // Bind the pipeline after configuring it and doing everything that may bind
  // other pipelines.
  if (current_guest_pipeline_ != pipeline_handle) {
    deferred_command_list_.SetPipelineStateHandle(reinterpret_cast<void*>(pipeline_handle));
    current_guest_pipeline_ = pipeline_handle;
    current_external_pipeline_ = nullptr;
  }

  // Get dynamic rasterizer state.
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  bool convert_z_to_float24 =
      host_render_targets_used && render_target_cache_->depth_float24_convert_in_pixel_shader();
  bool ps_writes_depth = pixel_shader && pixel_shader->writes_depth();

  draw_util::ViewportInfo viewport_info;
  draw_util::Scissor scissor;
  {
    Nx1ScopedHitchTimer nx1_viewport_state_timer("D3D12.IssueDraw.ViewportState",
                                                 frame_current_, nx1_draw_probe_call);
    // Build a cache key from all viewport-affecting state to skip redundant
    // recalculation when the viewport registers haven't changed between draws.
    ViewportCacheKey viewport_key;
    viewport_key.pa_cl_clip_cntl = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
    viewport_key.pa_cl_vte_cntl = regs[XE_GPU_REG_PA_CL_VTE_CNTL];
    viewport_key.pa_su_sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
    viewport_key.pa_su_vtx_cntl = regs[XE_GPU_REG_PA_SU_VTX_CNTL];
    viewport_key.pa_sc_window_offset = regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET];
    viewport_key.normalized_depth_control = normalized_depth_control.value;
    std::memcpy(viewport_key.vport_regs, &regs[XE_GPU_REG_PA_CL_VPORT_XSCALE],
                sizeof(viewport_key.vport_regs));
    viewport_key.flags = (uint32_t(convert_z_to_float24) << 0) |
                         (uint32_t(host_render_targets_used) << 1) |
                         (uint32_t(ps_writes_depth) << 2);

    if (viewport_cache_valid_ && viewport_key == previous_viewport_key_) {
      viewport_info = previous_viewport_info_;
    } else {
      draw_util::GetHostViewportInfo(regs, draw_resolution_scale_x, draw_resolution_scale_y, true,
                                     D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX, false,
                                     normalized_depth_control, convert_z_to_float24,
                                     host_render_targets_used, ps_writes_depth, viewport_info);
      previous_viewport_key_ = viewport_key;
      previous_viewport_info_ = viewport_info;
      viewport_cache_valid_ = true;
    }

    draw_util::GetScissor(regs, scissor);
    scissor.offset[0] *= draw_resolution_scale_x;
    scissor.offset[1] *= draw_resolution_scale_y;
    scissor.extent[0] *= draw_resolution_scale_x;
    scissor.extent[1] *= draw_resolution_scale_y;

    // Update viewport, scissor, blend factor and stencil reference.
    UpdateFixedFunctionState(viewport_info, scissor, primitive_polygonal,
                             normalized_depth_control);
  }

  // Update system constants before uploading them.
  // TODO(Triang3l): With ROV, pass the disabled render target mask for safety.
  {
    Nx1ScopedHitchTimer nx1_system_constants_timer("D3D12.IssueDraw.SystemConstants",
                                                   frame_current_, nx1_draw_probe_call);
    UpdateSystemConstantValues(memexport_used, primitive_polygonal,
                               primitive_processing_result.line_loop_closing_index,
                               primitive_processing_result.host_shader_index_endian, viewport_info,
                               used_texture_mask, normalized_depth_control, normalized_color_mask);
  }

  // Update constant buffers, descriptors and root parameters.
  {
    Nx1ScopedHitchTimer nx1_update_bindings_timer("D3D12.IssueDraw.UpdateBindings",
                                                  frame_current_, nx1_draw_probe_call);
    if (!UpdateBindings(vertex_shader, pixel_shader, root_signature, memexport_used)) {
      if (nx1_restore_fetch0) {
        Nx1WriteTextureFetchConstant(*register_file_, 0, nx1_saved_fetch0);
        texture_cache_->TextureFetchConstantWritten(0);
      }
      return nx1_log_draw_drop("UpdateBindings failed");
    }
  }
  if (nx1_restore_fetch0) {
    Nx1WriteTextureFetchConstant(*register_file_, 0, nx1_saved_fetch0);
    texture_cache_->TextureFetchConstantWritten(0);
  }
  // Must not call anything that can change the descriptor heap from now on!

  // Ensure vertex buffers are resident.
  {
    Nx1ScopedHitchTimer nx1_vertex_residency_timer("D3D12.IssueDraw.VertexResidency",
                                                   frame_current_, nx1_draw_probe_call);
    const Shader::ConstantRegisterMap& constant_map_vertex = vertex_shader->constant_register_map();
    for (uint32_t i = 0; i < rex::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
      uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
      uint32_t j;
      while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
        vfetch_bits_remaining &= ~(uint32_t(1) << j);
        uint32_t vfetch_index = i * 32 + j;
        uint64_t vfetch_bit = uint64_t(1) << (vfetch_index & 63);
        if (vertex_buffers_in_sync_[vfetch_index >> 6] & vfetch_bit) {
          continue;
        }
        xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
        switch (vfetch_constant.type) {
          case xenos::FetchConstantType::kVertex:
            break;
          case xenos::FetchConstantType::kInvalidVertex:
            if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
              break;
            }
            REXGPU_WARN(
                "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
                "This is incorrect behavior, but you can try bypassing this by "
                "launching Xenia with --gpu_allow_invalid_fetch_constants=true.",
                vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
            return nx1_log_draw_drop("invalid vertex fetch constant");
          default:
            REXGPU_WARN("Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
                        vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
            return nx1_log_draw_drop("malformed vertex fetch constant");
        }
        VertexBufferState& state = vertex_buffer_states_[vfetch_index];
        if (state.address == vfetch_constant.address && state.size == vfetch_constant.size) {
          vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
          continue;
        }
        if (!shared_memory_->RequestRange(vfetch_constant.address << 2,
                                          vfetch_constant.size << 2)) {
          REXGPU_ERROR(
              "Failed to request vertex buffer at 0x{:08X} (size {}) in the "
              "shared memory",
              vfetch_constant.address << 2, vfetch_constant.size << 2);
          return nx1_log_draw_drop("shared-memory vertex buffer request failed");
        }
        state.address = vfetch_constant.address;
        state.size = vfetch_constant.size;
        vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
      }
    }
  }

  {
    Nx1ScopedHitchTimer nx1_memexport_residency_timer(
        "D3D12.IssueDraw.MemexportResidency", frame_current_, nx1_draw_probe_call);
    // Gather memexport ranges and ensure the heaps for them are resident, and
    // also load the data surrounding the export and to fill the regions that
    // won't be modified by the shaders.
    memexport_ranges_.clear();
    if (memexport_used_vertex) {
      draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
    }
    if (memexport_used_pixel) {
      draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
    }
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      if (!shared_memory_->RequestRange(memexport_range.base_address_dwords << 2,
                                        memexport_range.size_bytes)) {
        REXGPU_ERROR(
            "Failed to request memexport stream at 0x{:08X} (size {}) in the "
            "shared memory",
            memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
        return nx1_log_draw_drop("shared-memory memexport request failed");
      }
    }
    if (memexport_used && memexport_ranges_.empty()) {
      if (!shared_memory_->RequestRange(0, SharedMemory::kBufferSize)) {
        REXGPU_ERROR(
            "Failed to request full shared memory residency for unresolved "
            "memexport destinations");
        return nx1_log_draw_drop("full shared-memory memexport request failed");
      }
    }
  }

  {
    Nx1ScopedHitchTimer nx1_topology_timer("D3D12.IssueDraw.Topology", frame_current_,
                                           nx1_draw_probe_call);
    // Primitive topology.
    D3D_PRIMITIVE_TOPOLOGY primitive_topology;
    if (primitive_processing_result.IsTessellated()) {
      switch (primitive_processing_result.host_primitive_type) {
        // TODO(Triang3l): Support all primitive types.
        case xenos::PrimitiveType::kTriangleList:
          primitive_topology = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
          break;
        case xenos::PrimitiveType::kQuadList:
          primitive_topology = D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
          break;
        case xenos::PrimitiveType::kTrianglePatch:
          primitive_topology =
              (regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive)
                  ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                  : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
          break;
        case xenos::PrimitiveType::kQuadPatch:
          primitive_topology =
              (regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive)
                  ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                  : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
          break;
        default:
          REXGPU_ERROR(
              "Host tessellated primitive type {} returned by the primitive "
              "processor is not supported by the Direct3D 12 command processor",
              uint32_t(primitive_processing_result.host_primitive_type));
          assert_unhandled_case(primitive_processing_result.host_primitive_type);
          return nx1_log_draw_drop("unsupported tessellated primitive type");
      }
    } else {
      switch (primitive_processing_result.host_primitive_type) {
        case xenos::PrimitiveType::kPointList:
          primitive_topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
          break;
        case xenos::PrimitiveType::kLineList:
          primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
          break;
        case xenos::PrimitiveType::kLineStrip:
          primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
          break;
        case xenos::PrimitiveType::kTriangleList:
        case xenos::PrimitiveType::kRectangleList:
          primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
          break;
        case xenos::PrimitiveType::kTriangleStrip:
          primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
          break;
        case xenos::PrimitiveType::kQuadList:
          primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
          break;
        default:
          REXGPU_ERROR(
              "Host primitive type {} returned by the primitive processor is not "
              "supported by the Direct3D 12 command processor",
              uint32_t(primitive_processing_result.host_primitive_type));
          assert_unhandled_case(primitive_processing_result.host_primitive_type);
          return nx1_log_draw_drop("unsupported primitive type");
      }
    }
    SetPrimitiveTopology(primitive_topology);
  }
  // Must not call anything that may change the primitive topology from now on!

  auto nx1_log_draw_issue = [&](bool indexed) {
    const uint64_t issued = ++nx1_draw_probe.issued;
    ++nx1_frame_probe.issued;
    if (indexed) {
      ++nx1_frame_probe.indexed_draws;
    }
    if (memexport_used) {
      ++nx1_frame_probe.memexport_draws;
    }
    if (normalized_color_mask) {
      ++nx1_frame_probe.color_draws;
      nx1_frame_probe.color_vertices += primitive_processing_result.host_draw_vertex_count;
      const bool nx1_large_color_draw =
          viewport_info.xy_extent[0] >= 512 && viewport_info.xy_extent[1] >= 300 &&
          scissor.extent[0] >= 256 && scissor.extent[1] >= 150;
      const bool nx1_thin_color_draw =
          (viewport_info.xy_extent[0] >= 128 && viewport_info.xy_extent[1] > 0 &&
           viewport_info.xy_extent[1] <= 96) ||
          (scissor.extent[0] >= 128 && scissor.extent[1] > 0 && scissor.extent[1] <= 96);
      if (nx1_large_color_draw) {
        ++nx1_frame_probe.large_color_draws;
      }
      if (nx1_thin_color_draw) {
        ++nx1_frame_probe.thin_color_draws;
      }
      nx1_frame_probe.last_color_rt_bits = bound_depth_and_color_render_target_bits;
      nx1_frame_probe.last_color_mask = normalized_color_mask;
      nx1_frame_probe.last_color_viewport_width = viewport_info.xy_extent[0];
      nx1_frame_probe.last_color_viewport_height = viewport_info.xy_extent[1];
      nx1_frame_probe.last_color_scissor_width = scissor.extent[0];
      nx1_frame_probe.last_color_scissor_height = scissor.extent[1];
      nx1_frame_probe.last_color_textures = used_texture_mask;
      xenos::xe_gpu_texture_fetch_t fetch0 = regs.GetTextureFetch(0);
      nx1_frame_probe.last_color_fetch0_base = fetch0.base_address << 12;
      nx1_frame_probe.last_color_fetch0_width = fetch0.size_2d.width + 1;
      nx1_frame_probe.last_color_fetch0_height = fetch0.size_2d.height + 1;
      nx1_frame_probe.last_color_fetch0_format = uint32_t(fetch0.format);
      nx1_frame_probe.last_color_fetch0_dwords[0] = fetch0.dword_0;
      nx1_frame_probe.last_color_fetch0_dwords[1] = fetch0.dword_1;
      nx1_frame_probe.last_color_fetch0_dwords[2] = fetch0.dword_2;
      nx1_frame_probe.last_color_fetch0_dwords[3] = fetch0.dword_3;
      nx1_frame_probe.last_color_fetch0_dwords[4] = fetch0.dword_4;
      nx1_frame_probe.last_color_fetch0_dwords[5] = fetch0.dword_5;
      nx1_frame_probe.last_color_vs = vertex_shader->ucode_data_hash();
      nx1_frame_probe.last_color_ps = pixel_shader ? pixel_shader->ucode_data_hash() : uint64_t(0);
      if (nx1_large_color_draw || nx1_thin_color_draw) {
        auto& snapshot = nx1_frame_probe.color_ring[nx1_frame_probe.color_ring_next];
        snapshot = {};
        snapshot.issue = issued;
        snapshot.call = nx1_draw_probe_call;
        snapshot.indexed = indexed;
        snapshot.rt_bits = bound_depth_and_color_render_target_bits;
        snapshot.color_mask = normalized_color_mask;
        snapshot.viewport_width = viewport_info.xy_extent[0];
        snapshot.viewport_height = viewport_info.xy_extent[1];
        snapshot.viewport_x = viewport_info.xy_offset[0];
        snapshot.viewport_y = viewport_info.xy_offset[1];
        snapshot.scissor_width = scissor.extent[0];
        snapshot.scissor_height = scissor.extent[1];
        snapshot.scissor_x = scissor.offset[0];
        snapshot.scissor_y = scissor.offset[1];
        snapshot.texture_mask = used_texture_mask;
        snapshot.guest_indices = index_count;
        snapshot.host_vertices = primitive_processing_result.host_draw_vertex_count;
        snapshot.vs = vertex_shader->ucode_data_hash();
        snapshot.ps = pixel_shader ? pixel_shader->ucode_data_hash() : uint64_t(0);
        for (uint32_t texture_slot = 0; texture_slot < 32 &&
                                        snapshot.texture_count < rex::countof(snapshot.textures);
             ++texture_slot) {
          if (!(used_texture_mask & (uint32_t(1) << texture_slot))) {
            continue;
          }
          xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(texture_slot);
          auto& texture_snapshot = snapshot.textures[snapshot.texture_count++];
          texture_snapshot.slot = texture_slot;
          texture_snapshot.base = fetch.base_address << 12;
          texture_snapshot.width = fetch.size_2d.width + 1;
          texture_snapshot.height = fetch.size_2d.height + 1;
          texture_snapshot.format = uint32_t(fetch.format);
          texture_snapshot.dwords[0] = fetch.dword_0;
          texture_snapshot.dwords[1] = fetch.dword_1;
          texture_snapshot.dwords[2] = fetch.dword_2;
          texture_snapshot.dwords[3] = fetch.dword_3;
          texture_snapshot.dwords[4] = fetch.dword_4;
          texture_snapshot.dwords[5] = fetch.dword_5;
        }
        nx1_frame_probe.color_ring_next =
            (nx1_frame_probe.color_ring_next + 1) % rex::countof(nx1_frame_probe.color_ring);
        if (nx1_frame_probe.color_ring_count < rex::countof(nx1_frame_probe.color_ring)) {
          ++nx1_frame_probe.color_ring_count;
        }
      }

      const bool nx1_composite_target_candidate =
          bound_depth_and_color_render_target_bits == 0x3 && normalized_color_mask == 0x7 &&
          viewport_info.xy_extent[0] == 1024 && viewport_info.xy_extent[1] == 600;
      if (REXCVAR_GET(nx1_gpu_probe_logging) && nx1_composite_target_candidate) {
        static uint64_t nx1_composite_target_state_count = 0;
        ++nx1_composite_target_state_count;
        if (nx1_composite_target_state_count <= 32 ||
            (nx1_composite_target_state_count % 240) == 0) {
          const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
          const auto color_info0 =
              regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
          const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
          const auto depth_control_raw = regs.Get<reg::RB_DEPTHCONTROL>();
          const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
          const auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
          const uint32_t color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
          const uint32_t blend0 = regs[reg::RB_BLENDCONTROL::rt_register_indices[0]];
          Nx1DrawProbeStats::FrameStats::TextureSnapshot texture_log[4] = {};
          uint32_t texture_log_count = 0;
          for (uint32_t texture_slot = 0;
               texture_slot < 32 && texture_log_count < rex::countof(texture_log);
               ++texture_slot) {
            if (!(used_texture_mask & (uint32_t(1) << texture_slot))) {
              continue;
            }
            xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(texture_slot);
            auto& texture_snapshot = texture_log[texture_log_count++];
            texture_snapshot.slot = texture_slot;
            texture_snapshot.base = fetch.base_address << 12;
            texture_snapshot.width = fetch.size_2d.width + 1;
            texture_snapshot.height = fetch.size_2d.height + 1;
            texture_snapshot.format = uint32_t(fetch.format);
            texture_snapshot.dwords[0] = fetch.dword_0;
            texture_snapshot.dwords[1] = fetch.dword_1;
            texture_snapshot.dwords[2] = fetch.dword_2;
            texture_snapshot.dwords[3] = fetch.dword_3;
            texture_snapshot.dwords[4] = fetch.dword_4;
            texture_snapshot.dwords[5] = fetch.dword_5;
          }
          const auto& texture0 = texture_log[0];
          const auto& texture1 = texture_log[1];
          const auto& texture2 = texture_log[2];
          const auto& texture3 = texture_log[3];
          REXGPU_WARN(
              "[NX1 composite-target-state] #{} call={} vs={:016X} ps={:016X} "
              "rt_bits=0x{:X} bound_fmt0={} surface_pitch={} msaa={} color0=0x{:08X} "
              "base={} base_bit11={} fmt={} exp={} depth=0x{:08X} depth_base={} "
              "depth_fmt={} depth_raw=0x{:08X} depth_norm=0x{:08X} z={} zw={} "
              "zfunc={} stencil={} color_mask=0x{:08X} blend0=0x{:08X} "
              "colorcontrol=0x{:08X} stencilrefmask=0x{:08X}",
              nx1_composite_target_state_count, nx1_draw_probe_call,
              vertex_shader->ucode_data_hash(),
              pixel_shader ? pixel_shader->ucode_data_hash() : uint64_t(0),
              bound_depth_and_color_render_target_bits,
              bound_depth_and_color_render_target_formats[1], surface_info.surface_pitch,
              uint32_t(surface_info.msaa_samples), color_info0.value, color_info0.color_base,
              color_info0.color_base_bit_11, uint32_t(color_info0.color_format),
              int32_t(color_info0.color_exp_bias), depth_info.value, depth_info.depth_base,
              uint32_t(depth_info.depth_format), depth_control_raw.value,
              normalized_depth_control.value, uint32_t(normalized_depth_control.z_enable),
              uint32_t(normalized_depth_control.z_write_enable),
              uint32_t(normalized_depth_control.zfunc),
              uint32_t(normalized_depth_control.stencil_enable), color_mask, blend0,
              color_control.value, stencil_ref_mask.value);
          REXGPU_WARN(
              "[NX1 composite-target-fetches] #{} call={} indexed={} prim={} host_prim={} "
              "guest_indices={} host_vertices={} textures=0x{:08X} texture_count={} "
              "f0=s{} 0x{:08X} {}x{} fmt={} {:08X}:{:08X}:{:08X}:{:08X}:{:08X}:{:08X} "
              "f1=s{} 0x{:08X} {}x{} fmt={} {:08X}:{:08X}:{:08X}:{:08X}:{:08X}:{:08X} "
              "f2=s{} 0x{:08X} {}x{} fmt={} {:08X}:{:08X}:{:08X}:{:08X}:{:08X}:{:08X} "
              "f3=s{} 0x{:08X} {}x{} fmt={} {:08X}:{:08X}:{:08X}:{:08X}:{:08X}:{:08X}",
              nx1_composite_target_state_count, nx1_draw_probe_call, indexed,
              uint32_t(primitive_type),
              uint32_t(primitive_processing_result.host_primitive_type), index_count,
              primitive_processing_result.host_draw_vertex_count, used_texture_mask,
              texture_log_count, texture0.slot, texture0.base, texture0.width, texture0.height,
              texture0.format, texture0.dwords[0], texture0.dwords[1], texture0.dwords[2],
              texture0.dwords[3], texture0.dwords[4], texture0.dwords[5], texture1.slot,
              texture1.base, texture1.width, texture1.height, texture1.format,
              texture1.dwords[0], texture1.dwords[1], texture1.dwords[2], texture1.dwords[3],
              texture1.dwords[4], texture1.dwords[5], texture2.slot, texture2.base,
              texture2.width, texture2.height, texture2.format, texture2.dwords[0],
              texture2.dwords[1], texture2.dwords[2], texture2.dwords[3], texture2.dwords[4],
              texture2.dwords[5], texture3.slot, texture3.base, texture3.width,
              texture3.height, texture3.format, texture3.dwords[0], texture3.dwords[1],
              texture3.dwords[2], texture3.dwords[3], texture3.dwords[4],
              texture3.dwords[5]);
        }
      }
    } else if (is_rasterization_done) {
      ++nx1_frame_probe.depth_only_draws;
    }
    if (Nx1ShouldLogDrawProbe(issued)) {
      REXGPU_INFO(
          "[NX1 draw-probe] issued draw #{} call #{}: indexed={} prim={} host_prim={} "
          "guest_indices={} host_vertices={} vs={:016X} ps={:016X} textures=0x{:08X} "
          "viewport={}x{}+{},{} scissor={}x{}+{},{} rt_bits=0x{:X} color_mask=0x{:X} "
          "raster={} memexport={}",
          issued, nx1_draw_probe_call, indexed, uint32_t(primitive_type),
          uint32_t(primitive_processing_result.host_primitive_type), index_count,
          primitive_processing_result.host_draw_vertex_count, vertex_shader->ucode_data_hash(),
          pixel_shader ? pixel_shader->ucode_data_hash() : uint64_t(0), used_texture_mask,
          viewport_info.xy_extent[0], viewport_info.xy_extent[1], viewport_info.xy_offset[0],
          viewport_info.xy_offset[1], scissor.extent[0], scissor.extent[1], scissor.offset[0],
          scissor.offset[1], bound_depth_and_color_render_target_bits, normalized_color_mask,
          is_rasterization_done, memexport_used);
    }
  };

  // Draw.
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    if (memexport_used) {
      shared_memory_->UseForWriting();
    } else {
      shared_memory_->UseForReading();
      if (nx1_native_shader && !SetNx1NativeVertexBuffers(vertex_shader)) {
        return nx1_log_draw_drop("native vertex buffer binding failed");
      }
    }
    {
      Nx1ScopedHitchTimer nx1_draw_submit_timer(
          "D3D12.IssueDraw.DrawInstancedSubmitBarriers", frame_current_, nx1_draw_probe_call);
      SubmitBarriers();
    }
    PROFILE_DRAW_CALL();
    PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
    {
      Nx1ScopedHitchTimer nx1_draw_submit_timer("D3D12.IssueDraw.DrawProbeLog",
                                                frame_current_, nx1_draw_probe_call);
      nx1_log_draw_issue(false);
    }
    {
      Nx1ScopedHitchTimer nx1_draw_submit_timer("D3D12.IssueDraw.DrawInstancedCommand",
                                                frame_current_, nx1_draw_probe_call);
      deferred_command_list_.D3DDrawInstanced(primitive_processing_result.host_draw_vertex_count, 1,
                                              0, 0);
    }
  } else {
    D3D12_INDEX_BUFFER_VIEW index_buffer_view;
    ID3D12Resource* scratch_index_buffer = nullptr;
    {
      Nx1ScopedHitchTimer nx1_index_setup_timer("D3D12.IssueDraw.IndexBufferSetup",
                                                frame_current_, nx1_draw_probe_call);
      index_buffer_view.SizeInBytes = primitive_processing_result.host_draw_vertex_count;
      if (primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16) {
        index_buffer_view.SizeInBytes *= sizeof(uint16_t);
        index_buffer_view.Format = DXGI_FORMAT_R16_UINT;
      } else {
        index_buffer_view.SizeInBytes *= sizeof(uint32_t);
        index_buffer_view.Format = DXGI_FORMAT_R32_UINT;
      }
      switch (primitive_processing_result.index_buffer_type) {
        case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA: {
          if (memexport_used) {
            // If the shared memory is a UAV, it can't be used as an index buffer
            // (UAV is a read/write state, index buffer is a read-only state).
            // Need to copy the indices to a buffer in the index buffer state.
            scratch_index_buffer = RequestScratchGPUBuffer(index_buffer_view.SizeInBytes,
                                                           D3D12_RESOURCE_STATE_COPY_DEST);
            if (scratch_index_buffer == nullptr) {
              return nx1_log_draw_drop("scratch index buffer allocation failed");
            }
            shared_memory_->UseAsCopySource();
            SubmitBarriers();
            deferred_command_list_.D3DCopyBufferRegion(
                scratch_index_buffer, 0, shared_memory_->GetBuffer(),
                primitive_processing_result.guest_index_base, index_buffer_view.SizeInBytes);
            PushTransitionBarrier(scratch_index_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_INDEX_BUFFER);
            index_buffer_view.BufferLocation = scratch_index_buffer->GetGPUVirtualAddress();
          } else {
            index_buffer_view.BufferLocation =
                shared_memory_->GetGPUAddress() + primitive_processing_result.guest_index_base;
          }
        } break;
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
          index_buffer_view.BufferLocation =
              primitive_processor_->GetConvertedIndexBufferGpuAddress(
                  primitive_processing_result.host_index_buffer_handle);
          break;
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
          index_buffer_view.BufferLocation =
              primitive_processor_->GetBuiltinIndexBufferGpuAddress(
                  primitive_processing_result.host_index_buffer_handle);
          break;
        default:
          assert_unhandled_case(primitive_processing_result.index_buffer_type);
          return nx1_log_draw_drop("unsupported index buffer type");
      }
    }
    deferred_command_list_.D3DIASetIndexBuffer(&index_buffer_view);
    if (memexport_used) {
      shared_memory_->UseForWriting();
    } else {
      shared_memory_->UseForReading();
      if (nx1_native_shader && !SetNx1NativeVertexBuffers(vertex_shader)) {
        return nx1_log_draw_drop("native vertex buffer binding failed");
      }
    }
    {
      Nx1ScopedHitchTimer nx1_draw_submit_timer(
          "D3D12.IssueDraw.DrawIndexedSubmitBarriers", frame_current_, nx1_draw_probe_call);
      SubmitBarriers();
    }
    PROFILE_DRAW_CALL();
    PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
    {
      Nx1ScopedHitchTimer nx1_draw_submit_timer("D3D12.IssueDraw.DrawProbeLog",
                                                frame_current_, nx1_draw_probe_call);
      nx1_log_draw_issue(true);
    }
    {
      Nx1ScopedHitchTimer nx1_draw_submit_timer("D3D12.IssueDraw.DrawIndexedCommand",
                                                frame_current_, nx1_draw_probe_call);
      deferred_command_list_.D3DDrawIndexedInstanced(
          primitive_processing_result.host_draw_vertex_count, 1, 0, 0, 0);
    }
    if (scratch_index_buffer != nullptr) {
      Nx1ScopedHitchTimer nx1_scratch_release_timer("D3D12.IssueDraw.ScratchIndexRelease",
                                                    frame_current_, nx1_draw_probe_call);
      ReleaseScratchGPUBuffer(scratch_index_buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
  }

  if (memexport_used) {
    // Make sure this memexporting draw is ordered with other work using shared
    // memory as a UAV.
    // TODO(Triang3l): Find some PM4 command that can be used for indication of
    // when memexports should be awaited?
    shared_memory_->MarkUAVWritesCommitNeeded();
    // Invalidate textures in memexported memory and watch for changes.
    if (!memexport_ranges_.empty()) {
      for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
        shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                          memexport_range.size_bytes);
      }
    } else {
      // Stream constants can be invalid or dynamic, so exact destinations may
      // be unknown. Keep invalidation conservative in this case.
      shared_memory_->RangeWrittenByGpu(0, SharedMemory::kBufferSize);
    }
    if (IsReadbackMemexportEnabled(REXCVAR_GET(d3d12_readback_memexport)) &&
        !memexport_ranges_.empty()) {
      uint32_t memexport_total_size = 0;
      for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
        memexport_total_size += memexport_range.size_bytes;
      }
      if (memexport_total_size != 0) {
        Nx1ScopedHitchTimer nx1_memexport_readback_timer(
            "D3D12.IssueDraw.MemexportReadback", frame_current_, nx1_draw_probe_call);
        if (REXCVAR_GET(readback_memexport_fast)) {
          IssueDraw_MemexportReadbackFastPath(memexport_total_size);
        } else {
          IssueDraw_MemexportReadbackFullPath(memexport_total_size);
        }
      }
    }
  }

  return true;
}

bool D3D12CommandProcessor::IssueDraw_MemexportReadbackFullPath(uint32_t total_size) {
  Nx1ScopedHitchTimer nx1_memexport_full_timer("D3D12.MemexportReadback.FullPath",
                                               frame_current_, total_size);
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  ID3D12Resource* readback_buffer = RequestReadbackBuffer(total_size);
  if (!readback_buffer) {
    return true;
  }

  shared_memory_->UseAsCopySource();
  SubmitBarriers();
  ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
  uint32_t readback_buffer_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    deferred_command_list_.D3DCopyBufferRegion(
        readback_buffer, readback_buffer_offset, shared_memory_buffer,
        memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    readback_buffer_offset += memexport_range.size_bytes;
  }

  if (!AwaitAllQueueOperationsCompletion()) {
    return true;
  }

  D3D12_RANGE readback_range = {};
  readback_range.Begin = 0;
  readback_range.End = total_size;
  void* readback_mapping = nullptr;
  if (FAILED(readback_buffer->Map(0, &readback_range, &readback_mapping))) {
    return true;
  }

  const uint8_t* readback_bytes = reinterpret_cast<const uint8_t*>(readback_mapping);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }

  D3D12_RANGE readback_write_range = {};
  readback_buffer->Unmap(0, &readback_write_range);
  return true;
}

bool D3D12CommandProcessor::IssueDraw_MemexportReadbackFastPath(uint32_t total_size) {
  Nx1ScopedHitchTimer nx1_memexport_fast_timer("D3D12.MemexportReadback.FastPath",
                                               frame_current_, total_size);
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  const uint64_t readback_key =
      MakeMemexportReadbackKey(memexport_ranges_.front().base_address_dwords, total_size);
  ReadbackBuffer& readback = memexport_readback_buffers_[readback_key];
  readback.last_used_frame = frame_current_;

  auto ensure_readback_slot = [&](uint32_t index, uint32_t size) -> bool {
    if (readback.buffers[index] && readback.mapped_data[index] && size <= readback.sizes[index]) {
      return true;
    }

    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      return false;
    }

    D3D12_RANGE read_range = {0, size};
    void* mapped_data = nullptr;
    if (FAILED(buffer->Map(0, &read_range, &mapped_data))) {
      buffer->Release();
      return false;
    }

    if (readback.buffers[index]) {
      if (!AwaitAllQueueOperationsCompletion()) {
        buffer->Unmap(0, nullptr);
        buffer->Release();
        return false;
      }
      if (readback.mapped_data[index]) {
        readback.buffers[index]->Unmap(0, nullptr);
      }
      readback.buffers[index]->Release();
    }

    readback.buffers[index] = buffer;
    readback.mapped_data[index] = mapped_data;
    readback.sizes[index] = size;
    readback.submission_written[index] = 0;
    readback.written_size[index] = 0;
    return true;
  };

  const uint32_t write_index = readback.current_index;
  const uint32_t read_index = 1 - write_index;
  const uint32_t readback_size = AlignReadbackBufferSize(total_size);
  if (!ensure_readback_slot(write_index, readback_size)) {
    return IssueDraw_MemexportReadbackFullPath(total_size);
  }

  shared_memory_->UseAsCopySource();
  SubmitBarriers();
  ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
  uint32_t readback_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    deferred_command_list_.D3DCopyBufferRegion(
        readback.buffers[write_index], readback_offset, shared_memory_buffer,
        memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    readback_offset += memexport_range.size_bytes;
  }
  readback.submission_written[write_index] = submission_current_;
  readback.written_size[write_index] = total_size;

  CheckSubmissionFence(0);
  bool previous_slot_ready = readback.buffers[read_index] && readback.mapped_data[read_index] &&
                             total_size <= readback.sizes[read_index] &&
                             total_size <= readback.written_size[read_index] &&
                             readback.submission_written[read_index] &&
                             readback.submission_written[read_index] <= submission_completed_;
  if (!previous_slot_ready) {
    IssueDraw_MemexportReadbackFullPath(total_size);
    readback.current_index = read_index;
    return true;
  }

  const uint8_t* readback_bytes = static_cast<const uint8_t*>(readback.mapped_data[read_index]);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }
  readback.current_index = read_index;
  return true;
}

void D3D12CommandProcessor::InitializeTrace() {
  CommandProcessor::InitializeTrace();

  if (!BeginSubmission(false)) {
    return;
  }
  bool render_target_cache_submitted = render_target_cache_->InitializeTraceSubmitDownloads();
  bool shared_memory_submitted = shared_memory_->InitializeTraceSubmitDownloads();
  if (!render_target_cache_submitted && !shared_memory_submitted) {
    return;
  }
  AwaitAllQueueOperationsCompletion();
  if (render_target_cache_submitted) {
    render_target_cache_->InitializeTraceCompleteDownloads();
  }
  if (shared_memory_submitted) {
    shared_memory_->InitializeTraceCompleteDownloads();
  }
}

bool D3D12CommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  Nx1ScopedHitchTimer nx1_issue_copy_timer("D3D12.IssueCopy", frame_current_,
                                           Nx1DrawProbe().frame.copy_packets);
  if (!BeginSubmission(true)) {
    return false;
  }
  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(d3d12_readback_resolve));
  if (readback_mode == ReadbackResolveMode::kDisabled) {
    uint32_t written_address = 0, written_length = 0;
    bool resolved = false;
    {
      Nx1ScopedHitchTimer nx1_resolve_timer("D3D12.IssueCopy.Resolve", frame_current_,
                                            Nx1DrawProbe().frame.copy_packets);
      resolved = render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                               written_address, written_length);
    }
    Nx1RecordFrameResolve(Nx1DrawProbe().frame, resolved, written_address, written_length);
    return resolved;
  }
  return IssueCopy_ReadbackResolvePath();
}

bool D3D12CommandProcessor::IssueCopy_ReadbackResolvePath() {
  Nx1ScopedHitchTimer nx1_readback_resolve_timer("D3D12.IssueCopy.ReadbackResolvePath",
                                                 frame_current_,
                                                 Nx1DrawProbe().frame.copy_packets);
  uint32_t written_address = 0, written_length = 0;
  bool resolved = false;
  {
    Nx1ScopedHitchTimer nx1_resolve_timer("D3D12.IssueCopy.ResolveReadback", frame_current_,
                                          Nx1DrawProbe().frame.copy_packets);
    resolved = render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                             written_address, written_length);
  }
  if (!resolved) {
    Nx1RecordFrameResolve(Nx1DrawProbe().frame, false, written_address, written_length);
    return false;
  }
  Nx1RecordFrameResolve(Nx1DrawProbe().frame, true, written_address, written_length);

  if (!written_length) {
    return true;
  }

  if (!memory_->TranslatePhysical(written_address)) {
    return true;
  }

  bool is_scaled = texture_cache_->IsDrawResolutionScaled();
  uint64_t resolve_key = MakeReadbackResolveKey(written_address, written_length);
  ReadbackBuffer& rb = readback_buffers_[resolve_key];
  rb.last_used_frame = frame_current_;

  uint32_t write_index = rb.current_index;
  uint32_t size = AlignReadbackBufferSize(written_length);

  if (size > rb.sizes[write_index]) {
    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      REXGPU_ERROR("Failed to create a {} MB readback buffer", size >> 20);
      return true;
    }
    if (rb.buffers[write_index]) {
      if (rb.mapped_data[write_index]) {
        rb.buffers[write_index]->Unmap(0, nullptr);
        rb.mapped_data[write_index] = nullptr;
      }
      rb.buffers[write_index]->Release();
    }
    rb.buffers[write_index] = buffer;
    rb.sizes[write_index] = size;
    D3D12_RANGE read_range = {0, size};
    if (FAILED(buffer->Map(0, &read_range, &rb.mapped_data[write_index]))) {
      REXGPU_ERROR("Failed to persistently map resolve readback buffer");
      rb.mapped_data[write_index] = nullptr;
    }
  }

  if (!rb.buffers[write_index]) {
    return true;
  }

  if (is_scaled) {
    if (!resolve_downscale_pipeline_ || !resolve_downscale_root_signature_) {
      return true;
    }

    reg::RB_COPY_DEST_INFO copy_dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
    const FormatInfo* format_info = FormatInfo::Get(uint32_t(copy_dest_info.copy_dest_format));
    uint32_t bits_per_pixel = format_info->bits_per_pixel;
    if (bits_per_pixel != 8 && bits_per_pixel != 16 && bits_per_pixel != 32 &&
        bits_per_pixel != 64) {
      return true;
    }

    uint32_t pixel_size_log2;
    if (!rex::bit_scan_forward(bits_per_pixel >> 3, &pixel_size_log2)) {
      return true;
    }
    uint32_t tile_size_1x = 32 * 32 * (uint32_t(1) << pixel_size_log2);
    uint32_t tile_count = written_length / tile_size_1x;
    if (!tile_count) {
      return true;
    }

    uint32_t scaled_length = uint32_t(texture_cache_->GetCurrentScaledResolveRangeLengthScaled());
    uint64_t scaled_address = texture_cache_->GetCurrentScaledResolveRangeStartScaled();
    if (!scaled_length) {
      return true;
    }

    uint32_t downscale_buffer_size = AlignReadbackBufferSize(written_length);
    if (downscale_buffer_size > resolve_downscale_buffer_size_) {
      const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
      ID3D12Device* device = provider.GetDevice();
      D3D12_RESOURCE_DESC buffer_desc;
      ui::d3d12::util::FillBufferResourceDesc(buffer_desc, downscale_buffer_size,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      ID3D12Resource* buffer = nullptr;
      if (FAILED(device->CreateCommittedResource(
              &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
              &buffer_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
              IID_PPV_ARGS(&buffer)))) {
        REXGPU_ERROR("Failed to create a {} MB resolve downscale buffer",
                     downscale_buffer_size >> 20);
        return true;
      }
      if (resolve_downscale_buffer_) {
        resources_for_deletion_.emplace_back(GetCurrentSubmission(),
                                             resolve_downscale_buffer_.Detach());
      }
      resolve_downscale_buffer_.Attach(buffer);
      resolve_downscale_buffer_size_ = downscale_buffer_size;
    }

    if (!resolve_downscale_buffer_) {
      return true;
    }

    ID3D12Resource* scaled_resolve_buffer = texture_cache_->GetCurrentScaledResolveBufferResource();
    size_t scaled_resolve_buffer_index = texture_cache_->GetCurrentScaledResolveBufferIndexPublic();
    if (!scaled_resolve_buffer) {
      return true;
    }
    uint64_t scaled_buffer_base = uint64_t(scaled_resolve_buffer_index) << 30;
    if (scaled_address < scaled_buffer_base) {
      return true;
    }
    uint64_t source_offset = scaled_address - scaled_buffer_base;

    ui::d3d12::util::DescriptorCpuGpuHandlePair downscale_descriptors[2];
    if (!RequestOneUseSingleViewDescriptors(2, downscale_descriptors)) {
      return true;
    }

    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    uint32_t aligned_scaled_length =
        rex::align(scaled_length, uint32_t(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT));
    ui::d3d12::util::CreateBufferRawSRV(device, downscale_descriptors[0].first,
                                        scaled_resolve_buffer, aligned_scaled_length,
                                        source_offset);
    uint32_t aligned_written_length =
        rex::align(written_length, uint32_t(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT));
    ui::d3d12::util::CreateBufferRawUAV(device, downscale_descriptors[1].first,
                                        resolve_downscale_buffer_.Get(), aligned_written_length, 0);

    PushUAVBarrier(scaled_resolve_buffer);
    texture_cache_->TransitionCurrentScaledResolveRange(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    SubmitBarriers();

    SetExternalPipeline(resolve_downscale_pipeline_.Get());
    deferred_command_list_.D3DSetComputeRootSignature(resolve_downscale_root_signature_.Get());
    ResolveDownscaleConstants constants;
    constants.scale_x = texture_cache_->draw_resolution_scale_x();
    constants.scale_y = texture_cache_->draw_resolution_scale_y();
    constants.pixel_size_log2 = pixel_size_log2;
    constants.tile_count = tile_count;
    constants.half_pixel_offset = (REXCVAR_GET(readback_resolve_half_pixel_offset) &&
                                   (constants.scale_x > 1 || constants.scale_y > 1))
                                      ? 1u
                                      : 0u;
    deferred_command_list_.D3DSetComputeRoot32BitConstants(
        UINT(ResolveDownscaleRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t),
        &constants, 0);
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(ResolveDownscaleRootParameter::kSource), downscale_descriptors[0].second);
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(ResolveDownscaleRootParameter::kDestination), downscale_descriptors[1].second);
    {
      Nx1ScopedHitchTimer nx1_downscale_dispatch_timer(
          "D3D12.IssueCopy.ReadbackResolveDownscale", frame_current_, written_length);
      deferred_command_list_.D3DDispatch(tile_count, 1, 1);
    }

    PushUAVBarrier(resolve_downscale_buffer_.Get());
    PushTransitionBarrier(resolve_downscale_buffer_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_SOURCE);
    SubmitBarriers();
    deferred_command_list_.D3DCopyBufferRegion(rb.buffers[write_index], 0,
                                               resolve_downscale_buffer_.Get(), 0, written_length);
    PushTransitionBarrier(resolve_downscale_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    texture_cache_->TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SubmitBarriers();
  } else {
    shared_memory_->UseAsCopySource();
    SubmitBarriers();
    ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
    deferred_command_list_.D3DCopyBufferRegion(rb.buffers[write_index], 0, shared_memory_buffer,
                                               written_address, written_length);
  }

  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(d3d12_readback_resolve));
  bool use_delayed_sync =
      readback_mode == ReadbackResolveMode::kFast || readback_mode == ReadbackResolveMode::kSome;
  uint32_t read_index = write_index;
  if (use_delayed_sync) {
    read_index = 1 - write_index;
  } else if (!AwaitAllQueueOperationsCompletion()) {
    return true;
  }

  bool is_cache_miss = false;
  if (use_delayed_sync && (!rb.buffers[read_index] || written_length > rb.sizes[read_index] ||
                           !rb.mapped_data[read_index])) {
    is_cache_miss = true;
    read_index = write_index;
    if (!AwaitAllQueueOperationsCompletion()) {
      return true;
    }
  }

  bool should_copy = (readback_mode == ReadbackResolveMode::kSome) ? is_cache_miss : true;
  if (should_copy && rb.buffers[read_index] && written_length <= rb.sizes[read_index] &&
      rb.mapped_data[read_index]) {
    Nx1LogResolveReadbackSamples(written_address, written_length, rb.mapped_data[read_index]);
    uint8_t* destination = memory_->TranslatePhysical(written_address);
    if (destination) {
      std::memcpy(destination, static_cast<uint8_t*>(rb.mapped_data[read_index]), written_length);
    }
  }

  rb.current_index = 1 - rb.current_index;
  return true;
}

void D3D12CommandProcessor::CheckSubmissionFence(uint64_t await_submission) {
  if (await_submission >= submission_current_) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // Ending an open submission should result in queue operations done directly
    // (like UpdateTileMappings) to be tracked within the scope of that
    // submission, but just in case of a failure, or queue operations being done
    // outside of a submission, await explicitly.
    if (queue_operations_done_since_submission_signal_) {
      UINT64 fence_value = ++queue_operations_since_submission_fence_last_;
      ID3D12CommandQueue* direct_queue = GetD3D12Provider().GetDirectQueue();
      if (SUCCEEDED(direct_queue->Signal(queue_operations_since_submission_fence_, fence_value) &&
                    SUCCEEDED(queue_operations_since_submission_fence_->SetEventOnCompletion(
                        fence_value, fence_completion_event_)))) {
        PROFILE_CMD_BUFFER_STALL();
        WaitForSingleObject(fence_completion_event_, INFINITE);
        queue_operations_done_since_submission_signal_ = false;
      } else {
        REXGPU_ERROR(
            "Failed to await an out-of-submission queue operation completion "
            "Direct3D 12 fence");
      }
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = submission_current_ - 1;
  }

  uint64_t submission_completed_before = submission_completed_;
  submission_completed_ = submission_fence_->GetCompletedValue();
  if (submission_completed_ < await_submission) {
    if (SUCCEEDED(
            submission_fence_->SetEventOnCompletion(await_submission, fence_completion_event_))) {
      PROFILE_CMD_BUFFER_STALL();
      WaitForSingleObject(fence_completion_event_, INFINITE);
      submission_completed_ = submission_fence_->GetCompletedValue();
    }
  }
  if (submission_completed_ < await_submission) {
    REXGPU_ERROR("Failed to await a submission completion Direct3D 12 fence");
  }
  if (submission_completed_ <= submission_completed_before) {
    // Not updated - no need to reclaim or download things.
    return;
  }

  // Reclaim command allocators.
  while (command_allocator_submitted_first_) {
    if (command_allocator_submitted_first_->last_usage_submission > submission_completed_) {
      break;
    }
    if (command_allocator_writable_last_) {
      command_allocator_writable_last_->next = command_allocator_submitted_first_;
    } else {
      command_allocator_writable_first_ = command_allocator_submitted_first_;
    }
    command_allocator_writable_last_ = command_allocator_submitted_first_;
    command_allocator_submitted_first_ = command_allocator_submitted_first_->next;
    command_allocator_writable_last_->next = nullptr;
  }
  if (!command_allocator_submitted_first_) {
    command_allocator_submitted_last_ = nullptr;
  }

  // Release single-use bindless descriptors.
  while (!view_bindless_one_use_descriptors_.empty()) {
    if (view_bindless_one_use_descriptors_.front().second > submission_completed_) {
      break;
    }
    ReleaseViewBindlessDescriptorImmediately(view_bindless_one_use_descriptors_.front().first);
    view_bindless_one_use_descriptors_.pop_front();
  }
  if (native_present_view_heap_pool_) {
    native_present_view_heap_pool_->Reclaim(submission_completed_);
  }

  // Delete transient resources marked for deletion.
  while (!resources_for_deletion_.empty()) {
    if (resources_for_deletion_.front().first > submission_completed_) {
      break;
    }
    resources_for_deletion_.front().second->Release();
    resources_for_deletion_.pop_front();
  }

  shared_memory_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  primitive_processor_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(submission_completed_);
}

void D3D12CommandProcessor::LogDeviceRemovalDiagnostics(ID3D12Device* device, HRESULT reason) {
  const char* reason_str = "Unknown";
  switch (reason) {
    case DXGI_ERROR_DEVICE_HUNG:
      reason_str = "DEVICE_HUNG (TDR - GPU command took too long)";
      break;
    case DXGI_ERROR_DEVICE_REMOVED:
      reason_str = "DEVICE_REMOVED (driver internal error or hot-unplug)";
      break;
    case DXGI_ERROR_DEVICE_RESET:
      reason_str = "DEVICE_RESET (bad GPU command)";
      break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
      reason_str = "DRIVER_INTERNAL_ERROR";
      break;
    case DXGI_ERROR_INVALID_CALL:
      reason_str = "INVALID_CALL";
      break;
  }
  REXGPU_ERROR("D3D12 device removed: HRESULT 0x{:08X} - {}", static_cast<unsigned>(reason),
               reason_str);

  Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    return;
  }

  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node;
         node = node->pNext) {
      if (!node->pLastBreadcrumbValue || !node->pCommandHistory ||
          *node->pLastBreadcrumbValue == 0) {
        continue;
      }
      REXGPU_ERROR("DRED breadcrumb: completed {} of {} ops", *node->pLastBreadcrumbValue,
                   node->BreadcrumbCount);
      uint32_t last = std::min(*node->pLastBreadcrumbValue, node->BreadcrumbCount);
      uint32_t start = last > 3 ? last - 3 : 0;
      uint32_t end = std::min(last + 1, node->BreadcrumbCount);
      for (uint32_t i = start; i < end; i++) {
        REXGPU_ERROR("  [{}] op type {}{}", i, static_cast<int>(node->pCommandHistory[i]),
                     i == last ? " <-- FAULT" : "");
      }
    }
  }

  D3D12_DRED_PAGE_FAULT_OUTPUT page_fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&page_fault)) && page_fault.PageFaultVA != 0) {
    REXGPU_ERROR("DRED page fault at VA 0x{:016X}", page_fault.PageFaultVA);
  }
}

bool D3D12CommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_removed_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // Check if the device is still available.
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  HRESULT device_removed_reason = device->GetDeviceRemovedReason();
  if (FAILED(device_removed_reason)) {
    device_removed_ = true;
    LogDeviceRemovalDiagnostics(device, device_removed_reason);
    if (graphics_system_) {
      graphics_system_->OnHostGpuLossFromAnyThread(device_removed_reason !=
                                                   DXGI_ERROR_DEVICE_REMOVED);
    }
    return false;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame.
  CheckSubmissionFence(is_opening_frame ? closed_frame_submissions_[frame_current_ % kQueueFrames]
                                        : 0);
  // TODO(Triang3l): If failed to await (completed submission < awaited frame
  // submission), do something like dropping the draw command that wanted to
  // open the frame.
  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ = std::max(frame_current_, uint64_t(kQueueFrames)) - kQueueFrames;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_; ++frame) {
      if (closed_frame_submissions_[frame % kQueueFrames] > submission_completed_) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;

    // Start a new deferred command list - will submit it to the real one in the
    // end of the submission (when async pipeline creation requests are
    // fulfilled).
    deferred_command_list_.Reset();

    // Reset cached state of the command list.
    ff_viewport_update_needed_ = true;
    ff_scissor_update_needed_ = true;
    ff_blend_factor_update_needed_ = true;
    ff_stencil_ref_update_needed_ = true;
    viewport_cache_valid_ = false;
    current_guest_pipeline_ = nullptr;
    current_external_pipeline_ = nullptr;
    current_graphics_root_signature_ = nullptr;
    current_graphics_root_up_to_date_ = 0;
    if (bindless_resources_used_) {
      deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                sampler_bindless_heap_current_);
    } else {
      view_bindful_heap_current_ = nullptr;
      sampler_bindful_heap_current_ = nullptr;
    }
    primitive_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    render_target_cache_->BeginSubmission();

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(submission_current_);
  }

  if (is_opening_frame) {
    frame_open_ = true;

    // Reset bindings that depend on the data stored in the pools.
    std::memset(current_float_constant_map_vertex_, 0, sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
    cbuffer_binding_system_.up_to_date = false;
    cbuffer_binding_float_vertex_.up_to_date = false;
    cbuffer_binding_float_pixel_.up_to_date = false;
    cbuffer_binding_bool_loop_.up_to_date = false;
    cbuffer_binding_fetch_.up_to_date = false;
    current_shared_memory_binding_is_uav_.reset();
    if (bindless_resources_used_) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    } else {
      draw_view_bindful_heap_index_ = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      draw_sampler_bindful_heap_index_ = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    constant_buffer_pool_->Reclaim(frame_completed_);
    if (!bindless_resources_used_) {
      view_bindful_heap_pool_->Reclaim(frame_completed_);
      sampler_bindful_heap_pool_->Reclaim(frame_completed_);
    }
    EvictOldReadbackBuffers(readback_buffers_);
    EvictOldReadbackBuffers(memexport_readback_buffers_);

    pix_capturing_ = pix_capture_requested_.exchange(false, std::memory_order_relaxed);
    if (pix_capturing_) {
      IDXGraphicsAnalysis* graphics_analysis = GetD3D12Provider().GetGraphicsAnalysis();
      if (graphics_analysis != nullptr) {
        graphics_analysis->BeginCapture();
      }
    }

    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();
  }

  return true;
}

bool D3D12CommandProcessor::EndSubmission(bool is_swap) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  const uint64_t nx1_submission_probe_id = submission_current_;

  // Make sure there is a command allocator to write commands to.
  if (submission_open_ && !command_allocator_writable_first_) {
    Nx1ScopedHitchTimer nx1_create_allocator_timer(
        "D3D12.EndSubmission.CreateCommandAllocator", frame_current_, nx1_submission_probe_id);
    ID3D12CommandAllocator* command_allocator;
    if (FAILED(provider.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&command_allocator)))) {
      REXGPU_ERROR("Failed to create a command allocator");
      // Try to submit later. Completely dropping the submission is not
      // permitted because resources would be left in an undefined state.
      return false;
    }
    command_allocator_writable_first_ = new CommandAllocator;
    command_allocator_writable_first_->command_allocator = command_allocator;
    command_allocator_writable_first_->last_usage_submission = 0;
    command_allocator_writable_first_->next = nullptr;
    command_allocator_writable_last_ = command_allocator_writable_first_;
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    {
      Nx1ScopedHitchTimer nx1_texture_end_frame_timer("D3D12.EndSubmission.TextureCacheEndFrame",
                                                      frame_current_, nx1_submission_probe_id);
      texture_cache_->EndFrame();
    }
    {
      Nx1ScopedHitchTimer nx1_primitive_end_frame_timer(
          "D3D12.EndSubmission.PrimitiveProcessorEndFrame", frame_current_,
          nx1_submission_probe_id);
      primitive_processor_->EndFrame();
    }
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    if (active_occlusion_query_.valid && occlusion_query_heap_) {
      deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                         active_occlusion_query_.host_index);
      active_occlusion_query_ = {};
    }

    {
      Nx1ScopedHitchTimer nx1_pipeline_end_submission_timer(
          "D3D12.EndSubmission.PipelineCacheEndSubmission", frame_current_,
          nx1_submission_probe_id);
      pipeline_cache_->EndSubmission();
    }

    // Submit barriers now because resources with the queued barriers may be
    // destroyed between frames.
    {
      Nx1ScopedHitchTimer nx1_submit_barriers_timer("D3D12.EndSubmission.SubmitBarriers",
                                                    frame_current_, nx1_submission_probe_id);
      SubmitBarriers();
    }

    ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

    // Submit the deferred command list.
    // Only one deferred command list must be executed in the same
    // ExecuteCommandLists - the boundaries of ExecuteCommandLists are a full
    // UAV and aliasing barrier, and subsystems of the emulator assume it
    // happens between Xenia submissions.
    ID3D12CommandAllocator* command_allocator =
        command_allocator_writable_first_->command_allocator;
    {
      Nx1ScopedHitchTimer nx1_record_timer("D3D12.EndSubmission.RecordDeferredCommandList",
                                           frame_current_, nx1_submission_probe_id);
      command_allocator->Reset();
      command_list_->Reset(command_allocator, nullptr);
      deferred_command_list_.Execute(command_list_, command_list_1_);
      command_list_->Close();
    }
    ID3D12CommandList* execute_command_lists[] = {command_list_};
    {
      Nx1ScopedHitchTimer nx1_execute_timer("D3D12.EndSubmission.ExecuteCommandLists",
                                            frame_current_, nx1_submission_probe_id);
      direct_queue->ExecuteCommandLists(1, execute_command_lists);
    }
    command_allocator_writable_first_->last_usage_submission = submission_current_;
    if (command_allocator_submitted_last_) {
      command_allocator_submitted_last_->next = command_allocator_writable_first_;
    } else {
      command_allocator_submitted_first_ = command_allocator_writable_first_;
    }
    command_allocator_submitted_last_ = command_allocator_writable_first_;
    command_allocator_writable_first_ = command_allocator_writable_first_->next;
    command_allocator_submitted_last_->next = nullptr;
    if (!command_allocator_writable_first_) {
      command_allocator_writable_last_ = nullptr;
    }

    {
      Nx1ScopedHitchTimer nx1_signal_timer("D3D12.EndSubmission.Signal", frame_current_,
                                           nx1_submission_probe_id);
      direct_queue->Signal(submission_fence_, submission_current_++);
    }

    submission_open_ = false;
    primary_buffers_since_submission_ = 0;

    // Queue operations done directly (like UpdateTileMappings) will be awaited
    // alongside the last submission if needed.
    queue_operations_done_since_submission_signal_ = false;
  }

  if (is_closing_frame) {
    if (REXCVAR_GET(clear_memory_page_state) && shared_memory_) {
      Nx1ScopedHitchTimer nx1_page_state_timer("D3D12.EndSubmission.PageStateSync",
                                               frame_current_, nx1_submission_probe_id);
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }
    // Close the capture after submitting.
    if (pix_capturing_) {
      IDXGraphicsAnalysis* graphics_analysis = provider.GetGraphicsAnalysis();
      if (graphics_analysis != nullptr) {
        graphics_analysis->EndCapture();
      }
      pix_capturing_ = false;
    }
    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kQueueFrames] = submission_current_ - 1;

    bool nx1_cache_clear_can_run = false;
    if (cache_clear_requested_) {
      Nx1ScopedHitchTimer nx1_cache_clear_wait_timer(
          "D3D12.EndSubmission.ClearCacheAwaitQueue", frame_current_, nx1_submission_probe_id);
      nx1_cache_clear_can_run = AwaitAllQueueOperationsCompletion();
    }
    if (cache_clear_requested_ && nx1_cache_clear_can_run) {
      Nx1ScopedHitchTimer nx1_cache_clear_timer("D3D12.EndSubmission.ClearCaches",
                                                frame_current_, nx1_submission_probe_id);
      cache_clear_requested_ = false;

      ClearCommandAllocatorCache();

      ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
      scratch_buffer_size_ = 0;

      if (bindless_resources_used_) {
        texture_cache_bindless_sampler_map_.clear();
        for (const auto& sampler_bindless_heap_overflowed : sampler_bindless_heaps_overflowed_) {
          sampler_bindless_heap_overflowed.first->Release();
        }
        sampler_bindless_heaps_overflowed_.clear();
        sampler_bindless_heap_allocated_ = 0;
      } else {
        sampler_bindful_heap_pool_->ClearCache();
        view_bindful_heap_pool_->ClearCache();
      }
      if (native_present_view_heap_pool_) {
        native_present_view_heap_pool_->ClearCache();
      }
      constant_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      // Not clearing the root signatures as they're referenced by pipelines,
      // which are not destroyed.

      primitive_processor_->ClearCache();

      render_target_cache_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

bool D3D12CommandProcessor::CanEndSubmissionImmediately() const {
  return !submission_open_ || !pipeline_cache_->IsCreatingPipelines();
}

void D3D12CommandProcessor::ClearCommandAllocatorCache() {
  while (command_allocator_submitted_first_) {
    auto next = command_allocator_submitted_first_->next;
    command_allocator_submitted_first_->command_allocator->Release();
    delete command_allocator_submitted_first_;
    command_allocator_submitted_first_ = next;
  }
  command_allocator_submitted_last_ = nullptr;
  while (command_allocator_writable_first_) {
    auto next = command_allocator_writable_first_->next;
    command_allocator_writable_first_->command_allocator->Release();
    delete command_allocator_writable_first_;
    command_allocator_writable_first_ = next;
  }
  command_allocator_writable_last_ = nullptr;
}

void D3D12CommandProcessor::UpdateFixedFunctionState(
    const draw_util::ViewportInfo& viewport_info, const draw_util::Scissor& scissor,
    bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Viewport.
  D3D12_VIEWPORT viewport;
  viewport.TopLeftX = float(viewport_info.xy_offset[0]);
  viewport.TopLeftY = float(viewport_info.xy_offset[1]);
  viewport.Width = float(viewport_info.xy_extent[0]);
  viewport.Height = float(viewport_info.xy_extent[1]);
  viewport.MinDepth = viewport_info.z_min;
  viewport.MaxDepth = viewport_info.z_max;
  SetViewport(viewport);

  // Scissor.
  D3D12_RECT scissor_rect;
  scissor_rect.left = LONG(scissor.offset[0]);
  scissor_rect.top = LONG(scissor.offset[1]);
  scissor_rect.right = LONG(scissor.offset[0] + scissor.extent[0]);
  scissor_rect.bottom = LONG(scissor.offset[1] + scissor.extent[1]);
  SetScissorRect(scissor_rect);

  if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    const RegisterFile& regs = *register_file_;

    // Blend factor.
    float blend_factor[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    ff_blend_factor_update_needed_ |=
        std::memcmp(ff_blend_factor_, blend_factor, sizeof(float) * 4) != 0;
    if (ff_blend_factor_update_needed_) {
      std::memcpy(ff_blend_factor_, blend_factor, sizeof(float) * 4);
      deferred_command_list_.D3DOMSetBlendFactor(ff_blend_factor_);
      ff_blend_factor_update_needed_ = false;
    }

    // Stencil reference value. Per-face reference not supported by Direct3D 12,
    // choose the back face one only if drawing only back faces.
    Register stencil_ref_mask_reg;
    auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (primitive_polygonal && normalized_depth_control.backface_enable &&
        pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
    } else {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
    }
    uint32_t stencil_ref = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg).stencilref;
    ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
    if (ff_stencil_ref_update_needed_) {
      ff_stencil_ref_ = stencil_ref;
      deferred_command_list_.D3DOMSetStencilRef(ff_stencil_ref_);
      ff_stencil_ref_update_needed_ = false;
    }
  }
}

void D3D12CommandProcessor::UpdateSystemConstantValues(
    bool shared_memory_is_uav, bool primitive_polygonal, uint32_t line_loop_closing_index,
    xenos::Endian index_endian, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_stencilrefmask = regs.Get<reg::RB_STENCILREFMASK>();
  auto rb_stencilrefmask_bf = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto sq_context_misc = regs.Get<reg::SQ_CONTEXT_MISC>();
  auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vgt_indx_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t vgt_max_vtx_indx = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  uint32_t vgt_min_vtx_indx = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;

  bool edram_rov_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  // Get the color info register values for each render target. Also, for ROV,
  // exclude components that don't exist in the format from the write mask.
  // Don't exclude fully overlapping render targets, however - two render
  // targets with the same base address are used in the lighting pass of
  // 4D5307E6, for example, with the needed one picked with dynamic control
  // flow.
  reg::RB_COLOR_INFO color_infos[4];
  float rt_clamp[4][4];
  // Two UINT32_MAX if no components actually existing in the RT are written.
  uint32_t rt_keep_masks[4][2];
  for (uint32_t i = 0; i < 4; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    color_infos[i] = color_info;
    if (edram_rov_used) {
      RenderTargetCache::GetPSIColorFormatInfo(
          color_info.color_format, (normalized_color_mask >> (i * 4)) & 0b1111, rt_clamp[i][0],
          rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3], rt_keep_masks[i][0], rt_keep_masks[i][1]);
    }
  }

  if (REXCVAR_GET(nx1_disable_scene_composite_alpha_test) &&
      rb_surface_info.surface_pitch == 1040 && color_infos[0].color_base == 98 &&
      color_infos[0].color_base_bit_11 == 0 && (normalized_color_mask & 0x7) == 0x7 &&
      (rb_colorcontrol.alpha_test_enable || rb_colorcontrol.alpha_to_mask_enable)) {
    static uint64_t nx1_disable_alpha_count = 0;
    ++nx1_disable_alpha_count;
    if (nx1_disable_alpha_count <= 16 || (nx1_disable_alpha_count % 240) == 0) {
      REXGPU_WARN(
          "[NX1 disable-composite-alpha] count={} rb_colorcontrol=0x{:08X} "
          "alpha_ref={}",
          nx1_disable_alpha_count, rb_colorcontrol.value, rb_alpha_ref);
    }
    rb_colorcontrol.alpha_test_enable = 0;
    rb_colorcontrol.alpha_to_mask_enable = 0;
    rb_colorcontrol.alpha_func = xenos::CompareFunction::kAlways;
  }

  // Disable depth and stencil if it aliases a color render target (for
  // instance, during the XBLA logo in 58410954, though depth writing is already
  // disabled there).
  bool depth_stencil_enabled =
      normalized_depth_control.stencil_enable || normalized_depth_control.z_enable;
  if (edram_rov_used && depth_stencil_enabled) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (rb_depth_info.depth_base == color_infos[i].color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
        break;
      }
    }
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  // Whether shared memory is an SRV or a UAV. Because a resource can't be in a
  // read-write (UAV) and a read-only (SRV, IBV) state at once, if any shader in
  // the pipeline uses memexport, the shared memory buffer must be a UAV.
  if (shared_memory_is_uav) {
    flags |= DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // Whether the primitive is polygonal and SV_IsFrontFace matters.
  if (primitive_polygonal) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive type.
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  }
  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= DxbcShaderTranslator::kSysFlag_DepthFloat24;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing.
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  if (edram_rov_used && depth_stencil_enabled) {
    flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencil;
    if (normalized_depth_control.z_enable) {
      flags |= uint32_t(normalized_depth_control.zfunc)
               << DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess_Shift;
      if (normalized_depth_control.z_write_enable) {
        flags |= DxbcShaderTranslator::kSysFlag_ROVDepthWrite;
      }
    } else {
      // In case stencil is used without depth testing - always pass, and
      // don't modify the stored depth.
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfEqual |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfGreater;
    }
    if (normalized_depth_control.stencil_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVStencilTest;
    }
    // Hint - if not applicable to the shader, will not have effect.
    if (alpha_test_function == xenos::CompareFunction::kAlways &&
        !rb_colorcontrol.alpha_to_mask_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencilEarlyWrite;
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Tessellation factor range, plus 1.0 according to the images in
  // https://www.slideshare.net/blackdevilvikas/next-generation-graphics-programming-on-xbox-360
  float tessellation_factor_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tessellation_factor_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  dirty |= system_constants_.tessellation_factor_range_min != tessellation_factor_min;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  dirty |= system_constants_.tessellation_factor_range_max != tessellation_factor_max;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  // Line loop closing index (or 0 when drawing other primitives or using an
  // index buffer).
  dirty |= system_constants_.line_loop_closing_index != line_loop_closing_index;
  system_constants_.line_loop_closing_index = line_loop_closing_index;

  // Index or tessellation edge factor buffer endianness.
  dirty |= system_constants_.vertex_index_endian != index_endian;
  system_constants_.vertex_index_endian = index_endian;

  // Vertex index offset.
  dirty |= system_constants_.vertex_index_offset != vgt_indx_offset;
  system_constants_.vertex_index_offset = vgt_indx_offset;

  // Vertex index range.
  dirty |= system_constants_.vertex_index_min != vgt_min_vtx_indx;
  dirty |= system_constants_.vertex_index_max != vgt_max_vtx_indx;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  // User clip planes (UCP_ENA_#), when not CLIP_DISABLE.
  // The shader knows only the total count - tightly packing the user clip
  // planes that are actually used.
  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  // Conversion to Direct3D 12 normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x = float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y = float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min != point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max != point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] != point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] != point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader.
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / gamma.
  uint32_t textures_resolution_scaled = 0;
  uint32_t textures_remaining = used_texture_mask;
  uint32_t texture_index;
  while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(uint32_t(1) << texture_index);
    uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
    uint32_t texture_signs_shift = (texture_index & 3) * 8;
    uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
    uint32_t texture_signs_shifted = uint32_t(texture_signs) << texture_signs_shift;
    uint32_t texture_signs_mask = uint32_t(0b11111111) << texture_signs_shift;
    dirty |= (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
    texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
    textures_resolution_scaled |=
        uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index)) << texture_index;
  }
  dirty |= system_constants_.textures_resolution_scaled != textures_resolution_scaled;
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;

  // Log2 of sample count, for alpha to mask and with ROV, for EDRAM address
  // calculation with MSAA.
  uint32_t sample_count_log2_x = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  uint32_t sample_count_log2_y = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  dirty |= system_constants_.sample_count_log2[0] != sample_count_log2_x;
  dirty |= system_constants_.sample_count_log2[1] != sample_count_log2_y;
  system_constants_.sample_count_log2[0] = sample_count_log2_x;
  system_constants_.sample_count_log2[1] = sample_count_log2_y;

  // Alpha test and alpha to coverage.
  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;
  uint32_t alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (1 << 8) : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  uint32_t edram_tile_dwords_scaled = xenos::kEdramTileWidthSamples *
                                      xenos::kEdramTileHeightSamples *
                                      (draw_resolution_scale_x * draw_resolution_scale_y);

  // EDRAM pitch for ROV writing.
  if (edram_rov_used) {
    // Align, then multiply by 32bpp tile size in dwords.
    uint32_t edram_32bpp_tile_pitch_dwords_scaled =
        ((rb_surface_info.surface_pitch *
          (rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1)) +
         (xenos::kEdramTileWidthSamples - 1)) /
        xenos::kEdramTileWidthSamples * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_32bpp_tile_pitch_dwords_scaled !=
             edram_32bpp_tile_pitch_dwords_scaled;
    system_constants_.edram_32bpp_tile_pitch_dwords_scaled = edram_32bpp_tile_pitch_dwords_scaled;
  }

  // Color exponent bias and ROV render target writing.
  for (uint32_t i = 0; i < 4; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets &&
          !render_target_cache_->IsFixed16TruncatedToMinus1To1()) {
        // Remap from -32...32 to -1...1 by dividing the output values by 32,
        // losing blending correctness, but getting the full range.
        color_exp_bias -= 5;
      }
    }
    auto color_exp_bias_scale =
        rex::memory::Reinterpret<float>(int32_t(0x3F800000 + (color_exp_bias << 23)));
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
    if (edram_rov_used) {
      dirty |= system_constants_.edram_rt_keep_mask[i][0] != rt_keep_masks[i][0];
      system_constants_.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
      dirty |= system_constants_.edram_rt_keep_mask[i][1] != rt_keep_masks[i][1];
      system_constants_.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
      if (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX) {
        uint32_t rt_base_dwords_scaled = color_info.color_base * edram_tile_dwords_scaled;
        dirty |= system_constants_.edram_rt_base_dwords_scaled[i] != rt_base_dwords_scaled;
        system_constants_.edram_rt_base_dwords_scaled[i] = rt_base_dwords_scaled;
        uint32_t format_flags = RenderTargetCache::AddPSIColorFormatFlags(color_info.color_format);
        dirty |= system_constants_.edram_rt_format_flags[i] != format_flags;
        system_constants_.edram_rt_format_flags[i] = format_flags;
        // Can't do float comparisons here because NaNs would result in always
        // setting the dirty flag.
        dirty |=
            std::memcmp(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float)) != 0;
        std::memcpy(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float));
        uint32_t blend_factors_ops =
            regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
        dirty |= system_constants_.edram_rt_blend_factors_ops[i] != blend_factors_ops;
        system_constants_.edram_rt_blend_factors_ops[i] = blend_factors_ops;
      }
    }
  }

  if (edram_rov_used) {
    uint32_t depth_base_dwords_scaled = rb_depth_info.depth_base * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_depth_base_dwords_scaled != depth_base_dwords_scaled;
    system_constants_.edram_depth_base_dwords_scaled = depth_base_dwords_scaled;

    // For non-polygons, front polygon offset is used, and it's enabled if
    // POLY_OFFSET_PARA_ENABLED is set, for polygons, separate front and back
    // are used.
    float poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
    float poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
    if (primitive_polygonal) {
      if (pa_su_sc_mode_cntl.poly_offset_front_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      if (pa_su_sc_mode_cntl.poly_offset_back_enable) {
        poly_offset_back_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
        poly_offset_back_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      }
    } else {
      if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
        poly_offset_back_scale = poly_offset_front_scale;
        poly_offset_back_offset = poly_offset_front_offset;
      }
    }
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    float poly_offset_scale_factor = xenos::kPolygonOffsetScaleSubpixelUnit *
                                     std::max(draw_resolution_scale_x, draw_resolution_scale_y);
    poly_offset_front_scale *= poly_offset_scale_factor;
    poly_offset_back_scale *= poly_offset_scale_factor;
    dirty |= system_constants_.edram_poly_offset_front_scale != poly_offset_front_scale;
    system_constants_.edram_poly_offset_front_scale = poly_offset_front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset != poly_offset_front_offset;
    system_constants_.edram_poly_offset_front_offset = poly_offset_front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale != poly_offset_back_scale;
    system_constants_.edram_poly_offset_back_scale = poly_offset_back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset != poly_offset_back_offset;
    system_constants_.edram_poly_offset_back_offset = poly_offset_back_offset;

    if (depth_stencil_enabled && normalized_depth_control.stencil_enable) {
      dirty |= system_constants_.edram_stencil_front_reference != rb_stencilrefmask.stencilref;
      system_constants_.edram_stencil_front_reference = rb_stencilrefmask.stencilref;
      dirty |= system_constants_.edram_stencil_front_read_mask != rb_stencilrefmask.stencilmask;
      system_constants_.edram_stencil_front_read_mask = rb_stencilrefmask.stencilmask;
      dirty |=
          system_constants_.edram_stencil_front_write_mask != rb_stencilrefmask.stencilwritemask;
      system_constants_.edram_stencil_front_write_mask = rb_stencilrefmask.stencilwritemask;
      uint32_t stencil_func_ops = (normalized_depth_control.value >> 8) & ((1 << 12) - 1);
      dirty |= system_constants_.edram_stencil_front_func_ops != stencil_func_ops;
      system_constants_.edram_stencil_front_func_ops = stencil_func_ops;

      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        dirty |= system_constants_.edram_stencil_back_reference != rb_stencilrefmask_bf.stencilref;
        system_constants_.edram_stencil_back_reference = rb_stencilrefmask_bf.stencilref;
        dirty |= system_constants_.edram_stencil_back_read_mask != rb_stencilrefmask_bf.stencilmask;
        system_constants_.edram_stencil_back_read_mask = rb_stencilrefmask_bf.stencilmask;
        dirty |= system_constants_.edram_stencil_back_write_mask !=
                 rb_stencilrefmask_bf.stencilwritemask;
        system_constants_.edram_stencil_back_write_mask = rb_stencilrefmask_bf.stencilwritemask;
        uint32_t stencil_func_ops_bf = (normalized_depth_control.value >> 20) & ((1 << 12) - 1);
        dirty |= system_constants_.edram_stencil_back_func_ops != stencil_func_ops_bf;
        system_constants_.edram_stencil_back_func_ops = stencil_func_ops_bf;
      } else {
        dirty |= std::memcmp(system_constants_.edram_stencil_back,
                             system_constants_.edram_stencil_front, 4 * sizeof(uint32_t)) != 0;
        std::memcpy(system_constants_.edram_stencil_back, system_constants_.edram_stencil_front,
                    4 * sizeof(uint32_t));
      }
    }

    dirty |= system_constants_.edram_blend_constant[0] != regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    system_constants_.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    dirty |=
        system_constants_.edram_blend_constant[1] != regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    system_constants_.edram_blend_constant[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    dirty |= system_constants_.edram_blend_constant[2] != regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    system_constants_.edram_blend_constant[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    dirty |=
        system_constants_.edram_blend_constant[3] != regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
    system_constants_.edram_blend_constant[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  }

  cbuffer_binding_system_.up_to_date &= !dirty;
}

bool D3D12CommandProcessor::UpdateNx1NativeBindings(const D3D12Shader* vertex_shader,
                                                    const D3D12Shader* pixel_shader) {
  assert_true(bindless_resources_used_);

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  const RegisterFile& regs = *register_file_;

  constexpr uint32_t kFloatConstantsSize = 256 * 4 * sizeof(uint32_t);
  uint8_t* vertex_constants = constant_buffer_pool_->Request(
      frame_current_, kFloatConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr,
      nullptr, &cbuffer_binding_nx1_native_vertex_constants_.address);
  if (!vertex_constants) {
    return false;
  }
  std::memcpy(vertex_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_000_X], kFloatConstantsSize);

  uint8_t* pixel_constants = constant_buffer_pool_->Request(
      frame_current_, kFloatConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr,
      nullptr, &cbuffer_binding_nx1_native_pixel_constants_.address);
  if (!pixel_constants) {
    return false;
  }
  std::memcpy(pixel_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_256_X], kFloatConstantsSize);

  constexpr uint32_t kFetchConstantsSize = 32 * 6 * sizeof(uint32_t);
  uint8_t* fetch_constants = constant_buffer_pool_->Request(
      frame_current_, kFetchConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr,
      nullptr, &cbuffer_binding_nx1_native_fetch_constants_.address);
  if (!fetch_constants) {
    return false;
  }
  std::memcpy(fetch_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);

  Nx1NativeSharedConstants shared_constants = {};
  constexpr uint32_t kNullTextureDescriptorIndex =
      uint32_t(SystemBindlessView::kNullTexture2DArray) -
      uint32_t(SystemBindlessView::kUnboundedSRVsStart);
  for (uint32_t i = 0; i < 16; ++i) {
    shared_constants.texture_1d_descriptor_indices[i] = kNullTextureDescriptorIndex;
    shared_constants.texture_2d_descriptor_indices[i] = kNullTextureDescriptorIndex;
    shared_constants.texture_3d_descriptor_indices[i] = kNullTextureDescriptorIndex;
    shared_constants.texture_cube_descriptor_indices[i] = kNullTextureDescriptorIndex;
  }
  shared_constants.booleans = regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031];
  shared_constants.swapped_texcoords = 0;
  shared_constants.half_pixel_offset[0] = 0.0f;
  shared_constants.half_pixel_offset[1] = 0.0f;
  shared_constants.alpha_threshold = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  // The native VS uses SV_VertexID to fetch vertices from shared memory; the
  // guest index buffer is big-endian, so the shader must swap the index by the
  // same endianness the translated path applies (set in UpdateSystemConstantValues,
  // which runs earlier this draw). Without this, vertices are fetched at
  // byte-swapped indices -> garbage/NaN positions.
  shared_constants.vertex_index_endian = uint32_t(system_constants_.vertex_index_endian);
  // Guest viewport NDC transform for the native VS (set earlier this draw by
  // UpdateSystemConstantValues from the host viewport). The native VS applies
  // position.xyz = position.xyz * ndc_scale + ndc_offset * position.w so its
  // depth matches the depth buffer (the translated path does the same).
  for (uint32_t i = 0; i < 3; ++i) {
    shared_constants.ndc_scale[i] = system_constants_.ndc_scale[i];
    shared_constants.ndc_offset[i] = system_constants_.ndc_offset[i];
  }
  uint32_t shader_spec_constants_mask = 0;
  Nx1NativeDxilBlob native_shader_blob;
  if (vertex_shader &&
      GetNx1NativeDxilBlob(vertex_shader->ucode_data_hash(), native_shader_blob)) {
    shader_spec_constants_mask |= native_shader_blob.spec_constants_mask;
  }
  if (pixel_shader &&
      GetNx1NativeDxilBlob(pixel_shader->ucode_data_hash(), native_shader_blob)) {
    shader_spec_constants_mask |= native_shader_blob.spec_constants_mask;
  }
  shared_constants.spec_constants =
      shader_spec_constants_mask & kNx1NativeSpecConstantR11G11B10Normal;
  const auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  const xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  shared_constants.alpha_test_function = uint32_t(alpha_test_function);
  if ((shader_spec_constants_mask & kNx1NativeSpecConstantAlphaTest) &&
      alpha_test_function != xenos::CompareFunction::kAlways) {
    shared_constants.spec_constants |= kNx1NativeSpecConstantAlphaTest;
  }
  for (uint32_t i = 0; i < 32; ++i) {
    const uint32_t loop_constant = regs[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + i];
    shared_constants.integer_constants[i][0] = loop_constant & 0xFF;
    shared_constants.integer_constants[i][1] =
        uint32_t(int32_t(int8_t((loop_constant >> 8) & 0xFF)));
    shared_constants.integer_constants[i][2] =
        uint32_t(int32_t(int8_t((loop_constant >> 16) & 0xFF)));
    shared_constants.integer_constants[i][3] =
        uint32_t(int32_t(int8_t((loop_constant >> 24) & 0xFF)));
  }

  auto write_texture_descriptor = [&](const D3D12Shader::TextureBinding& texture) -> bool {
    if (texture.fetch_constant >= 16) {
      return false;
    }
    uint32_t descriptor_index = texture_cache_->GetActiveTextureBindlessSRVIndex(texture);
    if (descriptor_index < uint32_t(SystemBindlessView::kUnboundedSRVsStart)) {
      return false;
    }
    descriptor_index -= uint32_t(SystemBindlessView::kUnboundedSRVsStart);
    if (descriptor_index == kNullTextureDescriptorIndex) {
      // The native shader has a single descriptor slot per (dimension, fetch
      // constant), but the translation can emit BOTH a signed and an unsigned
      // binding for the same fetch constant. The unwanted one resolves to the
      // null/placeholder texture; do NOT let it clobber a valid descriptor
      // already written for this slot (the init value is also the null index,
      // so a slot that stays null simply samples black as before).
      return true;
    }
    uint32_t* target = nullptr;
    switch (texture.dimension) {
      case xenos::FetchOpDimension::k1D:
        target = shared_constants.texture_1d_descriptor_indices;
        break;
      case xenos::FetchOpDimension::k2D:
        target = shared_constants.texture_2d_descriptor_indices;
        break;
      case xenos::FetchOpDimension::k3DOrStacked:
        target = shared_constants.texture_3d_descriptor_indices;
        break;
      case xenos::FetchOpDimension::kCube:
        target = shared_constants.texture_cube_descriptor_indices;
        break;
      default:
        return false;
    }
    target[texture.fetch_constant] = descriptor_index;
    return true;
  };

  for (const D3D12Shader::TextureBinding& texture :
       vertex_shader->GetTextureBindingsAfterTranslation()) {
    if (!write_texture_descriptor(texture)) {
      return false;
    }
  }
  if (pixel_shader) {
    for (const D3D12Shader::TextureBinding& texture :
         pixel_shader->GetTextureBindingsAfterTranslation()) {
      if (!write_texture_descriptor(texture)) {
        return false;
      }
    }
  }

  auto get_sampler_descriptor = [&](const D3D12Shader::SamplerBinding& sampler,
                                    uint32_t& descriptor_index_out) -> bool {
    D3D12TextureCache::SamplerParameters parameters =
        texture_cache_->GetSamplerParameters(sampler);
    auto it = texture_cache_bindless_sampler_map_.find(parameters.value);
    if (it != texture_cache_bindless_sampler_map_.end()) {
      descriptor_index_out = it->second;
      return true;
    }
    if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
      REXGPU_ERROR("NX1 native shader: bindless sampler heap overflow");
      return false;
    }
    descriptor_index_out = sampler_bindless_heap_allocated_++;
    texture_cache_->WriteSampler(
        parameters, provider.OffsetSamplerDescriptor(sampler_bindless_heap_cpu_start_,
                                                     descriptor_index_out));
    texture_cache_bindless_sampler_map_.emplace(parameters.value, descriptor_index_out);
    current_graphics_root_up_to_date_ &= ~(1u << UINT(Nx1NativeRootParameter::kSamplerHeap));
    return true;
  };

  auto write_sampler_descriptor = [&](const D3D12Shader::SamplerBinding& sampler) -> bool {
    if (sampler.fetch_constant >= 16) {
      return false;
    }
    uint32_t sampler_index;
    if (!get_sampler_descriptor(sampler, sampler_index)) {
      return false;
    }
    shared_constants.sampler_descriptor_indices[sampler.fetch_constant] = sampler_index;
    return true;
  };

  for (const D3D12Shader::SamplerBinding& sampler :
       vertex_shader->GetSamplerBindingsAfterTranslation()) {
    if (!write_sampler_descriptor(sampler)) {
      return false;
    }
  }
  if (pixel_shader) {
    for (const D3D12Shader::SamplerBinding& sampler :
         pixel_shader->GetSamplerBindingsAfterTranslation()) {
      if (!write_sampler_descriptor(sampler)) {
        return false;
      }
    }
  }

  uint8_t* shared_constants_upload = constant_buffer_pool_->Request(
      frame_current_, sizeof(shared_constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
      nullptr, nullptr, &cbuffer_binding_nx1_native_shared_constants_.address);
  if (!shared_constants_upload) {
    return false;
  }
  std::memcpy(shared_constants_upload, &shared_constants, sizeof(shared_constants));

  deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
      UINT(Nx1NativeRootParameter::kVertexConstants),
      cbuffer_binding_nx1_native_vertex_constants_.address);
  deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
      UINT(Nx1NativeRootParameter::kPixelConstants),
      cbuffer_binding_nx1_native_pixel_constants_.address);
  deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
      UINT(Nx1NativeRootParameter::kSharedConstants),
      cbuffer_binding_nx1_native_shared_constants_.address);
  deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
      UINT(Nx1NativeRootParameter::kFetchConstants),
      cbuffer_binding_nx1_native_fetch_constants_.address);
  deferred_command_list_.D3DSetGraphicsRootShaderResourceView(
      UINT(Nx1NativeRootParameter::kSharedMemory), shared_memory_->GetGPUAddress());
  deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
      UINT(Nx1NativeRootParameter::kSamplerHeap), sampler_bindless_heap_gpu_start_);
  deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
      UINT(Nx1NativeRootParameter::kViewHeap), view_bindless_heap_gpu_start_);
  current_graphics_root_up_to_date_ =
      (uint32_t(1) << UINT(Nx1NativeRootParameter::kCount)) - 1;
  return true;
}

bool D3D12CommandProcessor::SetNx1NativeVertexBuffers(const D3D12Shader* vertex_shader) {
  assert_not_null(vertex_shader);
  // Native NX1 vertex shaders now decode vfetches from guest shared memory via
  // SV_VertexID, matching the translated Xenos path more closely than D3D IA.
  return true;
}

bool D3D12CommandProcessor::UpdateBindings(const D3D12Shader* vertex_shader,
                                           const D3D12Shader* pixel_shader,
                                           ID3D12RootSignature* root_signature,
                                           bool shared_memory_is_uav) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  const RegisterFile& regs = *register_file_;

#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Set the new root signature.
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    if (!bindless_resources_used_) {
      GetRootBindfulExtraParameterIndices(vertex_shader, pixel_shader,
                                          current_graphics_root_bindful_extras_);
    }
    // Changing the root signature invalidates all bindings.
    current_graphics_root_up_to_date_ = 0;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }
  if (root_signature == root_signature_nx1_native_shader_) {
    return UpdateNx1NativeBindings(vertex_shader, pixel_shader);
  }

  // Select the root parameter indices depending on the used binding model.
  uint32_t root_parameter_fetch_constants = bindless_resources_used_
                                                ? kRootParameter_Bindless_FetchConstants
                                                : kRootParameter_Bindful_FetchConstants;
  uint32_t root_parameter_float_constants_vertex =
      bindless_resources_used_ ? kRootParameter_Bindless_FloatConstantsVertex
                               : kRootParameter_Bindful_FloatConstantsVertex;
  uint32_t root_parameter_float_constants_pixel = bindless_resources_used_
                                                      ? kRootParameter_Bindless_FloatConstantsPixel
                                                      : kRootParameter_Bindful_FloatConstantsPixel;
  uint32_t root_parameter_system_constants = bindless_resources_used_
                                                 ? kRootParameter_Bindless_SystemConstants
                                                 : kRootParameter_Bindful_SystemConstants;
  uint32_t root_parameter_bool_loop_constants = bindless_resources_used_
                                                    ? kRootParameter_Bindless_BoolLoopConstants
                                                    : kRootParameter_Bindful_BoolLoopConstants;
  uint32_t root_parameter_shared_memory_and_bindful_edram =
      bindless_resources_used_ ? kRootParameter_Bindless_SharedMemory
                               : kRootParameter_Bindful_SharedMemoryAndEdram;

  //
  // Update root constant buffers that are common for bindful and bindless.
  //

  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ on the Xbox
  // 360 (however, OpenGL ES on Adreno 200 on Android has different ranges).
  assert_true(regs[XE_GPU_REG_SQ_VS_CONST] == 0x000FF000 ||
              regs[XE_GPU_REG_SQ_VS_CONST] == 0x00000000);
  assert_true(regs[XE_GPU_REG_SQ_PS_CONST] == 0x000FF100 ||
              regs[XE_GPU_REG_SQ_PS_CONST] == 0x00000000);
  // Check if the float constant layout is still the same and get the counts.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] = float_constant_map_vertex.float_bitmap[i];
      // If no float constants at all, we can reuse any buffer for them, so not
      // invalidating.
      if (float_constant_count_vertex) {
        cbuffer_binding_float_vertex_.up_to_date = false;
      }
    }
  }
  uint32_t float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] = float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
  }

  // Write the constant buffer data.
  if (!cbuffer_binding_system_.up_to_date) {
    uint8_t* system_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(system_constants_), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_system_.address);
    if (system_constants == nullptr) {
      return false;
    }
    std::memcpy(system_constants, &system_constants_, sizeof(system_constants_));
    cbuffer_binding_system_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_system_constants);
  }
  if (!cbuffer_binding_float_vertex_.up_to_date) {
    // Even if the shader doesn't need any float constants, a valid binding must
    // still be provided, so if the first draw in the frame with the current
    // root signature doesn't have float constants at all, still allocate an
    // empty buffer.
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * std::max(float_constant_count_vertex, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_vertex_.address);
    if (float_constants == nullptr) {
      return false;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t float_constant_map_entry = float_constant_map_vertex.float_bitmap[i];
      uint32_t float_constant_index;
      while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
        float_constant_map_entry &= ~(1ull << float_constant_index);
        std::memcpy(
            float_constants,
            &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) + (float_constant_index << 2)],
            4 * sizeof(float));
        float_constants += 4 * sizeof(float);
      }
    }
    cbuffer_binding_float_vertex_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_float_constants_vertex);
  }
  if (!cbuffer_binding_float_pixel_.up_to_date) {
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * std::max(float_constant_count_pixel, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_pixel_.address);
    if (float_constants == nullptr) {
      return false;
    }
    if (pixel_shader != nullptr) {
      const Shader::ConstantRegisterMap& float_constant_map_pixel =
          pixel_shader->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry = float_constant_map_pixel.float_bitmap[i];
        uint32_t float_constant_index;
        while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(
              float_constants,
              &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) + (float_constant_index << 2)],
              4 * sizeof(float));
          float_constants += 4 * sizeof(float);
        }
      }
    }
    cbuffer_binding_float_pixel_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_float_constants_pixel);
  }
  if (!cbuffer_binding_bool_loop_.up_to_date) {
    constexpr uint32_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
    uint8_t* bool_loop_constants = constant_buffer_pool_->Request(
        frame_current_, kBoolLoopConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_bool_loop_.address);
    if (bool_loop_constants == nullptr) {
      return false;
    }
    std::memcpy(bool_loop_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsSize);
    cbuffer_binding_bool_loop_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_bool_loop_constants);
  }
  if (!cbuffer_binding_fetch_.up_to_date) {
    constexpr uint32_t kFetchConstantsSize = 32 * 6 * sizeof(uint32_t);
    uint8_t* fetch_constants = constant_buffer_pool_->Request(
        frame_current_, kFetchConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_fetch_.address);
    if (fetch_constants == nullptr) {
      return false;
    }
    std::memcpy(fetch_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);
    cbuffer_binding_fetch_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_fetch_constants);
  }

  //
  // Update descriptors.
  //

  if (!current_shared_memory_binding_is_uav_.has_value() ||
      current_shared_memory_binding_is_uav_.value() != shared_memory_is_uav) {
    current_shared_memory_binding_is_uav_ = shared_memory_is_uav;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_shared_memory_and_bindful_edram);
  }

  // Get textures and samplers used by the vertex shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_vertex = vertex_shader->GetTextureBindingLayoutUserUID();
  size_t sampler_layout_uid_vertex = vertex_shader->GetSamplerBindingLayoutUserUID();
  const std::vector<D3D12Shader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  const std::vector<D3D12Shader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  size_t texture_count_vertex = textures_vertex.size();
  size_t sampler_count_vertex = samplers_vertex.size();
  if (sampler_count_vertex) {
    if (current_sampler_layout_uid_vertex_ != sampler_layout_uid_vertex) {
      current_sampler_layout_uid_vertex_ = sampler_layout_uid_vertex;
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      bindful_samplers_written_vertex_ = false;
    }
    current_samplers_vertex_.resize(
        std::max(current_samplers_vertex_.size(), sampler_count_vertex));
    for (size_t i = 0; i < sampler_count_vertex; ++i) {
      D3D12TextureCache::SamplerParameters parameters =
          texture_cache_->GetSamplerParameters(samplers_vertex[i]);
      if (current_samplers_vertex_[i] != parameters) {
        cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
        bindful_samplers_written_vertex_ = false;
        current_samplers_vertex_[i] = parameters;
      }
    }
  }

  // Get textures and samplers used by the pixel shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_pixel, sampler_layout_uid_pixel;
  const std::vector<D3D12Shader::TextureBinding>* textures_pixel;
  const std::vector<D3D12Shader::SamplerBinding>* samplers_pixel;
  size_t texture_count_pixel, sampler_count_pixel;
  if (pixel_shader != nullptr) {
    texture_layout_uid_pixel = pixel_shader->GetTextureBindingLayoutUserUID();
    sampler_layout_uid_pixel = pixel_shader->GetSamplerBindingLayoutUserUID();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    texture_count_pixel = textures_pixel->size();
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    sampler_count_pixel = samplers_pixel->size();
    if (sampler_count_pixel) {
      if (current_sampler_layout_uid_pixel_ != sampler_layout_uid_pixel) {
        current_sampler_layout_uid_pixel_ = sampler_layout_uid_pixel;
        cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
        bindful_samplers_written_pixel_ = false;
      }
      current_samplers_pixel_.resize(
          std::max(current_samplers_pixel_.size(), size_t(sampler_count_pixel)));
      for (uint32_t i = 0; i < sampler_count_pixel; ++i) {
        D3D12TextureCache::SamplerParameters parameters =
            texture_cache_->GetSamplerParameters((*samplers_pixel)[i]);
        if (current_samplers_pixel_[i] != parameters) {
          current_samplers_pixel_[i] = parameters;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          bindful_samplers_written_pixel_ = false;
        }
      }
    }
  } else {
    texture_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    sampler_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    textures_pixel = nullptr;
    texture_count_pixel = 0;
    samplers_pixel = nullptr;
    sampler_count_pixel = 0;
  }

  assert_true(sampler_count_vertex + sampler_count_pixel <= kSamplerHeapSize);

  if (bindless_resources_used_) {
    //
    // Bindless descriptors path.
    //

    // Check if need to write new descriptor indices.
    // Samplers have already been checked.
    if (texture_count_vertex && cbuffer_binding_descriptor_indices_vertex_.up_to_date &&
        (current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(current_texture_srv_keys_vertex_.data(),
                                                          textures_vertex.data(),
                                                          texture_count_vertex))) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
    }
    if (texture_count_pixel && cbuffer_binding_descriptor_indices_pixel_.up_to_date &&
        (current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(current_texture_srv_keys_pixel_.data(),
                                                          textures_pixel->data(),
                                                          texture_count_pixel))) {
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    }

    // Get sampler descriptor indices, write new samplers, and handle sampler
    // heap overflow if it happens.
    if ((sampler_count_vertex && !cbuffer_binding_descriptor_indices_vertex_.up_to_date) ||
        (sampler_count_pixel && !cbuffer_binding_descriptor_indices_pixel_.up_to_date)) {
      for (uint32_t i = 0; i < 2; ++i) {
        if (i) {
          // Overflow happened - invalidate sampler bindings because their
          // descriptor indices can't be used anymore (and even if heap creation
          // fails, because current_sampler_bindless_indices_#_ are in an
          // undefined state now) and switch to a new sampler heap.
          cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          ID3D12DescriptorHeap* sampler_heap_new;
          if (!sampler_bindless_heaps_overflowed_.empty() &&
              sampler_bindless_heaps_overflowed_.front().second <= submission_completed_) {
            sampler_heap_new = sampler_bindless_heaps_overflowed_.front().first;
            sampler_bindless_heaps_overflowed_.pop_front();
          } else {
            D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_new_desc;
            sampler_heap_new_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            sampler_heap_new_desc.NumDescriptors = kSamplerHeapSize;
            sampler_heap_new_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            sampler_heap_new_desc.NodeMask = 0;
            if (FAILED(device->CreateDescriptorHeap(&sampler_heap_new_desc,
                                                    IID_PPV_ARGS(&sampler_heap_new)))) {
              REXGPU_ERROR(
                  "Failed to create a new bindless sampler descriptor heap "
                  "after an overflow of the previous one");
              return false;
            }
          }
          // Only change the heap if a new heap was created successfully, not to
          // leave the values in an undefined state in case CreateDescriptorHeap
          // has failed.
          sampler_bindless_heaps_overflowed_.push_back(
              std::make_pair(sampler_bindless_heap_current_, submission_current_));
          sampler_bindless_heap_current_ = sampler_heap_new;
          sampler_bindless_heap_cpu_start_ =
              sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_gpu_start_ =
              sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_allocated_ = 0;
          // The only thing the heap is used for now is texture cache samplers -
          // invalidate all of them.
          texture_cache_bindless_sampler_map_.clear();
          deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                    sampler_bindless_heap_current_);
          current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_SamplerHeap);
        }
        bool samplers_overflowed = false;
        if (sampler_count_vertex && !cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
          current_sampler_bindless_indices_vertex_.resize(std::max(
              current_sampler_bindless_indices_vertex_.size(), size_t(sampler_count_vertex)));
          for (uint32_t j = 0; j < sampler_count_vertex; ++j) {
            D3D12TextureCache::SamplerParameters sampler_parameters = current_samplers_vertex_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(sampler_parameters,
                                           provider.OffsetSamplerDescriptor(
                                               sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_vertex_[j] = sampler_index;
          }
        }
        if (samplers_overflowed) {
          continue;
        }
        if (sampler_count_pixel && !cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
          current_sampler_bindless_indices_pixel_.resize(std::max(
              current_sampler_bindless_indices_pixel_.size(), size_t(sampler_count_pixel)));
          for (uint32_t j = 0; j < sampler_count_pixel; ++j) {
            D3D12TextureCache::SamplerParameters sampler_parameters = current_samplers_pixel_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(sampler_parameters,
                                           provider.OffsetSamplerDescriptor(
                                               sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_pixel_[j] = sampler_index;
          }
        }
        if (!samplers_overflowed) {
          break;
        }
      }
    }

    if (!cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
      uint32_t* descriptor_indices = reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
          frame_current_,
          std::max(texture_count_vertex + sampler_count_vertex, size_t(1)) * sizeof(uint32_t),
          D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
          &cbuffer_binding_descriptor_indices_vertex_.address));
      if (!descriptor_indices) {
        return false;
      }
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        const D3D12Shader::TextureBinding& texture = textures_vertex[i];
        descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      if (texture_count_vertex) {
        current_texture_srv_keys_vertex_.resize(
            std::max(current_texture_srv_keys_vertex_.size(), size_t(texture_count_vertex)));
        texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_vertex_.data(),
                                                  textures_vertex.data(), texture_count_vertex);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        descriptor_indices[samplers_vertex[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_vertex_[i];
      }
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = true;
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_DescriptorIndicesVertex);
    }

    if (!cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
      uint32_t* descriptor_indices = reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
          frame_current_,
          std::max(texture_count_pixel + sampler_count_pixel, size_t(1)) * sizeof(uint32_t),
          D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
          &cbuffer_binding_descriptor_indices_pixel_.address));
      if (!descriptor_indices) {
        return false;
      }
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        const D3D12Shader::TextureBinding& texture = (*textures_pixel)[i];
        descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      if (texture_count_pixel) {
        current_texture_srv_keys_pixel_.resize(
            std::max(current_texture_srv_keys_pixel_.size(), size_t(texture_count_pixel)));
        texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_pixel_.data(),
                                                  textures_pixel->data(), texture_count_pixel);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        descriptor_indices[(*samplers_pixel)[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_pixel_[i];
      }
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = true;
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_DescriptorIndicesPixel);
    }
  } else {
    //
    // Bindful descriptors path.
    //

    // See what descriptors need to be updated.
    // Samplers have already been checked.
    bool write_textures_vertex =
        texture_count_vertex && (!bindful_textures_written_vertex_ ||
                                 current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
                                 !texture_cache_->AreActiveTextureSRVKeysUpToDate(
                                     current_texture_srv_keys_vertex_.data(),
                                     textures_vertex.data(), texture_count_vertex));
    bool write_textures_pixel =
        texture_count_pixel &&
        (!bindful_textures_written_pixel_ ||
         current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(
             current_texture_srv_keys_pixel_.data(), textures_pixel->data(), texture_count_pixel));
    bool write_samplers_vertex = sampler_count_vertex && !bindful_samplers_written_vertex_;
    bool write_samplers_pixel = sampler_count_pixel && !bindful_samplers_written_pixel_;
    bool edram_rov_used =
        render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

    // Allocate the descriptors.
    size_t view_count_partial_update = 0;
    if (write_textures_vertex) {
      view_count_partial_update += texture_count_vertex;
    }
    if (write_textures_pixel) {
      view_count_partial_update += texture_count_pixel;
    }
    // Shared memory SRV and null UAV + null SRV and shared memory UAV +
    // textures.
    size_t view_count_full_update = 4 + texture_count_vertex + texture_count_pixel;
    if (edram_rov_used) {
      // + EDRAM UAV in two tables (with the shared memory SRV and with the
      // shared memory UAV).
      view_count_full_update += 2;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE view_cpu_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE view_gpu_handle;
    uint32_t descriptor_size_view = provider.GetViewDescriptorSize();
    uint64_t view_heap_index = RequestViewBindfulDescriptors(
        draw_view_bindful_heap_index_, uint32_t(view_count_partial_update),
        uint32_t(view_count_full_update), view_cpu_handle, view_gpu_handle);
    if (view_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      REXGPU_ERROR("Failed to allocate view descriptors");
      return false;
    }
    size_t sampler_count_partial_update = 0;
    if (write_samplers_vertex) {
      sampler_count_partial_update += sampler_count_vertex;
    }
    if (write_samplers_pixel) {
      sampler_count_partial_update += sampler_count_pixel;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu_handle = {};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu_handle = {};
    uint32_t descriptor_size_sampler = provider.GetSamplerDescriptorSize();
    uint64_t sampler_heap_index = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
    if (sampler_count_vertex != 0 || sampler_count_pixel != 0) {
      sampler_heap_index = RequestSamplerBindfulDescriptors(
          draw_sampler_bindful_heap_index_, uint32_t(sampler_count_partial_update),
          uint32_t(sampler_count_vertex + sampler_count_pixel), sampler_cpu_handle,
          sampler_gpu_handle);
      if (sampler_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
        REXGPU_ERROR("Failed to allocate sampler descriptors");
        return false;
      }
    }
    if (draw_view_bindful_heap_index_ != view_heap_index) {
      // Need to update all view descriptors.
      write_textures_vertex = texture_count_vertex != 0;
      write_textures_pixel = texture_count_pixel != 0;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      // If updating fully, write the shared memory SRV and UAV descriptors and,
      // if needed, the EDRAM descriptor.
      // SRV + null UAV + EDRAM.
      gpu_handle_shared_memory_srv_and_edram_ = view_gpu_handle;
      shared_memory_->WriteRawSRVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      ui::d3d12::util::CreateBufferRawUAV(device, view_cpu_handle, nullptr, 0);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      if (edram_rov_used) {
        render_target_cache_->WriteEdramUintPow2UAVDescriptor(view_cpu_handle, 2);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      // Null SRV + UAV + EDRAM.
      gpu_handle_shared_memory_uav_and_edram_ = view_gpu_handle;
      ui::d3d12::util::CreateBufferRawSRV(device, view_cpu_handle, nullptr, 0);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      shared_memory_->WriteRawUAVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      if (edram_rov_used) {
        render_target_cache_->WriteEdramUintPow2UAVDescriptor(view_cpu_handle, 2);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindful_SharedMemoryAndEdram);
    }
    if (sampler_heap_index != ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid &&
        draw_sampler_bindful_heap_index_ != sampler_heap_index) {
      write_samplers_vertex = sampler_count_vertex != 0;
      write_samplers_pixel = sampler_count_pixel != 0;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Write the descriptors.
    if (write_textures_vertex) {
      assert_true(current_graphics_root_bindful_extras_.textures_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_vertex_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV(textures_vertex[i], view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      current_texture_srv_keys_vertex_.resize(
          std::max(current_texture_srv_keys_vertex_.size(), size_t(texture_count_vertex)));
      texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_vertex_.data(),
                                                textures_vertex.data(), texture_count_vertex);
      bindful_textures_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_vertex);
    }
    if (write_textures_pixel) {
      assert_true(current_graphics_root_bindful_extras_.textures_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_pixel_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV((*textures_pixel)[i], view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      current_texture_srv_keys_pixel_.resize(
          std::max(current_texture_srv_keys_pixel_.size(), size_t(texture_count_pixel)));
      texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_pixel_.data(),
                                                textures_pixel->data(), texture_count_pixel);
      bindful_textures_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_pixel);
    }
    if (write_samplers_vertex) {
      assert_true(current_graphics_root_bindful_extras_.samplers_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_vertex_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        texture_cache_->WriteSampler(current_samplers_vertex_[i], sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_vertex);
    }
    if (write_samplers_pixel) {
      assert_true(current_graphics_root_bindful_extras_.samplers_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_pixel_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        texture_cache_->WriteSampler(current_samplers_pixel_[i], sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_pixel);
    }

    // Wrote new descriptors on the current page.
    draw_view_bindful_heap_index_ = view_heap_index;
    if (sampler_heap_index != ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      draw_sampler_bindful_heap_index_ = sampler_heap_index;
    }
  }

  // Update the root parameters.
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_fetch_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_fetch_constants,
                                                                cbuffer_binding_fetch_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_fetch_constants;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_float_constants_vertex))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_vertex, cbuffer_binding_float_vertex_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_float_constants_vertex;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_float_constants_pixel))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_pixel, cbuffer_binding_float_pixel_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_float_constants_pixel;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_system_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_system_constants,
                                                                cbuffer_binding_system_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_system_constants;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_bool_loop_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_bool_loop_constants,
                                                                cbuffer_binding_bool_loop_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_bool_loop_constants;
  }
  if (!(current_graphics_root_up_to_date_ &
        (1u << root_parameter_shared_memory_and_bindful_edram))) {
    assert_true(current_shared_memory_binding_is_uav_.has_value());
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_shared_memory_and_bindful_edram;
    if (bindless_resources_used_) {
      gpu_handle_shared_memory_and_bindful_edram = provider.OffsetViewDescriptor(
          view_bindless_heap_gpu_start_,
          uint32_t(current_shared_memory_binding_is_uav_.value()
                       ? SystemBindlessView ::kNullRawSRVAndSharedMemoryRawUAVStart
                       : SystemBindlessView ::kSharedMemoryRawSRVAndNullRawUAVStart));
    } else {
      gpu_handle_shared_memory_and_bindful_edram = current_shared_memory_binding_is_uav_.value()
                                                       ? gpu_handle_shared_memory_uav_and_edram_
                                                       : gpu_handle_shared_memory_srv_and_edram_;
    }
    deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
        root_parameter_shared_memory_and_bindful_edram, gpu_handle_shared_memory_and_bindful_edram);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_shared_memory_and_bindful_edram;
  }
  if (bindless_resources_used_) {
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesPixel))) {
      deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
          kRootParameter_Bindless_DescriptorIndicesPixel,
          cbuffer_binding_descriptor_indices_pixel_.address);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_DescriptorIndicesPixel;
    }
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesVertex))) {
      deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
          kRootParameter_Bindless_DescriptorIndicesVertex,
          cbuffer_binding_descriptor_indices_vertex_.address);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_DescriptorIndicesVertex;
    }
    if (!(current_graphics_root_up_to_date_ & (1u << kRootParameter_Bindless_SamplerHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(kRootParameter_Bindless_SamplerHeap,
                                                               sampler_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_SamplerHeap;
    }
    if (!(current_graphics_root_up_to_date_ & (1u << kRootParameter_Bindless_ViewHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(kRootParameter_Bindless_ViewHeap,
                                                               view_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_ViewHeap;
    }
  } else {
    uint32_t extra_index;
    extra_index = current_graphics_root_bindful_extras_.textures_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_textures_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_samplers_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.textures_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_textures_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_samplers_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
  }

  return true;
}

void D3D12CommandProcessor::EvictOldReadbackBuffers(
    std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map) {
  if (buffer_map.empty()) {
    return;
  }
  const uint64_t eviction_frame_floor = (frame_current_ > kReadbackBufferEvictionAgeFrames)
                                            ? (frame_current_ - kReadbackBufferEvictionAgeFrames)
                                            : 0;
  for (auto it = buffer_map.begin(); it != buffer_map.end();) {
    ReadbackBuffer& readback = it->second;
    bool evict =
        buffer_map.size() > kMaxReadbackBuffers || readback.last_used_frame < eviction_frame_floor;
    if (!evict) {
      ++it;
      continue;
    }
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
        }
        readback.buffers[i]->Release();
      }
      readback.buffers[i] = nullptr;
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
    it = buffer_map.erase(it);
  }
}

ID3D12Resource* D3D12CommandProcessor::RequestReadbackBuffer(uint32_t size) {
  if (size == 0) {
    return nullptr;
  }
  size = rex::align(size, kReadbackBufferSizeIncrement);
  if (size > readback_buffer_size_) {
    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      REXGPU_ERROR("Failed to create a {} MB readback buffer", size >> 20);
      return nullptr;
    }
    if (readback_buffer_ != nullptr) {
      readback_buffer_->Release();
    }
    readback_buffer_ = buffer;
    readback_buffer_size_ = size;
  }
  return readback_buffer_;
}

bool D3D12CommandProcessor::InitializeOcclusionQueryResources() {
  active_occlusion_query_ = {};
  occlusion_query_cursor_ = 0;
  occlusion_sample_accumulator_ = 0;
  occlusion_query_resources_available_ = false;
  occlusion_query_heap_.Reset();
  occlusion_query_readback_.Reset();
  occlusion_query_readback_mapping_ = nullptr;

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  if (!device) {
    return false;
  }

  D3D12_QUERY_HEAP_DESC heap_desc;
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  heap_desc.Count = kMaxOcclusionQueries;
  heap_desc.NodeMask = 0;
  if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&occlusion_query_heap_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to create occlusion query heap, using fake sample counts");
    return false;
  }

  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, sizeof(uint64_t) * kMaxOcclusionQueries,
                                          D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesReadback,
                                             GetD3D12Provider().GetHeapFlagCreateNotZeroed(),
                                             &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&occlusion_query_readback_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to allocate occlusion query readback buffer, using fake "
        "sample counts");
    occlusion_query_heap_.Reset();
    return false;
  }

  D3D12_RANGE read_range = {0, sizeof(uint64_t) * kMaxOcclusionQueries};
  void* mapping = nullptr;
  if (FAILED(occlusion_query_readback_->Map(0, &read_range, &mapping))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to map occlusion query readback buffer, using fake sample "
        "counts");
    occlusion_query_readback_.Reset();
    occlusion_query_heap_.Reset();
    return false;
  }

  occlusion_query_readback_mapping_ = reinterpret_cast<uint64_t*>(mapping);
  occlusion_query_resources_available_ = true;
  return true;
}

void D3D12CommandProcessor::ShutdownOcclusionQueryResources() {
  DisableHostOcclusionQueries();

  if (occlusion_query_readback_ && occlusion_query_readback_mapping_) {
    occlusion_query_readback_->Unmap(0, nullptr);
  }
  occlusion_query_readback_mapping_ = nullptr;
  occlusion_query_readback_.Reset();
  occlusion_query_heap_.Reset();
}

bool D3D12CommandProcessor::AcquireOcclusionQueryIndex(uint32_t& host_index_out) {
  if (occlusion_query_cursor_ >= kMaxOcclusionQueries) {
    occlusion_query_cursor_ = 0;
  }
  host_index_out = occlusion_query_cursor_++;
  return true;
}

void D3D12CommandProcessor::DisableHostOcclusionQueries() {
  if (active_occlusion_query_.valid && occlusion_query_heap_) {
    uint32_t host_index = active_occlusion_query_.host_index;
    // Clear before EndSubmission to prevent the EndSubmission safety net from issuing a second
    // EndQuery for the same index.
    active_occlusion_query_ = {};
    if (BeginSubmission(true)) {
      deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                         host_index);
      EndSubmission(false);
    }
  } else {
    active_occlusion_query_ = {};
  }
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
}

bool D3D12CommandProcessor::BeginGuestOcclusionQuery(uint32_t sample_count_address) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return false;
  }
  if (active_occlusion_query_.valid) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Occlusion query begin issued while another query is active");
    DisableHostOcclusionQueries();
    return false;
  }

  uint32_t host_index = 0;
  if (!AcquireOcclusionQueryIndex(host_index)) {
    return false;
  }
  if (!BeginSubmission(true)) {
    return false;
  }

  deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                       host_index);
  active_occlusion_query_.sample_count_address = sample_count_address;
  active_occlusion_query_.host_index = host_index;
  active_occlusion_query_.valid = true;
  return true;
}

bool D3D12CommandProcessor::EndGuestOcclusionQuery(
    uint32_t sample_count_address, xenos::xe_gpu_depth_sample_counts* sample_counts) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||
      !active_occlusion_query_.valid || !occlusion_query_heap_ || !occlusion_query_readback_) {
    return false;
  }

  uint32_t host_index = active_occlusion_query_.host_index;
  active_occlusion_query_ = {};

  if (!BeginSubmission(true)) {
    return false;
  }

  deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                     host_index);
  deferred_command_list_.D3DResolveQueryData(
      occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, host_index, 1,
      occlusion_query_readback_.Get(), sizeof(uint64_t) * host_index);

  if (!EndSubmission(false)) {
    return false;
  }

  uint64_t query_submission = submission_current_ ? submission_current_ - 1 : 0;
  CheckSubmissionFence(query_submission);
  if (submission_completed_ < query_submission) {
    return false;
  }
  if (!occlusion_query_readback_mapping_) {
    return false;
  }

  uint64_t raw_samples = occlusion_query_readback_mapping_[host_index];
  uint64_t samples = NormalizeOcclusionSamples(raw_samples);
  occlusion_sample_accumulator_ += samples;
  static uint64_t nx1_occlusion_result_count = 0;
  ++nx1_occlusion_result_count;
  if (Nx1ShouldLogOcclusionProbe(nx1_occlusion_result_count)) {
    REXGPU_WARN(
        "[NX1 occlusion-probe] result #{} host_index={} sample_addr=0x{:08X} raw={} "
        "normalized={} cumulative={}",
        nx1_occlusion_result_count, host_index, sample_count_address, raw_samples, samples,
        occlusion_sample_accumulator_);
  }
  WriteGuestOcclusionResult(sample_counts, occlusion_sample_accumulator_);
  return true;
}

uint64_t D3D12CommandProcessor::NormalizeOcclusionSamples(uint64_t samples) const {
  if (samples == 0 || !texture_cache_) {
    return samples;
  }
  uint64_t scale_x = texture_cache_->draw_resolution_scale_x();
  uint64_t scale_y = texture_cache_->draw_resolution_scale_y();
  uint64_t scale = scale_x * scale_y;
  if (scale <= 1) {
    return samples;
  }
  return (samples + (scale >> 1)) / scale;
}

void D3D12CommandProcessor::WriteGuestOcclusionResult(
    xenos::xe_gpu_depth_sample_counts* sample_counts, uint64_t samples) {
  if (!sample_counts) {
    return;
  }
  uint32_t sample_value = uint32_t(samples);
  sample_counts->Total_A = sample_value;
  sample_counts->Total_B = 0;
  sample_counts->ZPass_A = sample_value;
  sample_counts->ZPass_B = 0;
  sample_counts->ZFail_A = 0;
  sample_counts->ZFail_B = 0;
  sample_counts->StencilFail_A = 0;
  sample_counts->StencilFail_B = 0;
}

void D3D12CommandProcessor::WriteGammaRampSRV(bool is_pwl,
                                              D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
  if (is_pwl) {
    desc.Format = DXGI_FORMAT_R16G16_UINT;
    desc.Buffer.FirstElement = 256 * 4 / 4;
    desc.Buffer.NumElements = 128 * 3;
  } else {
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = 256;
  }
  device->CreateShaderResourceView(gamma_ramp_buffer_.Get(), &desc, handle);
}

}  // namespace rex::graphics::d3d12
