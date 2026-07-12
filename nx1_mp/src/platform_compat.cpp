// Host CRT compatibility shims for generated code.

#include "generated/5-nx1mp_demo/nx1_mp_init.h"

#include <rex/cvar.h>
#include <rex/input/mnk/mouse_look.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/system/debug_hud.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xlive_web_client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#if defined(_WIN32)
extern "C" float roundevenf(float value) {
  if (!std::isfinite(value) || value == 0.0f) {
    return value;
  }

  const float lower = std::floor(value);
  const float fraction = value - lower;
  if (fraction < 0.5f) {
    return lower;
  }
  if (fraction > 0.5f) {
    return lower + 1.0f;
  }

  const float half_lower = lower * 0.5f;
  return half_lower == std::trunc(half_lower) ? lower : lower + 1.0f;
}
#endif

namespace {
constexpr uint32_t kRDisplayOutputScaledPtr = 0x84176418;
constexpr uint32_t kCgDrawFPSDvarPtr = 0x82639ED8;
constexpr uint32_t kCgDrawFPSLabelsDvarPtr = 0x8264B540;
bool g_mp_logged_native_mouse_hook = false;
bool g_mp_logged_dvar_filter = false;
bool g_mp_logged_blackbox_skip = false;

uint32_t ClampWidth(uint32_t width) {
  return std::clamp(width, 640u, 0x0FFFu);
}

uint32_t ClampHeight(uint32_t height) {
  return std::clamp(height, 480u, 0x0FFFu);
}

uint32_t Nx1InternalResolutionWidth() {
  return ClampWidth(rex::cvar::Query<uint32_t>("nx1_internal_resolution_width"));
}

uint32_t Nx1InternalResolutionHeight() {
  return ClampHeight(rex::cvar::Query<uint32_t>("nx1_internal_resolution_height"));
}

struct Nx1RenderResolution {
  uint32_t requested_width;
  uint32_t requested_height;
  uint32_t guest_width;
  uint32_t guest_height;
  uint32_t draw_scale_x;
  uint32_t draw_scale_y;
};

Nx1RenderResolution ResolveNx1RenderResolution() {
  constexpr uint32_t kScaledBaseWidth = 1280;
  constexpr uint32_t kScaledBaseHeight = 720;
  constexpr uint32_t kMaxDrawScale = 8;

  Nx1RenderResolution resolution{};
  resolution.requested_width = Nx1InternalResolutionWidth();
  resolution.requested_height = Nx1InternalResolutionHeight();
  resolution.guest_width = resolution.requested_width;
  resolution.guest_height = resolution.requested_height;
  resolution.draw_scale_x = 1;
  resolution.draw_scale_y = 1;

  if (!rex::cvar::Query<bool>("nx1_internal_resolution_auto_draw_scale")) {
    return resolution;
  }

  if (resolution.requested_width % kScaledBaseWidth != 0 ||
      resolution.requested_height % kScaledBaseHeight != 0) {
    return resolution;
  }

  const uint32_t scale_x = resolution.requested_width / kScaledBaseWidth;
  const uint32_t scale_y = resolution.requested_height / kScaledBaseHeight;
  if (scale_x <= 1 || scale_y <= 1 || scale_x > kMaxDrawScale ||
      scale_y > kMaxDrawScale) {
    return resolution;
  }

  resolution.guest_width = kScaledBaseWidth;
  resolution.guest_height = kScaledBaseHeight;
  resolution.draw_scale_x = scale_x;
  resolution.draw_scale_y = scale_y;
  return resolution;
}

void SetDisplayOutputScaled(PPCContext& ctx, uint8_t* base, bool scaled) {
  const uint32_t dvar = REX_LOAD_U32(kRDisplayOutputScaledPtr);
  if (dvar == 0) {
    return;
  }

  PPCContext dvar_ctx = ctx;
  dvar_ctx.r3.u64 = dvar;
  dvar_ctx.r4.u64 = scaled ? 1 : 0;
  rex_Dvar_SetBool_YAXPBUdvar_t_N_Z(dvar_ctx, base);
}

bool GuestStringEquals(uint8_t* base, uint32_t guest_string,
                       std::string_view expected) {
  if (guest_string == 0) {
    return false;
  }

  for (size_t i = 0; i < expected.size(); ++i) {
    if (REX_LOAD_U8(guest_string + static_cast<uint32_t>(i)) !=
        static_cast<uint8_t>(expected[i])) {
      return false;
    }
  }

  return REX_LOAD_U8(guest_string + static_cast<uint32_t>(expected.size())) == 0;
}

bool GuestStringStartsWith(uint8_t* base, uint32_t guest_string,
                           std::string_view prefix) {
  if (guest_string == 0) {
    return false;
  }

  for (size_t i = 0; i < prefix.size(); ++i) {
    if (REX_LOAD_U8(guest_string + static_cast<uint32_t>(i)) !=
        static_cast<uint8_t>(prefix[i])) {
      return false;
    }
  }

  return true;
}

bool IsNx1DeveloperOverlayDvarName(uint8_t* base, uint32_t guest_name) {
  return GuestStringEquals(base, guest_name, "developer") ||
         GuestStringStartsWith(base, guest_name, "developer_") ||
         GuestStringEquals(base, guest_name, "cg_drawFPS") ||
         GuestStringEquals(base, guest_name, "cg_drawFPSLabels") ||
         GuestStringEquals(base, guest_name, "cg_drawViewpos");
}

bool ShouldClampNx1DvarName(uint8_t* base, uint32_t guest_name) {
  if (rex::system::IsNx1ForceGpRdvarCamoOffEnabled() &&
      GuestStringEquals(base, guest_name, "gp_rdvar_camo_enabled")) {
    return true;
  }

  if (rex::system::IsNx1ForceScreenFilterQuadsOffEnabled() &&
      GuestStringEquals(base, guest_name, "r_screenFilterQuads")) {
    return true;
  }

  if (rex::system::IsNx1ForceResampleSceneOffEnabled() &&
      GuestStringEquals(base, guest_name, "r_resampleScene")) {
    return true;
  }

  if (rex::system::IsNx1ForceGlowOffEnabled() &&
      (GuestStringEquals(base, guest_name, "r_glow") ||
       GuestStringEquals(base, guest_name, "r_glow_allowed") ||
       GuestStringEquals(base, guest_name, "r_glow_allowed_script_forced") ||
       GuestStringEquals(base, guest_name, "r_glowUseTweaks") ||
       GuestStringEquals(base, guest_name, "r_glowTweakEnable"))) {
    return true;
  }

  if (rex::system::IsNx1ForceFilmOffEnabled() &&
      (GuestStringEquals(base, guest_name, "r_filmEnable") ||
       GuestStringEquals(base, guest_name, "r_filmUseTweaks") ||
       GuestStringEquals(base, guest_name, "r_filmTweakEnable") ||
       GuestStringEquals(base, guest_name, "r_filmAltShader"))) {
    return true;
  }

  if (rex::system::IsNx1ForceColorCurveOffEnabled() &&
      (GuestStringEquals(base, guest_name, "r_colorCurveEnable") ||
       GuestStringEquals(base, guest_name, "r_colorCurveUseTweaks") ||
       GuestStringEquals(base, guest_name, "r_colorCurveTweakEnable"))) {
    return true;
  }

  if (rex::system::IsNx1ForceColorLutOffEnabled() &&
      (GuestStringEquals(base, guest_name, "r_colorLutEnable") ||
       GuestStringEquals(base, guest_name, "r_colorLutNearest"))) {
    return true;
  }

  if (rex::system::IsNx1ForceDofOffEnabled() &&
      (GuestStringEquals(base, guest_name, "r_dof_enable") ||
       GuestStringEquals(base, guest_name, "r_dof_tweak"))) {
    return true;
  }

  if (rex::system::IsNx1ForceDistortionOffEnabled() &&
      (GuestStringEquals(base, guest_name, "r_distortion") ||
       GuestStringEquals(base, guest_name, "r_hit_distortion_params"))) {
    return true;
  }

  if (rex::system::IsNx1ForceBlurOffEnabled() &&
      GuestStringEquals(base, guest_name, "r_blur")) {
    return true;
  }

  return rex::system::IsNx1ForceDeveloperDvarOffEnabled() &&
         IsNx1DeveloperOverlayDvarName(base, guest_name);
}

bool ShouldForceNx1DebugDvarOnName(uint8_t* base, uint32_t guest_name) {
  return rex::system::IsNx1DebugHudEnabled() &&
         IsNx1DeveloperOverlayDvarName(base, guest_name);
}

bool GetForcedNx1DvarValueName(uint8_t* base, uint32_t guest_name,
                               uint32_t* out_value) {
  if (ShouldForceNx1DebugDvarOnName(base, guest_name)) {
    *out_value = GuestStringEquals(base, guest_name, "cg_drawFPS") ? 3u : 1u;
    return true;
  }

  if (ShouldClampNx1DvarName(base, guest_name)) {
    *out_value = 0;
    return true;
  }

  return false;
}

bool GetForcedNx1DvarValue(uint8_t* base, uint32_t guest_dvar,
                           uint32_t* out_value) {
  if (guest_dvar == 0) {
    return false;
  }

  return GetForcedNx1DvarValueName(base, REX_LOAD_U32(guest_dvar), out_value);
}

void LogNx1DvarFilterOnce() {
  if (g_mp_logged_dvar_filter) {
    return;
  }

  g_mp_logged_dvar_filter = true;
  REXLOG_INFO(
      "NX1 MP: dvar filter active debug_hud={} force_developer_off={} "
      "force_glow_off={}",
      rex::system::IsNx1DebugHudEnabled(),
      rex::system::IsNx1ForceDeveloperDvarOffEnabled(),
      rex::system::IsNx1ForceGlowOffEnabled());
}

void PinForcedNx1DvarValue(uint8_t* base, uint32_t guest_dvar) {
  uint32_t forced_value = 0;
  if (!GetForcedNx1DvarValue(base, guest_dvar, &forced_value)) {
    return;
  }

  const uint8_t dvar_type = REX_LOAD_U8(guest_dvar + 10);
  if (dvar_type == 0) {
    REX_STORE_U8(guest_dvar + 12, forced_value != 0);
  } else {
    REX_STORE_U32(guest_dvar + 12, forced_value);
  }
}

void PinNx1DebugHudDvarPointer(uint8_t* base, uint32_t guest_dvar_pointer,
                               uint32_t value) {
  const uint32_t guest_dvar = REX_LOAD_U32(guest_dvar_pointer);
  if (guest_dvar == 0) {
    return;
  }

  const uint8_t dvar_type = REX_LOAD_U8(guest_dvar + 10);
  if (dvar_type == 0) {
    REX_STORE_U8(guest_dvar + 12, value != 0);
  } else {
    REX_STORE_U32(guest_dvar + 12, value);
  }
}

void PinNx1DebugHudDvars(uint8_t* base) {
  if (!rex::system::IsNx1DebugHudEnabled()) {
    return;
  }

  PinNx1DebugHudDvarPointer(base, kCgDrawFPSDvarPtr, 3);
  PinNx1DebugHudDvarPointer(base, kCgDrawFPSLabelsDvarPtr, 1);
}

bool ApplyForcedNx1DvarValueName(uint8_t* base, uint32_t guest_name,
                                 PPCRegister& value) {
  uint32_t forced_value = 0;
  if (!GetForcedNx1DvarValueName(base, guest_name, &forced_value)) {
    return false;
  }

  LogNx1DvarFilterOnce();
  value.u64 = forced_value;
  return true;
}

bool ApplyForcedNx1DvarValue(uint8_t* base, uint32_t guest_dvar,
                             PPCRegister& value) {
  uint32_t forced_value = 0;
  if (!GetForcedNx1DvarValue(base, guest_dvar, &forced_value)) {
    return false;
  }

  LogNx1DvarFilterOnce();
  value.u64 = forced_value;
  return true;
}

void LogBlackBoxSkipOnce() {
  if (g_mp_logged_blackbox_skip) {
    return;
  }

  g_mp_logged_blackbox_skip = true;
  REXLOG_WARN("NX1 MP: BB_Connect skipped; blackbox telemetry server is unavailable");
}

void NoteMpDiscordMenus(std::string_view reason) {
  REXLOG_INFO("NX1 MP: Discord Rich Presence returned to menus ({})", reason);
  rex::system::XLiveWebClient::Get().ClearLoadedMap(REX_KERNEL_STATE());
}

float LoadGuestFloat(uint8_t* base, uint32_t guest_address) {
  PPCRegister value{};
  value.u32 = REX_LOAD_U32(guest_address);
  return value.f32;
}

bool IsPointerInsideRect(float pointer_x, float pointer_y, float rect_x,
                         float rect_y, float rect_w, float rect_h,
                         float padding) {
  return pointer_x >= rect_x - padding &&
         pointer_x <= rect_x + rect_w + padding &&
         pointer_y >= rect_y - padding &&
         pointer_y <= rect_y + rect_h + padding;
}

bool IsMenuAcceptKey(uint32_t key) {
  return key == 13 || key == 156 || key == 157 || key == 163 || key == 164;
}

struct MenuPointerCandidate {
  float x;
  float y;
  uint32_t priority;
};

struct MenuHoverRect {
  float x;
  float y;
  float w;
  float h;
};

uint32_t GetMpActiveScreenPlacement(PPCContext& ctx, uint8_t* base,
                                    uint32_t local_client) {
  (void)ctx;
  (void)base;
  (void)local_client;
  return 0;
}

float MpVirtualPlacementOffsetForX(uint32_t align) {
  switch (align) {
    case 2:
    case 7:
    case 9:
      return 320.0f;
    case 3:
    case 10:
      return 640.0f;
    case 0:
    case 1:
    case 4:
    case 5:
    case 6:
    case 8:
    default:
      return 0.0f;
  }
}

float MpVirtualPlacementOffsetForY(uint32_t align) {
  switch (align) {
    case 2:
    case 7:
    case 9:
      return 240.0f;
    case 3:
    case 10:
      return 480.0f;
    case 0:
    case 1:
    case 4:
    case 5:
    case 6:
    case 8:
    default:
      return 0.0f;
  }
}

float MpScreenPlacementOffsetForX(uint8_t* base, uint32_t placement,
                                  uint32_t align) {
  switch (align) {
    case 1:
      return LoadGuestFloat(base, placement + 56);
    case 2:
      return LoadGuestFloat(base, placement + 32) * 0.5f;
    case 3:
      return LoadGuestFloat(base, placement + 64);
    case 7:
      return (LoadGuestFloat(base, placement + 56) +
              LoadGuestFloat(base, placement + 64)) *
             0.5f;
    case 8:
      return LoadGuestFloat(base, placement + 88);
    case 9:
      return (LoadGuestFloat(base, placement + 88) +
              LoadGuestFloat(base, placement + 96)) *
             0.5f;
    case 10:
      return LoadGuestFloat(base, placement + 96);
    case 0:
    default:
      return LoadGuestFloat(base, placement + 104);
  }
}

float MpScreenPlacementOffsetForY(uint8_t* base, uint32_t placement,
                                  uint32_t align) {
  switch (align) {
    case 1:
      return LoadGuestFloat(base, placement + 60);
    case 2:
      return LoadGuestFloat(base, placement + 36) * 0.5f;
    case 3:
      return LoadGuestFloat(base, placement + 68);
    case 7:
      return (LoadGuestFloat(base, placement + 60) +
              LoadGuestFloat(base, placement + 68)) *
             0.5f;
    case 8:
      return LoadGuestFloat(base, placement + 92);
    case 9:
      return (LoadGuestFloat(base, placement + 92) +
              LoadGuestFloat(base, placement + 100)) *
             0.5f;
    case 10:
      return LoadGuestFloat(base, placement + 100);
    case 0:
    default:
      return 0.0f;
  }
}

void ApplyMpScreenPlacementToRect(uint8_t* base, uint32_t placement,
                                  uint32_t horizontal_align,
                                  uint32_t vertical_align,
                                  MenuHoverRect* rect) {
  if (rect == nullptr) {
    return;
  }

  if (placement == 0) {
    rect->x += MpVirtualPlacementOffsetForX(horizontal_align);
    rect->y += MpVirtualPlacementOffsetForY(vertical_align);
    return;
  }

  if (horizontal_align == 4) {
    const float scale = LoadGuestFloat(base, placement + 8);
    rect->x *= scale;
    rect->w *= scale;
  } else if (horizontal_align == 5) {
    // no scaling
  } else if (horizontal_align == 6) {
    const float scale = LoadGuestFloat(base, placement + 16);
    rect->x *= scale;
    rect->w *= scale;
  } else {
    const float scale = LoadGuestFloat(base, placement + 0);
    rect->x = rect->x * scale +
              MpScreenPlacementOffsetForX(base, placement, horizontal_align);
    rect->w *= scale;
  }

  if (vertical_align == 4) {
    const float scale = LoadGuestFloat(base, placement + 12);
    rect->y *= scale;
    rect->h *= scale;
  } else if (vertical_align == 5) {
    // no scaling
  } else if (vertical_align == 6) {
    const float scale = LoadGuestFloat(base, placement + 20);
    rect->y *= scale;
    rect->h *= scale;
  } else {
    const float scale = LoadGuestFloat(base, placement + 4);
    rect->y = rect->y * scale +
              MpScreenPlacementOffsetForY(base, placement, vertical_align);
    rect->h *= scale;
  }
}

MenuHoverRect GetMpDrawnItemRect(uint8_t* base, uint32_t item,
                                 uint32_t placement) {
  MenuHoverRect rect{
      LoadGuestFloat(base, item + 4),
      LoadGuestFloat(base, item + 8),
      LoadGuestFloat(base, item + 12),
      LoadGuestFloat(base, item + 16),
  };
  ApplyMpScreenPlacementToRect(base, placement, REX_LOAD_U8(item + 20),
                               REX_LOAD_U8(item + 21), &rect);
  return rect;
}

MenuHoverRect GetMpCorrectedItemRect(uint8_t* base, uint32_t item) {
  return MenuHoverRect{
      LoadGuestFloat(base, item + 92),
      LoadGuestFloat(base, item + 96),
      LoadGuestFloat(base, item + 100),
      LoadGuestFloat(base, item + 104),
  };
}

bool IsValidMenuHoverRect(const MenuHoverRect& rect) {
  return std::isfinite(rect.x) && std::isfinite(rect.y) &&
         std::isfinite(rect.w) && std::isfinite(rect.h) &&
         rect.w > 0.0f && rect.h > 0.0f;
}

uint32_t GetMpTopOpenMenu(uint8_t* base, uint32_t ui_context) {
  constexpr uint32_t kUiStackCountOffset = 2680;
  constexpr uint32_t kUiStackBaseIndex = 654;

  const uint32_t stack_count = REX_LOAD_U32(ui_context + kUiStackCountOffset);
  if (stack_count == 0 || stack_count > 64) {
    return 0;
  }

  return REX_LOAD_U32(
      ui_context + (kUiStackBaseIndex + stack_count - 1) * 4);
}

bool MpMenuPaintsOnLayer(uint8_t* base, uint32_t menu, uint32_t layer) {
  if (menu == 0) {
    return false;
  }

  const uint32_t flags = REX_LOAD_U32(menu + 72);
  const uint32_t menu_layer = ((flags << 10) | (flags >> 22)) & 1u;
  return menu_layer == (layer & 1u);
}

bool IsMpMenuInOpenStack(uint8_t* base, uint32_t ui_context, uint32_t menu) {
  constexpr uint32_t kUiStackCountOffset = 2680;
  constexpr uint32_t kUiStackBaseIndex = 654;

  const uint32_t stack_count = REX_LOAD_U32(ui_context + kUiStackCountOffset);
  if (stack_count == 0 || stack_count > 64 || menu == 0) {
    return false;
  }

  for (uint32_t i = 0; i < stack_count; ++i) {
    const uint32_t stack_menu =
        REX_LOAD_U32(ui_context + (kUiStackBaseIndex + i) * 4);
    if (stack_menu == menu) {
      return true;
    }
  }

  return false;
}

uint32_t GetMpFrontmostPaintedMenu(uint8_t* base, uint32_t ui_context,
                                   uint32_t layer) {
  constexpr uint32_t kUiActiveMenuBaseOffset = 52;
  constexpr uint32_t kUiActiveMenuCountOffset = 2612;
  constexpr uint32_t kUiStackCountOffset = 2680;
  constexpr uint32_t kUiStackBaseIndex = 654;

  uint32_t frontmost = 0;
  const uint32_t active_count =
      REX_LOAD_U32(ui_context + kUiActiveMenuCountOffset);
  if (active_count <= 256) {
    for (uint32_t i = 0; i < active_count; ++i) {
      const uint32_t menu =
          REX_LOAD_U32(ui_context + kUiActiveMenuBaseOffset + i * 4);
      if (!IsMpMenuInOpenStack(base, ui_context, menu) &&
          MpMenuPaintsOnLayer(base, menu, layer)) {
        frontmost = menu;
      }
    }
  }

  const uint32_t stack_count = REX_LOAD_U32(ui_context + kUiStackCountOffset);
  if (stack_count <= 64) {
    for (uint32_t i = 0; i < stack_count; ++i) {
      const uint32_t menu =
          REX_LOAD_U32(ui_context + (kUiStackBaseIndex + i) * 4);
      if (MpMenuPaintsOnLayer(base, menu, layer)) {
        frontmost = menu;
      }
    }
  }

  return frontmost;
}

bool IsMpTopMenuOnStack(uint8_t* base, uint32_t ui_context, uint32_t menu) {
  const uint32_t top_menu = GetMpTopOpenMenu(base, ui_context);
  return top_menu == 0 || top_menu == menu;
}

uint32_t GetMpFocusedMenu(PPCContext& ctx, uint8_t* base,
                          uint32_t ui_context) {
  if (ui_context == 0) {
    return 0;
  }

  PPCContext focus_ctx = ctx;
  focus_ctx.r3.u64 = ui_context;
  rex_Menu_GetFocused_YAPAUmenuDef_t_PAUUiContext_Z(focus_ctx, base);
  return focus_ctx.r3.u32;
}

struct MenuHoverFocusTarget {
  uint32_t ui_context = 0;
  uint32_t menu = 0;
  uint32_t local_client = UINT32_MAX;
  uint32_t item = 0;
  int32_t item_index = -1;
  int32_t rect_x = 0;
  int32_t rect_y = 0;
  int32_t rect_w = 0;
  int32_t rect_h = 0;
  uint32_t pending_ui_context = 0;
  uint32_t pending_menu = 0;
  uint32_t pending_local_client = UINT32_MAX;
  uint32_t pending_item = 0;
  int32_t pending_item_index = -1;
  uint32_t pending_frames = 0;
};

MenuHoverFocusTarget g_mp_menu_hover_focus_target;
std::unordered_map<uint64_t, MenuHoverFocusTarget> g_mp_menu_hover_focus_targets;
std::unordered_map<uint64_t, int32_t> g_mp_ownerdraw_feeder_selections;
std::unordered_map<uint64_t, bool> g_mp_logged_ownerdraw_feeders;
std::unordered_map<uint64_t, bool> g_mp_logged_menu_hover_probes;
std::unordered_map<uint64_t, bool> g_mp_logged_menu_hover_skips;
uint32_t g_mp_logged_ownerdraw_feeder_count = 0;
uint32_t g_mp_logged_menu_hover_probe_count = 0;
uint32_t g_mp_logged_menu_hover_skip_count = 0;

uint64_t MakeMenuHoverFocusKey(uint32_t menu, uint32_t local_client) {
  return (static_cast<uint64_t>(menu) << 2) | (local_client & 0x3u);
}

uint32_t ClearMpMenuFocusOnce(PPCContext& ctx, uint8_t* base,
                              uint32_t ui_context, uint32_t menu) {
  if (ui_context == 0 || menu == 0) {
    return 0;
  }

  PPCContext focus_ctx = ctx;
  focus_ctx.r3.u64 = ui_context;
  focus_ctx.r4.u64 = menu;
  rex_Menu_ClearFocus_YAPAUitemDef_s_PAUUiContext_PAUmenuDef_t_Z(
      focus_ctx, base);
  return focus_ctx.r3.u32;
}

void ClearMpMenuFocusCompletely(PPCContext& ctx, uint8_t* base,
                                uint32_t ui_context, uint32_t menu) {
  if (ui_context == 0 || menu == 0) {
    return;
  }

  const uint32_t item_count = REX_LOAD_U32(menu + 180);
  const uint32_t max_iterations =
      item_count > 0 && item_count <= 512 ? item_count : 512;
  for (uint32_t i = 0; i < max_iterations; ++i) {
    if (ClearMpMenuFocusOnce(ctx, base, ui_context, menu) == 0) {
      break;
    }
  }
}

void RemoveMpItemFocusFlag(PPCContext& ctx, uint8_t* base,
                           uint32_t local_client, uint32_t item) {
  if (item == 0 || local_client > 3) {
    return;
  }

  PPCContext focus_ctx = ctx;
  focus_ctx.r3.u64 = local_client;
  focus_ctx.r4.u64 = item;
  focus_ctx.r5.u64 = 2;
  rex_Window_RemoveDynamicFlags_YAXHPAUwindowDef_t_H_Z(focus_ctx, base);
}

void ClearMpMenuFocusFlags(PPCContext& ctx, uint8_t* base,
                           uint32_t local_client, uint32_t menu) {
  if (menu == 0 || local_client > 3) {
    return;
  }

  const uint32_t item_count = REX_LOAD_U32(menu + 180);
  const uint32_t items = REX_LOAD_U32(menu + 184);
  if (items == 0 || item_count == 0 || item_count > 512) {
    return;
  }

  for (uint32_t i = 0; i < item_count; ++i) {
    RemoveMpItemFocusFlag(ctx, base, local_client, REX_LOAD_U32(items + i * 4));
  }
}

bool NoteMpMenuHoverFocusTarget(uint32_t ui_context, uint32_t menu,
                                uint32_t local_client, uint32_t item,
                                int32_t item_index, int32_t rect_x,
                                int32_t rect_y, int32_t rect_w, int32_t rect_h,
                                bool* item_pointer_changed);
bool IsMpMenuHoverCandidateStable(uint32_t ui_context, uint32_t menu,
                                  uint32_t local_client, uint32_t item,
                                  int32_t item_index);
void ApplyMpFullMenuHoverFocus(PPCContext& ctx, uint8_t* base,
                               uint32_t ui_context, uint32_t menu,
                               uint32_t local_client, uint32_t item);

bool IsMpMenuItemVisible(PPCContext& ctx, uint8_t* base,
                         uint32_t local_client, uint32_t item) {
  if (item == 0 || local_client > 3) {
    return false;
  }

  PPCContext probe_ctx = ctx;
  probe_ctx.r3.u64 = local_client;
  probe_ctx.r4.u64 = item;
  rex_Item_IsVisible_YAHHPAUitemDef_s_Z(probe_ctx, base);
  return probe_ctx.r3.u32 != 0;
}

bool IsMpOwnerDrawItem(uint8_t* base, uint32_t item) {
  if (item == 0) {
    return false;
  }

  return REX_LOAD_U32(item + 256) == 8 || REX_LOAD_U32(item + 260) == 8;
}

uint32_t GetMpListBoxDef(PPCContext& ctx, uint8_t* base, uint32_t item) {
  if (item == 0) {
    return 0;
  }

  PPCContext list_ctx = ctx;
  list_ctx.r3.u64 = item;
  rex_Item_GetListBoxDef_YAPAUlistBoxDef_s_PAUitemDef_s_Z(list_ctx, base);
  return list_ctx.r3.u32;
}

int32_t QueryMpListBoxVisibleCount(PPCContext& ctx, uint8_t* base,
                                   uint32_t item) {
  PPCContext count_ctx = ctx;
  count_ctx.r3.u64 = item;
  rex_Item_ListBox_GetVisibleElementCount(count_ctx, base);
  int32_t visible_count = count_ctx.r3.s32;
  if (visible_count > 0) {
    return visible_count;
  }

  count_ctx = ctx;
  count_ctx.r3.u64 = item;
  rex_Item_ListBox_Viewmax_YAHPAUitemDef_s_Z(count_ctx, base);
  visible_count = count_ctx.r3.s32;
  return visible_count > 0 ? visible_count : 1;
}

int32_t QueryMpListBoxFeederCount(PPCContext& ctx, uint8_t* base,
                                  uint32_t local_client, uint32_t item) {
  PPCRegister feeder{};
  feeder.u32 = REX_LOAD_U32(item + 376);

  PPCContext count_ctx = ctx;
  count_ctx.r3.u64 = local_client;
  count_ctx.f1.f64 = double(feeder.f32);
  rex_UI_FeederCount_YAHHM_Z(count_ctx, base);
  return count_ctx.r3.s32;
}

int32_t QueryMpFeederCount(PPCContext& ctx, uint8_t* base,
                           uint32_t local_client, uint32_t feeder_bits) {
  PPCRegister feeder{};
  feeder.u32 = feeder_bits;
  if (!std::isfinite(feeder.f32)) {
    return 0;
  }

  PPCContext count_ctx = ctx;
  count_ctx.r3.u64 = local_client;
  count_ctx.f1.f64 = double(feeder.f32);
  rex_UI_FeederCount_YAHHM_Z(count_ctx, base);

  const int32_t count = count_ctx.r3.s32;
  return count > 0 && count <= 512 ? count : 0;
}

struct MpMenuItemRef {
  uint32_t menu = 0;
  int32_t item_index = -1;
};

bool MpMenuContainsItem(uint8_t* base, uint32_t menu, uint32_t item,
                        int32_t* item_index) {
  if (menu == 0 || item == 0) {
    return false;
  }

  const uint32_t item_count = REX_LOAD_U32(menu + 180);
  const uint32_t items = REX_LOAD_U32(menu + 184);
  if (items == 0 || item_count == 0 || item_count > 512) {
    return false;
  }

  for (uint32_t i = 0; i < item_count; ++i) {
    if (REX_LOAD_U32(items + i * 4) == item) {
      if (item_index != nullptr) {
        *item_index = static_cast<int32_t>(i);
      }
      return true;
    }
  }

  return false;
}

MpMenuItemRef FindMpMenuContainingItem(uint8_t* base, uint32_t ui_context,
                                       uint32_t item) {
  constexpr uint32_t kUiActiveMenuBaseOffset = 52;
  constexpr uint32_t kUiActiveMenuCountOffset = 2612;
  constexpr uint32_t kUiStackCountOffset = 2680;
  constexpr uint32_t kUiStackBaseIndex = 654;

  MpMenuItemRef result{};
  if (ui_context == 0 || item == 0) {
    return result;
  }

  const uint32_t stack_count = REX_LOAD_U32(ui_context + kUiStackCountOffset);
  if (stack_count <= 64) {
    for (uint32_t i = stack_count; i > 0; --i) {
      const uint32_t menu =
          REX_LOAD_U32(ui_context + (kUiStackBaseIndex + i - 1) * 4);
      int32_t item_index = -1;
      if (MpMenuContainsItem(base, menu, item, &item_index)) {
        result.menu = menu;
        result.item_index = item_index;
        return result;
      }
    }
  }

  const uint32_t active_count =
      REX_LOAD_U32(ui_context + kUiActiveMenuCountOffset);
  if (active_count <= 256) {
    for (uint32_t i = active_count; i > 0; --i) {
      const uint32_t menu =
          REX_LOAD_U32(ui_context + kUiActiveMenuBaseOffset + (i - 1) * 4);
      int32_t item_index = -1;
      if (MpMenuContainsItem(base, menu, item, &item_index)) {
        result.menu = menu;
        result.item_index = item_index;
        return result;
      }
    }
  }

  return result;
}

bool FindMpListBoxPointerRow(PPCContext& ctx, uint8_t* base,
                             uint32_t local_client, uint32_t item,
                             uint32_t list_box,
                             const MenuHoverRect* rects,
                             uint32_t rect_count,
                             const MenuPointerCandidate* candidates,
                             uint32_t candidate_count, float hit_padding,
                             int32_t* selection_out,
                             int32_t* visible_count_out) {
  if (selection_out == nullptr || visible_count_out == nullptr ||
      rects == nullptr || candidates == nullptr ||
      rect_count == 0 || candidate_count == 0) {
    return false;
  }

  const int32_t feeder_count =
      QueryMpListBoxFeederCount(ctx, base, local_client, item);
  if (feeder_count <= 0) {
    return false;
  }

  int32_t visible_count = QueryMpListBoxVisibleCount(ctx, base, item);
  visible_count = std::clamp(visible_count, 1, 512);

  const MenuPointerCandidate* best_candidate = nullptr;
  const MenuHoverRect* best_rect = nullptr;
  bool best_is_broad_row_match = false;
  for (uint32_t pass = 0; pass < 2; ++pass) {
    for (uint32_t rect_index = 0; rect_index < rect_count; ++rect_index) {
      const MenuHoverRect& rect = rects[rect_index];
      if (!IsValidMenuHoverRect(rect)) {
        continue;
      }

      const float row_x_padding =
          std::max(96.0f, std::min(260.0f, rect.w * 1.25f));
      for (uint32_t candidate_index = 0; candidate_index < candidate_count;
           ++candidate_index) {
        const MenuPointerCandidate& candidate = candidates[candidate_index];
        const bool strict_hit = IsPointerInsideRect(
            candidate.x, candidate.y, rect.x, rect.y, rect.w, rect.h,
            hit_padding);
        const bool broad_row_hit =
            candidate.y >= rect.y - hit_padding &&
            candidate.y <= rect.y + rect.h + hit_padding &&
            candidate.x >= rect.x - row_x_padding &&
            candidate.x <= rect.x + rect.w + row_x_padding;
        if ((pass == 0 && !strict_hit) ||
            (pass == 1 && (strict_hit || !broad_row_hit))) {
          continue;
        }

        if (best_candidate == nullptr ||
            candidate.priority < best_candidate->priority ||
            (candidate.priority == best_candidate->priority &&
             pass < (best_is_broad_row_match ? 1u : 0u))) {
          best_candidate = &candidate;
          best_rect = &rect;
          best_is_broad_row_match = pass != 0;
        }
      }
    }

    if (best_candidate != nullptr) {
      break;
    }
  }

  if (best_candidate == nullptr || best_rect == nullptr) {
    return false;
  }

  float row_height = LoadGuestFloat(base, list_box + 36);
  if (!std::isfinite(row_height) || row_height <= 1.0f ||
      row_height > best_rect->h + 8.0f) {
    row_height = best_rect->h / static_cast<float>(visible_count);
  }
  if (!std::isfinite(row_height) || row_height <= 0.0f) {
    return false;
  }

  int32_t row = static_cast<int32_t>(
      std::floor((best_candidate->y - best_rect->y) / row_height));
  if (row < 0 || row >= visible_count) {
    return false;
  }

  const int32_t top_index = std::clamp(
      static_cast<int32_t>(REX_LOAD_U32(list_box + local_client * 4)), 0,
      std::max(0, feeder_count - 1));
  *selection_out = std::clamp(top_index + row, 0, feeder_count - 1);
  *visible_count_out = visible_count;
  return true;
}

bool FindMpOwnerDrawFeederPointerRow(
    const MenuHoverRect* rects, uint32_t rect_count,
    const MenuPointerCandidate* candidates, uint32_t candidate_count,
    float hit_padding, int32_t feeder_count, int32_t* selection_out) {
  if (rects == nullptr || candidates == nullptr || selection_out == nullptr ||
      rect_count == 0 || candidate_count == 0 || feeder_count <= 1) {
    return false;
  }

  const MenuPointerCandidate* best_candidate = nullptr;
  const MenuHoverRect* best_rect = nullptr;
  bool best_is_broad_match = false;
  for (uint32_t pass = 0; pass < 2; ++pass) {
    for (uint32_t rect_index = 0; rect_index < rect_count; ++rect_index) {
      const MenuHoverRect& rect = rects[rect_index];
      if (!IsValidMenuHoverRect(rect) || rect.h < 8.0f || rect.w < 8.0f) {
        continue;
      }

      const float row_x_padding =
          std::max(32.0f, std::min(160.0f, rect.w * 0.65f));
      for (uint32_t candidate_index = 0; candidate_index < candidate_count;
           ++candidate_index) {
        const MenuPointerCandidate& candidate = candidates[candidate_index];
        const bool strict_hit = IsPointerInsideRect(
            candidate.x, candidate.y, rect.x, rect.y, rect.w, rect.h,
            hit_padding);
        const bool broad_row_hit =
            candidate.y >= rect.y - hit_padding &&
            candidate.y <= rect.y + rect.h + hit_padding &&
            candidate.x >= rect.x - row_x_padding &&
            candidate.x <= rect.x + rect.w + row_x_padding;
        if ((pass == 0 && !strict_hit) ||
            (pass == 1 && (strict_hit || !broad_row_hit))) {
          continue;
        }

        if (best_candidate == nullptr ||
            candidate.priority < best_candidate->priority ||
            (candidate.priority == best_candidate->priority &&
             pass < (best_is_broad_match ? 1u : 0u))) {
          best_candidate = &candidate;
          best_rect = &rect;
          best_is_broad_match = pass != 0;
        }
      }
    }

    if (best_candidate != nullptr) {
      break;
    }
  }

  if (best_candidate == nullptr || best_rect == nullptr) {
    return false;
  }

  int32_t visible_count = feeder_count;
  float row_height = best_rect->h / static_cast<float>(visible_count);
  if (visible_count > 18 || row_height < 12.0f || row_height > 44.0f) {
    constexpr float kExpectedOwnerDrawRowHeight = 24.0f;
    visible_count = std::clamp(
        static_cast<int32_t>(std::lround(best_rect->h /
                                         kExpectedOwnerDrawRowHeight)),
        1, std::min(feeder_count, 24));
    row_height = best_rect->h / static_cast<float>(visible_count);
  }
  if (!std::isfinite(row_height) || row_height <= 0.0f) {
    return false;
  }

  const int32_t row = static_cast<int32_t>(
      std::floor((best_candidate->y - best_rect->y) / row_height));
  if (row < 0 || row >= visible_count) {
    return false;
  }

  *selection_out = std::clamp(row, 0, feeder_count - 1);
  return true;
}

bool ApplyMpListBoxSelection(PPCContext& ctx, uint8_t* base,
                             uint32_t local_client, uint32_t item,
                             int32_t visible_count, int32_t selection) {
  const int32_t current_selection =
      static_cast<int32_t>(REX_LOAD_U32(item + (local_client + 95) * 4));
  if (current_selection == selection) {
    return false;
  }

  PPCContext list_ctx = ctx;
  list_ctx.r3.u64 = local_client;
  list_ctx.r4.u64 = item;
  list_ctx.r5.u64 = static_cast<uint32_t>(visible_count);
  list_ctx.r6.u64 = static_cast<uint32_t>(selection);
  rex_Item_ListBox_SetCursorPos(list_ctx, base);
  return true;
}

bool ApplyMpOwnerDrawFeederSelection(PPCContext& ctx, uint8_t* base,
                                     uint32_t ui_context, uint32_t menu,
                                     uint32_t local_client, uint32_t item,
                                     uint32_t feeder_bits,
                                     int32_t selection) {
  PPCRegister feeder{};
  feeder.u32 = feeder_bits;
  if (!std::isfinite(feeder.f32) || local_client > 3 || selection < 0) {
    return false;
  }

  const uint64_t key = (static_cast<uint64_t>(item) << 32) ^
                       (static_cast<uint64_t>(feeder_bits) << 2) ^
                       (local_client & 0x3u);
  const auto previous = g_mp_ownerdraw_feeder_selections.find(key);
  const int32_t current_item_selection =
      item != 0 ? static_cast<int32_t>(
                      REX_LOAD_U32(item + (local_client + 95) * 4))
                : INT32_MIN;
  if (previous != g_mp_ownerdraw_feeder_selections.end() &&
      previous->second == selection && current_item_selection == selection) {
    return false;
  }
  g_mp_ownerdraw_feeder_selections[key] = selection;

  const int32_t integral_feeder =
      static_cast<int32_t>(std::lround(feeder.f32));
  if (ui_context != 0 && menu != 0 &&
      std::fabs(feeder.f32 - static_cast<float>(integral_feeder)) < 0.001f) {
    PPCContext menu_ctx = ctx;
    menu_ctx.r3.u64 = ui_context;
    menu_ctx.r4.u64 = menu;
    menu_ctx.r5.u64 = static_cast<uint32_t>(integral_feeder);
    menu_ctx.r6.u64 = static_cast<uint32_t>(selection);
    menu_ctx.r7.u64 = 0;
    rex_Menu_SetFeederSelection_YAXPAUUiContext_PAUmenuDef_t_HHPBD_Z(
        menu_ctx, base);
    return true;
  }

  if (item != 0) {
    REX_STORE_U32(item + (local_client + 95) * 4,
                  static_cast<uint32_t>(selection));
  }
  PPCContext feeder_ctx = ctx;
  feeder_ctx.r3.u64 = local_client;
  feeder_ctx.f1.f64 = double(feeder.f32);
  feeder_ctx.r5.u64 = static_cast<uint32_t>(selection);
  rex_UI_FeederSelection_YAXHMH_Z(feeder_ctx, base);
  return true;
}

bool TryApplyMpOwnerDrawFeederMouseHover(
    PPCContext& ctx, uint8_t* base, uint32_t ui_context, uint32_t menu,
    uint32_t local_client, uint32_t item, int32_t item_index,
    const MenuHoverRect* rects, uint32_t rect_count,
    const MenuPointerCandidate* candidates, uint32_t candidate_count,
    float hit_padding, uint32_t pointer_generation, bool force_full_focus) {
  if (!IsMpOwnerDrawItem(base, item) ||
      GetMpListBoxDef(ctx, base, item) != 0 ||
      !IsMpMenuItemVisible(ctx, base, local_client, item)) {
    return false;
  }

  const uint32_t feeder_bits = REX_LOAD_U32(item + 376);
  const int32_t feeder_count =
      QueryMpFeederCount(ctx, base, local_client, feeder_bits);
  if (feeder_count <= 1) {
    return false;
  }

  int32_t selection = 0;
  if (!FindMpOwnerDrawFeederPointerRow(rects, rect_count, candidates,
                                       candidate_count, hit_padding,
                                       feeder_count, &selection)) {
    return false;
  }

  if (menu != 0 && item_index >= 0) {
    const uint32_t state = REX_LOAD_U32(menu + 0);
    if (state != 0) {
      const uint32_t cursor_offset = (local_client + 25) * 4;
      if (REX_LOAD_U32(state + cursor_offset) !=
          static_cast<uint32_t>(item_index)) {
        REX_STORE_U32(state + cursor_offset, static_cast<uint32_t>(item_index));
      }
    }

    bool item_pointer_changed = false;
    const bool focus_target_changed = NoteMpMenuHoverFocusTarget(
        ui_context, menu, local_client, item, item_index,
        static_cast<int32_t>(std::lround(rects[0].x)),
        static_cast<int32_t>(std::lround(rects[0].y)),
        static_cast<int32_t>(std::lround(rects[0].w)),
        static_cast<int32_t>(std::lround(rects[0].h)), &item_pointer_changed);
    if (force_full_focus || focus_target_changed || item_pointer_changed) {
      ApplyMpFullMenuHoverFocus(ctx, base, ui_context, menu, local_client,
                                item);
    }
  }

  (void)ApplyMpOwnerDrawFeederSelection(ctx, base, ui_context, menu,
                                        local_client, item, feeder_bits,
                                        selection);
  rex::input::mnk::ReportMenuDirectHoverApplied(pointer_generation);
  return true;
}

bool TryApplyMpUiOwnerDrawFeederMouseHover(
    PPCContext& ctx, uint8_t* base, uint32_t local_client, uint32_t ownerdraw,
    float x, float y, float w, float h, uint32_t feeder_bits,
    const MenuPointerCandidate* candidates, uint32_t candidate_count,
    float hit_padding, uint32_t pointer_generation) {
  if (local_client > 3 || candidates == nullptr || candidate_count == 0) {
    return false;
  }

  const int32_t feeder_count =
      QueryMpFeederCount(ctx, base, local_client, feeder_bits);
  if (feeder_count <= 1) {
    return false;
  }

  const MenuHoverRect rects[] = {
      {x, y, w, h},
      {x - w * 0.5f, y, w, h},
      {x, y - h * 0.5f, w, h},
  };

  int32_t selection = 0;
  if (!FindMpOwnerDrawFeederPointerRow(
          rects, static_cast<uint32_t>(sizeof(rects) / sizeof(rects[0])),
          candidates, candidate_count, hit_padding, feeder_count,
          &selection)) {
    return false;
  }

  const uint64_t log_key = (static_cast<uint64_t>(ownerdraw) << 32) ^
                           (static_cast<uint64_t>(feeder_bits) << 2) ^
                           (local_client & 0x3u);
  if (g_mp_logged_ownerdraw_feeder_count < 16 &&
      g_mp_logged_ownerdraw_feeders.emplace(log_key, true).second) {
    PPCRegister feeder{};
    feeder.u32 = feeder_bits;
    ++g_mp_logged_ownerdraw_feeder_count;
    REXLOG_INFO(
        "NX1 MP: ownerdraw feeder hover owner={} feeder={} count={} rect=({}, {}, {}, {}) selection={}",
        ownerdraw, feeder.f32, feeder_count, x, y, w, h, selection);
  }

  (void)ApplyMpOwnerDrawFeederSelection(ctx, base, 0, 0, local_client, 0,
                                        feeder_bits, selection);
  rex::input::mnk::ReportMenuDirectHoverApplied(pointer_generation);
  return true;
}

bool TryApplyMpListBoxSelectionOnly(PPCContext& ctx, uint8_t* base,
                                    uint32_t local_client, uint32_t item,
                                    const MenuHoverRect* rects,
                                    uint32_t rect_count,
                                    const MenuPointerCandidate* candidates,
                                    uint32_t candidate_count,
                                    float hit_padding,
                                    uint32_t pointer_generation) {
  const uint32_t list_box = GetMpListBoxDef(ctx, base, item);
  if (list_box == 0 ||
      !IsMpMenuItemVisible(ctx, base, local_client, item)) {
    return false;
  }

  int32_t selection = 0;
  int32_t visible_count = 0;
  if (!FindMpListBoxPointerRow(ctx, base, local_client, item, list_box, rects,
                               rect_count, candidates, candidate_count,
                               hit_padding, &selection, &visible_count)) {
    return false;
  }

  (void)ApplyMpListBoxSelection(ctx, base, local_client, item, visible_count,
                                selection);
  rex::input::mnk::ReportMenuDirectHoverApplied(pointer_generation);
  return true;
}

bool TryApplyMpListBoxMouseHover(PPCContext& ctx, uint8_t* base,
                                 uint32_t ui_context, uint32_t menu,
                                 uint32_t local_client, uint32_t item,
                                 int32_t item_index,
                                 const MenuHoverRect& rect,
                                 const MenuPointerCandidate* candidates,
                                 uint32_t candidate_count, float hit_padding,
                                 uint32_t pointer_generation,
                                 bool force_full_focus) {
  const uint32_t list_box = GetMpListBoxDef(ctx, base, item);
  if (list_box == 0 ||
      !IsMpMenuItemVisible(ctx, base, local_client, item)) {
    return false;
  }

  const MenuHoverRect rects[] = {
      rect,
      GetMpCorrectedItemRect(base, item),
  };
  int32_t selection = 0;
  int32_t visible_count = 0;
  if (!FindMpListBoxPointerRow(
          ctx, base, local_client, item, list_box, rects,
          static_cast<uint32_t>(sizeof(rects) / sizeof(rects[0])), candidates,
          candidate_count, hit_padding, &selection, &visible_count)) {
    return false;
  }

  if (!force_full_focus &&
      !IsMpMenuHoverCandidateStable(ui_context, menu, local_client, item,
                                    item_index)) {
    rex::input::mnk::ReportMenuDirectHoverApplied(pointer_generation);
    return true;
  }

  const uint32_t state = REX_LOAD_U32(menu + 0);
  if (state != 0) {
    const uint32_t menu_cursor_offset = (local_client + 25) * 4;
    const uint32_t current_menu_cursor =
        REX_LOAD_U32(state + menu_cursor_offset);
    if (current_menu_cursor != static_cast<uint32_t>(item_index)) {
      REX_STORE_U32(state + menu_cursor_offset,
                    static_cast<uint32_t>(item_index));
    }
  }

  bool item_pointer_changed = false;
  const bool focus_target_changed = NoteMpMenuHoverFocusTarget(
      ui_context, menu, local_client, item, item_index,
      static_cast<int32_t>(std::lround(rect.x)),
      static_cast<int32_t>(std::lround(rect.y)),
      static_cast<int32_t>(std::lround(rect.w)),
      static_cast<int32_t>(std::lround(rect.h)), &item_pointer_changed);

  if (force_full_focus || focus_target_changed || item_pointer_changed) {
    ApplyMpFullMenuHoverFocus(ctx, base, ui_context, menu, local_client, item);
  }

  (void)ApplyMpListBoxSelection(ctx, base, local_client, item, visible_count,
                                selection);

  rex::input::mnk::ReportMenuDirectHoverApplied(pointer_generation);
  return true;
}

bool IsMpMenuDirectHoverSelectableCandidate(PPCContext& ctx, uint8_t* base,
                                            uint32_t local_client,
                                            uint32_t item) {
  if (item == 0 || local_client > 3) {
    return false;
  }

  PPCContext probe_ctx = ctx;
  probe_ctx.r3.u64 = local_client;
  probe_ctx.r4.u64 = item;
  rex_Item_CanHaveFocus_YAHHPAUitemDef_s_Z(probe_ctx, base);
  if (probe_ctx.r3.u32 == 0) {
    return false;
  }

  probe_ctx = ctx;
  probe_ctx.r3.u64 = local_client;
  probe_ctx.r4.u64 = item;
  rex_Item_IsVisible_YAHHPAUitemDef_s_Z(probe_ctx, base);
  return probe_ctx.r3.u32 != 0;
}

bool LooksLikeGuestPointer(uint32_t guest_address) {
  return guest_address >= 0x80000000u && guest_address < 0xC0000000u;
}

std::string GuestStringPreview(uint8_t* base, uint32_t guest_string) {
  if (!LooksLikeGuestPointer(guest_string)) {
    return {};
  }

  std::string value;
  value.reserve(32);
  for (uint32_t i = 0; i < 32; ++i) {
    const uint8_t ch = REX_LOAD_U8(guest_string + i);
    if (ch == 0) {
      break;
    }
    value.push_back(ch >= 32 && ch < 127 ? static_cast<char>(ch) : '?');
  }
  return value;
}

uint32_t QueryMpItemVisible(PPCContext& ctx, uint8_t* base,
                            uint32_t local_client, uint32_t item) {
  if (item == 0 || local_client > 3) {
    return 0;
  }

  PPCContext probe_ctx = ctx;
  probe_ctx.r3.u64 = local_client;
  probe_ctx.r4.u64 = item;
  rex_Item_IsVisible_YAHHPAUitemDef_s_Z(probe_ctx, base);
  return probe_ctx.r3.u32 != 0 ? 1u : 0u;
}

uint32_t QueryMpItemCanHaveFocus(PPCContext& ctx, uint8_t* base,
                                 uint32_t local_client, uint32_t item) {
  if (item == 0 || local_client > 3) {
    return 0;
  }

  PPCContext probe_ctx = ctx;
  probe_ctx.r3.u64 = local_client;
  probe_ctx.r4.u64 = item;
  rex_Item_CanHaveFocus_YAHHPAUitemDef_s_Z(probe_ctx, base);
  return probe_ctx.r3.u32 != 0 ? 1u : 0u;
}

void LogMpMenuHoverSkipOnce(uint8_t* base, uint32_t ui_context,
                            uint32_t menu, uint32_t top_menu,
                            uint32_t focused_menu) {
  if (g_mp_logged_menu_hover_skip_count >= 16) {
    return;
  }

  const uint64_t key = (static_cast<uint64_t>(menu) << 32) ^
                       (static_cast<uint64_t>(top_menu) << 1) ^
                       focused_menu;
  if (!g_mp_logged_menu_hover_skips.emplace(key, true).second) {
    return;
  }

  ++g_mp_logged_menu_hover_skip_count;
  REXLOG_INFO(
      "NX1 MP: menu hover skipped menu={} top={} focused={} active_count={} stack_count={}",
      menu, top_menu, focused_menu, REX_LOAD_U32(ui_context + 2612),
      REX_LOAD_U32(ui_context + 2680));
}

void LogMpMenuHoverProbeOnce(PPCContext& ctx, uint8_t* base,
                             uint32_t ui_context, uint32_t menu,
                             uint32_t local_client, uint32_t item_count,
                             uint32_t items, float pointer_x, float pointer_y,
                             bool top_menu_match,
                             bool focused_menu_match) {
  if (g_mp_logged_menu_hover_probe_count >= 24) {
    return;
  }

  const uint64_t key =
      (static_cast<uint64_t>(menu) << 32) ^
      (static_cast<uint64_t>(item_count) << 16) ^ local_client;
  if (!g_mp_logged_menu_hover_probes.emplace(key, true).second) {
    return;
  }

  ++g_mp_logged_menu_hover_probe_count;
  const uint32_t state = REX_LOAD_U32(menu + 0);
  const uint32_t cursor =
      state != 0 && local_client <= 3
          ? REX_LOAD_U32(state + (local_client + 25) * 4)
          : UINT32_MAX;
  REXLOG_INFO(
      "NX1 MP: menu hover probe menu={} ui={} lc={} items={} cursor={} pointer=({}, {}) top_match={} focused_match={}",
      menu, ui_context, local_client, item_count, cursor, pointer_x, pointer_y,
      top_menu_match ? 1 : 0, focused_menu_match ? 1 : 0);

  const uint32_t max_logged_items = std::min<uint32_t>(item_count, 48u);
  for (uint32_t i = 0; i < max_logged_items; ++i) {
    const uint32_t item = REX_LOAD_U32(items + i * 4);
    if (item == 0) {
      REXLOG_INFO("NX1 MP: menu hover probe item {} null", i);
      continue;
    }

    const MenuHoverRect drawn_rect = GetMpDrawnItemRect(base, item, 0);
    const MenuHoverRect corrected_rect = GetMpCorrectedItemRect(base, item);
    const uint32_t item_state_flags =
        local_client <= 3 ? REX_LOAD_U32(item + (local_client + 18) * 4) : 0;
    const uint32_t type = REX_LOAD_U32(item + 256);
    const uint32_t def_type = REX_LOAD_U32(item + 260);
    const uint32_t static_flags = REX_LOAD_U32(item + 68);
    const uint32_t text_ptr = REX_LOAD_U32(item + 300);
    const uint32_t list_box = GetMpListBoxDef(ctx, base, item);
    const uint32_t visible = QueryMpItemVisible(ctx, base, local_client, item);
    const uint32_t can_focus =
        QueryMpItemCanHaveFocus(ctx, base, local_client, item);
    const bool ownerdraw_item = IsMpOwnerDrawItem(base, item);
    int32_t feeder_count = -1;
    if (ownerdraw_item) {
      feeder_count = QueryMpFeederCount(ctx, base, local_client,
                                        REX_LOAD_U32(item + 56));
    }

    REXLOG_INFO(
        "NX1 MP: menu hover probe item {} addr={} type={} def={} flags={} state_flags={} vis={} focus={} listbox={} ownerdraw={} feeder_count={} rect=({}, {}, {}, {}) corr=({}, {}, {}, {}) text={} '{}'",
        i, item, type, def_type, static_flags, item_state_flags, visible,
        can_focus, list_box, ownerdraw_item ? 1 : 0, feeder_count,
        drawn_rect.x, drawn_rect.y, drawn_rect.w, drawn_rect.h,
        corrected_rect.x, corrected_rect.y, corrected_rect.w,
        corrected_rect.h, text_ptr, GuestStringPreview(base, text_ptr));
  }
}

bool NoteMpMenuHoverFocusTarget(uint32_t ui_context, uint32_t menu,
                                uint32_t local_client, uint32_t item,
                                int32_t item_index, int32_t rect_x,
                                int32_t rect_y, int32_t rect_w, int32_t rect_h,
                                bool* item_pointer_changed) {
  MenuHoverFocusTarget& target =
      g_mp_menu_hover_focus_targets[MakeMenuHoverFocusKey(menu, local_client)];
  const bool stable_target_changed =
      target.ui_context != ui_context ||
      target.menu != menu ||
      target.local_client != local_client ||
      target.item_index != item_index;
  const bool item_changed = target.item != item;
  if (item_pointer_changed != nullptr) {
    *item_pointer_changed = item_changed;
  }

  if (!stable_target_changed && !item_changed) {
    return false;
  }

  target.ui_context = ui_context;
  target.menu = menu;
  target.local_client = local_client;
  target.item = item;
  target.item_index = item_index;
  target.rect_x = rect_x;
  target.rect_y = rect_y;
  target.rect_w = rect_w;
  target.rect_h = rect_h;
  target.pending_ui_context = 0;
  target.pending_menu = 0;
  target.pending_local_client = UINT32_MAX;
  target.pending_item = 0;
  target.pending_item_index = -1;
  target.pending_frames = 0;
  return stable_target_changed;
}

bool IsMpMenuHoverCandidateStable(uint32_t ui_context, uint32_t menu,
                                  uint32_t local_client, uint32_t item,
                                  int32_t item_index) {
  const uint32_t required_frames = static_cast<uint32_t>(
      std::clamp(rex::cvar::Query<int32_t>(
                     "mnk_menu_direct_hover_stable_frames"),
                 0, 30));
  if (required_frames <= 1) {
    return true;
  }

  MenuHoverFocusTarget& target =
      g_mp_menu_hover_focus_targets[MakeMenuHoverFocusKey(menu, local_client)];
  if (target.ui_context == ui_context && target.menu == menu &&
      target.local_client == local_client && target.item == item &&
      target.item_index == item_index) {
    target.pending_frames = 0;
    return true;
  }

  const bool pending_changed =
      target.pending_ui_context != ui_context ||
      target.pending_menu != menu ||
      target.pending_local_client != local_client ||
      target.pending_item != item ||
      target.pending_item_index != item_index;
  if (pending_changed) {
    target.pending_ui_context = ui_context;
    target.pending_menu = menu;
    target.pending_local_client = local_client;
    target.pending_item = item;
    target.pending_item_index = item_index;
    target.pending_frames = 1;
    return false;
  }

  if (target.pending_frames < UINT32_MAX) {
    ++target.pending_frames;
  }
  return target.pending_frames >= required_frames;
}

void ResetMpMenuHoverFocusTarget() {
  g_mp_menu_hover_focus_target = MenuHoverFocusTarget{};
  g_mp_menu_hover_focus_targets.clear();
  g_mp_ownerdraw_feeder_selections.clear();
  g_mp_logged_ownerdraw_feeders.clear();
  g_mp_logged_menu_hover_probes.clear();
  g_mp_logged_menu_hover_skips.clear();
  g_mp_logged_ownerdraw_feeder_count = 0;
  g_mp_logged_menu_hover_probe_count = 0;
  g_mp_logged_menu_hover_skip_count = 0;
}

void ApplyMpQuietMenuHoverFocus(PPCContext& ctx, uint8_t* base,
                                uint32_t ui_context, uint32_t menu,
                                uint32_t local_client, uint32_t item,
                                int32_t item_index, bool clear_old_focus) {
  (void)ui_context;
  (void)item;
  if (clear_old_focus) {
    ClearMpMenuFocusCompletely(ctx, base, ui_context, menu);
  }

  PPCContext focus_ctx = ctx;
  focus_ctx.r3.u64 = local_client;
  focus_ctx.r4.u64 = menu;
  focus_ctx.r5.u64 = static_cast<uint32_t>(item_index);
  rex_Menu_SetCursorItem_YAXHPAUmenuDef_t_H_Z(focus_ctx, base);
}

void ApplyMpFullMenuHoverFocus(PPCContext& ctx, uint8_t* base,
                               uint32_t ui_context, uint32_t menu,
                               uint32_t local_client, uint32_t item) {
  ClearMpMenuFocusCompletely(ctx, base, ui_context, menu);
  ClearMpMenuFocusFlags(ctx, base, local_client, menu);

  PPCContext focus_ctx = ctx;
  focus_ctx.r3.u64 = ui_context;
  focus_ctx.r4.u64 = item;
  rex_Item_SetFocus(focus_ctx, base);
}

bool ApplyMenuDirectMouseHover(PPCContext& ctx, uint8_t* base,
                               uint32_t ui_context, uint32_t menu,
                               bool force_full_focus = false) {
  if (!rex::cvar::Query<bool>("mnk_menu_direct_hover") ||
      ui_context == 0 || menu == 0) {
    return false;
  }

  const uint32_t top_menu = GetMpTopOpenMenu(base, ui_context);
  const uint32_t focused_menu = GetMpFocusedMenu(ctx, base, ui_context);
  const bool top_menu_match = top_menu == 0 || top_menu == menu;
  const bool focused_menu_match = focused_menu == menu;
  if (!top_menu_match && !focused_menu_match) {
    LogMpMenuHoverSkipOnce(base, ui_context, menu, top_menu, focused_menu);
    return false;
  }

  float pointer_x = 0.0f;
  float pointer_y = 0.0f;
  uint32_t pointer_generation = 0;
  if (!rex::input::mnk::QueryMenuPointerVirtualPosition(
          &pointer_x, &pointer_y, &pointer_generation)) {
    return false;
  }

  const uint32_t local_client = REX_LOAD_U32(ui_context + 0);
  if (local_client > 3) {
    return false;
  }

  const uint32_t state = REX_LOAD_U32(menu + 0);
  const uint32_t item_count = REX_LOAD_U32(menu + 180);
  const uint32_t items = REX_LOAD_U32(menu + 184);
  if (state == 0 || items == 0 || item_count == 0 || item_count > 512) {
    return false;
  }

  LogMpMenuHoverProbeOnce(ctx, base, ui_context, menu, local_client,
                          item_count, items, pointer_x, pointer_y,
                          top_menu_match, focused_menu_match);

  const float padding = static_cast<float>(
      std::clamp(rex::cvar::Query<double>("mnk_menu_direct_hover_padding"),
                 -16.0, 32.0));
  const float hit_padding = force_full_focus ? std::max(padding, 6.0f)
                                             : padding;
  const float normalized_x = pointer_x / 640.0f;
  const float normalized_y = pointer_y / 480.0f;
  const uint32_t output_width = Nx1InternalResolutionWidth();
  const uint32_t output_height = Nx1InternalResolutionHeight();
  const MenuPointerCandidate pointer_candidates[] = {
      {pointer_x, pointer_y, 0},
      {normalized_x * 1280.0f, normalized_y * 720.0f, 1},
      {normalized_x * static_cast<float>(output_width),
       normalized_y * static_cast<float>(output_height), 2},
  };
  const uint32_t screen_placement =
      GetMpActiveScreenPlacement(ctx, base, local_client);
  int32_t best_item = -1;
  uint32_t best_priority = UINT32_MAX;
  float best_area = 1.0e30f;
  float best_distance = 1.0e30f;
  float best_rect_x = 0.0f;
  float best_rect_y = 0.0f;
  float best_rect_w = 0.0f;
  float best_rect_h = 0.0f;
  const bool allow_stack_row_fallback =
      IsMpMenuInOpenStack(base, ui_context, menu) || focused_menu_match;
  int32_t fallback_item = -1;
  uint32_t fallback_priority = UINT32_MAX;
  float fallback_distance = 1.0e30f;
  float fallback_rect_x = 0.0f;
  float fallback_rect_y = 0.0f;
  float fallback_rect_w = 0.0f;
  float fallback_rect_h = 0.0f;

  for (uint32_t i = 0; i < item_count; ++i) {
    const uint32_t item = REX_LOAD_U32(items + i * 4);
    if (item == 0) {
      continue;
    }

    const uint32_t static_flags = REX_LOAD_U32(item + 68);
    if ((static_flags & 0x100000u) != 0) {
      continue;
    }

    const MenuHoverRect drawn_rect =
        GetMpDrawnItemRect(base, item, screen_placement);
    const float rect_x = drawn_rect.x;
    const float rect_y = drawn_rect.y;
    const float rect_w = drawn_rect.w;
    const float rect_h = drawn_rect.h;
    if (!std::isfinite(rect_x) || !std::isfinite(rect_y) ||
        !std::isfinite(rect_w) || !std::isfinite(rect_h) ||
        rect_w <= 0.0f || rect_h <= 0.0f) {
      continue;
    }

    bool selectable_checked = false;
    bool selectable = false;
    auto ensure_selectable = [&]() {
      if (!selectable_checked) {
        selectable = IsMpMenuDirectHoverSelectableCandidate(
            ctx, base, local_client, item);
        selectable_checked = true;
      }
      return selectable;
    };

    if (TryApplyMpListBoxMouseHover(
            ctx, base, ui_context, menu, local_client, item,
            static_cast<int32_t>(i), drawn_rect, pointer_candidates,
            static_cast<uint32_t>(sizeof(pointer_candidates) /
                                  sizeof(pointer_candidates[0])),
            hit_padding, pointer_generation, force_full_focus)) {
      return true;
    }

    const MenuHoverRect ownerdraw_rects[] = {
        drawn_rect,
        GetMpCorrectedItemRect(base, item),
    };
    if (TryApplyMpOwnerDrawFeederMouseHover(
            ctx, base, ui_context, menu, local_client, item,
            static_cast<int32_t>(i), ownerdraw_rects,
            static_cast<uint32_t>(sizeof(ownerdraw_rects) /
                                  sizeof(ownerdraw_rects[0])),
            pointer_candidates,
            static_cast<uint32_t>(sizeof(pointer_candidates) /
                                  sizeof(pointer_candidates[0])),
            hit_padding, pointer_generation, force_full_focus)) {
      return true;
    }

    for (const MenuPointerCandidate& candidate : pointer_candidates) {
      if (!IsPointerInsideRect(candidate.x, candidate.y, rect_x, rect_y,
                               rect_w, rect_h, hit_padding)) {
        continue;
      }

      if (!ensure_selectable()) {
        continue;
      }

      const float area = rect_w * rect_h;
      const float center_x = rect_x + rect_w * 0.5f;
      const float center_y = rect_y + rect_h * 0.5f;
      const float dx = candidate.x - center_x;
      const float dy = candidate.y - center_y;
      const float distance = dx * dx + dy * dy;
      if (best_item < 0 || candidate.priority < best_priority ||
          (candidate.priority == best_priority && area < best_area) ||
          (candidate.priority == best_priority && area == best_area &&
           distance < best_distance)) {
        best_item = static_cast<int32_t>(i);
        best_priority = candidate.priority;
        best_area = area;
        best_distance = distance;
        best_rect_x = rect_x;
        best_rect_y = rect_y;
        best_rect_w = rect_w;
        best_rect_h = rect_h;
      }
    }

    if (!allow_stack_row_fallback) {
      continue;
    }

    const float center_x = rect_x + rect_w * 0.5f;
    const float center_y = rect_y + rect_h * 0.5f;
    const float row_half_height =
        std::max(7.0f, std::min(20.0f, rect_h * 0.75f));
    const float row_x_padding =
        std::max(48.0f, std::min(180.0f, rect_w * 0.9f));
    for (const MenuPointerCandidate& candidate : pointer_candidates) {
      if (candidate.y < center_y - row_half_height ||
          candidate.y > center_y + row_half_height ||
          candidate.x < rect_x - row_x_padding ||
          candidate.x > rect_x + rect_w + row_x_padding) {
        continue;
      }

      if (!ensure_selectable()) {
        continue;
      }

      const float dx = candidate.x - center_x;
      const float dy = candidate.y - center_y;
      const float distance = dy * dy + dx * dx * 0.015f;
      if (fallback_item < 0 || candidate.priority < fallback_priority ||
          (candidate.priority == fallback_priority &&
           distance < fallback_distance)) {
        fallback_item = static_cast<int32_t>(i);
        fallback_priority = candidate.priority;
        fallback_distance = distance;
        fallback_rect_x = rect_x;
        fallback_rect_y = rect_y;
        fallback_rect_w = rect_w;
        fallback_rect_h = rect_h;
      }
    }
  }

  if (best_item < 0 && fallback_item >= 0) {
    best_item = fallback_item;
    best_priority = fallback_priority;
    best_distance = fallback_distance;
    best_rect_x = fallback_rect_x;
    best_rect_y = fallback_rect_y;
    best_rect_w = fallback_rect_w;
    best_rect_h = fallback_rect_h;
  }

  if (best_item < 0) {
    return false;
  }

  const uint32_t best_item_address =
      REX_LOAD_U32(items + static_cast<uint32_t>(best_item) * 4);
  if (best_item_address == 0) {
    return false;
  }

  if (!force_full_focus &&
      !IsMpMenuHoverCandidateStable(ui_context, menu, local_client,
                                    best_item_address, best_item)) {
    rex::input::mnk::ReportMenuDirectHoverApplied(pointer_generation);
    return true;
  }

  const uint32_t cursor_offset = (local_client + 25) * 4;
  const uint32_t current_cursor = REX_LOAD_U32(state + cursor_offset);
  const bool cursor_changed = current_cursor != static_cast<uint32_t>(best_item);
  if (cursor_changed) {
    REX_STORE_U32(state + cursor_offset, static_cast<uint32_t>(best_item));
  }

  bool item_pointer_changed = false;
  const bool focus_target_changed = NoteMpMenuHoverFocusTarget(
      ui_context, menu, local_client, best_item_address, best_item,
      static_cast<int32_t>(std::lround(best_rect_x)),
      static_cast<int32_t>(std::lround(best_rect_y)),
      static_cast<int32_t>(std::lround(best_rect_w)),
      static_cast<int32_t>(std::lround(best_rect_h)), &item_pointer_changed);

  if (rex::cvar::Query<bool>("mnk_menu_direct_hover_cursor_only")) {
    if (force_full_focus) {
      ApplyMpFullMenuHoverFocus(ctx, base, ui_context, menu, local_client,
                                best_item_address);
    } else if (cursor_changed || focus_target_changed || item_pointer_changed) {
      ApplyMpFullMenuHoverFocus(ctx, base, ui_context, menu, local_client,
                                best_item_address);
    }
  } else if (force_full_focus) {
    ApplyMpFullMenuHoverFocus(ctx, base, ui_context, menu, local_client,
                              best_item_address);
  } else if (focus_target_changed) {
    ApplyMpFullMenuHoverFocus(ctx, base, ui_context, menu, local_client,
                              best_item_address);
  } else if (cursor_changed || item_pointer_changed) {
    ApplyMpQuietMenuHoverFocus(ctx, base, ui_context, menu, local_client,
                               best_item_address, best_item, false);
  }

  rex::input::mnk::ReportMenuDirectHoverApplied(pointer_generation);
  return true;
}
}  // namespace

extern "C" void rex_Menu_Paint_YAHPAUUiContext_PAUmenuDef_t_Z(
    PPCContext& ctx, uint8_t* base) {
  const uint32_t ui_context = ctx.r3.u32;
  const uint32_t menu = ctx.r4.u32;
  __imp__rex_Menu_Paint_YAHPAUUiContext_PAUmenuDef_t_Z(ctx, base);
  (void)ApplyMenuDirectMouseHover(ctx, base, ui_context, menu);
}

extern "C" void rex_Menu_PaintAll_Internal(PPCContext& ctx, uint8_t* base) {
  const uint32_t ui_context = ctx.r3.u32;
  const uint32_t layer = ctx.r4.u32;
  rex::input::mnk::ClearMenuDirectHoverApplied();
  __imp__rex_Menu_PaintAll_Internal(ctx, base);

  const uint32_t frontmost_menu =
      GetMpFrontmostPaintedMenu(base, ui_context, layer);
  if (frontmost_menu != 0) {
    (void)ApplyMenuDirectMouseHover(ctx, base, ui_context, frontmost_menu);
  }
}

extern "C" void rex_Menu_HandleKey_YAXPAUUiContext_PAUmenuDef_t_HH_Z(
    PPCContext& ctx, uint8_t* base) {
  const bool force_mouse_focus = ctx.r6.u32 != 0 && IsMenuAcceptKey(ctx.r5.u32);
  (void)ApplyMenuDirectMouseHover(ctx, base, ctx.r3.u32, ctx.r4.u32,
                                  force_mouse_focus);
  __imp__rex_Menu_HandleKey_YAXPAUUiContext_PAUmenuDef_t_HH_Z(ctx, base);
}

extern "C" void rex_Item_ListBox_Paint(PPCContext& ctx, uint8_t* base) {
  const uint32_t ui_context = ctx.r3.u32;
  const uint32_t item = ctx.r4.u32;
  if (rex::cvar::Query<bool>("mnk_menu_direct_hover") &&
      ui_context != 0 && item != 0) {
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    uint32_t pointer_generation = 0;
    if (rex::input::mnk::QueryMenuPointerVirtualPosition(
            &pointer_x, &pointer_y, &pointer_generation)) {
      const uint32_t local_client = REX_LOAD_U32(ui_context + 0);
      if (local_client <= 3) {
        const float padding = static_cast<float>(
            std::clamp(rex::cvar::Query<double>(
                           "mnk_menu_direct_hover_padding"),
                       -16.0, 32.0));
        const float normalized_x = pointer_x / 640.0f;
        const float normalized_y = pointer_y / 480.0f;
        const uint32_t output_width = Nx1InternalResolutionWidth();
        const uint32_t output_height = Nx1InternalResolutionHeight();
        const MenuPointerCandidate pointer_candidates[] = {
            {pointer_x, pointer_y, 0},
            {normalized_x * 1280.0f, normalized_y * 720.0f, 1},
            {normalized_x * static_cast<float>(output_width),
             normalized_y * static_cast<float>(output_height), 2},
        };
        const MenuHoverRect rects[] = {
            GetMpDrawnItemRect(base, item,
                               GetMpActiveScreenPlacement(ctx, base,
                                                          local_client)),
            GetMpCorrectedItemRect(base, item),
        };
        (void)TryApplyMpListBoxSelectionOnly(
            ctx, base, local_client, item, rects,
            static_cast<uint32_t>(sizeof(rects) / sizeof(rects[0])),
            pointer_candidates,
            static_cast<uint32_t>(sizeof(pointer_candidates) /
                                  sizeof(pointer_candidates[0])),
            padding, pointer_generation);
      }
    }
  }

  __imp__rex_Item_ListBox_Paint(ctx, base);
}

extern "C" void rex_Item_OwnerDraw_Paint(PPCContext& ctx, uint8_t* base) {
  const uint32_t ui_context = ctx.r3.u32;
  const uint32_t item = ctx.r4.u32;
  if (rex::cvar::Query<bool>("mnk_menu_direct_hover") &&
      ui_context != 0 && item != 0) {
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    uint32_t pointer_generation = 0;
    if (rex::input::mnk::QueryMenuPointerVirtualPosition(
            &pointer_x, &pointer_y, &pointer_generation)) {
      const uint32_t local_client = REX_LOAD_U32(ui_context + 0);
      if (local_client <= 3) {
        const float padding = static_cast<float>(
            std::clamp(rex::cvar::Query<double>(
                           "mnk_menu_direct_hover_padding"),
                       -16.0, 32.0));
        const float normalized_x = pointer_x / 640.0f;
        const float normalized_y = pointer_y / 480.0f;
        const uint32_t output_width = Nx1InternalResolutionWidth();
        const uint32_t output_height = Nx1InternalResolutionHeight();
        const MenuPointerCandidate pointer_candidates[] = {
            {pointer_x, pointer_y, 0},
            {normalized_x * 1280.0f, normalized_y * 720.0f, 1},
            {normalized_x * static_cast<float>(output_width),
             normalized_y * static_cast<float>(output_height), 2},
        };
        const MpMenuItemRef menu_ref =
            FindMpMenuContainingItem(base, ui_context, item);
        const MenuHoverRect rects[] = {
            GetMpDrawnItemRect(base, item,
                               GetMpActiveScreenPlacement(ctx, base,
                                                          local_client)),
            GetMpCorrectedItemRect(base, item),
        };
        (void)TryApplyMpOwnerDrawFeederMouseHover(
            ctx, base, ui_context, menu_ref.menu, local_client, item,
            menu_ref.item_index, rects,
            static_cast<uint32_t>(sizeof(rects) / sizeof(rects[0])),
            pointer_candidates,
            static_cast<uint32_t>(sizeof(pointer_candidates) /
                                  sizeof(pointer_candidates[0])),
            padding, pointer_generation, false);
      }
    }
  }

  __imp__rex_Item_OwnerDraw_Paint(ctx, base);
}

extern "C" void
rex_UI_OwnerDraw_YAXHMMMMHHMMHHHMPAUFont_s_MQAMPAUMaterial_HUrectDef_s_HPBDW4EScreenLayer_Z(
    PPCContext& ctx, uint8_t* base) {
  const PPCContext original_ctx = ctx;

  if (rex::cvar::Query<bool>("mnk_menu_direct_hover")) {
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    uint32_t pointer_generation = 0;
    if (rex::input::mnk::QueryMenuPointerVirtualPosition(
            &pointer_x, &pointer_y, &pointer_generation)) {
      const uint32_t local_client = ctx.r3.u32;
      if (local_client <= 3) {
        const float padding = static_cast<float>(
            std::clamp(rex::cvar::Query<double>(
                           "mnk_menu_direct_hover_padding"),
                       -16.0, 32.0));
        const float normalized_x = pointer_x / 640.0f;
        const float normalized_y = pointer_y / 480.0f;
        const uint32_t output_width = Nx1InternalResolutionWidth();
        const uint32_t output_height = Nx1InternalResolutionHeight();
        const MenuPointerCandidate pointer_candidates[] = {
            {pointer_x, pointer_y, 0},
            {normalized_x * 1280.0f, normalized_y * 720.0f, 1},
            {normalized_x * static_cast<float>(output_width),
             normalized_y * static_cast<float>(output_height), 2},
        };

        PPCRegister feeder{};
        feeder.f32 = static_cast<float>(ctx.f7.f64);
        const uint32_t ownerdraw = REX_LOAD_U32(ctx.r1.u32 + 92);
        const float x = static_cast<float>(ctx.f1.f64 + ctx.f5.f64);
        const float y = static_cast<float>(ctx.f2.f64 + ctx.f6.f64);
        const float w = static_cast<float>(ctx.f3.f64);
        const float h = static_cast<float>(ctx.f4.f64);

        PPCContext hover_ctx = ctx;
        (void)TryApplyMpUiOwnerDrawFeederMouseHover(
            hover_ctx, base, local_client, ownerdraw, x, y, w, h, feeder.u32,
            pointer_candidates,
            static_cast<uint32_t>(sizeof(pointer_candidates) /
                                  sizeof(pointer_candidates[0])),
            padding, pointer_generation);
      }
    }
  }

  ctx = original_ctx;
  __imp__rex_UI_OwnerDraw_YAXHMMMMHHMMHHHMPAUFont_s_MQAMPAUMaterial_HUrectDef_s_HPBDW4EScreenLayer_Z(
      ctx, base);
}

extern "C" void rex_CL_ShutdownCGame_YAXH_Z(PPCContext& ctx, uint8_t* base) {
  NoteMpDiscordMenus("CL_ShutdownCGame");
  __imp__rex_CL_ShutdownCGame_YAXH_Z(ctx, base);
}

extern "C" void rex_CL_Disconnect_YAXH_Z(PPCContext& ctx, uint8_t* base) {
  NoteMpDiscordMenus("CL_Disconnect");
  __imp__rex_CL_Disconnect_YAXH_Z(ctx, base);
}

extern "C" void rex_CL_DisconnectLocalClient_YAXH_Z(PPCContext& ctx,
                                                     uint8_t* base) {
  NoteMpDiscordMenus("CL_DisconnectLocalClient");
  __imp__rex_CL_DisconnectLocalClient_YAXH_Z(ctx, base);
}

extern "C" void rex_R_UnloadWorld_YAXXZ(PPCContext& ctx, uint8_t* base) {
  NoteMpDiscordMenus("R_UnloadWorld");
  __imp__rex_R_UnloadWorld_YAXXZ(ctx, base);
}

extern "C" void rex_CG_DrawFPS_YAMHPBUScreenPlacement_M_Z(PPCContext& ctx,
                                                           uint8_t* base) {
  if (!rex::system::IsNx1DebugHudEnabled()) {
    return;
  }

  PinNx1DebugHudDvars(base);
  __imp__rex_CG_DrawFPS_YAMHPBUScreenPlacement_M_Z(ctx, base);
}

extern "C" void rex_CG_CornerDebugPrint_YAMPBUScreenPlacement_MMMPBD1QBM_Z(
    PPCContext& ctx, uint8_t* base) {
  PinNx1DebugHudDvars(base);
  __imp__rex_CG_CornerDebugPrint_YAMPBUScreenPlacement_MMMPBD1QBM_Z(ctx, base);
}

extern "C" void rex_CG_DrawDebugOverlays_YAXH_Z(PPCContext& ctx,
                                                 uint8_t* base) {
  if (!rex::system::IsNx1DebugHudEnabled()) {
    return;
  }

  PinNx1DebugHudDvars(base);
  __imp__rex_CG_DrawDebugOverlays_YAXH_Z(ctx, base);
}

extern "C" void rex_CG_DrawUpperRightDebugInfo(PPCContext& ctx,
                                                uint8_t* base) {
  if (!rex::system::IsNx1DebugHudEnabled()) {
    return;
  }

  PinNx1DebugHudDvars(base);
  __imp__rex_CG_DrawUpperRightDebugInfo(ctx, base);
}

extern "C" void rex_CG_DrawFullScreenDebugOverlays_YAXH_Z(PPCContext& ctx,
                                                           uint8_t* base) {
  if (!rex::system::IsNx1DebugHudEnabled()) {
    return;
  }

  PinNx1DebugHudDvars(base);
  __imp__rex_CG_DrawFullScreenDebugOverlays_YAXH_Z(ctx, base);
}

extern "C" void rex_Con_DrawMiniConsole_YAXHHHMW4EScreenLayer_Z(
    PPCContext& ctx, uint8_t* base) {
  if (!rex::system::IsNx1DebugHudEnabled()) {
    return;
  }

  __imp__rex_Con_DrawMiniConsole_YAXHHHMW4EScreenLayer_Z(ctx, base);
}

extern "C" void rex_Con_DrawErrors_YAXHHHMW4EScreenLayer_Z(PPCContext& ctx,
                                                            uint8_t* base) {
  if (!rex::system::IsNx1DebugHudEnabled()) {
    return;
  }

  __imp__rex_Con_DrawErrors_YAXHHHMW4EScreenLayer_Z(ctx, base);
}

extern "C" void rex_CL_GetMouseMovement(PPCContext& ctx, uint8_t* base) {
  float host_mouse_x = 0.0f;
  float host_mouse_y = 0.0f;
  if (ctx.r4.u32 != 0 && ctx.r5.u32 != 0 &&
      rex::input::mnk::ConsumeNativeMouseMovement(&host_mouse_x, &host_mouse_y)) {
    ResetMpMenuHoverFocusTarget();
    if (!g_mp_logged_native_mouse_hook) {
      g_mp_logged_native_mouse_hook = true;
      REXLOG_INFO("NX1 MP: native mouse-look hook is feeding CL_GetMouseMovement");
    }

    PPCRegister temp{};
    temp.f32 = host_mouse_x;
    REX_STORE_U32(ctx.r4.u32, temp.u32);
    temp.f32 = host_mouse_y;
    REX_STORE_U32(ctx.r5.u32, temp.u32);
    return;
  }

  __imp__rex_CL_GetMouseMovement(ctx, base);
}

extern "C" void rex_Key_AddCatcher_YAXHH_Z(PPCContext& ctx, uint8_t* base) {
  const int32_t local_client_num = ctx.r3.s32;
  const uint32_t catcher_mask = ctx.r4.u32;
  __imp__rex_Key_AddCatcher_YAXHH_Z(ctx, base);
  rex::input::mnk::AddGameKeyCatcherMask(local_client_num, catcher_mask);
}

extern "C" void rex_Key_RemoveCatcher_YAXHH_Z(PPCContext& ctx, uint8_t* base) {
  const int32_t local_client_num = ctx.r3.s32;
  const uint32_t catcher_mask = ctx.r4.u32;
  __imp__rex_Key_RemoveCatcher_YAXHH_Z(ctx, base);
  rex::input::mnk::RemoveGameKeyCatcherMask(local_client_num, catcher_mask);
}

extern "C" void rex_Key_SetCatcher_YAXHH_Z(PPCContext& ctx, uint8_t* base) {
  const int32_t local_client_num = ctx.r3.s32;
  const uint32_t catcher_mask = ctx.r4.u32;
  __imp__rex_Key_SetCatcher_YAXHH_Z(ctx, base);
  rex::input::mnk::SetGameKeyCatcherMask(local_client_num, catcher_mask);
}

extern "C" void rex_Dvar_GetBool_YA_NPBD_Z(PPCContext& ctx, uint8_t* base) {
  uint32_t forced_value = 0;
  if (GetForcedNx1DvarValueName(base, ctx.r3.u32, &forced_value)) {
    LogNx1DvarFilterOnce();
    ctx.r3.u64 = forced_value != 0;
    return;
  }

  __imp__rex_Dvar_GetBool_YA_NPBD_Z(ctx, base);
}

extern "C" void rex_Dvar_GetInt_YAHPBD_Z(PPCContext& ctx, uint8_t* base) {
  uint32_t forced_value = 0;
  if (GetForcedNx1DvarValueName(base, ctx.r3.u32, &forced_value)) {
    LogNx1DvarFilterOnce();
    ctx.r3.s64 = static_cast<int32_t>(forced_value);
    return;
  }

  __imp__rex_Dvar_GetInt_YAHPBD_Z(ctx, base);
}

extern "C" void rex_Dvar_SetVariant(PPCContext& ctx, uint8_t* base) {
  const uint32_t guest_dvar = ctx.r3.u32;
  ApplyForcedNx1DvarValue(base, guest_dvar, ctx.r4);
  __imp__rex_Dvar_SetVariant(ctx, base);
  PinForcedNx1DvarValue(base, guest_dvar);
}

extern "C" void rex_Dvar_RegisterBool_YAPBUdvar_t_PBD_NG0_Z(
    PPCContext& ctx, uint8_t* base) {
  ApplyForcedNx1DvarValueName(base, ctx.r3.u32, ctx.r4);
  ctx.r4.u64 = ctx.r4.u32 != 0;
  __imp__rex_Dvar_RegisterBool_YAPBUdvar_t_PBD_NG0_Z(ctx, base);
  PinForcedNx1DvarValue(base, ctx.r3.u32);
}

extern "C" void rex_Dvar_RegisterInt_YAPBUdvar_t_PBDHHHG0_Z(
    PPCContext& ctx, uint8_t* base) {
  if (ApplyForcedNx1DvarValueName(base, ctx.r3.u32, ctx.r4)) {
    if (ctx.r5.s32 > ctx.r4.s32) {
      ctx.r5.s64 = ctx.r4.s32;
    }
    if (ctx.r6.s32 < ctx.r4.s32) {
      ctx.r6.s64 = ctx.r4.s32;
    }
  }

  __imp__rex_Dvar_RegisterInt_YAPBUdvar_t_PBDHHHG0_Z(ctx, base);
  PinForcedNx1DvarValue(base, ctx.r3.u32);
}

extern "C" void rex_Dvar_RegisterFloat_YAPBUdvar_t_PBDMMMG0_Z(
    PPCContext& ctx, uint8_t* base) {
  uint32_t forced_value = 0;
  if (GetForcedNx1DvarValueName(base, ctx.r3.u32, &forced_value)) {
    LogNx1DvarFilterOnce();
    const double value = double(float(forced_value));
    ctx.f1.f64 = value;
    if (ctx.f2.f64 > value) {
      ctx.f2.f64 = value;
    }
    if (ctx.f3.f64 < value) {
      ctx.f3.f64 = value;
    }
  }

  __imp__rex_Dvar_RegisterFloat_YAPBUdvar_t_PBDMMMG0_Z(ctx, base);
  PinForcedNx1DvarValue(base, ctx.r3.u32);
}

extern "C" void rex_Dvar_SetBoolFromSource_YAXPBUdvar_t_NW4DvarSetSource_Z(
    PPCContext& ctx, uint8_t* base) {
  const uint32_t guest_dvar = ctx.r3.u32;
  ApplyForcedNx1DvarValue(base, guest_dvar, ctx.r4);
  ctx.r4.u64 = ctx.r4.u32 != 0;
  __imp__rex_Dvar_SetBoolFromSource_YAXPBUdvar_t_NW4DvarSetSource_Z(ctx, base);
  PinForcedNx1DvarValue(base, guest_dvar);
}

extern "C" void rex_Dvar_SetIntFromSource_YAXPBUdvar_t_HW4DvarSetSource_Z(
    PPCContext& ctx, uint8_t* base) {
  const uint32_t guest_dvar = ctx.r3.u32;
  ApplyForcedNx1DvarValue(base, guest_dvar, ctx.r4);
  __imp__rex_Dvar_SetIntFromSource_YAXPBUdvar_t_HW4DvarSetSource_Z(ctx, base);
  PinForcedNx1DvarValue(base, guest_dvar);
}

extern "C" void rex_Dvar_SetFloatFromSource_YAXPBUdvar_t_MW4DvarSetSource_Z(
    PPCContext& ctx, uint8_t* base) {
  const uint32_t guest_dvar = ctx.r3.u32;
  uint32_t forced_value = 0;
  if (GetForcedNx1DvarValue(base, guest_dvar, &forced_value)) {
    LogNx1DvarFilterOnce();
    ctx.f1.f64 = double(float(forced_value));
  }

  __imp__rex_Dvar_SetFloatFromSource_YAXPBUdvar_t_MW4DvarSetSource_Z(ctx, base);
  PinForcedNx1DvarValue(base, guest_dvar);
}

extern "C" void rex_Dvar_SetBoolByName_YAXPBD_N_Z(PPCContext& ctx,
                                                   uint8_t* base) {
  ApplyForcedNx1DvarValueName(base, ctx.r3.u32, ctx.r4);
  ctx.r4.u64 = ctx.r4.u32 != 0;
  __imp__rex_Dvar_SetBoolByName_YAXPBD_N_Z(ctx, base);
}

extern "C" void rex_Dvar_SetIntByName_YAXPBDH_Z(PPCContext& ctx,
                                                 uint8_t* base) {
  ApplyForcedNx1DvarValueName(base, ctx.r3.u32, ctx.r4);
  __imp__rex_Dvar_SetIntByName_YAXPBDH_Z(ctx, base);
}

extern "C" void rex_Dvar_SetFloatByName_YAXPBDM_Z(PPCContext& ctx,
                                                   uint8_t* base) {
  uint32_t forced_value = 0;
  if (GetForcedNx1DvarValueName(base, ctx.r3.u32, &forced_value)) {
    LogNx1DvarFilterOnce();
    ctx.f1.f64 = double(float(forced_value));
  }

  __imp__rex_Dvar_SetFloatByName_YAXPBDM_Z(ctx, base);
}

extern "C" void rex_BB_Connect_YAXXZ(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  LogBlackBoxSkipOnce();
}

extern "C" void rex_BB_TryReconnect_YAXXZ(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  LogBlackBoxSkipOnce();
}

extern "C" void rex_R_SetWndParms_YAXPAUGfxWindowParms_Z(PPCContext& ctx, uint8_t* base) {
  if (!rex::cvar::Query<bool>("nx1_internal_resolution_patch")) {
    __imp__rex_R_SetWndParms_YAXPAUGfxWindowParms_Z(ctx, base);
    return;
  }

  rex::system::X_VIDEO_MODE video_mode{};
  rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);

  const uint32_t display_width = ClampWidth(uint32_t(video_mode.display_width));
  const uint32_t display_height = ClampHeight(uint32_t(video_mode.display_height));
  const bool is_widescreen = uint32_t(video_mode.is_widescreen) != 0;
  const bool is_hi_def = uint32_t(video_mode.is_hi_def) != 0;
  const Nx1RenderResolution resolution = ResolveNx1RenderResolution();
  const uint32_t render_width = resolution.guest_width;
  const uint32_t render_height = resolution.guest_height;
  const uint32_t wnd_parms = ctx.r3.u32;

  static bool logged_resolution = false;
  if (!logged_resolution) {
    logged_resolution = true;
    REXLOG_INFO(
        "NX1 MP: overriding R_SetWndParms internal resolution to {}x{} guest for {}x{} output",
        render_width, render_height, resolution.requested_width, resolution.requested_height);
  }

  PPCRegister temp{};
  temp.f32 = float(video_mode.refresh_rate);

  REX_STORE_U8(wnd_parms + 0, is_widescreen ? 1 : 0);
  REX_STORE_U8(wnd_parms + 1, is_hi_def ? 1 : 0);
  REX_STORE_U32(wnd_parms + 4, temp.u32);

  uint32_t output_width = 0;
  uint32_t output_height = 0;
  if (is_hi_def) {
    REX_STORE_U32(wnd_parms + 8, 1024);
    REX_STORE_U32(wnd_parms + 12, 600);
    output_width = render_width;
    output_height = render_height;
  } else {
    REX_STORE_U32(wnd_parms + 8, 896);
    REX_STORE_U32(wnd_parms + 12, 576);
    output_width = std::clamp(display_width, 640u, render_width);
    output_height = std::clamp(display_height, 480u, render_height);
  }

  REX_STORE_U32(wnd_parms + 16, output_width);
  REX_STORE_U32(wnd_parms + 20, output_height);
  REX_STORE_U32(wnd_parms + 24, 2);

  SetDisplayOutputScaled(ctx, base,
                         output_width != display_width || output_height != display_height);
}
