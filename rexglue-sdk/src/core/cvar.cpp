/**
 * @file        cvar.cpp
 * @brief       Configuration variable system implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <CLI/CLI.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <toml++/toml.hpp>

namespace rex::cvar {

namespace {

bool g_finalized = false;
bool g_lifecycle_override = false;
std::mutex g_mutex;

// Recursive: FlagRegistrar chain methods re-enter; change callbacks invoked
// from SetFlagByName must not mutate the registry.
std::recursive_mutex& GetRegistryMutex() {
  static std::recursive_mutex m;
  return m;
}

// Flag registry - use functions to avoid static init order issues
std::vector<FlagEntry>& GetRegistryStorage() {
  static std::vector<FlagEntry> registry;
  return registry;
}

std::unordered_map<std::string, size_t>& GetRegistryIndex() {
  static std::unordered_map<std::string, size_t> index;
  return index;
}

std::unordered_map<std::string, std::string>& GetCommandLineOverrides() {
  static std::unordered_map<std::string, std::string> overrides;
  return overrides;
}

std::unordered_map<std::string, std::string>& GetEnvironmentOverrides() {
  static std::unordered_map<std::string, std::string> overrides;
  return overrides;
}

// Convert flag name to environment variable: gpu_vsync -> REX_GPU_VSYNC
std::string FlagNameToEnvVar(std::string_view name) {
  std::string result = "REX_";
  for (char c : name) {
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

// Recursively apply TOML values
void ApplyTomlTable(const toml::table& table, const std::string& prefix) {
  for (const auto& [key, value] : table) {
    std::string full_key = prefix.empty() ? std::string(key) : prefix + "_" + std::string(key);

    if (value.is_table()) {
      ApplyTomlTable(*value.as_table(), full_key);
    } else {
      std::string value_str;
      if (value.is_boolean()) {
        value_str = value.as_boolean()->get() ? "true" : "false";
      } else if (value.is_integer()) {
        value_str = std::to_string(value.as_integer()->get());
      } else if (value.is_floating_point()) {
        value_str = std::to_string(value.as_floating_point()->get());
      } else if (value.is_string()) {
        value_str = value.as_string()->get();
      } else {
        REXLOG_WARN("Config: unsupported type for key '{}'", full_key);
        continue;
      }

      if (SetFlagByName(full_key, value_str)) {
        REXLOG_DEBUG("Config: {} = {}", full_key, value_str);
      } else {
        REXLOG_WARN("Config: unknown cvar '{}'", full_key);
      }
    }
  }
}

// todo(tomc): move restart manager to Runtime
std::vector<std::string>& GetPendingRestartStorage() {
  static std::vector<std::string> pending;
  return pending;
}

// Callback storage for change notifications
std::unordered_map<std::string, std::vector<ChangeCallback>>& GetCallbackStorage() {
  static std::unordered_map<std::string, std::vector<ChangeCallback>> callbacks;
  return callbacks;
}

void MarkPendingRestart(std::string_view name) {
  auto& pending = GetPendingRestartStorage();
  std::string name_str(name);
  if (std::find(pending.begin(), pending.end(), name_str) == pending.end()) {
    pending.push_back(name_str);
  }
}

bool ValidateConstraints(const FlagEntry& entry, std::string_view value) {
  const auto& c = entry.constraints;

  // Range validation for numeric types
  if (c.HasRangeConstraint()) {
    double numeric_val = 0;
    if (entry.type == FlagType::String || entry.type == FlagType::Boolean) {
      // These types don't have numeric range constraints
    } else if (entry.type == FlagType::Double) {
      if (!ParseDouble(value, numeric_val))
        return false;
    } else {
      // Integer types
      int64_t int_val = 0;
      auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), int_val);
      if (ec != std::errc())
        return false;
      numeric_val = static_cast<double>(int_val);
    }

    if (c.min.has_value() && numeric_val < *c.min) {
      REXLOG_WARN("Flag '{}': value {} below min ({})", entry.name, value, *c.min);
      return false;
    }
    if (c.max.has_value() && numeric_val > *c.max) {
      REXLOG_WARN("Flag '{}': value {} exceeds max ({})", entry.name, value, *c.max);
      return false;
    }
  }

  // Allowed values validation
  if (c.HasAllowedValues()) {
    bool found = false;
    for (const auto& allowed : c.allowed_values) {
      if (allowed == value) {
        found = true;
        break;
      }
    }
    if (!found) {
      REXLOG_WARN("Flag '{}': '{}' not in allowed values", entry.name, value);
      return false;
    }
  }

  // Custom validator
  if (c.custom_validator && !c.custom_validator(value)) {
    REXLOG_WARN("Flag '{}': custom validation failed for '{}'", entry.name, value);
    return false;
  }

  return true;
}

void ApplyStoredOverrides(const std::unordered_map<std::string, std::string>& overrides,
                          std::string_view source) {
  int count = 0;
  for (const auto& [name, value] : overrides) {
    if (SetFlagByName(name, value)) {
      REXLOG_DEBUG("{} override: {} = {}", source, name, value);
      ++count;
    } else {
      REXLOG_WARN("{} override: failed to parse {} = {}", source, name, value);
    }
  }

  if (count > 0) {
    REXLOG_INFO("Re-applied {} {} override(s)", count, source);
  }
}

const char* FlagTypeName(FlagType type) {
  switch (type) {
    case FlagType::Boolean:
      return "bool";
    case FlagType::Int32:
      return "int32";
    case FlagType::Int64:
      return "int64";
    case FlagType::Uint32:
      return "uint32";
    case FlagType::Uint64:
      return "uint64";
    case FlagType::Double:
      return "double";
    case FlagType::String:
      return "string";
    case FlagType::Command:
      return "command";
  }
  return "unknown";
}

const char* LifecycleName(Lifecycle lifecycle) {
  switch (lifecycle) {
    case Lifecycle::kInitOnly:
      return "init-only";
    case Lifecycle::kHotReload:
      return "hot-reload";
    case Lifecycle::kRequiresRestart:
      return "requires-restart";
  }
  return "unknown";
}

std::string EscapeTomlString(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  constexpr char kHex[] = "0123456789ABCDEF";

  for (unsigned char ch : value) {
    switch (ch) {
      case '\b':
        escaped += "\\b";
        break;
      case '\t':
        escaped += "\\t";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      default:
        if (ch < 0x20) {
          escaped += "\\u00";
          escaped += kHex[(ch >> 4) & 0x0F];
          escaped += kHex[ch & 0x0F];
        } else {
          escaped.push_back(static_cast<char>(ch));
        }
        break;
    }
  }

  return escaped;
}

std::string FormatTomlValue(const FlagEntry& entry, std::string_view value) {
  if (entry.type == FlagType::String) {
    return "\"" + EscapeTomlString(value) + "\"";
  }
  return std::string(value);
}

std::string FormatCommentNumber(double value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

void AppendCommentLine(std::string& result, std::string_view text) {
  if (text.empty()) {
    return;
  }

  result += "# ";
  for (char ch : text) {
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      result += "\n# ";
    } else {
      result.push_back(ch);
    }
  }
  result += "\n";
}

void AppendCvarTomlEntry(std::string& result, const FlagEntry& entry, bool with_comments) {
  if (with_comments) {
    AppendCommentLine(result, entry.description);
    result += "# Type: ";
    result += FlagTypeName(entry.type);
    result += "; Default: ";
    result += FormatTomlValue(entry, entry.default_value);
    result += "; Lifecycle: ";
    result += LifecycleName(entry.lifecycle);
    result += "\n";

    if (entry.constraints.HasRangeConstraint()) {
      result += "# Range: ";
      result += entry.constraints.min ? FormatCommentNumber(*entry.constraints.min) : "-inf";
      result += "..";
      result += entry.constraints.max ? FormatCommentNumber(*entry.constraints.max) : "+inf";
      result += "\n";
    }

    if (entry.constraints.HasAllowedValues()) {
      result += "# Allowed: ";
      for (size_t i = 0; i < entry.constraints.allowed_values.size(); ++i) {
        if (i != 0) {
          result += ", ";
        }
        result += "\"";
        result += EscapeTomlString(entry.constraints.allowed_values[i]);
        result += "\"";
      }
      result += "\n";
    }

    if (entry.is_debug_only) {
      result += "# Debug-only: true\n";
    }
  }

  result += entry.name;
  result += " = ";
  result += FormatTomlValue(entry, entry.getter());
  result += "\n";
}

std::string SerializeRegistryToTOML(std::string_view category, bool filter_category,
                                    bool include_defaults, bool with_comments) {
  std::vector<const FlagEntry*> entries;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.type == FlagType::Command) {
      continue;
    }
    if (filter_category && entry.category != category) {
      continue;
    }
    if (!include_defaults && entry.getter() == entry.default_value) {
      continue;
    }
    entries.push_back(&entry);
  }

  if (with_comments) {
    std::stable_sort(entries.begin(), entries.end(), [](const FlagEntry* lhs, const FlagEntry* rhs) {
      if (lhs->category != rhs->category) {
        return lhs->category < rhs->category;
      }
      return lhs->name < rhs->name;
    });
  }

  std::string result;
  std::string current_category;
  for (const auto* entry : entries) {
    if (with_comments && entry->category != current_category) {
      if (!result.empty()) {
        result += "\n";
      }
      current_category = entry->category;
      result += "# =============================================================================\n";
      result += "# ";
      result += current_category.empty() ? std::string("Uncategorized") : current_category;
      result += "\n";
      result += "# =============================================================================\n\n";
    }

    AppendCvarTomlEntry(result, *entry, with_comments);
    if (with_comments) {
      result += "\n";
    }
  }

  return result;
}

}  // namespace

//=============================================================================
// Registry
//=============================================================================

std::vector<FlagEntry>& GetRegistry() {
  return GetRegistryStorage();
}

std::optional<size_t> RegisterFlag(FlagEntry entry) {
  std::lock_guard lock(GetRegistryMutex());
  (void)GetCallbackStorage();
  (void)GetPendingRestartStorage();
  auto& index = GetRegistryIndex();
  auto& storage = GetRegistryStorage();
  auto it = index.find(entry.name);
  if (it != index.end()) {
    REXLOG_ERROR("cvar: duplicate registration of '{}'; second registration ignored", entry.name);
    return std::nullopt;
  }
  size_t pos = storage.size();
  index[entry.name] = pos;
  storage.push_back(std::move(entry));
  return pos;
}

void UnregisterFlag(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto& index = GetRegistryIndex();
  auto& storage = GetRegistryStorage();
  std::string key(name);
  auto idx_it = index.find(key);
  if (idx_it == index.end()) {
    return;
  }
  size_t pos = idx_it->second;
  index.erase(idx_it);
  storage.erase(storage.begin() + pos);
  for (auto& [n, i] : index) {
    if (i > pos) {
      --i;
    }
  }
  GetCallbackStorage().erase(key);
  auto& pending = GetPendingRestartStorage();
  pending.erase(std::remove(pending.begin(), pending.end(), key), pending.end());
}

void FlagRegistrar::apply_(std::function<void(FlagEntry&)> fn) {
  if (owned_name_.empty()) {
    return;
  }
  std::lock_guard lock(GetRegistryMutex());
  auto& index = GetRegistryIndex();
  auto it = index.find(owned_name_);
  if (it == index.end()) {
    return;
  }
  fn(GetRegistryStorage()[it->second]);
}

bool SetFlagByName(std::string_view name, std::string_view value) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return false;
  }

  const auto& entry = GetRegistryStorage()[it->second];

  // Check lifecycle
  if (!g_lifecycle_override && entry.lifecycle == Lifecycle::kInitOnly && IsFinalized()) {
    REXLOG_WARN("Cannot modify init-only flag '{}' after initialization", name);
    return false;
  }

  // Validate constraints
  if (!ValidateConstraints(entry, value)) {
    return false;
  }

  bool success = entry.setter(value);

  // Track pending restart flags
  if (success && entry.lifecycle == Lifecycle::kRequiresRestart) {
    MarkPendingRestart(name);
  }

  // Invoke registered callbacks
  if (success) {
    auto& callbacks = GetCallbackStorage();
    auto it = callbacks.find(std::string(name));
    if (it != callbacks.end()) {
      for (const auto& callback : it->second) {
        callback(name, value);
      }
    }
  }

  return success;
}

std::string GetFlagByName(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return "";
  }

  return GetRegistryStorage()[it->second].getter();
}

std::vector<std::string> ListFlags() {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  result.reserve(GetRegistryStorage().size());
  for (const auto& entry : GetRegistryStorage()) {
    result.push_back(entry.name);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> ListFlagsByCategory(std::string_view category) {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.category == category) {
      result.push_back(entry.name);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> ListFlagsByLifecycle(Lifecycle lc) {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.lifecycle == lc) {
      result.push_back(entry.name);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

const FlagEntry* GetFlagInfo(std::string_view name) {
  // Pointer is invalidated by any subsequent registry call.
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return nullptr;
  }
  return &GetRegistryStorage()[it->second];
}

template <>
bool Query<bool>(std::string_view name) {
  std::string v = GetFlagByName(name);
  return v == "true" || v == "1" || v == "yes";
}

template <>
int32_t Query<int32_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  int32_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
int64_t Query<int64_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  int64_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
uint32_t Query<uint32_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  uint32_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
uint64_t Query<uint64_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  uint64_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
double Query<double>(std::string_view name) {
  std::string v = GetFlagByName(name);
  double out = 0.0;
  ParseDouble(v, out);
  return out;
}

template <>
std::string Query<std::string>(std::string_view name) {
  return GetFlagByName(name);
}

std::vector<std::string> GetPendingRestartFlags() {
  std::lock_guard lock(GetRegistryMutex());
  return GetPendingRestartStorage();
}

void ClearPendingRestartFlags() {
  std::lock_guard lock(GetRegistryMutex());
  GetPendingRestartStorage().clear();
}

void ResetToDefault(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return;
  }
  const auto& entry = GetRegistryStorage()[it->second];
  entry.setter(entry.default_value);
}

void ResetAllToDefaults() {
  std::lock_guard lock(GetRegistryMutex());
  for (const auto& entry : GetRegistryStorage()) {
    entry.setter(entry.default_value);
  }
}

bool HasNonDefaultValue(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return false;
  }
  const auto& entry = GetRegistryStorage()[it->second];
  return entry.getter() != entry.default_value;
}

std::vector<std::string> ListModifiedFlags() {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.getter() != entry.default_value) {
      result.push_back(entry.name);
    }
  }
  return result;
}

std::string SerializeToTOML() {
  std::lock_guard lock(GetRegistryMutex());
  return SerializeRegistryToTOML("", false, false, false);
}

std::string SerializeToTOML(std::string_view category) {
  std::lock_guard lock(GetRegistryMutex());
  return SerializeRegistryToTOML(category, true, false, false);
}

std::string SerializeAllToTOML() {
  std::lock_guard lock(GetRegistryMutex());
  return SerializeRegistryToTOML("", false, true, true);
}

void RegisterChangeCallback(std::string_view name, ChangeCallback callback) {
  std::lock_guard lock(GetRegistryMutex());
  GetCallbackStorage()[std::string(name)].push_back(std::move(callback));
}

void UnregisterChangeCallbacks(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  GetCallbackStorage().erase(std::string(name));
}

//=============================================================================
// Initialization
//=============================================================================

std::vector<std::string> Init(int argc, char** argv) {
  CLI::App app{"", ""};
  app.allow_extras();

  for (auto& entry : GetRegistryStorage()) {
    if (entry.type == FlagType::Boolean) {
      app.add_flag_function(
          "--" + entry.name + ",!--no-" + entry.name,
          [&entry](int64_t count) {
            const char* value = count > 0 ? "true" : "false";
            if (entry.setter(value)) {
              GetCommandLineOverrides()[entry.name] = value;
            }
          },
          entry.description);
    } else {
      app.add_option_function<std::string>(
          "--" + entry.name,
          [&entry](const std::string& val) {
            if (entry.setter(val)) {
              GetCommandLineOverrides()[entry.name] = val;
            }
          },
          entry.description);
    }
  }

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    // TODO(tomc): dumb workaround for the stupid chicken and its egg.
    //             dont call rex logging funcs here for now.
    fprintf(stderr, "cvar: CLI11  parse error: %s\n", e.what());
  }

  return app.remaining();
}

void LoadConfig(const std::filesystem::path& config_path) {
  if (!std::filesystem::exists(config_path)) {
    REXLOG_DEBUG("Config file not found: {}", config_path.string());
    return;
  }

  try {
    auto config = toml::parse_file(config_path.string());
    ApplyTomlTable(config, "");
    ApplyStoredOverrides(GetEnvironmentOverrides(), "environment");
    ApplyStoredOverrides(GetCommandLineOverrides(), "command-line");
    REXLOG_INFO("Loaded config from {}", config_path.string());
  } catch (const toml::parse_error& err) {
    REXLOG_ERROR("Failed to parse config {}: {}", config_path.string(), err.what());
  }
}

void ApplyEnvironment() {
  int count = 0;
  for (const auto& entry : GetRegistryStorage()) {
    std::string env_name = FlagNameToEnvVar(entry.name);
    const char* env_value = std::getenv(env_name.c_str());
    if (env_value != nullptr) {
      if (entry.setter(env_value)) {
        GetEnvironmentOverrides()[entry.name] = env_value;
        REXLOG_DEBUG("Env: {} = {} (from {})", entry.name, env_value, env_name);
        ++count;
      } else {
        REXLOG_WARN("Env: failed to parse {} = {}", env_name, env_value);
      }
    }
  }

  if (count > 0) {
    REXLOG_INFO("Applied {} environment variable override(s)", count);
  }
}

void FinalizeInit() {
  std::lock_guard lock(g_mutex);
  g_finalized = true;
  REXLOG_DEBUG("cvar: initialization finalized");
}

bool IsFinalized() {
  return g_finalized;
}

void SaveConfig(const std::filesystem::path& config_path) {
  std::string content = SerializeAllToTOML();
  if (content.empty()) {
    REXLOG_DEBUG("SaveConfig: no flags to save");
    return;
  }

  try {
    if (auto parent_path = config_path.parent_path(); !parent_path.empty()) {
      std::filesystem::create_directories(parent_path);
    }

    std::ofstream file(config_path);
    if (!file) {
      REXLOG_ERROR("SaveConfig: failed to open {}", config_path.string());
      return;
    }
    file << "# ReXGlue runtime configuration\n";
    file << "# Edit this file while the game is closed, then restart to apply init-only and\n";
    file << "# restart-required options. Hot-reload options can also be changed in-game.\n\n";
    file << content;
    REXLOG_INFO("Saved config to {}", config_path.string());
  } catch (const std::exception& e) {
    REXLOG_ERROR("SaveConfig: {}", e.what());
  }
}

namespace testing {

ScopedLifecycleOverride::ScopedLifecycleOverride() {
  g_lifecycle_override = true;
}

ScopedLifecycleOverride::~ScopedLifecycleOverride() {
  g_lifecycle_override = false;
}

void ResetAllForTesting() {
  ResetAllToDefaults();
  ClearPendingRestartFlags();
  GetCommandLineOverrides().clear();
  GetEnvironmentOverrides().clear();
  g_finalized = false;
}

}  // namespace testing

}  // namespace rex::cvar
