/**
 * @file    d3d9_log.h
 * @brief   Category switches for the native D3D9 renderer's recurring logging.
 *
 * WHY THIS EXISTS. The renderer accumulated 283 log statements over a long
 * investigation, 168 of them with no cvar behind them, and several firing on a
 * fixed cadence forever after the question they answered was settled -- the
 * command-buffer traffic line printed every 100 calls (1330 lines in one
 * session), the decode-change line every change (1058). That is a real frame-time
 * cost, and it lands on a build whose timing we are trying to MEASURE: the
 * remaining bug is a race against the guest's texture streamer, and diagnosing a
 * timing bug on a build whose timing is distorted by its own diagnostics is a
 * methodological problem, not just an annoyance.
 *
 * A global "disable all logging" already exists in the F4 overlay, but it is too
 * blunt -- an active experiment needs its own line kept while the rest is
 * silenced. These are per-category switches for exactly that.
 *
 * The cvars are registered under the category "Logging", which is all that is
 * needed to give them their own sidebar entry: the overlay builds its sidebar
 * from the set of cvar categories (d3d9_overlay.cpp, the "##cats" child).
 *
 * SCOPE, deliberately limited:
 *  - REXGPU_ERROR is never gated. An error that only prints sometimes is worse
 *    than one that costs a few microseconds.
 *  - One-shot logs (startup, capability probes, first-sighting reports) are left
 *    alone; they cost nothing and going through 283 call sites for them is churn
 *    with a real chance of breaking something.
 *  - Instruments that already have their own cvar (dbg_window, dbg_fetchset,
 *    tornpages, ...) keep it. Those are what you switch ON for an experiment;
 *    these are what you switch OFF to get the frame rate back.
 *
 * Defaults are TRUE so behaviour is unchanged until someone turns them off.
 */

#pragma once

#include <rex/cvar.h>
#include <rex/logging/macros.h>

// Declared at GLOBAL scope, matching where they are defined (d3d9_resources.cpp defines its cvars
// above the namespace opener). d3d9_hooks.cpp's REX_HOOK_RAW bodies are extern "C" at file scope,
// so a namespaced declaration would be invisible there -- REXCVAR_GET expands to an unqualified
// storage symbol.

/// Periodic renderer statistics: PROF, cache/MEM/MEMPOOLS, mipgen, resolve and
/// classification rollups. High volume, useful in aggregate, never urgent.
REXCVAR_DECLARE(bool, nx1_d3d9_log_stats);
/// Per-texture events: decode changes, new textures, tracked-address reports.
REXCVAR_DECLARE(bool, nx1_d3d9_log_texture);
/// Image-cache DMA and command-buffer traffic: DMACOPY, DMARETRY, CMDBUF, fences.
REXCVAR_DECLARE(bool, nx1_d3d9_log_dma);
/// Everything else recurring that does not fit above.
REXCVAR_DECLARE(bool, nx1_d3d9_log_misc);

// The guarded forms.
//
// do/while(0), NOT a bare `if`. A bare `if` expansion has a dangling-else hazard: any call site
// written as `if (x) NX1_LOGW_TEX(...); else ...` would bind the else to the macro's own if and
// silently change control flow. With 108 converted sites that is not a risk worth taking for
// tidier expansion, which was the only reason to prefer the bare form.
#define NX1_LOGI_STATS(...)   do { if (REXCVAR_GET(nx1_d3d9_log_stats)) REXGPU_INFO(__VA_ARGS__); } while (0)
#define NX1_LOGW_STATS(...)   do { if (REXCVAR_GET(nx1_d3d9_log_stats)) REXGPU_WARN(__VA_ARGS__); } while (0)
#define NX1_LOGI_TEX(...)   do { if (REXCVAR_GET(nx1_d3d9_log_texture)) REXGPU_INFO(__VA_ARGS__); } while (0)
#define NX1_LOGW_TEX(...)   do { if (REXCVAR_GET(nx1_d3d9_log_texture)) REXGPU_WARN(__VA_ARGS__); } while (0)
#define NX1_LOGI_DMA(...)   do { if (REXCVAR_GET(nx1_d3d9_log_dma)) REXGPU_INFO(__VA_ARGS__); } while (0)
#define NX1_LOGW_DMA(...)   do { if (REXCVAR_GET(nx1_d3d9_log_dma)) REXGPU_WARN(__VA_ARGS__); } while (0)
#define NX1_LOGI_MISC(...)   do { if (REXCVAR_GET(nx1_d3d9_log_misc)) REXGPU_INFO(__VA_ARGS__); } while (0)
#define NX1_LOGW_MISC(...)   do { if (REXCVAR_GET(nx1_d3d9_log_misc)) REXGPU_WARN(__VA_ARGS__); } while (0)
