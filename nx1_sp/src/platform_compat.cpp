// Host CRT compatibility shims for generated code.

#include "generated/1-nx1sp/nx1_sp_init.h"
#include "nx1_devgui_overlay.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/mnk/mouse_look.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/debug_hud.h>
#include <rex/ppc/context.h>
#include <rex/system/discord_presence.h>
#include <rex/system/kernel_state.h>

namespace {

constexpr uint32_t kXAssetTypeNameTable = 0x827EAE60;
constexpr uint32_t kXAssetPoolLimitTable = 0x827EB230;
constexpr uint32_t kXAssetTypeCount = 48;
constexpr uint32_t kRaisedGameMapPoolLimit = 4;
constexpr uint32_t kEmergencyGameMapAssetSize = 8;
constexpr uint32_t kBaseD3DHeapSize = 0x00769000;
constexpr uint32_t kBaseD3DHeapWidth = 1280;
constexpr uint32_t kBaseD3DHeapHeight = 720;
constexpr uint32_t kMin1080pD3DHeapSize = 0x01800000;
constexpr uint32_t kCgDrawFPSDvarPtr = 0x828866B4;
constexpr uint32_t kCgDrawFPSLabelsDvarPtr = 0x82A01C80;
constexpr uint32_t kSpDevGuiBase = 0x83106770;
constexpr uint32_t kSpDevGuiRootHandle = kSpDevGuiBase + 0x138A8;
constexpr uint32_t kSpDevGuiActive = kSpDevGuiBase + 0x138AD;
constexpr uint32_t kSpDevGuiEditing = kSpDevGuiBase + 0x138AE;
constexpr uint32_t kSpDevGuiSelectedHandle = kSpDevGuiBase + 0x138B0;
constexpr uint32_t kSpDevGuiOriginY = kSpDevGuiBase + 0x138BC;
constexpr uint32_t kSpDevGuiScreenHeight = kSpDevGuiBase + 0x138C0;
constexpr uint32_t kSpDevGuiFontGlobals = 0x82A5B6E0;
constexpr uint32_t kSpDevGuiFontPtr = kSpDevGuiFontGlobals + 384;
constexpr uint8_t kSpDevGuiChildMenu = 0;
constexpr uint8_t kSpDevGuiChildDvar = 1;
constexpr uint8_t kSpDevGuiChildCommand = 2;
constexpr uint8_t kSpDevGuiChildGraph = 3;

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

std::string NormalizeMapName(std::string value) {
  const size_t first = value.find_first_not_of(" \t\r\n");
  const size_t last = value.find_last_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  value = value.substr(first, last - first + 1);

  const size_t last_separator = value.find_last_of("/\\");
  if (last_separator != std::string::npos) {
    value.erase(0, last_separator + 1);
  }

  const size_t extension = value.find_last_of('.');
  if (extension != std::string::npos) {
    value.erase(extension);
  }

  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (value.size() >= 10 && value.substr(0, 10) == "localized_") {
    value.erase(0, 10);
  } else if (value.size() >= 6 && value.substr(0, 6) == "patch_") {
    value.erase(0, 6);
  }
  if (value.size() >= 5 && value.substr(value.size() - 5) == "_load") {
    value.erase(value.size() - 5);
  }
  return value;
}

bool IsCampaignMapName(std::string_view map_name) {
  static constexpr std::string_view kCampaignMaps[] = {
      "nx_border",      "nx_hospital",   "nx_exfil",      "nx_rocket",
      "nx_lava",        "nx_hithard",    "nx_hithard_b",  "nx_lunar",
      "nx_ss_rappel",   "nx_skyscraper",
  };

  for (const std::string_view candidate : kCampaignMaps) {
    if (candidate == map_name) {
      return true;
    }
  }
  return false;
}

std::string ReadGuestCString(uint32_t guest_address, size_t limit = 512) {
  if (guest_address == 0) {
    return "<null>";
  }

  const char* guest_string =
      REX_KERNEL_MEMORY()->TranslateVirtual<const char*>(guest_address);
  std::string value;
  value.reserve(64);
  for (size_t i = 0; i < limit && guest_string[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(guest_string[i]);
    value.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.');
  }
  return value;
}

uint32_t GuestLoadU32(uint32_t guest_address) {
  return rex::memory::load_and_swap<uint32_t>(
      REX_KERNEL_MEMORY()->TranslateVirtual<const uint32_t*>(guest_address));
}

void GuestStoreU32(uint32_t guest_address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(
      REX_KERNEL_MEMORY()->TranslateVirtual<uint32_t*>(guest_address), value);
}

bool IsGameMapAssetType(uint32_t type, std::string* name_out = nullptr) {
  if (type >= kXAssetTypeCount) {
    return false;
  }

  const uint32_t name_ptr = GuestLoadU32(kXAssetTypeNameTable + type * 4);
  const std::string name = ReadGuestCString(name_ptr, 64);
  if (name_out != nullptr) {
    *name_out = name;
  }
  return name == "game_map_mp" || name == "game_map_sp";
}

std::mutex g_memmap_profile_overflow_mutex;
std::unordered_map<uint32_t, uint32_t> g_memmap_profile_overflow_depth_by_tls;
std::mutex g_nxheap_failure_mutex;
std::unordered_map<uint32_t, uint32_t> g_nxheap_failure_count_by_heap;
bool g_sp_logged_d3d_heap_expansion = false;
bool g_sp_logged_native_mouse_hook = false;
bool g_sp_logged_dvar_filter = false;
bool g_sp_logged_blackbox_skip = false;
std::string g_sp_last_discord_campaign_map;
uint32_t g_sp_devgui_toggle_guard_frames = 0;
bool g_sp_logged_devgui_toggle_guard = false;
bool g_sp_logged_devgui_mouse = false;
uint64_t g_sp_devgui_overlay_snapshot_generation = 0;

struct DevGuiMouseCandidate {
  uint16_t handle = 0;
  uint16_t parent_handle = 0;
  uint32_t parent = 0;
  uint8_t child_index = 0;
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;
};

std::vector<DevGuiMouseCandidate> g_sp_devgui_mouse_candidates;

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

bool IsNx1UnsafeSpDebugDvarName(uint8_t* base, uint32_t guest_name) {
  return GuestStringEquals(base, guest_name, "g_entinfo") ||
         GuestStringEquals(base, guest_name, "g_entinfo_type") ||
         GuestStringEquals(base, guest_name, "g_entinfo_AItext") ||
         GuestStringEquals(base, guest_name, "g_entinfo_scale") ||
         GuestStringEquals(base, guest_name, "g_entinfo_maxdist") ||
         GuestStringEquals(base, guest_name, "ai_showNearbyNodes") ||
         GuestStringEquals(base, guest_name, "path_nodeInfoType");
}

bool ShouldClampNx1DvarName(uint8_t* base, uint32_t guest_name) {
  if (IsNx1UnsafeSpDebugDvarName(base, guest_name)) {
    return true;
  }

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
  if (g_sp_logged_dvar_filter) {
    return;
  }

  g_sp_logged_dvar_filter = true;
  REXLOG_INFO(
      "NX1 SP: dvar filter active debug_hud={} force_developer_off={} "
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

bool BlockUnsafeSpDebugDvarSetName(uint8_t* base, uint32_t guest_name) {
  if (!IsNx1UnsafeSpDebugDvarName(base, guest_name)) {
    return false;
  }

  LogNx1DvarFilterOnce();
  return true;
}

bool BlockUnsafeSpDebugDvarSet(uint8_t* base, uint32_t guest_dvar) {
  if (guest_dvar == 0 ||
      !IsNx1UnsafeSpDebugDvarName(base, REX_LOAD_U32(guest_dvar))) {
    return false;
  }

  LogNx1DvarFilterOnce();
  PinForcedNx1DvarValue(base, guest_dvar);
  return true;
}

void LogBlackBoxSkipOnce() {
  if (g_sp_logged_blackbox_skip) {
    return;
  }

  g_sp_logged_blackbox_skip = true;
  REXLOG_WARN("NX1 SP: BB_Connect skipped; blackbox telemetry server is unavailable");
}

void NoteSpDiscordMenus(std::string_view reason) {
  if (!g_sp_last_discord_campaign_map.empty()) {
    REXLOG_INFO("NX1 SP: Discord Rich Presence returned to menus ({})", reason);
  }
  g_sp_last_discord_campaign_map.clear();
  rex::system::DiscordPresenceClient::Get().SetMenuState(REX_KERNEL_STATE());
}

bool IsSpDevGuiActive(PPCContext& ctx, uint8_t* base) {
  PPCContext active_ctx = ctx;
  __imp__NX1DevGui_IsActive_YA_NXZ(active_ctx, base);
  return (active_ctx.r3.u32 & 0xFFu) != 0;
}

void ArmSpDevGuiToggleGuard() {
  g_sp_devgui_toggle_guard_frames = 8;
  if (!g_sp_logged_devgui_toggle_guard) {
    g_sp_logged_devgui_toggle_guard = true;
    REXLOG_INFO("NX1 SP: devgui toggle guard active");
  }
}

bool IsSpDevGuiHandleValid(uint32_t handle) {
  return handle >= 1 && handle <= 2000;
}

uint32_t GetSpDevGuiMenu(PPCContext& ctx, uint8_t* base, uint32_t handle) {
  if (!IsSpDevGuiHandleValid(handle)) {
    return 0;
  }

  PPCContext menu_ctx = ctx;
  menu_ctx.r3.u64 = handle;
  __imp__NX1DevGui_GetMenu_YAPAUDevMenuItem_G_Z(menu_ctx, base);
  return menu_ctx.r3.u32;
}

int32_t GetSpDevGuiRowHeight(PPCContext& ctx, uint8_t* base) {
  const uint32_t font = REX_LOAD_U32(kSpDevGuiFontPtr);
  if (font == 0) {
    return 20;
  }

  PPCContext height_ctx = ctx;
  height_ctx.r3.u64 = font;
  __imp__NX1R_TextHeight_YAHPAUFont_s_Z(height_ctx, base);
  return std::max(height_ctx.r3.s32 + 8, 12);
}

int32_t GetSpDevGuiMaxChildWidth(PPCContext& ctx, uint8_t* base,
                                 uint32_t menu) {
  if (menu == 0) {
    return 0;
  }

  PPCContext width_ctx = ctx;
  width_ctx.r3.u64 = menu;
  __imp__NX1DevGui_MaxChildMenuWidth_YAHPBUDevMenuItem_Z(width_ctx, base);
  return std::max(width_ctx.r3.s32, 32);
}

void SetSpDevGuiForceMenuMode(bool active) {
  rex::input::mnk::SetForceMenuMouseMode(active);
  if (!active) {
    rex::input::mnk::ClearMenuMouseButtons();
  }
}

uint32_t GetSpDevGuiMenuAddress(uint32_t handle) {
  if (!IsSpDevGuiHandleValid(handle)) {
    return 0;
  }
  return kSpDevGuiBase + (handle * 40u) - 40u;
}

bool IsSpNativeDevGuiOverlayEnabled() {
  return rex::cvar::Query<bool>("nx1_devgui_native_overlay");
}

std::string ReadSpDevGuiItemName(uint8_t* base, uint32_t item) {
  std::string name;
  name.reserve(26);
  for (uint32_t i = 0; i < 26; ++i) {
    const uint8_t raw = REX_LOAD_U8(item + i);
    if (raw == 0) {
      break;
    }
    name.push_back(raw >= 0x20 && raw < 0x7F ? static_cast<char>(raw) : '.');
  }
  return name;
}

uint8_t GetSpDevGuiItemType(uint8_t* base, uint16_t handle) {
  const uint32_t item = GetSpDevGuiMenuAddress(handle);
  return item != 0 ? REX_LOAD_U8(item + 26) : UINT8_MAX;
}

const char* SpDevGuiItemTypeName(uint8_t type) {
  switch (type) {
    case kSpDevGuiChildMenu:
      return "menu";
    case kSpDevGuiChildDvar:
      return "dvar";
    case kSpDevGuiChildCommand:
      return "command";
    case kSpDevGuiChildGraph:
      return "graph";
    default:
      return "invalid";
  }
}

std::string ReadSpDevGuiItemNameByHandle(uint8_t* base, uint16_t handle) {
  const uint32_t item = GetSpDevGuiMenuAddress(handle);
  return item != 0 ? ReadSpDevGuiItemName(base, item) : "<invalid>";
}

void AppendSpDevGuiOverlayItem(uint8_t* base, uint16_t handle, int depth,
                               std::unordered_set<uint16_t>& visited,
                               nx1::sp::DevGuiOverlaySnapshot& snapshot) {
  if (!IsSpDevGuiHandleValid(handle) || !visited.insert(handle).second ||
      snapshot.items.size() >= 2000) {
    return;
  }

  const uint32_t item_address = GetSpDevGuiMenuAddress(handle);
  if (item_address == 0) {
    return;
  }

  nx1::sp::DevGuiOverlayItem item{};
  item.handle = handle;
  item.name = ReadSpDevGuiItemName(base, item_address);
  item.type = REX_LOAD_U8(item_address + 26);
  item.child_index = REX_LOAD_U8(item_address + 27);
  item.next_handle = REX_LOAD_U16(item_address + 30);
  item.previous_handle = REX_LOAD_U16(item_address + 32);
  item.parent_handle = REX_LOAD_U16(item_address + 34);
  item.payload = REX_LOAD_U32(item_address + 36);
  item.depth = depth;
  if (item.type == 0) {
    item.first_child_handle = REX_LOAD_U16(item_address + 36);
  }
  snapshot.items.push_back(item);

  for (uint16_t child_handle = item.first_child_handle;
       IsSpDevGuiHandleValid(child_handle) && snapshot.items.size() < 2000;) {
    const uint32_t child_address = GetSpDevGuiMenuAddress(child_handle);
    if (child_address == 0) {
      break;
    }
    const uint16_t next_child = REX_LOAD_U16(child_address + 30);
    AppendSpDevGuiOverlayItem(base, child_handle, depth + 1, visited, snapshot);
    if (next_child == child_handle) {
      break;
    }
    child_handle = next_child;
  }
}

void PublishSpDevGuiOverlaySnapshot(PPCContext& ctx, uint8_t* base,
                                    bool active) {
  (void)ctx;
  nx1::sp::DevGuiOverlaySnapshot snapshot{};
  snapshot.active = active;
  snapshot.generation = ++g_sp_devgui_overlay_snapshot_generation;
  if (active) {
    snapshot.editing = REX_LOAD_U8(kSpDevGuiEditing) != 0;
    snapshot.root_handle = REX_LOAD_U16(kSpDevGuiRootHandle);
    snapshot.selected_handle = REX_LOAD_U16(kSpDevGuiSelectedHandle);

    std::unordered_set<uint16_t> visited;
    if (IsSpDevGuiHandleValid(snapshot.root_handle)) {
      AppendSpDevGuiOverlayItem(base, snapshot.root_handle, 0, visited,
                                snapshot);
    }
    if (IsSpDevGuiHandleValid(snapshot.selected_handle) &&
        visited.find(snapshot.selected_handle) == visited.end()) {
      AppendSpDevGuiOverlayItem(base, snapshot.selected_handle, 0, visited,
                                snapshot);
    }
  }
  nx1::sp::PublishDevGuiOverlaySnapshot(std::move(snapshot));
}

bool SelectSpDevGuiHandle(uint8_t* base, uint16_t handle) {
  if (!IsSpDevGuiHandleValid(handle)) {
    return false;
  }

  const uint32_t item = GetSpDevGuiMenuAddress(handle);
  if (item == 0) {
    return false;
  }

  REX_STORE_U16(kSpDevGuiSelectedHandle, handle);

  const uint16_t parent_handle = REX_LOAD_U16(item + 34);
  const uint32_t parent = GetSpDevGuiMenuAddress(parent_handle);
  if (parent == 0) {
    return true;
  }

  uint8_t child_index = 0;
  for (uint16_t child_handle = REX_LOAD_U16(parent + 36);
       IsSpDevGuiHandleValid(child_handle) && child_index < UINT8_MAX;
       ++child_index) {
    const uint32_t child = GetSpDevGuiMenuAddress(child_handle);
    if (child == 0) {
      break;
    }
    if (child_handle == handle) {
      REX_STORE_U8(parent + 27, child_index);
      return true;
    }
    const uint16_t next_child = REX_LOAD_U16(child + 30);
    if (next_child == child_handle) {
      break;
    }
    child_handle = next_child;
  }

  return true;
}

void ApplySpDevGuiOverlayCommands(PPCContext& ctx, uint8_t* base,
                                  uint32_t local_client_num) {
  if (!IsSpNativeDevGuiOverlayEnabled() || !IsSpDevGuiActive(ctx, base)) {
    return;
  }

  nx1::sp::DevGuiOverlayCommand command{};
  for (uint32_t command_count = 0;
       command_count < 32 &&
       nx1::sp::ConsumeDevGuiOverlayCommand(&command);
       ++command_count) {
    const bool editing = REX_LOAD_U8(kSpDevGuiEditing) != 0;
    const uint16_t selected_handle = REX_LOAD_U16(kSpDevGuiSelectedHandle);
    if (editing) {
      const uint8_t selected_type = GetSpDevGuiItemType(base, selected_handle);
      if (selected_type != kSpDevGuiChildDvar) {
        REXLOG_WARN(
            "NX1 SP: devgui native overlay found invalid edit target '{}' "
            "handle={} type={}; rejecting edit mode",
            ReadSpDevGuiItemNameByHandle(base, selected_handle),
            selected_handle, SpDevGuiItemTypeName(selected_type));
        PPCContext reject_ctx = ctx;
        __imp__NX1DevGui_Reject_YAXXZ(reject_ctx, base);
        continue;
      }

      if (command.kind == nx1::sp::DevGuiOverlayCommandKind::kSelect) {
        if (command.handle != selected_handle) {
          continue;
        }
        continue;
      }

      if (command.kind == nx1::sp::DevGuiOverlayCommandKind::kAccept &&
          command.handle != selected_handle) {
        continue;
      }
    }

    switch (command.kind) {
      case nx1::sp::DevGuiOverlayCommandKind::kSelect:
        SelectSpDevGuiHandle(base, command.handle);
        break;
      case nx1::sp::DevGuiOverlayCommandKind::kAccept: {
        if (!SelectSpDevGuiHandle(base, command.handle)) {
          break;
        }
        PPCContext accept_ctx = ctx;
        accept_ctx.r3.u64 = local_client_num;
        __imp__NX1DevGui_Accept_YAXH_Z(accept_ctx, base);
        break;
      }
      case nx1::sp::DevGuiOverlayCommandKind::kReject: {
        PPCContext reject_ctx = ctx;
        __imp__NX1DevGui_Reject_YAXXZ(reject_ctx, base);
        break;
      }
    }
  }
}

int32_t GetSpDevGuiSelectedChildIndex(uint8_t* base, uint32_t parent,
                                      uint32_t selected_handle,
                                      uint32_t* out_child_count) {
  if (out_child_count != nullptr) {
    *out_child_count = 0;
  }
  if (parent == 0) {
    return -1;
  }

  int32_t selected_index = -1;
  uint32_t child_count = 0;
  for (uint32_t child_handle = REX_LOAD_U16(parent + 36);
       IsSpDevGuiHandleValid(child_handle) && child_count < 2000;
       child_handle = REX_LOAD_U16(GetSpDevGuiMenuAddress(child_handle) + 30)) {
    const uint32_t child = GetSpDevGuiMenuAddress(child_handle);
    if (child == 0) {
      break;
    }
    if (child_handle == selected_handle) {
      selected_index = static_cast<int32_t>(child_count);
    }
    ++child_count;
  }
  if (out_child_count != nullptr) {
    *out_child_count = child_count;
  }
  return selected_index;
}

void RecordSpDevGuiVerticalMenu(PPCContext& ctx, uint8_t* base,
                                uint32_t parent, uint32_t selected_handle,
                                uint32_t origin_ptr) {
  if (parent == 0 || origin_ptr == 0) {
    return;
  }

  const int32_t row_height = GetSpDevGuiRowHeight(ctx, base);
  const int32_t max_width = GetSpDevGuiMaxChildWidth(ctx, base, parent);
  int32_t x = static_cast<int32_t>(REX_LOAD_U32(origin_ptr + 0));
  int32_t y = static_cast<int32_t>(REX_LOAD_U32(origin_ptr + 4));
  const int32_t screen_height =
      static_cast<int32_t>(REX_LOAD_U32(kSpDevGuiScreenHeight));
  uint32_t child_count = 0;
  const int32_t selected_index =
      GetSpDevGuiSelectedChildIndex(base, parent, selected_handle,
                                    &child_count);
  if (child_count == 0 || row_height <= 0 || max_width <= 0) {
    return;
  }

  int32_t first_visible_index = 0;
  const int32_t available_rows =
      std::max((screen_height - (row_height * 2) - y) / row_height, 1);
  if (available_rows < static_cast<int32_t>(child_count) &&
      selected_index > available_rows) {
    first_visible_index = selected_index - available_rows;
    y += row_height;
  }

  uint32_t child_handle = REX_LOAD_U16(parent + 36);
  for (uint32_t child_index = 0;
       IsSpDevGuiHandleValid(child_handle) && child_index < child_count;
       ++child_index) {
    const uint32_t child = GetSpDevGuiMenuAddress(child_handle);
    if (child == 0) {
      break;
    }

    if (static_cast<int32_t>(child_index) >= first_visible_index &&
        y < screen_height) {
      DevGuiMouseCandidate candidate{};
      candidate.handle = static_cast<uint16_t>(child_handle);
      candidate.parent_handle = REX_LOAD_U16(child + 34);
      candidate.parent = parent;
      candidate.child_index = static_cast<uint8_t>(
          std::min<uint32_t>(child_index, UINT8_MAX));
      candidate.x = x;
      candidate.y = y;
      candidate.w = max_width;
      candidate.h = row_height;
      g_sp_devgui_mouse_candidates.push_back(candidate);
      y += row_height;
    }

    child_handle = REX_LOAD_U16(child + 30);
  }
}

const DevGuiMouseCandidate* FindSpDevGuiMouseCandidate(float pointer_x,
                                                       float pointer_y) {
  const DevGuiMouseCandidate* best = nullptr;
  int64_t best_area = INT64_MAX;
  for (const DevGuiMouseCandidate& candidate : g_sp_devgui_mouse_candidates) {
    const int32_t hit_padding = 2;
    if (pointer_x < static_cast<float>(candidate.x - hit_padding) ||
        pointer_x > static_cast<float>(candidate.x + candidate.w + hit_padding) ||
        pointer_y < static_cast<float>(candidate.y - hit_padding) ||
        pointer_y > static_cast<float>(candidate.y + candidate.h + hit_padding)) {
      continue;
    }

    const int64_t area =
        static_cast<int64_t>(candidate.w) * static_cast<int64_t>(candidate.h);
    if (best == nullptr || area < best_area) {
      best = &candidate;
      best_area = area;
    }
  }
  return best;
}

void SelectSpDevGuiMouseCandidate(uint8_t* base,
                                  const DevGuiMouseCandidate& candidate) {
  REX_STORE_U16(kSpDevGuiSelectedHandle, candidate.handle);
  if (candidate.parent != 0) {
    REX_STORE_U8(candidate.parent + 27, candidate.child_index);
  }
}

void ApplySpDevGuiMouseInput(PPCContext& ctx, uint8_t* base) {
  if (!IsSpDevGuiActive(ctx, base)) {
    SetSpDevGuiForceMenuMode(false);
    return;
  }

  SetSpDevGuiForceMenuMode(true);
  if (IsSpNativeDevGuiOverlayEnabled()) {
    rex::input::mnk::ClearMenuMouseButtons();
    return;
  }

  if (!g_sp_logged_devgui_mouse) {
    g_sp_logged_devgui_mouse = true;
    REXLOG_INFO("NX1 SP: devgui mouse controls active");
  }

  uint32_t buttons_released = 0;
  rex::input::mnk::ConsumeMenuMouseButtonSnapshot(
      nullptr, nullptr, &buttons_released, nullptr);

  float pointer_x = 0.0f;
  float pointer_y = 0.0f;
  uint32_t pointer_generation = 0;
  if (rex::input::mnk::QueryMenuPointerVirtualPosition(
          &pointer_x, &pointer_y, &pointer_generation)) {
    const DevGuiMouseCandidate* candidate =
        FindSpDevGuiMouseCandidate(pointer_x, pointer_y);
    if (candidate != nullptr) {
      SelectSpDevGuiMouseCandidate(base, *candidate);
    }
  }

  if ((buttons_released & rex::input::mnk::kMenuMouseButtonRight) != 0) {
    PPCContext reject_ctx = ctx;
    __imp__NX1DevGui_Reject_YAXXZ(reject_ctx, base);
  }
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

uint32_t GetSpActiveScreenPlacement(PPCContext& ctx, uint8_t* base,
                                    uint32_t local_client) {
  (void)ctx;
  (void)base;
  (void)local_client;
  return 0;
}

float SpVirtualPlacementOffsetForX(uint32_t align) {
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

float SpVirtualPlacementOffsetForY(uint32_t align) {
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

float SpScreenPlacementOffsetForX(uint8_t* base, uint32_t placement,
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

float SpScreenPlacementOffsetForY(uint8_t* base, uint32_t placement,
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

void ApplySpScreenPlacementToRect(uint8_t* base, uint32_t placement,
                                  uint32_t horizontal_align,
                                  uint32_t vertical_align,
                                  MenuHoverRect* rect) {
  if (rect == nullptr) {
    return;
  }

  if (placement == 0) {
    rect->x += SpVirtualPlacementOffsetForX(horizontal_align);
    rect->y += SpVirtualPlacementOffsetForY(vertical_align);
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
              SpScreenPlacementOffsetForX(base, placement, horizontal_align);
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
              SpScreenPlacementOffsetForY(base, placement, vertical_align);
    rect->h *= scale;
  }
}

MenuHoverRect GetSpDrawnItemRect(uint8_t* base, uint32_t item,
                                 uint32_t placement) {
  MenuHoverRect rect{
      LoadGuestFloat(base, item + 4),
      LoadGuestFloat(base, item + 8),
      LoadGuestFloat(base, item + 12),
      LoadGuestFloat(base, item + 16),
  };
  ApplySpScreenPlacementToRect(base, placement, REX_LOAD_U8(item + 20),
                               REX_LOAD_U8(item + 21), &rect);
  return rect;
}

struct SpDirectCarouselHoverState {
  uint32_t menu = 0;
  uint32_t local_client = UINT32_MAX;
  int32_t column = -1;
};

SpDirectCarouselHoverState g_sp_direct_carousel_hover;

int32_t GetSpDirectCarouselColumnCount() {
  return std::clamp(
      rex::cvar::Query<int32_t>("mnk_menu_carousel_column_count"), 1, 16);
}

int32_t ClassifySpDirectCarouselColumn(float pointer_x, float pointer_y) {
  const double normalized_x = static_cast<double>(pointer_x) / 640.0;
  const double normalized_y = static_cast<double>(pointer_y) / 480.0;
  const double y_min = std::clamp(
      rex::cvar::Query<double>("mnk_menu_carousel_y_min"), 0.0, 1.0);
  const double y_max = std::clamp(
      rex::cvar::Query<double>("mnk_menu_carousel_y_max"), y_min, 1.0);
  if (normalized_y < y_min || normalized_y > y_max) {
    return -1;
  }

  const double first_x = std::clamp(
      rex::cvar::Query<double>("mnk_menu_carousel_first_x"), 0.0, 1.0);
  const double column_step = std::max(
      rex::cvar::Query<double>("mnk_menu_carousel_column_step"), 0.001);
  const double column_deadzone = std::max(
      rex::cvar::Query<double>("mnk_menu_carousel_column_deadzone"), 0.001);
  const int32_t column_count = GetSpDirectCarouselColumnCount();
  const int32_t column = static_cast<int32_t>(
      std::floor(((normalized_x - first_x) / column_step) + 0.5));
  if (column < 0 || column >= column_count) {
    return -1;
  }

  const double center_x = first_x + static_cast<double>(column) * column_step;
  if (std::abs(normalized_x - center_x) > column_deadzone) {
    return -1;
  }

  return column;
}

bool IsSpDirectCarouselMenu(uint8_t* base, uint32_t menu) {
  const uint32_t item_count = REX_LOAD_U32(menu + 180);
  const uint32_t items = REX_LOAD_U32(menu + 184);
  if (items == 0 || item_count == 0 || item_count > 512) {
    return false;
  }

  const double y_min = std::clamp(
      rex::cvar::Query<double>("mnk_menu_carousel_y_min"), 0.0, 1.0);
  const double y_max = std::clamp(
      rex::cvar::Query<double>("mnk_menu_carousel_y_max"), y_min, 1.0);
  const float card_y_min = static_cast<float>((y_min - 0.08) * 480.0);
  const float card_y_max = static_cast<float>((y_max + 0.08) * 480.0);
  int32_t large_card_count = 0;
  float min_center_x = 1.0e30f;
  float max_center_x = -1.0e30f;

  for (uint32_t i = 0; i < item_count; ++i) {
    const uint32_t item = REX_LOAD_U32(items + i * 4);
    if (item == 0) {
      continue;
    }

    const float rect_x = LoadGuestFloat(base, item + 4);
    const float rect_y = LoadGuestFloat(base, item + 8);
    const float rect_w = LoadGuestFloat(base, item + 12);
    const float rect_h = LoadGuestFloat(base, item + 16);
    if (!std::isfinite(rect_x) || !std::isfinite(rect_y) ||
        !std::isfinite(rect_w) || !std::isfinite(rect_h) ||
        rect_w < 70.0f || rect_h < 45.0f) {
      continue;
    }

    const float center_x = rect_x + rect_w * 0.5f;
    const float center_y = rect_y + rect_h * 0.5f;
    if (center_y < card_y_min || center_y > card_y_max) {
      continue;
    }

    ++large_card_count;
    min_center_x = std::min(min_center_x, center_x);
    max_center_x = std::max(max_center_x, center_x);
  }

  return large_card_count >= 3 && (max_center_x - min_center_x) >= 180.0f;
}

int32_t InferSpDirectCarouselColumnFromCursor(uint8_t* base, uint32_t state,
                                              uint32_t menu,
                                              uint32_t local_client) {
  const uint32_t item_count = REX_LOAD_U32(menu + 180);
  const uint32_t items = REX_LOAD_U32(menu + 184);
  if (state == 0 || items == 0 || item_count == 0 || item_count > 512 ||
      local_client > 3) {
    return -1;
  }

  const uint32_t cursor_offset = (local_client + 25) * 4;
  const uint32_t cursor = REX_LOAD_U32(state + cursor_offset);
  if (cursor >= item_count) {
    return -1;
  }

  const uint32_t item = REX_LOAD_U32(items + cursor * 4);
  if (item == 0) {
    return -1;
  }

  const float rect_x = LoadGuestFloat(base, item + 4);
  const float rect_y = LoadGuestFloat(base, item + 8);
  const float rect_w = LoadGuestFloat(base, item + 12);
  const float rect_h = LoadGuestFloat(base, item + 16);
  if (!std::isfinite(rect_x) || !std::isfinite(rect_y) ||
      !std::isfinite(rect_w) || !std::isfinite(rect_h) ||
      rect_w <= 0.0f || rect_h <= 0.0f) {
    return -1;
  }

  return ClassifySpDirectCarouselColumn(rect_x + rect_w * 0.5f,
                                        rect_y + rect_h * 0.5f);
}

void ApplySpDirectCarouselStep(PPCContext& ctx, uint8_t* base,
                               uint32_t ui_context, uint32_t menu,
                               bool next) {
  PPCContext nav_ctx = ctx;
  nav_ctx.r3.u64 = ui_context;
  nav_ctx.r4.u64 = menu;
  nav_ctx.r5.u64 = 0;
  if (next) {
    NX1Menu_SetNextCursorItem_YAPAUitemDef_s_PAUUiContext_PAUmenuDef_t_W4ItemFocusReason_Z(
        nav_ctx, base);
  } else {
    NX1Menu_SetPrevCursorItem_YAPAUitemDef_s_PAUUiContext_PAUmenuDef_t_W4ItemFocusReason_Z(
        nav_ctx, base);
  }
}

uint32_t GetSpTopOpenMenu(uint8_t* base, uint32_t ui_context) {
  constexpr uint32_t kUiStackCountOffset = 632;
  constexpr uint32_t kUiStackBaseIndex = 142;

  const uint32_t stack_count = REX_LOAD_U32(ui_context + kUiStackCountOffset);
  if (stack_count == 0 || stack_count > 64) {
    return 0;
  }

  return REX_LOAD_U32(
      ui_context + (kUiStackBaseIndex + stack_count - 1) * 4);
}

bool SpMenuPaintsOnLayer(uint8_t* base, uint32_t menu, uint32_t layer) {
  if (menu == 0) {
    return false;
  }

  const uint32_t flags = REX_LOAD_U32(menu + 72);
  const uint32_t menu_layer = ((flags << 10) | (flags >> 22)) & 1u;
  return menu_layer == (layer & 1u);
}

bool IsSpMenuInOpenStack(uint8_t* base, uint32_t ui_context, uint32_t menu) {
  constexpr uint32_t kUiStackCountOffset = 632;
  constexpr uint32_t kUiStackBaseIndex = 142;

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

uint32_t GetSpFrontmostPaintedMenu(uint8_t* base, uint32_t ui_context,
                                   uint32_t layer) {
  constexpr uint32_t kUiActiveMenuBaseOffset = 52;
  constexpr uint32_t kUiActiveMenuCountOffset = 564;
  constexpr uint32_t kUiStackCountOffset = 632;
  constexpr uint32_t kUiStackBaseIndex = 142;

  uint32_t frontmost = 0;
  const uint32_t active_count =
      REX_LOAD_U32(ui_context + kUiActiveMenuCountOffset);
  if (active_count <= 256) {
    for (uint32_t i = 0; i < active_count; ++i) {
      const uint32_t menu =
          REX_LOAD_U32(ui_context + kUiActiveMenuBaseOffset + i * 4);
      if (!IsSpMenuInOpenStack(base, ui_context, menu) &&
          SpMenuPaintsOnLayer(base, menu, layer)) {
        frontmost = menu;
      }
    }
  }

  const uint32_t stack_count = REX_LOAD_U32(ui_context + kUiStackCountOffset);
  if (stack_count <= 64) {
    for (uint32_t i = 0; i < stack_count; ++i) {
      const uint32_t menu =
          REX_LOAD_U32(ui_context + (kUiStackBaseIndex + i) * 4);
      if (SpMenuPaintsOnLayer(base, menu, layer)) {
        frontmost = menu;
      }
    }
  }

  return frontmost;
}

bool IsSpTopMenuOnStack(uint8_t* base, uint32_t ui_context, uint32_t menu) {
  const uint32_t top_menu = GetSpTopOpenMenu(base, ui_context);
  return top_menu == 0 || top_menu == menu;
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

MenuHoverFocusTarget g_sp_menu_hover_focus_target;
std::unordered_map<uint64_t, MenuHoverFocusTarget> g_sp_menu_hover_focus_targets;

uint64_t MakeMenuHoverFocusKey(uint32_t menu, uint32_t local_client) {
  return (static_cast<uint64_t>(menu) << 2) | (local_client & 0x3u);
}

uint32_t ClearSpMenuFocusOnce(PPCContext& ctx, uint8_t* base,
                              uint32_t ui_context, uint32_t menu) {
  if (ui_context == 0 || menu == 0) {
    return 0;
  }

  PPCContext focus_ctx = ctx;
  focus_ctx.r3.u64 = ui_context;
  focus_ctx.r4.u64 = menu;
  NX1Menu_ClearFocus_YAPAUitemDef_s_PAUUiContext_PAUmenuDef_t_Z(
      focus_ctx, base);
  return focus_ctx.r3.u32;
}

void ClearSpMenuFocusCompletely(PPCContext& ctx, uint8_t* base,
                                uint32_t ui_context, uint32_t menu) {
  if (ui_context == 0 || menu == 0) {
    return;
  }

  const uint32_t item_count = REX_LOAD_U32(menu + 180);
  const uint32_t max_iterations =
      item_count > 0 && item_count <= 512 ? item_count : 512;
  for (uint32_t i = 0; i < max_iterations; ++i) {
    if (ClearSpMenuFocusOnce(ctx, base, ui_context, menu) == 0) {
      break;
    }
  }
}

void RemoveSpItemFocusFlag(PPCContext& ctx, uint8_t* base,
                           uint32_t local_client, uint32_t item) {
  if (item == 0 || local_client > 3) {
    return;
  }

  PPCContext focus_ctx = ctx;
  focus_ctx.r3.u64 = local_client;
  focus_ctx.r4.u64 = item;
  focus_ctx.r5.u64 = 2;
  NX1Window_RemoveDynamicFlags_YAXHPAUwindowDef_t_H_Z(focus_ctx, base);
}

void ClearSpMenuFocusFlags(PPCContext& ctx, uint8_t* base,
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
    RemoveSpItemFocusFlag(ctx, base, local_client, REX_LOAD_U32(items + i * 4));
  }
}

bool IsSpMenuDirectHoverSelectableCandidate(PPCContext& ctx, uint8_t* base,
                                            uint32_t local_client,
                                            uint32_t item) {
  if (item == 0 || local_client > 3) {
    return false;
  }

  PPCContext probe_ctx = ctx;
  probe_ctx.r3.u64 = local_client;
  probe_ctx.r4.u64 = item;
  NX1Item_CanHaveFocus_YAHHPAUitemDef_s_Z(probe_ctx, base);
  if (probe_ctx.r3.u32 == 0) {
    return false;
  }

  probe_ctx = ctx;
  probe_ctx.r3.u64 = local_client;
  probe_ctx.r4.u64 = item;
  NX1Item_IsVisible_YAHHPAUitemDef_s_Z(probe_ctx, base);
  return probe_ctx.r3.u32 != 0;
}

bool NoteSpMenuHoverFocusTarget(uint32_t ui_context, uint32_t menu,
                                uint32_t local_client, uint32_t item,
                                int32_t item_index, int32_t rect_x,
                                int32_t rect_y, int32_t rect_w, int32_t rect_h,
                                bool* item_pointer_changed) {
  MenuHoverFocusTarget& target =
      g_sp_menu_hover_focus_targets[MakeMenuHoverFocusKey(menu, local_client)];
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

bool IsSpMenuHoverCandidateStable(uint32_t ui_context, uint32_t menu,
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
      g_sp_menu_hover_focus_targets[MakeMenuHoverFocusKey(menu, local_client)];
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

void ResetSpMenuHoverFocusTarget() {
  g_sp_menu_hover_focus_target = MenuHoverFocusTarget{};
  g_sp_menu_hover_focus_targets.clear();
}

void ApplySpQuietMenuHoverFocus(PPCContext& ctx, uint8_t* base,
                                uint32_t ui_context, uint32_t menu,
                                uint32_t local_client, uint32_t item,
                                int32_t item_index, bool clear_old_focus) {
  (void)ui_context;
  (void)item;
  if (clear_old_focus) {
    ClearSpMenuFocusCompletely(ctx, base, ui_context, menu);
  }

  PPCContext focus_ctx = ctx;
  focus_ctx.r3.u64 = local_client;
  focus_ctx.r4.u64 = menu;
  focus_ctx.r5.u64 = static_cast<uint32_t>(item_index);
  NX1Menu_SetCursorItem_YAXHPAUmenuDef_t_H_Z(focus_ctx, base);
}

void ApplySpFullMenuHoverFocus(PPCContext& ctx, uint8_t* base,
                               uint32_t ui_context, uint32_t menu,
                               uint32_t local_client, uint32_t item) {
  ClearSpMenuFocusCompletely(ctx, base, ui_context, menu);
  ClearSpMenuFocusFlags(ctx, base, local_client, menu);

  PPCContext focus_ctx = ctx;
  focus_ctx.r3.u64 = ui_context;
  focus_ctx.r4.u64 = item;
  focus_ctx.r5.u64 = 0;
  focus_ctx.f1.f64 = 0.0;
  focus_ctx.f2.f64 = 0.0;
  NX1Item_SetFocus_YAHPAUUiContext_PAUitemDef_s_MMW4ItemFocusReason_Z(
      focus_ctx, base);
}

bool ApplySpDirectCarouselMouseHover(PPCContext& ctx, uint8_t* base,
                                     uint32_t ui_context, uint32_t menu,
                                     uint32_t local_client, uint32_t state,
                                     float pointer_x, float pointer_y,
                                     uint32_t pointer_generation) {
  if (!rex::cvar::Query<bool>("mnk_menu_direct_hover_carousel")) {
    return false;
  }

  const int32_t target_column =
      ClassifySpDirectCarouselColumn(pointer_x, pointer_y);
  if (target_column < 0 || !IsSpDirectCarouselMenu(base, menu)) {
    return false;
  }

  const int32_t column_count = GetSpDirectCarouselColumnCount();
  int32_t current_column =
      InferSpDirectCarouselColumnFromCursor(base, state, menu, local_client);
  if (current_column < 0 &&
      g_sp_direct_carousel_hover.menu == menu &&
      g_sp_direct_carousel_hover.local_client == local_client) {
    current_column = g_sp_direct_carousel_hover.column;
  }
  if (current_column < 0) {
    current_column = std::clamp(
        rex::cvar::Query<int32_t>("mnk_menu_carousel_initial_column"), 0,
        column_count - 1);
  }

  g_sp_direct_carousel_hover.menu = menu;
  g_sp_direct_carousel_hover.local_client = local_client;
  g_sp_direct_carousel_hover.column = current_column;

  const int32_t delta = target_column - current_column;
  if (delta != 0) {
    const int32_t steps = std::clamp(std::abs(delta), 0, column_count - 1);
    const bool next = delta > 0;
    for (int32_t i = 0; i < steps; ++i) {
      ApplySpDirectCarouselStep(ctx, base, ui_context, menu, next);
    }
    g_sp_direct_carousel_hover.column = target_column;
    ResetSpMenuHoverFocusTarget();
  }

  rex::input::mnk::ReportMenuDirectHoverApplied(pointer_generation);
  return true;
}

bool ApplyMenuDirectMouseHover(PPCContext& ctx, uint8_t* base,
                               uint32_t ui_context, uint32_t menu,
                               bool force_full_focus = false) {
  if (!rex::cvar::Query<bool>("mnk_menu_direct_hover") ||
      ui_context == 0 || menu == 0) {
    return false;
  }

  if (!IsSpTopMenuOnStack(base, ui_context, menu)) {
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

  if (ApplySpDirectCarouselMouseHover(ctx, base, ui_context, menu,
                                      local_client, state, pointer_x,
                                      pointer_y, pointer_generation)) {
    return true;
  }

  const float padding = static_cast<float>(
      std::clamp(rex::cvar::Query<double>("mnk_menu_direct_hover_padding"),
                 -16.0, 32.0));
  const float hit_padding = force_full_focus ? std::max(padding, 6.0f)
                                             : padding;
  const float normalized_x = pointer_x / 640.0f;
  const float normalized_y = pointer_y / 480.0f;
  const uint32_t output_width =
      std::clamp(rex::cvar::Query<uint32_t>("nx1_internal_resolution_width"),
                 640u, 0x0FFFu);
  const uint32_t output_height =
      std::clamp(rex::cvar::Query<uint32_t>("nx1_internal_resolution_height"),
                 480u, 0x0FFFu);
  const MenuPointerCandidate pointer_candidates[] = {
      {pointer_x, pointer_y, 0},
      {normalized_x * 1280.0f, normalized_y * 720.0f, 1},
      {normalized_x * static_cast<float>(output_width),
       normalized_y * static_cast<float>(output_height), 2},
  };
  const uint32_t screen_placement =
      GetSpActiveScreenPlacement(ctx, base, local_client);
  int32_t best_item = -1;
  uint32_t best_priority = UINT32_MAX;
  float best_area = 1.0e30f;
  float best_distance = 1.0e30f;
  float best_rect_x = 0.0f;
  float best_rect_y = 0.0f;
  float best_rect_w = 0.0f;
  float best_rect_h = 0.0f;
  const bool allow_stack_row_fallback =
      IsSpMenuInOpenStack(base, ui_context, menu);
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
        GetSpDrawnItemRect(base, item, screen_placement);
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
        selectable = IsSpMenuDirectHoverSelectableCandidate(
            ctx, base, local_client, item);
        selectable_checked = true;
      }
      return selectable;
    };

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
      !IsSpMenuHoverCandidateStable(ui_context, menu, local_client,
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
  const bool focus_target_changed = NoteSpMenuHoverFocusTarget(
      ui_context, menu, local_client, best_item_address, best_item,
      static_cast<int32_t>(std::lround(best_rect_x)),
      static_cast<int32_t>(std::lround(best_rect_y)),
      static_cast<int32_t>(std::lround(best_rect_w)),
      static_cast<int32_t>(std::lround(best_rect_h)), &item_pointer_changed);

  if (rex::cvar::Query<bool>("mnk_menu_direct_hover_cursor_only")) {
    if (force_full_focus) {
      ApplySpFullMenuHoverFocus(ctx, base, ui_context, menu, local_client,
                                best_item_address);
    } else if (cursor_changed || focus_target_changed || item_pointer_changed) {
      ApplySpFullMenuHoverFocus(ctx, base, ui_context, menu, local_client,
                                best_item_address);
    }
  } else if (force_full_focus) {
    ApplySpFullMenuHoverFocus(ctx, base, ui_context, menu, local_client,
                              best_item_address);
  } else if (focus_target_changed) {
    ApplySpFullMenuHoverFocus(ctx, base, ui_context, menu, local_client,
                              best_item_address);
  } else if (cursor_changed || item_pointer_changed) {
    ApplySpQuietMenuHoverFocus(ctx, base, ui_context, menu, local_client,
                               best_item_address, best_item, false);
  }

  rex::input::mnk::ReportMenuDirectHoverApplied(pointer_generation);
  return true;
}

}  // namespace

extern "C" void NX1Menu_Paint_YAHPAUUiContext_PAUmenuDef_t_Z(
    PPCContext& ctx, uint8_t* base) {
  const uint32_t ui_context = ctx.r3.u32;
  const uint32_t menu = ctx.r4.u32;
  __imp__NX1Menu_Paint_YAHPAUUiContext_PAUmenuDef_t_Z(ctx, base);
  (void)ApplyMenuDirectMouseHover(ctx, base, ui_context, menu);
}

extern "C" void NX1Menu_PaintAll_Internal_YAXPAUUiContext_H_Z(
    PPCContext& ctx, uint8_t* base) {
  const uint32_t ui_context = ctx.r3.u32;
  const uint32_t layer = ctx.r4.u32;
  rex::input::mnk::ClearMenuDirectHoverApplied();
  __imp__NX1Menu_PaintAll_Internal_YAXPAUUiContext_H_Z(ctx, base);

  const uint32_t frontmost_menu =
      GetSpFrontmostPaintedMenu(base, ui_context, layer);
  if (frontmost_menu != 0) {
    (void)ApplyMenuDirectMouseHover(ctx, base, ui_context, frontmost_menu);
  }
}

extern "C" void NX1Menu_HandleKey_YAXPAUUiContext_PAUmenuDef_t_HH_Z(
    PPCContext& ctx, uint8_t* base) {
  const bool force_mouse_focus = ctx.r6.u32 != 0 && IsMenuAcceptKey(ctx.r5.u32);
  (void)ApplyMenuDirectMouseHover(ctx, base, ctx.r3.u32, ctx.r4.u32,
                                  force_mouse_focus);
  __imp__NX1Menu_HandleKey_YAXPAUUiContext_PAUmenuDef_t_HH_Z(ctx, base);
}

extern "C" void NX1DevGui_Draw_YAXH_Z(PPCContext& ctx, uint8_t* base) {
  const bool active = IsSpDevGuiActive(ctx, base);
  SetSpDevGuiForceMenuMode(active);
  PublishSpDevGuiOverlaySnapshot(ctx, base, active);
  if (active && IsSpNativeDevGuiOverlayEnabled()) {
    return;
  }
  if (active) {
    g_sp_devgui_mouse_candidates.clear();
  }
  __imp__NX1DevGui_Draw_YAXH_Z(ctx, base);
}

extern "C" void NX1DevGui_DrawMenuVertically_YAXPBUDevMenuItem_GQAH_Z(
    PPCContext& ctx, uint8_t* base) {
  if (IsSpDevGuiActive(ctx, base) && !IsSpNativeDevGuiOverlayEnabled()) {
    RecordSpDevGuiVerticalMenu(ctx, base, ctx.r3.u32, ctx.r4.u32,
                               ctx.r5.u32);
  }
  __imp__NX1DevGui_DrawMenuVertically_YAXPBUDevMenuItem_GQAH_Z(ctx, base);
}

extern "C" void NX1CL_DevGuiOpen_f_YAXXZ(PPCContext& ctx, uint8_t* base) {
  __imp__NX1CL_DevGuiOpen_f_YAXXZ(ctx, base);
  if (IsSpDevGuiActive(ctx, base)) {
    SetSpDevGuiForceMenuMode(true);
    ArmSpDevGuiToggleGuard();
  }
}

extern "C" void NX1DevGui_OpenMenu_YAXPBD_Z(PPCContext& ctx, uint8_t* base) {
  __imp__NX1DevGui_OpenMenu_YAXPBD_Z(ctx, base);
  if (IsSpDevGuiActive(ctx, base)) {
    SetSpDevGuiForceMenuMode(true);
    ArmSpDevGuiToggleGuard();
  }
}

extern "C" void NX1DevGui_Toggle_YAXXZ(PPCContext& ctx, uint8_t* base) {
  const bool was_active = IsSpDevGuiActive(ctx, base);
  if (was_active && g_sp_devgui_toggle_guard_frames != 0) {
    return;
  }

  __imp__NX1DevGui_Toggle_YAXXZ(ctx, base);

  if (!was_active && IsSpDevGuiActive(ctx, base)) {
    SetSpDevGuiForceMenuMode(true);
    ArmSpDevGuiToggleGuard();
  } else if (!IsSpDevGuiActive(ctx, base)) {
    SetSpDevGuiForceMenuMode(false);
    PublishSpDevGuiOverlaySnapshot(ctx, base, false);
  }
}

extern "C" void NX1DevGui_Update_YAXHM_Z(PPCContext& ctx, uint8_t* base) {
  const uint32_t local_client_num = ctx.r3.u32;
  ApplySpDevGuiMouseInput(ctx, base);
  __imp__NX1DevGui_Update_YAXHM_Z(ctx, base);
  ApplySpDevGuiOverlayCommands(ctx, base, local_client_num);
  const bool active = IsSpDevGuiActive(ctx, base);
  SetSpDevGuiForceMenuMode(active);
  PublishSpDevGuiOverlaySnapshot(ctx, base, active);
  if (g_sp_devgui_toggle_guard_frames != 0) {
    --g_sp_devgui_toggle_guard_frames;
  }
}

extern "C" void NX1CL_ShutdownCGame_YAXXZ(PPCContext& ctx, uint8_t* base) {
  NoteSpDiscordMenus("CL_ShutdownCGame");
  __imp__NX1CL_ShutdownCGame_YAXXZ(ctx, base);
}

extern "C" void NX1CL_Disconnect_YAXH_Z(PPCContext& ctx, uint8_t* base) {
  NoteSpDiscordMenus("CL_Disconnect");
  __imp__NX1CL_Disconnect_YAXH_Z(ctx, base);
}

extern "C" void NX1CL_DisconnectLocalClient_YAXH_N_Z(PPCContext& ctx,
                                                      uint8_t* base) {
  NoteSpDiscordMenus("CL_DisconnectLocalClient");
  __imp__NX1CL_DisconnectLocalClient_YAXH_N_Z(ctx, base);
}

extern "C" void NX1R_UnloadWorld_YAXXZ(PPCContext& ctx, uint8_t* base) {
  NoteSpDiscordMenus("R_UnloadWorld");
  __imp__NX1R_UnloadWorld_YAXXZ(ctx, base);
}

extern "C" void NX1CG_DrawFPS_YAMHPBUScreenPlacement_M_Z(PPCContext& ctx,
                                                          uint8_t* base) {
  if (!rex::system::IsNx1DebugHudEnabled()) {
    return;
  }

  PinNx1DebugHudDvars(base);
  __imp__NX1CG_DrawFPS_YAMHPBUScreenPlacement_M_Z(ctx, base);
}

extern "C" void NX1CG_CornerDebugPrint_YAMPBUScreenPlacement_MMMPBD1QBM_Z(
    PPCContext& ctx, uint8_t* base) {
  PinNx1DebugHudDvars(base);
  __imp__NX1CG_CornerDebugPrint_YAMPBUScreenPlacement_MMMPBD1QBM_Z(ctx, base);
}

extern "C" void NX1CG_DrawDebugOverlays_YAXH_Z(PPCContext& ctx, uint8_t* base) {
  if (!rex::system::IsNx1DebugHudEnabled()) {
    return;
  }

  PinNx1DebugHudDvars(base);
  __imp__NX1CG_DrawDebugOverlays_YAXH_Z(ctx, base);
}

extern "C" void NX1CG_DrawUpperRightDebugInfo_YAXHPBUScreenPlacement_Z(
    PPCContext& ctx, uint8_t* base) {
  if (!rex::system::IsNx1DebugHudEnabled()) {
    return;
  }

  PinNx1DebugHudDvars(base);
  __imp__NX1CG_DrawUpperRightDebugInfo_YAXHPBUScreenPlacement_Z(ctx, base);
}

extern "C" void NX1CG_DrawFullScreenDebugOverlays_YAXH_Z(PPCContext& ctx,
                                                          uint8_t* base) {
  if (!rex::system::IsNx1DebugHudEnabled()) {
    return;
  }

  PinNx1DebugHudDvars(base);
  __imp__NX1CG_DrawFullScreenDebugOverlays_YAXH_Z(ctx, base);
}

extern "C" void NX1Con_DrawMiniConsole_YAXHHHMW4EScreenLayer_Z(
    PPCContext& ctx, uint8_t* base) {
  if (!rex::system::IsNx1DebugHudEnabled()) {
    return;
  }

  __imp__NX1Con_DrawMiniConsole_YAXHHHMW4EScreenLayer_Z(ctx, base);
}

extern "C" void NX1Con_DrawErrors_YAXHHHMW4EScreenLayer_Z(PPCContext& ctx,
                                                           uint8_t* base) {
  if (!rex::system::IsNx1DebugHudEnabled()) {
    return;
  }

  __imp__NX1Con_DrawErrors_YAXHHHMW4EScreenLayer_Z(ctx, base);
}

extern "C" void NX1CL_GetMouseMovement_YAXPAUclientActive_t_PAM1_Z(
    PPCContext& ctx, uint8_t* base) {
  float host_mouse_x = 0.0f;
  float host_mouse_y = 0.0f;
  if (ctx.r4.u32 != 0 && ctx.r5.u32 != 0 &&
      rex::input::mnk::ConsumeNativeMouseMovement(&host_mouse_x, &host_mouse_y)) {
    ResetSpMenuHoverFocusTarget();
    if (!g_sp_logged_native_mouse_hook) {
      g_sp_logged_native_mouse_hook = true;
      REXLOG_INFO("NX1 SP: native mouse-look hook is feeding CL_GetMouseMovement");
    }

    PPCRegister temp{};
    temp.f32 = host_mouse_x;
    REX_STORE_U32(ctx.r4.u32, temp.u32);
    temp.f32 = host_mouse_y;
    REX_STORE_U32(ctx.r5.u32, temp.u32);
    return;
  }

  __imp__NX1CL_GetMouseMovement_YAXPAUclientActive_t_PAM1_Z(ctx, base);
}

extern "C" void NX1Key_AddCatcher_YAXHH_Z(PPCContext& ctx, uint8_t* base) {
  const int32_t local_client_num = ctx.r3.s32;
  const uint32_t catcher_mask = ctx.r4.u32;
  __imp__NX1Key_AddCatcher_YAXHH_Z(ctx, base);
  rex::input::mnk::AddGameKeyCatcherMask(local_client_num, catcher_mask);
}

extern "C" void NX1Key_RemoveCatcher_YAXHH_Z(PPCContext& ctx, uint8_t* base) {
  const int32_t local_client_num = ctx.r3.s32;
  const uint32_t catcher_mask = ctx.r4.u32;
  __imp__NX1Key_RemoveCatcher_YAXHH_Z(ctx, base);
  rex::input::mnk::RemoveGameKeyCatcherMask(local_client_num, catcher_mask);
}

extern "C" void NX1Key_SetCatcher_YAXHH_Z(PPCContext& ctx, uint8_t* base) {
  const int32_t local_client_num = ctx.r3.s32;
  const uint32_t catcher_mask = ctx.r4.u32;
  __imp__NX1Key_SetCatcher_YAXHH_Z(ctx, base);
  rex::input::mnk::SetGameKeyCatcherMask(local_client_num, catcher_mask);
}

extern "C" void NX1Dvar_GetBool_YA_NPBD_Z(PPCContext& ctx, uint8_t* base) {
  uint32_t forced_value = 0;
  if (GetForcedNx1DvarValueName(base, ctx.r3.u32, &forced_value)) {
    LogNx1DvarFilterOnce();
    ctx.r3.u64 = forced_value != 0;
    return;
  }

  __imp__NX1Dvar_GetBool_YA_NPBD_Z(ctx, base);
}

extern "C" void NX1Dvar_GetInt_YAHPBD_Z(PPCContext& ctx, uint8_t* base) {
  uint32_t forced_value = 0;
  if (GetForcedNx1DvarValueName(base, ctx.r3.u32, &forced_value)) {
    LogNx1DvarFilterOnce();
    ctx.r3.s64 = static_cast<int32_t>(forced_value);
    return;
  }

  __imp__NX1Dvar_GetInt_YAHPBD_Z(ctx, base);
}

extern "C" void NX1Dvar_SetVariant_YAXPAUdvar_t_TDvarValue_W4DvarSetSource_Z(
    PPCContext& ctx, uint8_t* base) {
  const uint32_t guest_dvar = ctx.r3.u32;
  ApplyForcedNx1DvarValue(base, guest_dvar, ctx.r4);
  __imp__NX1Dvar_SetVariant_YAXPAUdvar_t_TDvarValue_W4DvarSetSource_Z(ctx, base);
  PinForcedNx1DvarValue(base, guest_dvar);
}

extern "C" void NX1Dvar_RegisterBool_YAPBUdvar_t_PBD_NG0_Z(
    PPCContext& ctx, uint8_t* base) {
  ApplyForcedNx1DvarValueName(base, ctx.r3.u32, ctx.r4);
  ctx.r4.u64 = ctx.r4.u32 != 0;
  __imp__NX1Dvar_RegisterBool_YAPBUdvar_t_PBD_NG0_Z(ctx, base);
  PinForcedNx1DvarValue(base, ctx.r3.u32);
}

extern "C" void NX1Dvar_RegisterInt_YAPBUdvar_t_PBDHHHG0_Z(
    PPCContext& ctx, uint8_t* base) {
  if (ApplyForcedNx1DvarValueName(base, ctx.r3.u32, ctx.r4)) {
    if (ctx.r5.s32 > ctx.r4.s32) {
      ctx.r5.s64 = ctx.r4.s32;
    }
    if (ctx.r6.s32 < ctx.r4.s32) {
      ctx.r6.s64 = ctx.r4.s32;
    }
  }

  __imp__NX1Dvar_RegisterInt_YAPBUdvar_t_PBDHHHG0_Z(ctx, base);
  PinForcedNx1DvarValue(base, ctx.r3.u32);
}

extern "C" void NX1Dvar_RegisterFloat_YAPBUdvar_t_PBDMMMG0_Z(
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

  __imp__NX1Dvar_RegisterFloat_YAPBUdvar_t_PBDMMMG0_Z(ctx, base);
  PinForcedNx1DvarValue(base, ctx.r3.u32);
}

extern "C" void NX1Dvar_RegisterEnum_YAPBUdvar_t_PBDQAPBDHG0_Z(
    PPCContext& ctx, uint8_t* base) {
  const uint32_t guest_name = ctx.r3.u32;
  ApplyForcedNx1DvarValueName(base, guest_name, ctx.r5);
  __imp__NX1Dvar_RegisterEnum_YAPBUdvar_t_PBDQAPBDHG0_Z(ctx, base);
  PinForcedNx1DvarValue(base, ctx.r3.u32);
}

extern "C" void NX1Dvar_SetBoolFromSource_YAXPBUdvar_t_NW4DvarSetSource_Z(
    PPCContext& ctx, uint8_t* base) {
  const uint32_t guest_dvar = ctx.r3.u32;
  ApplyForcedNx1DvarValue(base, guest_dvar, ctx.r4);
  ctx.r4.u64 = ctx.r4.u32 != 0;
  __imp__NX1Dvar_SetBoolFromSource_YAXPBUdvar_t_NW4DvarSetSource_Z(ctx, base);
  PinForcedNx1DvarValue(base, guest_dvar);
}

extern "C" void NX1Dvar_SetIntFromSource_YAXPBUdvar_t_HW4DvarSetSource_Z(
    PPCContext& ctx, uint8_t* base) {
  const uint32_t guest_dvar = ctx.r3.u32;
  ApplyForcedNx1DvarValue(base, guest_dvar, ctx.r4);
  __imp__NX1Dvar_SetIntFromSource_YAXPBUdvar_t_HW4DvarSetSource_Z(ctx, base);
  PinForcedNx1DvarValue(base, guest_dvar);
}

extern "C" void NX1Dvar_SetFloatFromSource_YAXPBUdvar_t_MW4DvarSetSource_Z(
    PPCContext& ctx, uint8_t* base) {
  const uint32_t guest_dvar = ctx.r3.u32;
  uint32_t forced_value = 0;
  if (GetForcedNx1DvarValue(base, guest_dvar, &forced_value)) {
    LogNx1DvarFilterOnce();
    ctx.f1.f64 = double(float(forced_value));
  }

  __imp__NX1Dvar_SetFloatFromSource_YAXPBUdvar_t_MW4DvarSetSource_Z(ctx, base);
  PinForcedNx1DvarValue(base, guest_dvar);
}

extern "C" void NX1Dvar_SetStringFromSource_YAXPBUdvar_t_PBDW4DvarSetSource_Z(
    PPCContext& ctx, uint8_t* base) {
  const uint32_t guest_dvar = ctx.r3.u32;
  if (BlockUnsafeSpDebugDvarSet(base, guest_dvar)) {
    return;
  }

  __imp__NX1Dvar_SetStringFromSource_YAXPBUdvar_t_PBDW4DvarSetSource_Z(ctx, base);
  PinForcedNx1DvarValue(base, guest_dvar);
}

extern "C" void NX1Dvar_SetString_YAXPBUdvar_t_PBD_Z(PPCContext& ctx,
                                                       uint8_t* base) {
  const uint32_t guest_dvar = ctx.r3.u32;
  if (BlockUnsafeSpDebugDvarSet(base, guest_dvar)) {
    return;
  }

  __imp__NX1Dvar_SetString_YAXPBUdvar_t_PBD_Z(ctx, base);
  PinForcedNx1DvarValue(base, guest_dvar);
}

extern "C" void NX1Dvar_SetFromStringFromSource_YAXPBUdvar_t_PBDW4DvarSetSource_Z(
    PPCContext& ctx, uint8_t* base) {
  const uint32_t guest_dvar = ctx.r3.u32;
  if (BlockUnsafeSpDebugDvarSet(base, guest_dvar)) {
    return;
  }

  __imp__NX1Dvar_SetFromStringFromSource_YAXPBUdvar_t_PBDW4DvarSetSource_Z(ctx,
                                                                            base);
  PinForcedNx1DvarValue(base, guest_dvar);
}

extern "C" void NX1Dvar_SetFromString_YAXPBUdvar_t_PBD_Z(PPCContext& ctx,
                                                           uint8_t* base) {
  const uint32_t guest_dvar = ctx.r3.u32;
  if (BlockUnsafeSpDebugDvarSet(base, guest_dvar)) {
    return;
  }

  __imp__NX1Dvar_SetFromString_YAXPBUdvar_t_PBD_Z(ctx, base);
  PinForcedNx1DvarValue(base, guest_dvar);
}

extern "C" void NX1Dvar_SetBoolByName_YAXPBD_N_Z(PPCContext& ctx,
                                                  uint8_t* base) {
  ApplyForcedNx1DvarValueName(base, ctx.r3.u32, ctx.r4);
  ctx.r4.u64 = ctx.r4.u32 != 0;
  __imp__NX1Dvar_SetBoolByName_YAXPBD_N_Z(ctx, base);
}

extern "C" void NX1Dvar_SetIntByName_YAXPBDH_Z(PPCContext& ctx,
                                                uint8_t* base) {
  ApplyForcedNx1DvarValueName(base, ctx.r3.u32, ctx.r4);
  __imp__NX1Dvar_SetIntByName_YAXPBDH_Z(ctx, base);
}

extern "C" void NX1Dvar_SetFloatByName_YAXPBDM_Z(PPCContext& ctx,
                                                  uint8_t* base) {
  uint32_t forced_value = 0;
  if (GetForcedNx1DvarValueName(base, ctx.r3.u32, &forced_value)) {
    LogNx1DvarFilterOnce();
    ctx.f1.f64 = double(float(forced_value));
  }

  __imp__NX1Dvar_SetFloatByName_YAXPBDM_Z(ctx, base);
}

extern "C" void NX1Dvar_SetStringByName_YAXPBD0_Z(PPCContext& ctx,
                                                   uint8_t* base) {
  if (BlockUnsafeSpDebugDvarSetName(base, ctx.r3.u32)) {
    return;
  }

  __imp__NX1Dvar_SetStringByName_YAXPBD0_Z(ctx, base);
}

extern "C" void NX1Dvar_SetFromStringByNameFromSource_YAPBUdvar_t_PBD0W4DvarSetSource_Z(
    PPCContext& ctx, uint8_t* base) {
  if (BlockUnsafeSpDebugDvarSetName(base, ctx.r3.u32)) {
    ctx.r3.u64 = 0;
    return;
  }

  __imp__NX1Dvar_SetFromStringByNameFromSource_YAPBUdvar_t_PBD0W4DvarSetSource_Z(
      ctx, base);
}

extern "C" void NX1Dvar_SetFromStringByName_YAXPBD0_Z(PPCContext& ctx,
                                                       uint8_t* base) {
  if (BlockUnsafeSpDebugDvarSetName(base, ctx.r3.u32)) {
    return;
  }

  __imp__NX1Dvar_SetFromStringByName_YAXPBD0_Z(ctx, base);
}

extern "C" void NX1Dvar_SetCommand_YAXPBD0_Z(PPCContext& ctx,
                                              uint8_t* base) {
  if (BlockUnsafeSpDebugDvarSetName(base, ctx.r3.u32)) {
    return;
  }

  __imp__NX1Dvar_SetCommand_YAXPBD0_Z(ctx, base);
}

extern "C" void NX1BB_Connect_YAXXZ(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  LogBlackBoxSkipOnce();
}

extern "C" void NX1BB_TryReconnect_YAXXZ(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  LogBlackBoxSkipOnce();
}

void rex_sp_skip_storage_selector(PPCRegister& r3) {
  REXLOG_INFO("NX1 SP: bypassing storage device selector preflight");
  r3.u64 = 0;
}

void rex_sp_trace_get_allocator_by_id(PPCRegister& r3, PPCRegister& r4,
                                      PPCRegister& r12) {
  REXLOG_INFO("NX1 SP: GetAllocator(id) manager=0x{:08X} id=0x{:08X} caller=0x{:08X}",
              r3.u32, r4.u32, r12.u32);
}

bool rex_sp_skip_missing_allocator_assert(PPCRegister& r8, PPCRegister& r31) {
  REXLOG_WARN("NX1 SP: missing allocator id=0x{:08X}; using fallback slot {}",
              r8.u32, r31.u32);
  return true;
}

bool rex_sp_skip_virtual_commit_memory_assert(PPCRegister& r8, PPCRegister& r22,
                                              PPCRegister& r20, PPCRegister& r23) {
  REXLOG_WARN(
      "NX1 SP: skipping VirtualCommit memory tracker assert diff={} addr=0x{:08X} "
      "size=0x{:08X} expected=0x{:08X}",
      r8.s32, r22.u32, r20.u32, r23.u32);
  return true;
}

bool rex_sp_skip_virtual_decommit_memory_assert(PPCRegister& r8, PPCRegister& r20,
                                                PPCRegister& r19, PPCRegister& r22) {
  REXLOG_WARN(
      "NX1 SP: skipping VirtualDecommit memory tracker assert diff={} addr=0x{:08X} "
      "size=0x{:08X} expected_change=0x{:08X}",
      r8.s32, r20.u32, r19.u32, r22.u32);
  return true;
}

bool rex_sp_skip_virtual_free_memory_assert(PPCRegister& r8, PPCRegister& r22,
                                            PPCRegister& r20, PPCRegister& r31) {
  REXLOG_WARN(
      "NX1 SP: skipping VirtualFree memory tracker assert diff={} addr=0x{:08X} "
      "size=0x{:08X} expected_change=0x{:08X}",
      r8.s32, r22.u32, r20.u32, r31.u32);
  return true;
}

bool rex_sp_skip_blank_screen_memory_assert(PPCRegister& r31) {
  const int32_t change = r31.s32;
  REXLOG_WARN("NX1 SP: skipping blank-screen memory tracker assert change={}",
              change);
  if (change >= 0) {
    r31.u64 = 0;
  }
  return true;
}

void rex_sp_raise_game_map_pool_limit(PPCRegister& r3) {
  std::string type_name;
  if (!IsGameMapAssetType(r3.u32, &type_name)) {
    return;
  }

  const uint32_t limit_addr = kXAssetPoolLimitTable + r3.u32 * 4;
  const uint32_t old_limit = GuestLoadU32(limit_addr);
  if (old_limit >= kRaisedGameMapPoolLimit) {
    return;
  }

  GuestStoreU32(limit_addr, kRaisedGameMapPoolLimit);
  REXLOG_WARN("NX1 SP: raising '{}' asset pool limit {} -> {}", type_name,
              old_limit, kRaisedGameMapPoolLimit);
}

void rex_sp_supply_extra_game_map_asset(PPCRegister& r3, PPCRegister& r1) {
  if (r3.u32 != 0) {
    return;
  }

  const uint32_t type = GuestLoadU32(r1.u32 + 148);
  std::string type_name;
  if (!IsGameMapAssetType(type, &type_name)) {
    return;
  }

  const uint32_t guest_asset =
      REX_KERNEL_MEMORY()->SystemHeapAlloc(kEmergencyGameMapAssetSize, 8);
  if (guest_asset == 0) {
    REXLOG_ERROR("NX1 SP: failed to allocate emergency '{}' asset header",
                 type_name);
    return;
  }

  std::memset(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(guest_asset), 0,
              kEmergencyGameMapAssetSize);
  r3.u64 = guest_asset;

  const uint32_t limit = GuestLoadU32(kXAssetPoolLimitTable + type * 4);
  REXLOG_WARN(
      "NX1 SP: supplied emergency '{}' asset header at 0x{:08X} "
      "(pool limit={})",
      type_name, guest_asset, limit);
}

bool rex_sp_skip_game_map_pool_error(PPCRegister& r30, PPCRegister& r8,
                                     PPCRegister& r7) {
  const std::string type_name = ReadGuestCString(r8.u32, 64);
  if (type_name != "game_map_mp" && type_name != "game_map_sp") {
    return false;
  }

  const uint32_t guest_asset =
      REX_KERNEL_MEMORY()->SystemHeapAlloc(kEmergencyGameMapAssetSize, 8);
  if (guest_asset == 0) {
    REXLOG_ERROR("NX1 SP: failed to bypass '{}' pool limit error", type_name);
    return false;
  }

  std::memset(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(guest_asset), 0,
              kEmergencyGameMapAssetSize);
  r30.u64 = guest_asset;
  REXLOG_WARN(
      "NX1 SP: bypassing '{}' pool limit {} with emergency asset header "
      "0x{:08X}",
      type_name, r7.u32, guest_asset);
  return true;
}

void rex_sp_note_campaign_map(PPCRegister& r3) {
  const std::string map_name = NormalizeMapName(ReadGuestCString(r3.u32, 128));
  if (!IsCampaignMapName(map_name)) {
    return;
  }

  if (map_name == g_sp_last_discord_campaign_map) {
    return;
  }

  g_sp_last_discord_campaign_map = map_name;
  REXLOG_INFO("NX1 SP: Discord Rich Presence campaign map '{}'", map_name);
  rex::system::DiscordPresenceClient::Get().NoteLoadedMap(REX_KERNEL_STATE(), map_name);
}

void rex_sp_expand_d3d_heap_for_high_resolution(PPCRegister& wrapper,
                                                PPCRegister& size,
                                                PPCRegister& name) {
  if (!rex::cvar::Query<bool>("nx1_internal_resolution_patch")) {
    return;
  }

  if (size.u32 != kBaseD3DHeapSize) {
    return;
  }

  const uint32_t width =
      std::clamp(rex::cvar::Query<uint32_t>("nx1_internal_resolution_width"), 640u,
                 0x0FFFu);
  const uint32_t height =
      std::clamp(rex::cvar::Query<uint32_t>("nx1_internal_resolution_height"), 480u,
                 0x0FFFu);
  if (width <= kBaseD3DHeapWidth && height <= kBaseD3DHeapHeight) {
    return;
  }

  const uint64_t scaled_size =
      (uint64_t(kBaseD3DHeapSize) * width * height +
       uint64_t(kBaseD3DHeapWidth) * kBaseD3DHeapHeight - 1) /
      (uint64_t(kBaseD3DHeapWidth) * kBaseD3DHeapHeight);
  uint32_t expanded_size =
      AlignUp(uint32_t(std::min<uint64_t>(scaled_size, 0x08000000ull)), 0x10000);
  if (width >= 1920 && height >= 1080) {
    expanded_size = std::max(expanded_size, kMin1080pD3DHeapSize);
  }

  if (expanded_size <= size.u32) {
    return;
  }

  if (!g_sp_logged_d3d_heap_expansion) {
    g_sp_logged_d3d_heap_expansion = true;
    const std::string heap_name = ReadGuestCString(name.u32, 64);
    REXLOG_WARN(
        "NX1 SP: expanding D3D XMem heap '{}' wrapper=0x{:08X} for {}x{} internal "
        "resolution, 0x{:08X} -> 0x{:08X}",
        heap_name.empty() ? "<unnamed>" : heap_name, wrapper.u32, width, height,
        size.u32, expanded_size);
  }
  size.u64 = expanded_size;
}

bool rex_sp_skip_unlock_read_underflow(PPCRegister& r3, PPCRegister& r11) {
  if (r11.s32 > 0) {
    return false;
  }

  REXLOG_WARN(
      "NX1 SP: ignoring FastCriticalSection read unlock underflow "
      "critSect=0x{:08X} readCount={}",
      r3.u32, r11.s32);
  return true;
}

bool rex_sp_skip_unlock_write_underflow(PPCRegister& r3, PPCRegister& r11) {
  if (r11.s32 > 0) {
    return false;
  }

  REXLOG_WARN(
      "NX1 SP: ignoring FastCriticalSection write unlock underflow "
      "critSect=0x{:08X} writeCount={}",
      r3.u32, r11.s32);
  return true;
}

bool rex_sp_skip_memmap_profile_push_overflow(PPCRegister& depth, PPCRegister& r13,
                                              PPCRegister& name) {
  if (depth.u32 < 16) {
    return false;
  }

  uint32_t overflow_depth = 0;
  {
    std::lock_guard lock(g_memmap_profile_overflow_mutex);
    overflow_depth = ++g_memmap_profile_overflow_depth_by_tls[r13.u32];
  }

  if (overflow_depth == 1) {
    REXLOG_WARN(
        "NX1 SP: ignoring over-depth memmap profile push tls=0x{:08X} depth={} name='{}'",
        r13.u32, depth.u32, ReadGuestCString(name.u32, 128));
  }
  return true;
}

bool rex_sp_skip_memmap_profile_pop_overflow(PPCRegister& r13) {
  std::lock_guard lock(g_memmap_profile_overflow_mutex);
  auto it = g_memmap_profile_overflow_depth_by_tls.find(r13.u32);
  if (it == g_memmap_profile_overflow_depth_by_tls.end() || it->second == 0) {
    return false;
  }

  --it->second;
  if (it->second == 0) {
    g_memmap_profile_overflow_depth_by_tls.erase(it);
  }
  return true;
}

bool rex_sp_skip_memmap_profile_assert(PPCRegister& file, PPCRegister& line,
                                       PPCRegister& fmt) {
  const std::string file_name = ReadGuestCString(file.u32, 160);
  if (file_name.find("memmap_profile_tree.cpp") == std::string::npos) {
    return false;
  }

  if (line.u32 != 87 && line.u32 != 136) {
    return false;
  }

  REXLOG_WARN("NX1 SP: suppressing memmap profiler assert line={} fmt='{}'",
              line.u32, ReadGuestCString(fmt.u32, 160));
  return true;
}

void rex_sp_trace_nxheap_region_alloc_failure(PPCRegister& caller,
                                              PPCRegister& allocator,
                                              PPCRegister& heap,
                                              PPCRegister& request,
                                              PPCRegister& fatal,
                                              PPCRegister& direction,
                                              PPCRegister& candidate,
                                              PPCRegister& limit) {
  const uint32_t heap_name_addr = heap.u32 + 44;
  const uint32_t heap_direction = GuestLoadU32(heap.u32 + 36);
  const uint32_t cursor = GuestLoadU32(heap.u32 + 40);
  const uint32_t region_start = GuestLoadU32(allocator.u32 + 0);
  const uint32_t region_end = GuestLoadU32(allocator.u32 + 4);
  const uint32_t upward_heap = GuestLoadU32(allocator.u32 + 8);
  const uint32_t downward_heap = GuestLoadU32(allocator.u32 + 12);
  const uint32_t min_available = GuestLoadU32(allocator.u32 + 16);
  const uint32_t free_blocks = GuestLoadU32(heap.u32 + 68);
  const uint32_t used_blocks = GuestLoadU32(heap.u32 + 72);

  const bool grows_down = direction.u32 != 0;
  const uint32_t available =
      grows_down ? cursor - limit.u32 : limit.u32 - cursor;
  const uint32_t overflow =
      request.u32 > available ? request.u32 - available : 0;

  REXLOG_ERROR(
      "NX1 SP: nxheap region allocation failed caller=0x{:08X} heap='{}' heap=0x{:08X} "
      "allocator=0x{:08X} request=0x{:08X} available=0x{:08X} overflow=0x{:08X} "
      "fatal={} dir={} heap_dir=0x{:08X} cursor=0x{:08X} candidate=0x{:08X} "
      "limit=0x{:08X} region=[0x{:08X},0x{:08X}) active=[0x{:08X},0x{:08X}] "
      "min_available=0x{:08X} free_blocks=0x{:08X} used_blocks=0x{:08X}",
      caller.u32, ReadGuestCString(heap_name_addr, 32), heap.u32, allocator.u32,
      request.u32, available, overflow, fatal.u32 & 0xFF, grows_down ? "down" : "up",
      heap_direction, cursor, candidate.u32, limit.u32, region_start, region_end,
      upward_heap, downward_heap, min_available, free_blocks, used_blocks);
}

bool rex_sp_skip_nxheap_region_alloc_assert(PPCRegister& caller,
                                            PPCRegister& allocator,
                                            PPCRegister& heap,
                                            PPCRegister& request,
                                            PPCRegister& fatal,
                                            PPCRegister& direction,
                                            PPCRegister& candidate,
                                            PPCRegister& limit) {
  if ((fatal.u32 & 0xFF) == 0) {
    return false;
  }

  uint32_t failure_count = 0;
  {
    std::lock_guard lock(g_nxheap_failure_mutex);
    failure_count = ++g_nxheap_failure_count_by_heap[heap.u32];
  }

  if (failure_count <= 4 || (failure_count & (failure_count - 1)) == 0) {
    const uint32_t heap_name_addr = heap.u32 + 44;
    const uint32_t heap_direction = GuestLoadU32(heap.u32 + 36);
    const uint32_t cursor = GuestLoadU32(heap.u32 + 40);
    const uint32_t region_start = GuestLoadU32(allocator.u32 + 0);
    const uint32_t region_end = GuestLoadU32(allocator.u32 + 4);
    const uint32_t upward_heap = GuestLoadU32(allocator.u32 + 8);
    const uint32_t downward_heap = GuestLoadU32(allocator.u32 + 12);
    const uint32_t min_available = GuestLoadU32(allocator.u32 + 16);
    const bool grows_down = direction.u32 != 0;
    const uint32_t available =
        grows_down ? cursor - limit.u32 : limit.u32 - cursor;
    const uint32_t overflow =
        request.u32 > available ? request.u32 - available : 0;

    REXLOG_WARN(
        "NX1 SP: suppressing nxheap.cpp:310 assert #{} caller=0x{:08X} "
        "heap='{}' heap=0x{:08X} allocator=0x{:08X} request=0x{:08X} "
        "available=0x{:08X} overflow=0x{:08X} dir={} heap_dir=0x{:08X} "
        "cursor=0x{:08X} candidate=0x{:08X} limit=0x{:08X} "
        "region=[0x{:08X},0x{:08X}) active=[0x{:08X},0x{:08X}] "
        "min_available=0x{:08X}",
        failure_count, caller.u32, ReadGuestCString(heap_name_addr, 32), heap.u32,
        allocator.u32, request.u32, available, overflow, grows_down ? "down" : "up",
        heap_direction, cursor, candidate.u32, limit.u32, region_start, region_end,
        upward_heap, downward_heap, min_available);
  }
  return true;
}

void rex_sp_trace_com_error_verbose(PPCRegister& r12, PPCRegister& r3,
                                    PPCRegister& r4, PPCRegister& r5,
                                    PPCRegister& r6) {
  REXLOG_WARN(
      "NX1 SP: Com_ErrorVerbose caller=0x{:08X} file='{}' line={} parm={} fmt='{}'",
      r12.u32, ReadGuestCString(r3.u32), r4.u32, r5.u32,
      ReadGuestCString(r6.u32));
}

void rex_sp_trace_massert_vargs(PPCRegister& r12, PPCRegister& r3,
                                PPCRegister& r4, PPCRegister& r5,
                                PPCRegister& r6, PPCRegister& r7,
                                PPCRegister& r8, PPCRegister& r9,
                                PPCRegister& r10) {
  REXLOG_WARN(
      "NX1 SP: MAssert caller=0x{:08X} file='{}' line={} kind=0x{:08X} arg=0x{:08X} fmt='{}' "
      "varargs=[0x{:08X},0x{:08X},0x{:08X}]",
      r12.u32, ReadGuestCString(r3.u32), r4.u32, r5.u32, r6.u32,
      ReadGuestCString(r7.u32), r8.u32, r9.u32, r10.u32);
}

bool ShouldSuppressSpVirtualMemoryMAssert(uint32_t file_addr, uint32_t line,
                                          uint32_t fmt_addr) {
  if (line != 173) {
    return false;
  }

  const std::string file_name = ReadGuestCString(file_addr, 160);
  if (file_name.find("virtualmemory.cpp") == std::string::npos) {
    return false;
  }

  const std::string fmt = ReadGuestCString(fmt_addr, 160);
  return fmt.find("System memory change was not what was expected") !=
         std::string::npos;
}

bool ShouldSuppressSpVirtualMemoryMAssert(PPCContext& ctx) {
  return ShouldSuppressSpVirtualMemoryMAssert(ctx.r3.u32, ctx.r4.u32,
                                              ctx.r6.u32);
}

bool ShouldSuppressSpVirtualMemoryMAssertVargs(PPCContext& ctx) {
  return ShouldSuppressSpVirtualMemoryMAssert(ctx.r3.u32, ctx.r4.u32,
                                              ctx.r7.u32);
}

void rex_sp_trace_sys_quit(PPCRegister& r12) {
  REXLOG_WARN("NX1 SP: Sys_Quit caller=0x{:08X}", r12.u32);
}

void rex_sp_trace_longjmp(PPCRegister& r0, PPCRegister& r3, PPCRegister& r4) {
  REXLOG_WARN("NX1 SP: longjmp caller=0x{:08X} buffer=0x{:08X} value=0x{:08X}",
              r0.u32, r3.u32, r4.u32);
}

extern "C" void NX1MAssert(PPCContext& ctx, uint8_t* base) {
  if (ShouldSuppressSpVirtualMemoryMAssert(ctx)) {
    REXLOG_WARN(
        "NX1 SP: suppressing virtualmemory.cpp:173 tracker assert diff={} "
        "caller=0x{:08X}",
        ctx.r8.s32, static_cast<uint32_t>(ctx.lr));
    return;
  }

  __imp__NX1MAssert(ctx, base);
}

extern "C" void NX1MAssertVargs(PPCContext& ctx, uint8_t* base) {
  if (ShouldSuppressSpVirtualMemoryMAssertVargs(ctx)) {
    REXLOG_WARN(
        "NX1 SP: suppressing virtualmemory.cpp:173 tracker varargs assert diff={} "
        "caller=0x{:08X}",
        ctx.r8.s32, static_cast<uint32_t>(ctx.lr));
    return;
  }

  __imp__NX1MAssertVargs(ctx, base);
}

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
constexpr uint32_t kRDisplayOutputScaledPtr = 0x855A137C;

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
  NX1Dvar_SetBool_YAXPBUdvar_t_N_Z(dvar_ctx, base);
}
}  // namespace

extern "C" void NX1R_SetWndParms_YAXPAUGfxWindowParms_Z(PPCContext& ctx, uint8_t* base) {
  if (!rex::cvar::Query<bool>("nx1_internal_resolution_patch")) {
    __imp__NX1R_SetWndParms_YAXPAUGfxWindowParms_Z(ctx, base);
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
        "NX1 SP: overriding R_SetWndParms internal resolution to {}x{} guest for {}x{} output",
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
