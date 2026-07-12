// nx1_mp - ReXGlue Recompiled Project

#include "generated/5-nx1mp_demo/nx1_mp_init.h"

#include "nx1_mp_app.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(nx1_gamma_render_target_as_unorm16, false, "GPU",
                    "NX1 workaround: use ReXGlue's UNORM16 gamma render target path instead of "
                    "host sRGB gamma render targets")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(nx1_internal_resolution_patch, true, "NX1",
                    "Override NX1's internal HD render resolution")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(nx1_internal_resolution_width, 1920, "NX1",
                      "NX1 internal render width")
    .range(640, 0x0FFF)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(nx1_internal_resolution_height, 1080, "NX1",
                      "NX1 internal render height")
    .range(480, 0x0FFF)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(nx1_internal_resolution_auto_draw_scale, true, "NX1",
                    "Use ReXGlue draw-resolution scaling for requested resolutions that are "
                    "integer multiples of the native 720p frontbuffer")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_DEFINE_APP(nx1_mp, Nx1MpApp::Create)
