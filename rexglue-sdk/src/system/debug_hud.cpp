#include <rex/system/debug_hud.h>

REXCVAR_DEFINE_BOOL(nx1_debug_hud, false, "NX1",
                    "Show NX1 built-in debug HUD, text, and performance overlays");

REXCVAR_DEFINE_BOOL(nx1_debug_hud_stat_bars, false, "NX1",
                    "Show NX1 stat monitor warning bars and icons when debug HUD is enabled");

REXCVAR_DEFINE_BOOL(nx1_force_developer_dvar_off, true, "NX1",
                    "Force NX1 developer dvars to 0 unless nx1_debug_hud is enabled");
REXCVAR_DEFINE_BOOL(nx1_force_gp_rdvar_camo_off, true, "NX1",
                    "Force NX1 gp_rdvar_camo_enabled to 0 to suppress MP top-left flicker");
REXCVAR_DEFINE_BOOL(nx1_force_screen_filter_quads_off, false, "NX1",
                    "Force NX1 r_screenFilterQuads to 0 to avoid 1080p screen-filter strip artifacts");
REXCVAR_DEFINE_BOOL(nx1_force_resample_scene_off, false, "NX1",
                    "Force NX1 r_resampleScene to 0 for true internal-resolution rendering");
REXCVAR_DEFINE_BOOL(nx1_force_glow_off, false, "NX1",
                    "Force NX1 glow and bloom vision-set dvars/effects to 0");
REXCVAR_DEFINE_BOOL(nx1_force_film_off, false, "NX1",
                    "Force NX1 film vision-set dvars to 0");
REXCVAR_DEFINE_BOOL(nx1_force_color_curve_off, false, "NX1",
                    "Force NX1 color curve vision-set dvars to 0");
REXCVAR_DEFINE_BOOL(nx1_force_color_lut_off, false, "NX1",
                    "Force NX1 color LUT vision-set dvars to 0");
REXCVAR_DEFINE_BOOL(nx1_force_dof_off, false, "NX1",
                    "Force NX1 depth-of-field dvars/effects to 0");
REXCVAR_DEFINE_BOOL(nx1_force_distortion_off, false, "NX1",
                    "Force NX1 distortion dvars/effects to 0");
REXCVAR_DEFINE_BOOL(nx1_force_blur_off, false, "NX1",
                    "Force NX1 blur and radial blur effects to 0");
REXCVAR_DEFINE_BOOL(nx1_glow_filter_force_fullscreen, true, "NX1",
                    "Force NX1 glow filter/composite passes through fullscreen filtering to avoid 1080p viewport artifacts");
