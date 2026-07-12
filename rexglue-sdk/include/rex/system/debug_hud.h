#pragma once

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, nx1_debug_hud);
REXCVAR_DECLARE(bool, nx1_debug_hud_stat_bars);
REXCVAR_DECLARE(bool, nx1_force_developer_dvar_off);
REXCVAR_DECLARE(bool, nx1_force_gp_rdvar_camo_off);
REXCVAR_DECLARE(bool, nx1_force_screen_filter_quads_off);
REXCVAR_DECLARE(bool, nx1_force_resample_scene_off);
REXCVAR_DECLARE(bool, nx1_force_glow_off);
REXCVAR_DECLARE(bool, nx1_force_film_off);
REXCVAR_DECLARE(bool, nx1_force_color_curve_off);
REXCVAR_DECLARE(bool, nx1_force_color_lut_off);
REXCVAR_DECLARE(bool, nx1_force_dof_off);
REXCVAR_DECLARE(bool, nx1_force_distortion_off);
REXCVAR_DECLARE(bool, nx1_force_blur_off);
REXCVAR_DECLARE(bool, nx1_glow_filter_force_fullscreen);

namespace rex::system {

inline bool IsNx1DebugHudEnabled() {
  return REXCVAR_GET(nx1_debug_hud);
}

inline bool IsNx1DebugHudStatBarsEnabled() {
  return REXCVAR_GET(nx1_debug_hud_stat_bars);
}

inline bool IsNx1ForceDeveloperDvarOffEnabled() {
  return REXCVAR_GET(nx1_force_developer_dvar_off) && !IsNx1DebugHudEnabled();
}

inline bool IsNx1ForceGpRdvarCamoOffEnabled() {
  return REXCVAR_GET(nx1_force_gp_rdvar_camo_off);
}

inline bool IsNx1ForceScreenFilterQuadsOffEnabled() {
  return REXCVAR_GET(nx1_force_screen_filter_quads_off);
}

inline bool IsNx1ForceResampleSceneOffEnabled() {
  return REXCVAR_GET(nx1_force_resample_scene_off);
}

inline bool IsNx1ForceGlowOffEnabled() {
  return REXCVAR_GET(nx1_force_glow_off);
}

inline bool IsNx1ForceFilmOffEnabled() {
  return REXCVAR_GET(nx1_force_film_off);
}

inline bool IsNx1ForceColorCurveOffEnabled() {
  return REXCVAR_GET(nx1_force_color_curve_off);
}

inline bool IsNx1ForceColorLutOffEnabled() {
  return REXCVAR_GET(nx1_force_color_lut_off);
}

inline bool IsNx1ForceDofOffEnabled() {
  return REXCVAR_GET(nx1_force_dof_off);
}

inline bool IsNx1ForceDistortionOffEnabled() {
  return REXCVAR_GET(nx1_force_distortion_off);
}

inline bool IsNx1ForceBlurOffEnabled() {
  return REXCVAR_GET(nx1_force_blur_off);
}

inline bool IsNx1GlowFilterForceFullscreenEnabled() {
  return REXCVAR_GET(nx1_glow_filter_force_fullscreen);
}

}  // namespace rex::system
