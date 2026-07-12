#include <rex/system/discord_presence.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>

#if REX_HAS_DISCORD_SOCIAL_SDK && REX_PLATFORM_WIN32
#ifndef DISCORD_API
#define DISCORD_API __declspec(dllimport)
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <delayimp.h>
#include <cdiscord.h>
#endif

#if REX_HAS_DISCORD_SOCIAL_SDK && REX_PLATFORM_WIN32
namespace {

constexpr wchar_t kDiscordSdkResourceName[] = L"DISCORD_PARTNER_SDK_DLL";
constexpr wchar_t kDiscordSdkCacheDirectory[] = L"NX1DiscordSdk";
constexpr wchar_t kDiscordSdkCachedDllPrefix[] = L"discord_partner_sdk_";
constexpr char kDiscordSdkDllName[] = "discord_partner_sdk.dll";

bool SameAsciiNameInsensitive(const char* lhs, const char* rhs) {
  if (!lhs || !rhs) {
    return false;
  }
  while (*lhs && *rhs) {
    const auto left =
        static_cast<char>(std::tolower(static_cast<unsigned char>(*lhs)));
    const auto right =
        static_cast<char>(std::tolower(static_cast<unsigned char>(*rhs)));
    if (left != right) {
      return false;
    }
    ++lhs;
    ++rhs;
  }
  return *lhs == '\0' && *rhs == '\0';
}

std::wstring BuildDiscordSdkCachePath(uint32_t resource_size) {
  wchar_t temp_path[MAX_PATH] = {};
  const DWORD temp_path_length = GetTempPathW(MAX_PATH, temp_path);
  if (!temp_path_length || temp_path_length >= MAX_PATH) {
    return {};
  }

  std::wstring directory(temp_path, temp_path_length);
  if (!directory.empty() && directory.back() != L'\\' && directory.back() != L'/') {
    directory.push_back(L'\\');
  }
  directory += kDiscordSdkCacheDirectory;
  CreateDirectoryW(directory.c_str(), nullptr);

  std::wstring path = directory;
  path.push_back(L'\\');
  path += kDiscordSdkCachedDllPrefix;
  path += std::to_wstring(resource_size);
  path += L".dll";
  return path;
}

bool CachedDiscordSdkMatches(const std::wstring& path, uint32_t resource_size) {
  WIN32_FILE_ATTRIBUTE_DATA attributes = {};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
    return false;
  }

  const uint64_t file_size =
      (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) |
      static_cast<uint64_t>(attributes.nFileSizeLow);
  return file_size == resource_size;
}

bool WriteEmbeddedDiscordSdk(const std::wstring& path, const void* data,
                             uint32_t resource_size) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }

  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t total_written = 0;
  while (total_written < resource_size) {
    DWORD written = 0;
    const uint32_t remaining = resource_size - total_written;
    const DWORD chunk = static_cast<DWORD>(std::min<uint32_t>(remaining, 1u << 20));
    if (!WriteFile(file, bytes + total_written, chunk, &written, nullptr) || !written) {
      CloseHandle(file);
      DeleteFileW(path.c_str());
      return false;
    }
    total_written += written;
  }

  CloseHandle(file);
  return true;
}

HMODULE LoadEmbeddedDiscordSdk() {
  static HMODULE loaded_module = nullptr;
  if (loaded_module) {
    return loaded_module;
  }

  HMODULE executable = GetModuleHandleW(nullptr);
  HRSRC resource =
      FindResourceW(executable, kDiscordSdkResourceName, MAKEINTRESOURCEW(10));
  if (!resource) {
    return nullptr;
  }

  const DWORD resource_size = SizeofResource(executable, resource);
  HGLOBAL loaded_resource = LoadResource(executable, resource);
  const void* resource_data = loaded_resource ? LockResource(loaded_resource) : nullptr;
  if (!resource_size || !resource_data) {
    return nullptr;
  }

  const std::wstring cache_path = BuildDiscordSdkCachePath(resource_size);
  if (cache_path.empty()) {
    return nullptr;
  }

  if (!CachedDiscordSdkMatches(cache_path, resource_size) &&
      !WriteEmbeddedDiscordSdk(cache_path, resource_data, resource_size)) {
    REXSYS_WARN("Discord presence could not extract embedded Social SDK DLL");
    return nullptr;
  }

  loaded_module = LoadLibraryW(cache_path.c_str());
  if (!loaded_module) {
    REXSYS_WARN("Discord presence could not load embedded Social SDK DLL ({})",
                GetLastError());
    return nullptr;
  }

  REXSYS_INFO("Discord presence loaded embedded Social SDK DLL");
  return loaded_module;
}

}  // namespace

extern "C" FARPROC WINAPI RexDiscordDelayLoadHook(unsigned dli_notify,
                                                  PDelayLoadInfo delay_load_info) {
  if (dli_notify != dliNotePreLoadLibrary || !delay_load_info ||
      !SameAsciiNameInsensitive(delay_load_info->szDll, kDiscordSdkDllName)) {
    return nullptr;
  }

  return reinterpret_cast<FARPROC>(LoadEmbeddedDiscordSdk());
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = RexDiscordDelayLoadHook;
#endif

REXCVAR_DEFINE_BOOL(discord_presence_enabled, true, "Discord",
                    "Enable Discord Social SDK Rich Presence");
REXCVAR_DEFINE_UINT64(discord_presence_app_id, 1511484218331627682ULL, "Discord",
                      "Discord application ID for Rich Presence");
REXCVAR_DEFINE_UINT32(discord_presence_update_interval_ms, 15000, "Discord",
                      "Minimum interval for identical Discord Rich Presence refreshes")
    .range(1000, 60000);
REXCVAR_DEFINE_UINT32(discord_presence_default_max_players, 18, "Discord",
                      "Fallback Discord Rich Presence lobby capacity before session metadata arrives")
    .range(1, 64);
REXCVAR_DEFINE_STRING(discord_presence_large_image, "", "Discord",
                      "Optional Discord Rich Presence large image asset key");
REXCVAR_DEFINE_BOOL(discord_presence_log, false, "Discord",
                    "Log Discord Rich Presence status and update callbacks");

namespace rex::system {

namespace {

constexpr std::string_view kGameName = "Call of Duty: Future Warfare";

std::string Trim(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
    return std::isspace(c) != 0;
  }).base();
  return first >= last ? std::string() : std::string(first, last);
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

std::string NormalizeMapName(std::string map_name) {
  map_name = Trim(std::move(map_name));
  if (map_name.empty()) {
    return {};
  }

  const size_t last_separator = map_name.find_last_of("/\\");
  if (last_separator != std::string::npos) {
    map_name.erase(0, last_separator + 1);
  }

  const size_t extension = map_name.find_last_of('.');
  if (extension != std::string::npos) {
    map_name.erase(extension);
  }

  std::transform(map_name.begin(), map_name.end(), map_name.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (StartsWith(map_name, "localized_")) {
    map_name.erase(0, 10);
  } else if (StartsWith(map_name, "patch_")) {
    map_name.erase(0, 6);
  }
  if (EndsWith(map_name, "_load")) {
    map_name.erase(map_name.size() - 5);
  }
  return map_name;
}

std::string_view CampaignMissionName(std::string_view map_name) {
  static const std::unordered_map<std::string_view, std::string_view> kCampaignMaps = {
      {"nx_border", "Border"},
      {"nx_hospital", "Visiting Hours"},
      {"nx_exfil", "Into the Fire"},
      {"nx_rocket", "Winter Anvil"},
      {"nx_lava", "Descent"},
      {"nx_hithard", "Hit Hard at Home"},
      {"nx_hithard_b", "Hit Hard at Home"},
      {"nx_lunar", "Moonbase Assault"},
      {"nx_ss_rappel", "Operation Deadbolt"},
      {"nx_skyscraper", "Operation Deadbolt"},
  };

  if (const auto it = kCampaignMaps.find(map_name); it != kCampaignMaps.end()) {
    return it->second;
  }
  return {};
}

std::string TitleCaseToken(std::string_view token) {
  std::string result;
  result.reserve(token.size());
  bool next_upper = true;
  for (char c : token) {
    if (c == '_' || c == '-' || c == ' ') {
      if (!result.empty() && result.back() != ' ') {
        result.push_back(' ');
      }
      next_upper = true;
      continue;
    }
    unsigned char uc = static_cast<unsigned char>(c);
    result.push_back(next_upper ? static_cast<char>(std::toupper(uc))
                                : static_cast<char>(std::tolower(uc)));
    next_upper = false;
  }
  return result;
}

std::string PrettyMapName(std::string map_name) {
  map_name = NormalizeMapName(std::move(map_name));
  if (map_name.empty()) {
    return {};
  }

  if (const std::string_view mission_name = CampaignMissionName(map_name);
      !mission_name.empty()) {
    return std::string(mission_name);
  }

  static const std::unordered_map<std::string_view, std::string_view> kKnownMaps = {
      {"mp_nx_asylum_2", "Asylum"},
      {"mp_nx_blank1", "Blank"},
      {"mp_nx_bom", "BOM"},
      {"mp_nx_border", "Border"},
      {"mp_nx_contact", "Contact"},
      {"mp_nx_deadzone", "Deadzone"},
      {"mp_nx_fallout", "Fallout"},
      {"mp_nx_galleria", "Galleria"},
      {"mp_nx_import", "Import"},
      {"mp_nx_leg_crash", "Crash"},
      {"mp_nx_leg_over", "Overgrown"},
      {"mp_nx_leg_term", "Terminal"},
      {"mp_nx_lockdown", "Lockdown"},
      {"mp_nx_lockdown_v2", "Lockdown"},
      {"mp_nx_meteor", "Meteor"},
      {"mp_nx_monorail", "Monorail"},
      {"mp_nx_pitstop", "Pit Stop"},
      {"mp_nx_sandstorm", "Sandstorm"},
      {"mp_nx_seaport", "Seaport"},
      {"mp_nx_shipyard", "Shipyard"},
      {"mp_nx_skylab", "Skylab"},
      {"mp_nx_stasis", "Stasis"},
      {"mp_nx_streets", "Streets"},
      {"mp_nx_subyard", "Sub Yard"},
      {"mp_nx_ugvcontact", "UGV Contact"},
      {"mp_nx_ugvhh", "UGV Hangar"},
      {"mp_nx_ugvsand", "UGV Sand"},
      {"mp_nx_whiteout", "Whiteout"},
  };

  if (const auto it = kKnownMaps.find(map_name); it != kKnownMaps.end()) {
    return std::string(it->second);
  }

  std::string cleaned = map_name;
  if (StartsWith(cleaned, "mp_nx_")) {
    cleaned.erase(0, 6);
  } else if (StartsWith(cleaned, "mp_")) {
    cleaned.erase(0, 3);
  } else if (StartsWith(cleaned, "nx_")) {
    cleaned.erase(0, 3);
  }
  return TitleCaseToken(cleaned);
}

std::string PrettyGameType(std::string game_type) {
  game_type = Trim(std::move(game_type));
  if (game_type.empty()) {
    return {};
  }

  static const std::unordered_map<std::string_view, std::string_view> kKnownGameTypes = {
      {"war", "Team Deathmatch"},
      {"dm", "Free-for-All"},
      {"dom", "Domination"},
      {"sd", "Search and Destroy"},
      {"sab", "Sabotage"},
      {"koth", "Headquarters"},
      {"ctf", "Capture the Flag"},
  };

  if (const auto it = kKnownGameTypes.find(game_type); it != kKnownGameTypes.end()) {
    return std::string(it->second);
  }
  return TitleCaseToken(game_type);
}

}  // namespace

#if REX_HAS_DISCORD_SOCIAL_SDK && REX_PLATFORM_WIN32

namespace {

using Clock = std::chrono::steady_clock;

Discord_String DiscordString(std::string_view value) {
  return Discord_String{reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())),
                        value.size()};
}

std::string TakeDiscordString(Discord_String value) {
  std::string result;
  if (value.ptr && value.size) {
    result.assign(reinterpret_cast<const char*>(value.ptr), value.size);
  }
  if (value.ptr) {
    Discord_Free(value.ptr);
  }
  return result;
}

struct PresenceState {
  std::mutex mutex;
  Discord_Client client = {};
  bool initialized = false;
  std::atomic_bool callback_thread_stop{false};
  std::thread callback_thread;
  Clock::time_point last_publish = Clock::time_point::min();
  std::string last_signature;

  uint64_t session_id = 0;
  std::string map_name;
  std::string game_type;
  uint32_t players = 0;
  uint32_t max_players = 0;
  bool in_game = false;
};

PresenceState g_presence;

void PublishLocked(PresenceState& state, bool force);

void OnRichPresenceUpdated(Discord_ClientResult* result, void*) {
  if (!result) {
    return;
  }

  const bool successful = Discord_ClientResult_Successful(result);
  if (!successful || REXCVAR_GET(discord_presence_log)) {
    Discord_String error = {};
    Discord_ClientResult_Error(result, &error);
    REXSYS_INFO("Discord presence update {}", successful ? "succeeded" : "failed");
    const std::string error_text = TakeDiscordString(error);
    if (!successful && !error_text.empty()) {
      REXSYS_WARN("Discord presence update error: {}", error_text);
    }
  }
  Discord_ClientResult_Drop(result);
}

bool EnsureInitializedLocked(PresenceState& state) {
  if (!REXCVAR_GET(discord_presence_enabled)) {
    return false;
  }
  if (state.initialized) {
    return true;
  }

  const uint64_t app_id = REXCVAR_GET(discord_presence_app_id);
  if (!app_id) {
    REXSYS_WARN("Discord presence disabled: discord_presence_app_id is 0");
    return false;
  }

  Discord_SetFreeThreaded();
  Discord_Client_Init(&state.client);
  Discord_Client_SetApplicationId(&state.client, app_id);

  state.callback_thread_stop = false;
  state.callback_thread = std::thread([&state]() {
    while (!state.callback_thread_stop.load(std::memory_order_acquire)) {
      Discord_RunCallbacks();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  state.initialized = true;
  REXSYS_INFO("Discord presence initialized for app {}", app_id);
  return true;
}

std::string BuildSignature(const PresenceState& state, const std::string& details,
                           const std::string& status, const std::string& image) {
  return fmt::format("{}\n{}\n{}\n{:016X}\n{}\n{}", details, status, image,
                     state.session_id, state.players, state.max_players);
}

uint32_t EffectiveMaxPlayers(const PresenceState& state) {
  if (state.max_players) {
    return state.max_players;
  }
  return state.in_game ? REXCVAR_GET(discord_presence_default_max_players) : 0;
}

uint32_t EffectivePlayers(const PresenceState& state, uint32_t max_players) {
  uint32_t players = state.players;
  if (!players && state.in_game) {
    players = 1;
  }
  if (max_players) {
    players = std::clamp<uint32_t>(players, 1, max_players);
  }
  return players;
}

void PublishLocked(PresenceState& state, bool force) {
  if (!EnsureInitializedLocked(state)) {
    return;
  }

  const std::string pretty_map = PrettyMapName(state.map_name);
  const std::string pretty_game_type = PrettyGameType(state.game_type);
  const std::string normalized_map = NormalizeMapName(state.map_name);
  const bool campaign_map = !CampaignMissionName(normalized_map).empty();

  std::string details;
  std::string status;
  uint32_t effective_max_players = 0;
  uint32_t effective_players = 0;
  if (campaign_map) {
    details = "Playing Campaign";
    status = pretty_map;
  } else {
    details = state.in_game && !pretty_map.empty() ? fmt::format("Map: {}", pretty_map)
                                                   : "In Menus";
    effective_max_players = EffectiveMaxPlayers(state);
    effective_players = EffectivePlayers(state, effective_max_players);
    if (effective_max_players) {
      const std::string mode = pretty_game_type.empty() ? "Playing" : pretty_game_type;
      status = fmt::format("{} - {}/{} players", mode, effective_players,
                           effective_max_players);
    } else {
      status = state.in_game ? "Playing" : "Browsing menus";
    }
  }

  const std::string large_image = Trim(REXCVAR_GET(discord_presence_large_image));
  const std::string signature = BuildSignature(state, details, status, large_image);
  const auto now = Clock::now();
  const auto min_interval =
      std::chrono::milliseconds(REXCVAR_GET(discord_presence_update_interval_ms));
  if (!force && signature == state.last_signature && now - state.last_publish < min_interval) {
    return;
  }

  Discord_Activity activity = {};
  Discord_Activity_Init(&activity);

  uint64_t app_id = REXCVAR_GET(discord_presence_app_id);
  Discord_Activity_SetApplicationId(&activity, &app_id);
  Discord_Activity_SetName(&activity, DiscordString(kGameName));
  Discord_Activity_SetType(&activity, Discord_ActivityTypes_Playing);
  Discord_Activity_SetSupportedPlatforms(&activity, Discord_ActivityGamePlatforms_Desktop);

  Discord_StatusDisplayTypes display_type =
      campaign_map ? Discord_StatusDisplayTypes_Details : Discord_StatusDisplayTypes_State;
  Discord_Activity_SetStatusDisplayType(&activity, &display_type);

  Discord_String details_string = DiscordString(details);
  Discord_Activity_SetDetails(&activity, &details_string);
  Discord_String state_string = DiscordString(status);
  Discord_Activity_SetState(&activity, &state_string);

  Discord_ActivityAssets assets = {};
  bool has_assets = false;
  if (!large_image.empty()) {
    Discord_ActivityAssets_Init(&assets);
    Discord_String large_image_string = DiscordString(large_image);
    Discord_ActivityAssets_SetLargeImage(&assets, &large_image_string);
    Discord_String large_text_string = DiscordString(kGameName);
    Discord_ActivityAssets_SetLargeText(&assets, &large_text_string);
    Discord_Activity_SetAssets(&activity, &assets);
    has_assets = true;
  }

  Discord_ActivityParty party = {};
  bool has_party = false;
  if (effective_max_players) {
    Discord_ActivityParty_Init(&party);
    const std::string party_id =
        state.session_id ? fmt::format("nx1-{:016X}", state.session_id) : "nx1-session";
    Discord_ActivityParty_SetId(&party, DiscordString(party_id));
    Discord_ActivityParty_SetCurrentSize(&party, static_cast<int32_t>(effective_players));
    Discord_ActivityParty_SetMaxSize(&party, static_cast<int32_t>(effective_max_players));
    Discord_ActivityParty_SetPrivacy(&party, Discord_ActivityPartyPrivacy_Public);
    Discord_Activity_SetParty(&activity, &party);
    has_party = true;
  }

  Discord_Client_UpdateRichPresence(&state.client, &activity, OnRichPresenceUpdated, nullptr,
                                    nullptr);

  if (has_party) {
    Discord_ActivityParty_Drop(&party);
  }
  if (has_assets) {
    Discord_ActivityAssets_Drop(&assets);
  }
  Discord_Activity_Drop(&activity);

  state.last_publish = now;
  state.last_signature = signature;

  if (REXCVAR_GET(discord_presence_log)) {
    REXSYS_INFO("Discord presence published: details='{}' state='{}'", details, status);
  }
}

}  // namespace

DiscordPresenceClient::DiscordPresenceClient() = default;

DiscordPresenceClient::~DiscordPresenceClient() {
  Shutdown();
}

DiscordPresenceClient& DiscordPresenceClient::Get() {
  static DiscordPresenceClient client;
  return client;
}

void DiscordPresenceClient::SetMenuState(KernelState*) {
  std::lock_guard lock(g_presence.mutex);
  g_presence.in_game = false;
  g_presence.session_id = 0;
  g_presence.map_name.clear();
  g_presence.game_type.clear();
  g_presence.players = 0;
  g_presence.max_players = 0;
  PublishLocked(g_presence, true);
}

void DiscordPresenceClient::NoteLoadedMap(KernelState*, std::string map_name) {
  map_name = Trim(std::move(map_name));
  if (map_name.empty()) {
    return;
  }

  std::lock_guard lock(g_presence.mutex);
  g_presence.in_game = true;
  g_presence.map_name = std::move(map_name);
  PublishLocked(g_presence, false);
}

void DiscordPresenceClient::UpdateSessionInfo(KernelState*, uint64_t session_id,
                                              std::string map_name,
                                              std::string game_type, uint32_t players,
                                              uint32_t max_players) {
  std::lock_guard lock(g_presence.mutex);
  g_presence.in_game = true;
  g_presence.session_id = session_id;
  if (!Trim(map_name).empty()) {
    g_presence.map_name = Trim(std::move(map_name));
  }
  if (!Trim(game_type).empty()) {
    g_presence.game_type = Trim(std::move(game_type));
  }
  if (players) {
    g_presence.players = players;
  }
  if (max_players) {
    g_presence.max_players = max_players;
  }
  PublishLocked(g_presence, false);
}

void DiscordPresenceClient::Shutdown() {
  std::thread callback_thread;
  {
    std::lock_guard lock(g_presence.mutex);
    if (!g_presence.initialized) {
      return;
    }
    g_presence.callback_thread_stop.store(true, std::memory_order_release);
    callback_thread = std::move(g_presence.callback_thread);
  }

  if (callback_thread.joinable()) {
    callback_thread.join();
  }

  std::lock_guard lock(g_presence.mutex);
  Discord_RunCallbacks();
  Discord_Client_Drop(&g_presence.client);
  Discord_ResetCallbacks();
  g_presence.client = {};
  g_presence.initialized = false;
  g_presence.last_signature.clear();
  REXSYS_INFO("Discord presence shut down");
}

#else

DiscordPresenceClient::DiscordPresenceClient() = default;
DiscordPresenceClient::~DiscordPresenceClient() = default;

DiscordPresenceClient& DiscordPresenceClient::Get() {
  static DiscordPresenceClient client;
  return client;
}

void DiscordPresenceClient::SetMenuState(KernelState*) {}
void DiscordPresenceClient::NoteLoadedMap(KernelState*, std::string) {}
void DiscordPresenceClient::UpdateSessionInfo(KernelState*, uint64_t, std::string,
                                              std::string, uint32_t, uint32_t) {}
void DiscordPresenceClient::Shutdown() {}

#endif

}  // namespace rex::system
