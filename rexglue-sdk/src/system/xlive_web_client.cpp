#include <rex/system/xlive_web_client.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/system/discord_presence.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xex_module.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#include <winhttp.h>
#endif

REXCVAR_DEFINE_BOOL(xlive_web_enabled, true, "XLive",
                    "Enable the ReXGlue XLive web service client");
REXCVAR_DEFINE_BOOL(xlive_web_log_requests, false, "XLive",
                    "Log XLive web service registration and session requests");
REXCVAR_DEFINE_STRING(xlive_web_api_address,
                      "https://xenia-netplay-2a0298c0e3f4.herokuapp.com/", "XLive",
                      "XLive web service base address");
REXCVAR_DEFINE_UINT32(xlive_web_timeout_ms, 2000, "XLive",
                      "XLive web service request timeout in milliseconds")
    .range(100, 10000);
REXCVAR_DEFINE_BOOL(xlive_web_probe_on_startup, true, "XLive",
                    "Probe/register with the XLive web service during startup");
REXCVAR_DEFINE_BOOL(xlive_web_advertise_systemlink, true, "XLive",
                    "Advertise System Link host sessions through the XLive web service");
REXCVAR_DEFINE_BOOL(xlive_web_delete_stale_on_startup, false, "XLive",
                    "Delete stale sessions for this profile when the web client connects");
REXCVAR_DEFINE_BOOL(xlive_web_delete_stale_for_public_ip, false, "XLive",
                    "When pruning stale sessions on startup, delete all sessions from this public IP");
REXCVAR_DEFINE_BOOL(xlive_web_delete_session_on_end, true, "XLive",
                    "Delete advertised web sessions when XSessionEnd is called");
REXCVAR_DEFINE_BOOL(xlive_web_prune_profile_on_session_end, false, "XLive",
                    "Prune stale web sessions for this profile when a hosted session ends");
REXCVAR_DEFINE_BOOL(xlive_web_delete_created_sessions_on_shutdown, true, "XLive",
                    "Delete web sessions created by this process when the runtime shuts down");
REXCVAR_DEFINE_BOOL(xlive_web_prune_public_ip_on_shutdown, false, "XLive",
                    "Prune all web sessions from this public IP when the runtime shuts down");
REXCVAR_DECLARE(uint32_t, systemlink_port_offset);

namespace rex::system {

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr uint8_t kFallbackMacAddress[6] = {0x02, 0x4E, 0x58, 0x31, 0x00, 0x01};
constexpr uint16_t kFallbackSessionPort = 3074;
constexpr uint32_t kSessionFlagHost = 1u << 0;
constexpr uint32_t kSessionFlagPresence = 1u << 1;
constexpr uint32_t kSessionFlagMatchmaking = 1u << 3;
constexpr uint32_t kSessionFlagPeerNetwork = 1u << 5;
constexpr uint8_t kUserDataTypeContext = 0;
constexpr uint8_t kUserDataTypeInt32 = 1;
constexpr uint8_t kUserDataTypeWString = 4;
constexpr uint8_t kUserDataTypeBinary = 6;
constexpr uint32_t kMaxUserDataSize = 0x03E8;
constexpr uint32_t kRexPropertyMapName = 0x40009001;
constexpr uint32_t kRexPropertyHostName = 0x40009002;
constexpr uint32_t kRexPropertyGameType = 0x40009003;
constexpr uint32_t kRexPropertyClientCount = 0x10009004;
constexpr uint32_t kRexPropertyMaxClients = 0x10009005;
constexpr uint32_t kSystemPropertyGamerHostname = 0x40008109;
constexpr size_t kSerializedPropertyFixedSize = 20;

struct HttpResponse {
  uint32_t status = 0;
  std::string body;

  bool ok() const { return status >= 200 && status < 300; }
};

struct ClientState {
  std::mutex mutex;
  bool registered = false;
  bool unavailable_logged = false;
  Clock::time_point retry_after = Clock::time_point::min();
  uint32_t registered_title_id = 0;
  uint64_t registered_xuid = 0;
  uint32_t last_status = 0;
  std::string public_address;
  std::string last_error;
  std::unordered_map<uint64_t, std::unordered_set<uint64_t>> posted_members;
  std::unordered_map<uint64_t, std::unordered_set<uint64_t>> prejoined_members;
  std::unordered_map<uint64_t, uint64_t> session_host_xuids;
  std::unordered_map<uint32_t, std::string> cached_user_properties;
  std::unordered_map<uint64_t, std::unordered_map<uint32_t, std::string>> session_properties;
  std::string last_loaded_map;
  uint64_t active_session_id = 0;
  std::unordered_set<uint64_t> created_sessions;
  std::unordered_set<uint64_t> deleted_sessions;
  std::unordered_set<std::string> stale_prune_keys;
};

ClientState g_state;

std::string Trim(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
    return std::isspace(c) != 0;
  }).base();

  if (first >= last) {
    return {};
  }
  return std::string(first, last);
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

bool IsNx1GameplayMapName(std::string_view value) {
  return StartsWith(value, "mp_nx_") && !EndsWith(value, "_load");
}

bool IsUnknownMapName(std::string_view value) {
  return value.empty() || StartsWith(value, "mp_unknown");
}

std::string NormalizeBaseAddress(std::string address) {
  address = Trim(std::move(address));
  if (address.empty()) {
    address = "https://xenia-netplay-2a0298c0e3f4.herokuapp.com/";
  }

  if (address.find("://") == std::string::npos) {
    address = "http://" + address;
  }

  if (address.back() != '/') {
    address.push_back('/');
  }
  return address;
}

std::string BuildEndpoint(std::string_view endpoint) {
  std::string base = NormalizeBaseAddress(REXCVAR_GET(xlive_web_api_address));
  while (!endpoint.empty() && endpoint.front() == '/') {
    endpoint.remove_prefix(1);
  }
  return base + std::string(endpoint);
}

#if REX_PLATFORM_WIN32
std::wstring WidenUrl(std::string_view value) {
  std::wstring widened;
  widened.reserve(value.size());
  for (char c : value) {
    widened.push_back(static_cast<unsigned char>(c));
  }
  return widened;
}

// Persistent WinHTTP session + connection. Opening a session (which triggers a
// WPAD proxy probe) plus a fresh DNS resolve and TLS handshake on every call was
// costing tens-to-hundreds of ms per request over WAN. We now keep the session
// and connection alive and reuse them (WinHTTP keep-alives the underlying
// socket), rebuilding only when the target host/port/scheme/timeout changes or a
// pooled connection has gone stale.
struct HttpConnection {
  std::mutex mutex;
  HINTERNET session = nullptr;
  HINTERNET connection = nullptr;
  std::wstring host;
  INTERNET_PORT port = 0;
  int scheme = 0;
  DWORD timeout = 0;
};
HttpConnection g_http_connection;

void CloseHttpConnectionLocked(HttpConnection& conn) {
  if (conn.connection) {
    WinHttpCloseHandle(conn.connection);
    conn.connection = nullptr;
  }
  if (conn.session) {
    WinHttpCloseHandle(conn.session);
    conn.session = nullptr;
  }
  conn.host.clear();
  conn.port = 0;
  conn.scheme = 0;
  conn.timeout = 0;
}

bool EnsureHttpConnectionLocked(HttpConnection& conn, const std::wstring& host,
                                INTERNET_PORT port, int scheme, DWORD timeout) {
  if (conn.session && conn.connection && conn.host == host && conn.port == port &&
      conn.scheme == scheme && conn.timeout == timeout) {
    return true;
  }

  CloseHttpConnectionLocked(conn);

  conn.session = WinHttpOpen(L"ReXGlue XLive/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!conn.session) {
    return false;
  }
  WinHttpSetTimeouts(conn.session, timeout, timeout, timeout, timeout);

  conn.connection = WinHttpConnect(conn.session, host.c_str(), port, 0);
  if (!conn.connection) {
    WinHttpCloseHandle(conn.session);
    conn.session = nullptr;
    return false;
  }

  conn.host = host;
  conn.port = port;
  conn.scheme = scheme;
  conn.timeout = timeout;
  return true;
}

HttpResponse SendHttpRequest(std::string_view method, const std::string& url,
                             const std::string& body = {},
                             std::string_view content_type = "application/json") {
  HttpResponse response;

  const std::wstring wide_url = WidenUrl(url);

  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);

  if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
    return response;
  }

  const std::wstring host(components.lpszHostName, components.dwHostNameLength);
  std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
  if (components.dwExtraInfoLength) {
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  if (path.empty()) {
    path = L"/";
  }

  const DWORD timeout = std::max<uint32_t>(100, REXCVAR_GET(xlive_web_timeout_ms));
  const int scheme = components.nScheme;
  const INTERNET_PORT port = components.nPort;

  const std::wstring wide_method = WidenUrl(method);
  const DWORD flags = scheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;

  const bool has_body = !body.empty();
  std::wstring headers_storage = L"Accept: application/json\r\n";
  if (has_body) {
    headers_storage = L"Content-Type: " + WidenUrl(content_type) +
                      L"\r\nAccept: application/json\r\n";
  }
  const wchar_t* headers = headers_storage.c_str();
  const DWORD body_size = static_cast<DWORD>(body.size());
  LPVOID body_ptr = has_body ? const_cast<char*>(body.data()) : WINHTTP_NO_REQUEST_DATA;

  std::lock_guard lock(g_http_connection.mutex);

  // A pooled keep-alive connection can be closed by the server after an idle
  // period; that surfaces as a send/receive failure. Rebuild and retry once.
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!EnsureHttpConnectionLocked(g_http_connection, host, port, scheme, timeout)) {
      return response;
    }

    HINTERNET request =
        WinHttpOpenRequest(g_http_connection.connection, wide_method.c_str(), path.c_str(),
                           nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
      CloseHttpConnectionLocked(g_http_connection);
      continue;
    }
    WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);

    BOOL ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), body_ptr, body_size,
                                 body_size, 0);
    if (ok) {
      ok = WinHttpReceiveResponse(request, nullptr);
    }
    if (!ok) {
      WinHttpCloseHandle(request);
      // Drop the (likely stale) pooled connection and retry from a fresh one.
      CloseHttpConnectionLocked(g_http_connection);
      continue;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                            WINHTTP_NO_HEADER_INDEX)) {
      response.status = status;
    }

    for (;;) {
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(request, &available) || !available) {
        break;
      }

      std::string chunk(available, '\0');
      DWORD bytes_read = 0;
      if (!WinHttpReadData(request, chunk.data(), available, &bytes_read)) {
        break;
      }

      chunk.resize(bytes_read);
      response.body += chunk;
    }

    // Keep the session + connection open for reuse; only the request handle is
    // per-call.
    WinHttpCloseHandle(request);
    return response;
  }

  return response;
}
#else
HttpResponse SendHttpRequest(std::string_view method, const std::string& url,
                             const std::string& body = {},
                             std::string_view content_type = "application/json") {
  (void)method;
  (void)url;
  (void)body;
  (void)content_type;
  return {};
}
#endif

json ParseJson(std::string_view body) {
  if (body.empty()) {
    return {};
  }
  return json::parse(body, nullptr, false);
}

uint32_t ReadU32BE(const uint8_t* data) {
  return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
         (uint32_t(data[2]) << 8) | uint32_t(data[3]);
}

uint32_t ReadU32LE(const uint8_t* data) {
  return uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) |
         (uint32_t(data[3]) << 24);
}

void WriteU32BE(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
  data[offset + 0] = static_cast<uint8_t>((value >> 24) & 0xFF);
  data[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
  data[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
  data[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

void WriteU32LE(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
  data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
  data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

std::string Base64Encode(const std::vector<uint8_t>& data) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(((data.size() + 2) / 3) * 4);

  for (size_t i = 0; i < data.size(); i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = i + 1 < data.size() ? data[i + 1] : 0;
    const uint32_t b2 = i + 2 < data.size() ? data[i + 2] : 0;
    const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

    encoded.push_back(kAlphabet[(triple >> 18) & 0x3F]);
    encoded.push_back(kAlphabet[(triple >> 12) & 0x3F]);
    encoded.push_back(i + 1 < data.size() ? kAlphabet[(triple >> 6) & 0x3F] : '=');
    encoded.push_back(i + 2 < data.size() ? kAlphabet[triple & 0x3F] : '=');
  }

  return encoded;
}

int Base64Value(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return 26 + c - 'a';
  }
  if (c >= '0' && c <= '9') {
    return 52 + c - '0';
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

std::optional<std::vector<uint8_t>> Base64Decode(std::string_view text) {
  std::vector<uint8_t> decoded;
  int value = 0;
  int bits = -8;

  for (char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      continue;
    }
    if (c == '=') {
      break;
    }

    const int digit = Base64Value(c);
    if (digit < 0) {
      return std::nullopt;
    }

    value = (value << 6) | digit;
    bits += 6;
    if (bits >= 0) {
      decoded.push_back(static_cast<uint8_t>((value >> bits) & 0xFF));
      bits -= 8;
    }
  }

  return decoded;
}

uint8_t PropertyTypeFromId(uint32_t property_id) {
  return static_cast<uint8_t>((property_id >> 28) & 0xFF);
}

bool PropertyTypeNeedsExtendedData(uint8_t type) {
  return type == kUserDataTypeWString || type == kUserDataTypeBinary;
}

uint32_t ScalarPropertySize(uint8_t type, uint32_t provided_size) {
  if (provided_size == sizeof(uint32_t) || provided_size == sizeof(uint64_t)) {
    return provided_size;
  }

  switch (type) {
    case 0:
    case 1:
    case 5:
      return sizeof(uint32_t);
    case 2:
    case 3:
    case 7:
      return sizeof(uint64_t);
    default:
      return sizeof(uint64_t);
  }
}

std::string SerializePropertyToBase64(uint32_t property_id, uint8_t type,
                                      const uint8_t* value, uint32_t value_size) {
  const bool extended = PropertyTypeNeedsExtendedData(type);
  const uint32_t safe_value_size = extended ? std::min<uint32_t>(value_size, kMaxUserDataSize)
                                            : ScalarPropertySize(type, value_size);
  std::vector<uint8_t> raw(kSerializedPropertyFixedSize + (extended ? safe_value_size : 0), 0);

  WriteU32LE(raw, 0, property_id);
  raw[4] = type;
  if (extended) {
    if (value && safe_value_size) {
      std::memcpy(raw.data() + kSerializedPropertyFixedSize, value, safe_value_size);
    }
  } else if (value && safe_value_size) {
    std::memcpy(raw.data() + 12, value, std::min<uint32_t>(safe_value_size, 8));
  }

  return Base64Encode(raw);
}

std::vector<uint8_t> Utf8ToUtf16BE(std::string_view value) {
  std::vector<uint8_t> encoded;
  encoded.reserve((value.size() + 1) * 2);
  for (unsigned char c : value) {
    encoded.push_back(0);
    encoded.push_back(c);
  }
  encoded.push_back(0);
  encoded.push_back(0);
  return encoded;
}

std::string SerializeWStringPropertyToBase64(uint32_t property_id, std::string_view value) {
  const auto encoded = Utf8ToUtf16BE(value);
  return SerializePropertyToBase64(property_id, kUserDataTypeWString, encoded.data(),
                                   static_cast<uint32_t>(encoded.size()));
}

std::string SerializeU32PropertyToBase64(uint32_t property_id, uint32_t value) {
  std::vector<uint8_t> encoded(4);
  WriteU32BE(encoded, 0, value);
  return SerializePropertyToBase64(property_id, kUserDataTypeInt32, encoded.data(),
                                   static_cast<uint32_t>(encoded.size()));
}

std::string SerializeContextToBase64(uint32_t context_id, uint32_t value) {
  std::vector<uint8_t> encoded(4);
  WriteU32BE(encoded, 0, value);
  return SerializePropertyToBase64(context_id, kUserDataTypeContext, encoded.data(),
                                   static_cast<uint32_t>(encoded.size()));
}

std::optional<XLiveSessionProperty> DecodePropertyBase64(std::string_view base64) {
  const auto raw = Base64Decode(base64);
  if (!raw || raw->size() < kSerializedPropertyFixedSize) {
    return std::nullopt;
  }

  XLiveSessionProperty property;
  property.id = ReadU32LE(raw->data());
  property.type = (*raw)[4];

  if (PropertyTypeNeedsExtendedData(property.type)) {
    property.extended_data.assign(raw->begin() + kSerializedPropertyFixedSize, raw->end());
  } else {
    std::memcpy(property.scalar_data.data(), raw->data() + 12, property.scalar_data.size());
  }

  return property;
}

std::string DecodeUtf16BE(const std::vector<uint8_t>& data) {
  std::string value;
  value.reserve(data.size() / 2);
  for (size_t i = 0; i + 1 < data.size(); i += 2) {
    const uint16_t ch = (uint16_t(data[i]) << 8) | data[i + 1];
    if (!ch) {
      break;
    }
    value.push_back(ch <= 0x7F ? static_cast<char>(ch) : '?');
  }
  return value;
}

void ApplyMetadataProperty(const XLiveSessionProperty& property, XLiveSessionSummary* session) {
  if (!session) {
    return;
  }

  switch (property.id) {
    case kRexPropertyMapName:
      session->map_name = DecodeUtf16BE(property.extended_data);
      break;
    case kRexPropertyHostName:
      session->host_name = DecodeUtf16BE(property.extended_data);
      break;
    case kRexPropertyGameType:
      session->game_type = DecodeUtf16BE(property.extended_data);
      break;
    case kRexPropertyClientCount:
      session->advertised_clients = ReadU32BE(property.scalar_data.data());
      break;
    case kRexPropertyMaxClients:
      session->advertised_max_clients = ReadU32BE(property.scalar_data.data());
      break;
    default:
      break;
  }
}

std::string JsonString(const json& value, std::string_view key,
                       std::string fallback = {}) {
  if (!value.is_object()) {
    return fallback;
  }

  const auto it = value.find(std::string(key));
  if (it == value.end()) {
    return fallback;
  }

  if (it->is_string()) {
    return it->get<std::string>();
  }
  if (it->is_number_unsigned()) {
    return fmt::format("{}", it->get<uint64_t>());
  }
  if (it->is_number_integer()) {
    return fmt::format("{}", it->get<int64_t>());
  }
  return fallback;
}

uint32_t JsonU32(const json& value, std::string_view key, uint32_t fallback = 0) {
  if (!value.is_object()) {
    return fallback;
  }

  const auto it = value.find(std::string(key));
  if (it == value.end()) {
    return fallback;
  }

  if (it->is_number_unsigned()) {
    return static_cast<uint32_t>(it->get<uint64_t>());
  }
  if (it->is_number_integer()) {
    return static_cast<uint32_t>(std::max<int64_t>(0, it->get<int64_t>()));
  }
  if (it->is_string()) {
    uint32_t parsed = 0;
    const auto text = it->get<std::string>();
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const int base = text.starts_with("0x") || text.starts_with("0X") ? 16 : 10;
    if (base == 16) {
      begin += 2;
    }
    if (std::from_chars(begin, end, parsed, base).ec == std::errc()) {
      return parsed;
    }
  }
  return fallback;
}

bool ParseHexU64(std::string_view text, uint64_t* out_value) {
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
  }
  if (text.empty()) {
    return false;
  }

  uint64_t parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 16);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
    return false;
  }

  *out_value = parsed;
  return true;
}

bool ParseIpv4HostOrder(std::string_view text, uint32_t* out_addr) {
  std::array<uint32_t, 4> parts = {};
  size_t part_index = 0;
  size_t start = 0;

  while (start <= text.size() && part_index < parts.size()) {
    const size_t dot = text.find('.', start);
    const size_t end = dot == std::string_view::npos ? text.size() : dot;
    if (end == start) {
      return false;
    }

    uint32_t value = 0;
    const auto result = std::from_chars(text.data() + start, text.data() + end, value);
    if (result.ec != std::errc() || result.ptr != text.data() + end || value > 255) {
      return false;
    }

    parts[part_index++] = value;
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }

  if (part_index != 4) {
    return false;
  }

  *out_addr = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
  return true;
}

std::string Ipv4HostOrderToString(uint32_t addr) {
  return fmt::format("{}.{}.{}.{}", (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
                     (addr >> 8) & 0xFF, addr & 0xFF);
}

uint8_t HexNibble(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<uint8_t>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<uint8_t>(10 + c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return static_cast<uint8_t>(10 + c - 'A');
  }
  return 0xFF;
}

bool ParseMacAddress(std::string_view text, uint8_t* out_mac) {
  std::array<char, 12> hex = {};
  size_t hex_count = 0;
  for (char c : text) {
    if (c == ':' || c == '-') {
      continue;
    }
    if (hex_count >= hex.size() || HexNibble(c) == 0xFF) {
      return false;
    }
    hex[hex_count++] = c;
  }

  if (hex_count != hex.size()) {
    return false;
  }

  for (size_t i = 0; i < 6; ++i) {
    out_mac[i] = static_cast<uint8_t>((HexNibble(hex[i * 2]) << 4) | HexNibble(hex[i * 2 + 1]));
  }
  return true;
}

std::array<uint8_t, 6> MacAddressForXuid(uint64_t xuid) {
  std::array<uint8_t, 6> mac = {};
  if (!xuid) {
    mac = {kFallbackMacAddress[0], kFallbackMacAddress[1], kFallbackMacAddress[2],
           kFallbackMacAddress[3], kFallbackMacAddress[4], kFallbackMacAddress[5]};
  } else {
    mac = {0x02,
           static_cast<uint8_t>((xuid >> 32) & 0xFF),
           static_cast<uint8_t>((xuid >> 24) & 0xFF),
           static_cast<uint8_t>((xuid >> 16) & 0xFF),
           static_cast<uint8_t>((xuid >> 8) & 0xFF),
           static_cast<uint8_t>(xuid & 0xFF)};
  }

  const uint32_t port_offset = REXCVAR_GET(systemlink_port_offset);
  if (port_offset) {
    mac[1] ^= static_cast<uint8_t>((port_offset >> 8) & 0xFF);
    mac[2] ^= static_cast<uint8_t>(port_offset & 0xFF);
  }
  return mac;
}

std::string MacStringForXuid(uint64_t xuid) {
  const auto mac = MacAddressForXuid(xuid);
  return fmt::format("{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}", mac[0], mac[1], mac[2], mac[3],
                     mac[4], mac[5]);
}

uint64_t MachineIdForXuid(uint64_t xuid) {
  const auto mac = MacAddressForXuid(xuid);
  uint64_t machine_id = 0xFA00ull;
  for (uint8_t byte : mac) {
    machine_id = (machine_id << 8) | byte;
  }
  return machine_id;
}

uint32_t WebAdvertisedFlagsForSession(uint32_t flags) {
  const bool is_systemlink_host =
      (flags & kSessionFlagHost) && (flags & kSessionFlagPeerNetwork) &&
      !(flags & (kSessionFlagPresence | kSessionFlagMatchmaking));
  if (REXCVAR_GET(xlive_web_advertise_systemlink) && is_systemlink_host) {
    return flags | kSessionFlagMatchmaking;
  }
  return flags;
}

void GenerateExchangeKey(XNKEY* key) {
  for (size_t i = 0; i < sizeof(key->ab); ++i) {
    key->ab[i] = static_cast<uint8_t>(i);
  }
}

uint64_t CurrentXuid(KernelState* kernel_state) {
  const auto* profile = kernel_state ? kernel_state->user_profile() : nullptr;
  return profile ? profile->xuid() : 0xB13EBABEBABEBABEull;
}

std::string CurrentGamertag(KernelState* kernel_state) {
  const auto* profile = kernel_state ? kernel_state->user_profile() : nullptr;
  std::string name = profile ? profile->name() : "User";
  if (name.empty()) {
    name = "User";
  }
  return name;
}

std::string CurrentTitle(KernelState* kernel_state) {
  std::string title;
  if (kernel_state) {
    title = kernel_state->title_xdbf().title();
  }
  if (title.empty()) {
    title = "NX1";
  }
  return title;
}

uint32_t CurrentMediaId(KernelState* kernel_state) {
  if (!kernel_state) {
    return 0;
  }

  const auto module = kernel_state->GetExecutableModule();
  if (!module) {
    return 0;
  }

  const auto* execution_info = module->xex_module()->opt_execution_info();
  return execution_info ? static_cast<uint32_t>(execution_info->media_id) : 0;
}

uint32_t CurrentVersion(KernelState* kernel_state) {
  if (!kernel_state) {
    return 0;
  }

  const auto module = kernel_state->GetExecutableModule();
  if (!module) {
    return 0;
  }

  const auto* execution_info = module->xex_module()->opt_execution_info();
  return execution_info ? static_cast<uint32_t>(execution_info->version_value) : 0;
}

bool ParseSessionObject(const json& object, XLiveSessionSummary* out_session) {
  if (!out_session || !object.is_object()) {
    return false;
  }

  const std::string id_text = JsonString(object, "id");
  uint64_t session_id = 0;
  if (!ParseHexU64(id_text, &session_id)) {
    return false;
  }

  XLiveSessionSummary session = {};
  Uint64ToXnkid(session_id, &session.info.sessionID);

  uint32_t host_address = 0;
  if (!ParseIpv4HostOrder(JsonString(object, "hostAddress"), &host_address)) {
    return false;
  }

  session.info.hostAddress.ina = host_address;
  session.info.hostAddress.inaOnline = host_address;
  session.info.hostAddress.wPortOnline = static_cast<uint16_t>(
      std::clamp<uint32_t>(JsonU32(object, "port", kFallbackSessionPort), 1, 65535));

  if (!ParseMacAddress(JsonString(object, "macAddress", MacStringForXuid(0)),
                       session.info.hostAddress.abEnet)) {
    std::memcpy(session.info.hostAddress.abEnet, kFallbackMacAddress, sizeof(kFallbackMacAddress));
  }
  GenerateExchangeKey(&session.info.keyExchangeKey);

  ParseHexU64(JsonString(object, "xuid"), &session.host_xuid);
  session.flags = JsonU32(object, "flags");
  session.public_slots = JsonU32(object, "publicSlotsCount");
  session.private_slots = JsonU32(object, "privateSlotsCount");
  session.open_public_slots = JsonU32(object, "openPublicSlotsCount");
  session.open_private_slots = JsonU32(object, "openPrivateSlotsCount");
  session.filled_public_slots = JsonU32(object, "filledPublicSlotsCount");
  session.filled_private_slots = JsonU32(object, "filledPrivateSlotsCount");

  // The web backend stores the host XUID on the session object, but older or
  // interrupted creates can still have an empty players map. Treat such host
  // sessions as occupied by the host so games do not discard a 0-player lobby.
  if (session.host_xuid && session.public_slots &&
      !session.filled_public_slots && !session.filled_private_slots) {
    session.filled_public_slots = 1;
    session.open_public_slots =
        session.open_public_slots ? session.open_public_slots - 1
                                  : std::max<uint32_t>(1, session.public_slots) - 1;
  }

  *out_session = session;
  return true;
}

bool ParsePlayerObject(const json& object, XLivePlayerSummary* out_player) {
  if (!out_player || !object.is_object()) {
    return false;
  }

  XLivePlayerSummary player = {};
  ParseHexU64(JsonString(object, "xuid"), &player.xuid);
  ParseHexU64(JsonString(object, "machineId"), &player.machine_id);
  ParseHexU64(JsonString(object, "sessionId"), &player.session_id);
  ParseIpv4HostOrder(JsonString(object, "hostAddress"), &player.host_address);
  player.port = static_cast<uint16_t>(
      std::clamp<uint32_t>(JsonU32(object, "port", 0), 0, 65535));
  player.gamertag = JsonString(object, "gamertag");
  if (!ParseMacAddress(JsonString(object, "macAddress", MacStringForXuid(player.xuid)),
                       player.mac_address.data())) {
    player.mac_address = MacAddressForXuid(player.xuid);
  }

  if (!player.xuid || !player.host_address) {
    return false;
  }

  *out_player = std::move(player);
  return true;
}

void MarkUnavailableLocked(ClientState& state, std::string_view operation, uint32_t status) {
  state.registered = false;
  state.last_status = status;
  state.last_error =
      status ? fmt::format("{} failed (HTTP {})", operation, status)
             : fmt::format("{} failed", operation);
  state.retry_after = Clock::now() + std::chrono::seconds(5);

  if (!state.unavailable_logged) {
    if (status) {
      REXSYS_WARN("XLive web client unavailable during {} (HTTP {})", operation, status);
    } else {
      REXSYS_WARN("XLive web client unavailable during {}", operation);
    }
    state.unavailable_logged = true;
  }
}

void WarnRequestFailureLocked(ClientState& state, std::string_view operation, uint32_t status) {
  state.last_status = status;
  state.last_error =
      status ? fmt::format("{} failed (HTTP {})", operation, status)
             : fmt::format("{} failed", operation);

  if (status) {
    REXSYS_WARN("XLive web {} failed (HTTP {})", operation, status);
  } else {
    REXSYS_WARN("XLive web {} failed", operation);
  }
}

bool DeleteStaleSessionsLocked(KernelState* kernel_state, ClientState& state,
                               bool all_for_public_ip, std::string_view operation) {
  std::string endpoint = "DeleteSessions";
  if (!all_for_public_ip) {
    endpoint = fmt::format("DeleteSessions/{}", MacStringForXuid(CurrentXuid(kernel_state)));
  }

  const HttpResponse response = SendHttpRequest("DELETE", BuildEndpoint(endpoint));
  if (!response.ok()) {
    WarnRequestFailureLocked(state, operation, response.status);
    return false;
  }

  state.posted_members.clear();
  state.prejoined_members.clear();
  state.session_host_xuids.clear();
  state.session_properties.clear();
  state.created_sessions.clear();
  state.deleted_sessions.clear();
  state.last_status = response.status;
  state.last_error.clear();

  if (REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web pruned stale session(s) with {}", endpoint);
  }
  return true;
}

bool EnsureReadyLocked(KernelState* kernel_state, ClientState& state, bool force_probe = false) {
  if (!REXCVAR_GET(xlive_web_enabled)) {
    state.registered = false;
    state.last_status = 0;
    state.last_error = "disabled";
    return false;
  }

  if (force_probe) {
    state.registered = false;
    state.retry_after = Clock::time_point::min();
    state.unavailable_logged = false;
  }

  const auto now = Clock::now();
  if (!state.registered && now < state.retry_after) {
    state.last_error = "waiting before retry";
    return false;
  }

  const uint32_t title_id = kernel_state ? kernel_state->title_id() : 0;
  const uint64_t xuid = CurrentXuid(kernel_state);
  if (!force_probe && state.registered && state.registered_title_id == title_id &&
      state.registered_xuid == xuid) {
    return true;
  }

  const HttpResponse whoami = SendHttpRequest("GET", BuildEndpoint("whoami"));
  if (!whoami.ok()) {
    MarkUnavailableLocked(state, "whoami", whoami.status);
    return false;
  }

  const json whoami_json = ParseJson(whoami.body);
  if (whoami_json.is_discarded()) {
    MarkUnavailableLocked(state, "whoami parse", whoami.status);
    return false;
  }

  state.public_address = JsonString(whoami_json, "address");
  uint32_t unused_addr = 0;
  if (!ParseIpv4HostOrder(state.public_address, &unused_addr)) {
    MarkUnavailableLocked(state, "whoami address", whoami.status);
    return false;
  }
  if (REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web /whoami -> {}", state.public_address);
  }

  json player = {
      {"xuid", fmt::format("{:016X}", xuid)},
      {"gamertag", CurrentGamertag(kernel_state)},
      {"machineId", fmt::format("{:016X}", MachineIdForXuid(xuid))},
      {"hostAddress", state.public_address},
      {"macAddress", MacStringForXuid(xuid)},
      {"settings", json::object()},
  };

  const HttpResponse register_response =
      SendHttpRequest("POST", BuildEndpoint("players"), player.dump());
  if (!register_response.ok()) {
    MarkUnavailableLocked(state, "player registration", register_response.status);
    return false;
  }

  state.registered = true;
  state.registered_title_id = title_id;
  state.registered_xuid = xuid;
  state.cached_user_properties[kSystemPropertyGamerHostname] =
      SerializeWStringPropertyToBase64(kSystemPropertyGamerHostname,
                                       CurrentGamertag(kernel_state));
  state.last_status = register_response.status;
  state.last_error.clear();
  state.unavailable_logged = false;
  state.retry_after = Clock::time_point::min();
  if (REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web /players registered {} ({:016X})", CurrentGamertag(kernel_state),
                xuid);
  }

  const std::string stale_prune_key = fmt::format("{:08X}:{:016X}", title_id, xuid);
  if (REXCVAR_GET(xlive_web_delete_stale_on_startup) &&
      !state.stale_prune_keys.contains(stale_prune_key)) {
    state.stale_prune_keys.insert(stale_prune_key);
    DeleteStaleSessionsLocked(kernel_state, state,
                              REXCVAR_GET(xlive_web_delete_stale_for_public_ip),
                              "stale session prune");
  }
  return true;
}

std::string HostAddressForSession(const XSession& session, const std::string& public_address) {
  if (!public_address.empty()) {
    return public_address;
  }

  const uint32_t host_order =
      session.session_info().hostAddress.inaOnline
          ? static_cast<uint32_t>(session.session_info().hostAddress.inaOnline)
          : static_cast<uint32_t>(session.session_info().hostAddress.ina);
  return Ipv4HostOrderToString(host_order);
}

void LogSessionCountLocked(KernelState* kernel_state, ClientState& state,
                           uint64_t session_id, std::string_view operation) {
  const HttpResponse response =
      SendHttpRequest("GET",
                      BuildEndpoint(fmt::format("title/{:08X}/sessions/{:016X}",
                                                kernel_state ? kernel_state->title_id() : 0,
                                                session_id)));
  if (!response.ok()) {
    return;
  }

  const json object = ParseJson(response.body);
  if (!object.is_object()) {
    return;
  }

  state.last_status = response.status;
  state.last_error.clear();
  const uint32_t filled_public_slots = JsonU32(object, "filledPublicSlotsCount");
  const uint32_t filled_private_slots = JsonU32(object, "filledPrivateSlotsCount");
  const uint32_t public_slots = JsonU32(object, "publicSlotsCount");
  const uint32_t private_slots = JsonU32(object, "privateSlotsCount");
  const uint32_t current_players =
      std::max<uint32_t>(1, filled_public_slots + filled_private_slots);
  const uint32_t max_players =
      std::max<uint32_t>(1, public_slots + private_slots);
  if (state.active_session_id == session_id) {
    DiscordPresenceClient::Get().UpdateSessionInfo(kernel_state, session_id, "", "",
                                                   current_players, max_players);
  }
  REXSYS_INFO("XLive web session {:016X} count after {}: filled={}/{} open={}/{}",
              session_id, operation, filled_public_slots, filled_private_slots,
              JsonU32(object, "openPublicSlotsCount"),
              JsonU32(object, "openPrivateSlotsCount"));
}

void ClearGameplayPresenceLocked(KernelState* kernel_state, ClientState& state,
                                 std::string_view operation) {
  const bool had_gameplay_state =
      !state.last_loaded_map.empty() || state.active_session_id != 0;
  state.last_loaded_map.clear();
  state.active_session_id = 0;

  if (had_gameplay_state || REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web cleared NX1 Rich Presence gameplay state ({})", operation);
  }
  DiscordPresenceClient::Get().SetMenuState(kernel_state);
}

bool PostSessionJoinLocked(KernelState* kernel_state, ClientState& state, uint64_t session_id,
                           const std::vector<uint64_t>& xuids,
                           const std::vector<bool>& private_slots,
                           std::string_view operation) {
  json xuid_array = json::array();
  json private_slot_array = json::array();
  std::vector<uint64_t> new_xuids;

  auto& posted_members = state.posted_members[session_id];
  bool skipped_existing_member = false;
  auto queue_member = [&](uint64_t xuid, bool private_slot) {
    if (!xuid) {
      return;
    }
    if (std::find(new_xuids.begin(), new_xuids.end(), xuid) != new_xuids.end()) {
      return;
    }
    if (posted_members.contains(xuid)) {
      skipped_existing_member = true;
      return;
    }

    new_xuids.push_back(xuid);
    xuid_array.push_back(fmt::format("{:016X}", xuid));
    private_slot_array.push_back(private_slot);
  };

  if (!state.created_sessions.contains(session_id)) {
    const auto host_it = state.session_host_xuids.find(session_id);
    if (host_it != state.session_host_xuids.end() && host_it->second) {
      queue_member(host_it->second, false);
    }
  }

  for (size_t i = 0; i < xuids.size(); ++i) {
    queue_member(xuids[i], i < private_slots.size() ? private_slots[i] : false);
  }

  if (new_xuids.empty()) {
    if (skipped_existing_member && REXCVAR_GET(xlive_web_log_requests)) {
      REXSYS_INFO("XLive web skipped session {:016X} join because member(s) are already joined",
                  session_id);
    }
    return true;
  }

  json payload = {
      {"xuids", std::move(xuid_array)},
      {"privateSlots", std::move(private_slot_array)},
  };

  const HttpResponse response =
      SendHttpRequest("POST",
                      BuildEndpoint(fmt::format("title/{:08X}/sessions/{:016X}/join",
                                                kernel_state ? kernel_state->title_id() : 0,
                                                session_id)),
                      payload.dump());
  if (!response.ok()) {
    if (response.status == 404) {
      WarnRequestFailureLocked(state, operation, response.status);
      return false;
    }
    MarkUnavailableLocked(state, operation, response.status);
    return false;
  }

  for (uint64_t xuid : new_xuids) {
    posted_members.insert(xuid);
  }
  auto prejoined_it = state.prejoined_members.find(session_id);
  if (prejoined_it != state.prejoined_members.end()) {
    for (uint64_t xuid : new_xuids) {
      prejoined_it->second.erase(xuid);
    }
    if (prejoined_it->second.empty()) {
      state.prejoined_members.erase(prejoined_it);
    }
  }

  REXSYS_INFO("XLive web joined {} member(s) to session {:016X}", new_xuids.size(),
              session_id);
  LogSessionCountLocked(kernel_state, state, session_id, operation);
  return true;
}

bool PostSessionPreJoinLocked(KernelState* kernel_state, ClientState& state, uint64_t session_id,
                              const std::vector<uint64_t>& xuids,
                              std::string_view operation) {
  json xuid_array = json::array();
  std::vector<uint64_t> new_xuids;

  auto& prejoined_members = state.prejoined_members[session_id];
  auto joined_it = state.posted_members.find(session_id);
  for (uint64_t xuid : xuids) {
    if (!xuid) {
      continue;
    }
    if ((joined_it != state.posted_members.end() && joined_it->second.contains(xuid)) ||
        prejoined_members.contains(xuid)) {
      continue;
    }

    new_xuids.push_back(xuid);
    xuid_array.push_back(fmt::format("{:016X}", xuid));
  }

  if (new_xuids.empty()) {
    return true;
  }

  json payload = {{"xuids", std::move(xuid_array)}};
  const HttpResponse response =
      SendHttpRequest("POST",
                      BuildEndpoint(fmt::format("title/{:08X}/sessions/{:016X}/prejoin",
                                                kernel_state ? kernel_state->title_id() : 0,
                                                session_id)),
                      payload.dump());
  if (!response.ok()) {
    if (response.status == 404) {
      WarnRequestFailureLocked(state, operation, response.status);
      return false;
    }
    MarkUnavailableLocked(state, operation, response.status);
    return false;
  }

  for (uint64_t xuid : new_xuids) {
    prejoined_members.insert(xuid);
  }

  if (REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web prejoined {} member(s) to session {:016X}",
                new_xuids.size(), session_id);
  }
  return true;
}

bool PostSessionLeaveLocked(KernelState* kernel_state, ClientState& state, uint64_t session_id,
                            const std::vector<uint64_t>& xuids,
                            std::string_view operation) {
  json xuid_array = json::array();
  std::vector<uint64_t> leaving_xuids;

  for (uint64_t xuid : xuids) {
    if (!xuid) {
      continue;
    }
    leaving_xuids.push_back(xuid);
    xuid_array.push_back(fmt::format("{:016X}", xuid));
  }

  if (leaving_xuids.empty()) {
    return true;
  }

  json payload = {
      {"xuids", std::move(xuid_array)},
  };

  const HttpResponse response =
      SendHttpRequest("POST",
                      BuildEndpoint(fmt::format("title/{:08X}/sessions/{:016X}/leave",
                                                kernel_state ? kernel_state->title_id() : 0,
                                                session_id)),
                      payload.dump());
  if (!response.ok()) {
    WarnRequestFailureLocked(state, operation, response.status);
    return false;
  }

  auto posted_it = state.posted_members.find(session_id);
  if (posted_it != state.posted_members.end()) {
    for (uint64_t xuid : leaving_xuids) {
      posted_it->second.erase(xuid);
    }
    if (posted_it->second.empty()) {
      state.posted_members.erase(posted_it);
    }
  }
  auto prejoined_it = state.prejoined_members.find(session_id);
  if (prejoined_it != state.prejoined_members.end()) {
    for (uint64_t xuid : leaving_xuids) {
      prejoined_it->second.erase(xuid);
    }
    if (prejoined_it->second.empty()) {
      state.prejoined_members.erase(prejoined_it);
    }
  }

  if (REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web removed {} member(s) from session {:016X}",
                leaving_xuids.size(), session_id);
  }
  LogSessionCountLocked(kernel_state, state, session_id, operation);
  return true;
}

bool PostSessionPropertiesLocked(KernelState* kernel_state, ClientState& state,
                                 uint64_t session_id, std::string_view operation) {
  if (!session_id || state.deleted_sessions.contains(session_id)) {
    return true;
  }

  json properties = json::array();
  for (const auto& [id, encoded] : state.cached_user_properties) {
    (void)id;
    if (!encoded.empty()) {
      properties.push_back(encoded);
    }
  }

  const auto session_it = state.session_properties.find(session_id);
  if (session_it != state.session_properties.end()) {
    for (const auto& [id, encoded] : session_it->second) {
      (void)id;
      if (!encoded.empty()) {
        properties.push_back(encoded);
      }
    }
  }

  if (properties.empty()) {
    return true;
  }

  json payload = {{"properties", std::move(properties)}};
  const HttpResponse response =
      SendHttpRequest("POST",
                      BuildEndpoint(fmt::format("title/{:08X}/sessions/{:016X}/properties",
                                                kernel_state ? kernel_state->title_id() : 0,
                                                session_id)),
                      payload.dump());
  if (!response.ok()) {
    WarnRequestFailureLocked(state, operation, response.status);
    return false;
  }

  state.last_status = response.status;
  state.last_error.clear();
  if (REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web posted {} session propert(y/ies) for {:016X}",
                payload["properties"].size(), session_id);
  }
  return true;
}

bool SetSessionMapPropertyLocked(ClientState& state, uint64_t session_id,
                                 std::string_view map_name) {
  if (!session_id || map_name.empty()) {
    return false;
  }

  auto& properties = state.session_properties[session_id];
  const auto encoded = SerializeWStringPropertyToBase64(kRexPropertyMapName, map_name);
  auto it = properties.find(kRexPropertyMapName);
  if (it != properties.end() && it->second == encoded) {
    return false;
  }

  properties[kRexPropertyMapName] = encoded;
  return true;
}

bool FetchSessionPropertiesLocked(KernelState* kernel_state, ClientState& state,
                                  XLiveSessionSummary* session) {
  if (!session) {
    return false;
  }

  const uint64_t session_id = XnkidToUint64(session->info.sessionID);
  if (!session_id) {
    return false;
  }

  const HttpResponse response =
      SendHttpRequest("GET",
                      BuildEndpoint(fmt::format("title/{:08X}/sessions/{:016X}/properties",
                                                kernel_state ? kernel_state->title_id() : 0,
                                                session_id)));
  if (!response.ok()) {
    if (response.status != 404 || REXCVAR_GET(xlive_web_log_requests)) {
      WarnRequestFailureLocked(state, "session properties get", response.status);
    }
    return false;
  }

  const json object = ParseJson(response.body);
  if (!object.is_object()) {
    return false;
  }

  const auto properties_it = object.find("properties");
  if (properties_it == object.end() || !properties_it->is_array()) {
    return false;
  }

  session->contexts.clear();
  session->properties.clear();
  for (const auto& entry : *properties_it) {
    if (!entry.is_string()) {
      continue;
    }

    const auto decoded = DecodePropertyBase64(entry.get<std::string>());
    if (!decoded) {
      continue;
    }

    ApplyMetadataProperty(*decoded, session);
    if (decoded->type == kUserDataTypeContext) {
      XLiveSessionContext context;
      context.id = decoded->id;
      context.value = ReadU32BE(decoded->scalar_data.data());
      session->contexts.push_back(context);
    } else {
      session->properties.push_back(*decoded);
    }
  }

  state.last_status = response.status;
  state.last_error.clear();
  if (REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web fetched {} propert(y/ies) and {} context(s) for session {:016X}",
                session->properties.size(), session->contexts.size(), session_id);
  }
  return true;
}

bool DeleteSessionLocked(KernelState* kernel_state, ClientState& state, uint64_t session_id,
                         std::string_view operation) {
  if (!session_id) {
    return true;
  }

  if (state.deleted_sessions.contains(session_id)) {
    return true;
  }

  const HttpResponse response =
      SendHttpRequest("DELETE",
                      BuildEndpoint(fmt::format("title/{:08X}/sessions/{:016X}",
                                                kernel_state ? kernel_state->title_id() : 0,
                                                session_id)));
  if (!response.ok()) {
    WarnRequestFailureLocked(state, operation, response.status);
    return false;
  }

  state.posted_members.erase(session_id);
  state.prejoined_members.erase(session_id);
  state.session_host_xuids.erase(session_id);
  state.session_properties.erase(session_id);
  state.created_sessions.erase(session_id);
  state.deleted_sessions.insert(session_id);
  state.last_status = response.status;
  state.last_error.clear();

  if (REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web deleted session {:016X}", session_id);
  }
  return true;
}

}  // namespace

XLiveWebClient& XLiveWebClient::Get() {
  static XLiveWebClient client;
  return client;
}

XLiveWebStatus XLiveWebClient::Probe(KernelState* kernel_state, bool force_probe) {
  std::lock_guard lock(g_state.mutex);

  const bool ready = EnsureReadyLocked(kernel_state, g_state, force_probe);
  XLiveWebStatus status;
  status.enabled = REXCVAR_GET(xlive_web_enabled);
  status.connected = ready;
  status.registered = ready && g_state.registered;
  status.http_status = g_state.last_status;
  status.title_id = kernel_state ? kernel_state->title_id() : 0;
  status.xuid = CurrentXuid(kernel_state);
  status.gamertag = CurrentGamertag(kernel_state);
  status.public_address = g_state.public_address;
  status.api_address = NormalizeBaseAddress(REXCVAR_GET(xlive_web_api_address));
  status.message = ready ? "registered" : g_state.last_error;
  if (status.message.empty()) {
    status.message = "not connected";
  }
  return status;
}

void XLiveWebClient::LogStatus(KernelState* kernel_state, bool force_probe) {
  const XLiveWebStatus status = Probe(kernel_state, force_probe);
  if (status.connected) {
    REXSYS_INFO(
        "XLive web status: connected api={} public={} gamertag='{}' xuid={:016X} title={:08X}",
        status.api_address, status.public_address, status.gamertag, status.xuid,
        status.title_id);
  } else {
    REXSYS_WARN(
        "XLive web status: disconnected api={} reason='{}' http={} gamertag='{}' xuid={:016X} "
        "title={:08X}",
        status.api_address, status.message, status.http_status, status.gamertag, status.xuid,
        status.title_id);
  }
}

bool XLiveWebClient::CreateSession(KernelState* kernel_state, const XSession& session) {
  std::lock_guard lock(g_state.mutex);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return false;
  }

  const auto& session_info = session.session_info();
  const uint16_t port = static_cast<uint16_t>(session_info.hostAddress.wPortOnline)
                            ? static_cast<uint16_t>(session_info.hostAddress.wPortOnline)
                            : kFallbackSessionPort;
  const uint32_t web_flags = WebAdvertisedFlagsForSession(session.flags());

  json payload = {
      {"xuid", fmt::format("{:016X}", CurrentXuid(kernel_state))},
      {"title", CurrentTitle(kernel_state)},
      {"mediaId", fmt::format("{:08X}", CurrentMediaId(kernel_state))},
      {"version", fmt::format("{:08X}", CurrentVersion(kernel_state))},
      {"sessionId", fmt::format("{:016X}", session.session_id())},
      {"flags", web_flags},
      {"publicSlotsCount", session.max_public_slots()},
      {"privateSlotsCount", session.max_private_slots()},
      {"hostAddress", HostAddressForSession(session, g_state.public_address)},
      {"macAddress", MacStringForXuid(CurrentXuid(kernel_state))},
      {"port", port},
  };

  const HttpResponse response =
      SendHttpRequest("POST",
                      BuildEndpoint(fmt::format("title/{:08X}/sessions",
                                                kernel_state ? kernel_state->title_id() : 0)),
                      payload.dump());
  if (!response.ok()) {
    MarkUnavailableLocked(g_state, "session create", response.status);
    return false;
  }

  if (REXCVAR_GET(xlive_web_log_requests)) {
    const std::string host_address = HostAddressForSession(session, g_state.public_address);
    if (web_flags != session.flags()) {
      REXSYS_INFO(
          "XLive web created session {:016X} host={}:{} slots public={} private={} "
          "flags={:08X}->{:08X}",
          session.session_id(), host_address, port,
          session.max_public_slots(), session.max_private_slots(), session.flags(), web_flags);
    } else {
      REXSYS_INFO(
          "XLive web created session {:016X} host={}:{} slots public={} private={} "
          "flags={:08X}",
          session.session_id(), host_address, port, session.max_public_slots(),
          session.max_private_slots(), session.flags());
    }
  }

  const uint64_t session_id = session.session_id();
  const uint64_t host_xuid = CurrentXuid(kernel_state);
  const uint32_t max_players =
      std::max<uint32_t>(1, session.max_public_slots() + session.max_private_slots());
  g_state.active_session_id = session_id;
  DiscordPresenceClient::Get().UpdateSessionInfo(kernel_state, session_id, "", "", 1,
                                                 max_players);
  g_state.posted_members.erase(session_id);
  g_state.prejoined_members.erase(session_id);
  g_state.session_host_xuids[session_id] = host_xuid;
  g_state.created_sessions.insert(session_id);
  g_state.deleted_sessions.erase(session_id);
  if (!g_state.last_loaded_map.empty()) {
    SetSessionMapPropertyLocked(g_state, session_id, g_state.last_loaded_map);
  }

  if (host_xuid) {
    PostSessionJoinLocked(kernel_state, g_state, session_id, {host_xuid}, {false},
                          "host session join");
  }
  PostSessionPropertiesLocked(kernel_state, g_state, session_id, "host session properties");
  return true;
}

bool XLiveWebClient::ModifySession(KernelState* kernel_state, const XSession& session) {
  std::lock_guard lock(g_state.mutex);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return false;
  }

  const uint32_t web_flags = WebAdvertisedFlagsForSession(session.flags());
  json payload = {
      {"flags", web_flags},
      {"publicSlotsCount", session.max_public_slots()},
      {"privateSlotsCount", session.max_private_slots()},
  };

  const HttpResponse response =
      SendHttpRequest("POST",
                      BuildEndpoint(fmt::format("title/{:08X}/sessions/{:016X}/modify",
                                                kernel_state ? kernel_state->title_id() : 0,
                                                session.session_id())),
                      payload.dump());
  if (!response.ok()) {
    WarnRequestFailureLocked(g_state, "session modify", response.status);
    return false;
  }

  g_state.last_status = response.status;
  g_state.last_error.clear();
  if (g_state.active_session_id == session.session_id()) {
    DiscordPresenceClient::Get().UpdateSessionInfo(
        kernel_state, session.session_id(), "", "", 0,
        std::max<uint32_t>(1, session.max_public_slots() + session.max_private_slots()));
  }
  if (REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web modified session {:016X} slots public={} private={} flags={:08X}",
                session.session_id(), session.max_public_slots(), session.max_private_slots(),
                web_flags);
  }
  return true;
}

std::vector<XLiveSessionSummary> XLiveWebClient::SearchSessions(KernelState* kernel_state,
                                                                uint32_t search_index,
                                                                uint32_t results_count,
                                                                uint32_t num_users,
                                                                bool filter_own_sessions) {
  std::lock_guard lock(g_state.mutex);
  std::vector<XLiveSessionSummary> sessions;
  if (!results_count) {
    if (REXCVAR_GET(xlive_web_log_requests)) {
      REXSYS_INFO("XLive web session search skipped: results_count=0");
    }
    return sessions;
  }
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    if (REXCVAR_GET(xlive_web_log_requests)) {
      REXSYS_WARN("XLive web session search skipped: client not ready ({})",
                  g_state.last_error.empty() ? "unknown" : g_state.last_error);
    }
    return sessions;
  }

  json payload = {
      {"searchIndex", search_index},
      {"resultsCount", results_count},
      {"numUsers", num_users},
  };
  if (filter_own_sessions) {
    payload["searcher_xuid"] = fmt::format("{:016X}", CurrentXuid(kernel_state));
  }

  const HttpResponse response =
      SendHttpRequest("POST",
                      BuildEndpoint(fmt::format("title/{:08X}/sessions/search",
                                                kernel_state ? kernel_state->title_id() : 0)),
                      payload.dump());
  if (!response.ok()) {
    MarkUnavailableLocked(g_state, "session search", response.status);
    if (REXCVAR_GET(xlive_web_log_requests)) {
      REXSYS_WARN("XLive web session search failed (HTTP {})", response.status);
    }
    return sessions;
  }

  const json array = ParseJson(response.body);
  if (!array.is_array()) {
    if (REXCVAR_GET(xlive_web_log_requests)) {
      REXSYS_WARN("XLive web session search returned non-array response ({} bytes)",
                  response.body.size());
    }
    return sessions;
  }

  for (const auto& item : array) {
    XLiveSessionSummary session;
    if (ParseSessionObject(item, &session)) {
      FetchSessionPropertiesLocked(kernel_state, g_state, &session);
      const uint64_t session_id = XnkidToUint64(session.info.sessionID);
      if (session_id && session.host_xuid) {
        g_state.session_host_xuids[session_id] = session.host_xuid;
      }
      sessions.push_back(session);
    }
    if (sessions.size() >= results_count) {
      break;
    }
  }

  if (REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web session search index={} requested={} returned={}", search_index,
                results_count, sessions.size());
    for (size_t i = 0; i < sessions.size(); ++i) {
      const auto& session = sessions[i];
      const uint32_t host_address =
          static_cast<uint32_t>(session.info.hostAddress.inaOnline)
              ? static_cast<uint32_t>(session.info.hostAddress.inaOnline)
              : static_cast<uint32_t>(session.info.hostAddress.ina);
      REXSYS_INFO(
          "XLive web session[{}] id={:016X} host={}:{} flags={:08X} map='{}' game='{}' "
          "clients={}/{} open={}/{} filled={}/{}",
          i, XnkidToUint64(session.info.sessionID), Ipv4HostOrderToString(host_address),
          static_cast<uint16_t>(session.info.hostAddress.wPortOnline), session.flags,
          session.map_name, session.game_type, session.advertised_clients,
          session.advertised_max_clients, session.open_public_slots, session.open_private_slots,
          session.filled_public_slots, session.filled_private_slots);
    }
  }
  return sessions;
}

bool XLiveWebClient::GetSession(KernelState* kernel_state, uint64_t session_id,
                                XLiveSessionSummary* out_session) {
  std::lock_guard lock(g_state.mutex);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return false;
  }

  const HttpResponse response =
      SendHttpRequest("GET",
                      BuildEndpoint(fmt::format("title/{:08X}/sessions/{:016X}",
                                                kernel_state ? kernel_state->title_id() : 0,
                                                session_id)));
  if (!response.ok()) {
    MarkUnavailableLocked(g_state, "session get", response.status);
    return false;
  }

  const json object = ParseJson(response.body);
  const bool parsed = !object.is_discarded() && ParseSessionObject(object, out_session);
  if (parsed) {
    FetchSessionPropertiesLocked(kernel_state, g_state, out_session);
    if (out_session->host_xuid) {
      g_state.session_host_xuids[session_id] = out_session->host_xuid;
    }
  }
  if (parsed && REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web fetched session {:016X}", session_id);
  }
  return parsed;
}

bool XLiveWebClient::JoinSession(KernelState* kernel_state, uint64_t session_id,
                                  const std::vector<uint64_t>& xuids,
                                  const std::vector<bool>& private_slots) {
  std::lock_guard lock(g_state.mutex);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return false;
  }
  const uint64_t old_active_session_id = g_state.active_session_id;
  g_state.active_session_id = session_id;
  const bool joined =
      PostSessionJoinLocked(kernel_state, g_state, session_id, xuids, private_slots,
                            "session join");
  if (!joined) {
    g_state.active_session_id = old_active_session_id;
  }
  return joined;
}

bool XLiveWebClient::PreJoinSession(KernelState* kernel_state, uint64_t session_id,
                                    const std::vector<uint64_t>& xuids) {
  std::lock_guard lock(g_state.mutex);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return false;
  }
  return PostSessionPreJoinLocked(kernel_state, g_state, session_id, xuids, "session prejoin");
}

bool XLiveWebClient::LeaveSession(KernelState* kernel_state, uint64_t session_id,
                                   const std::vector<uint64_t>& xuids) {
  std::lock_guard lock(g_state.mutex);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return false;
  }
  const bool left =
      PostSessionLeaveLocked(kernel_state, g_state, session_id, xuids, "session leave");
  if (left && g_state.active_session_id == session_id) {
    ClearGameplayPresenceLocked(kernel_state, g_state, "session leave");
  }
  return left;
}

bool XLiveWebClient::FindPlayerByAddress(KernelState* kernel_state, uint32_t host_order_address,
                                          XLivePlayerSummary* out_player) {
  if (!host_order_address || !out_player) {
    return false;
  }

  std::lock_guard lock(g_state.mutex);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return false;
  }

  const std::string host_address = Ipv4HostOrderToString(host_order_address);
  json payload = {{"hostAddress", host_address}};
  const HttpResponse response =
      SendHttpRequest("POST", BuildEndpoint("players/find"), payload.dump());
  if (!response.ok()) {
    if (response.status != 404) {
      MarkUnavailableLocked(g_state, "player find", response.status);
    }
    return false;
  }

  const json object = ParseJson(response.body);
  const bool parsed = !object.is_discarded() && ParsePlayerObject(object, out_player);
  if (parsed && REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web found player {} ({:016X}) at {} session={:016X}",
                out_player->gamertag, out_player->xuid, host_address,
                out_player->session_id);
  }
  return parsed;
}

bool XLiveWebClient::PostQos(KernelState* kernel_state, uint64_t session_id, const uint8_t* data,
                             size_t data_size) {
  if (!session_id || (!data && data_size)) {
    return false;
  }

  std::lock_guard lock(g_state.mutex);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return false;
  }

  const std::string body(reinterpret_cast<const char*>(data), data_size);
  const HttpResponse response =
      SendHttpRequest("POST",
                      BuildEndpoint(fmt::format("title/{:08X}/sessions/{:016x}/qos",
                                                kernel_state ? kernel_state->title_id() : 0,
                                                session_id)),
                      body, "application/octet-stream");
  if (response.status != 201 && !response.ok()) {
    WarnRequestFailureLocked(g_state, "qos post", response.status);
    return false;
  }

  if (REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web posted {} QoS byte(s) for session {:016X}", data_size, session_id);
  }
  return true;
}

bool XLiveWebClient::GetQos(KernelState* kernel_state, uint64_t session_id,
                            std::vector<uint8_t>* out_data) {
  if (!session_id || !out_data) {
    return false;
  }

  std::lock_guard lock(g_state.mutex);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return false;
  }

  const HttpResponse response =
      SendHttpRequest("GET", BuildEndpoint(fmt::format("title/{:08X}/sessions/{:016x}/qos",
                                                       kernel_state ? kernel_state->title_id() : 0,
                                                       session_id)));
  if (response.status == 204) {
    out_data->clear();
    return true;
  }
  if (!response.ok()) {
    WarnRequestFailureLocked(g_state, "qos get", response.status);
    return false;
  }

  out_data->assign(response.body.begin(), response.body.end());
  if (REXCVAR_GET(xlive_web_log_requests)) {
    REXSYS_INFO("XLive web fetched {} QoS byte(s) for session {:016X}", out_data->size(),
                session_id);
  }
  return true;
}

void XLiveWebClient::SetUserContext(KernelState* kernel_state, uint32_t context_id,
                                    uint32_t value) {
  if (!context_id) {
    return;
  }

  std::lock_guard lock(g_state.mutex);
  g_state.cached_user_properties[context_id] = SerializeContextToBase64(context_id, value);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return;
  }

  for (uint64_t session_id : g_state.created_sessions) {
    if (!g_state.deleted_sessions.contains(session_id)) {
      PostSessionPropertiesLocked(kernel_state, g_state, session_id, "user context update");
    }
  }
}

void XLiveWebClient::SetUserProperty(KernelState* kernel_state, uint32_t property_id,
                                     const uint8_t* value, uint32_t value_size) {
  if (!property_id || (!value && value_size)) {
    return;
  }

  const uint8_t type = PropertyTypeFromId(property_id);
  if (PropertyTypeNeedsExtendedData(type) && value_size > kMaxUserDataSize) {
    value_size = kMaxUserDataSize;
  }

  std::lock_guard lock(g_state.mutex);
  g_state.cached_user_properties[property_id] =
      SerializePropertyToBase64(property_id, type, value, value_size);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return;
  }

  for (uint64_t session_id : g_state.created_sessions) {
    if (!g_state.deleted_sessions.contains(session_id)) {
      PostSessionPropertiesLocked(kernel_state, g_state, session_id, "user property update");
    }
  }
}

void XLiveWebClient::NoteLoadedMap(KernelState* kernel_state, std::string map_name) {
  map_name = Trim(std::move(map_name));
  if (!IsNx1GameplayMapName(map_name)) {
    return;
  }

  std::lock_guard lock(g_state.mutex);
  const bool changed_map = g_state.last_loaded_map != map_name;

  g_state.last_loaded_map = std::move(map_name);
  if (changed_map) {
    REXSYS_INFO("XLive web noted active NX1 map '{}'", g_state.last_loaded_map);
  }
  DiscordPresenceClient::Get().NoteLoadedMap(kernel_state, g_state.last_loaded_map);

  std::vector<uint64_t> updated_sessions;
  for (uint64_t session_id : g_state.created_sessions) {
    if (g_state.deleted_sessions.contains(session_id)) {
      continue;
    }
    if (SetSessionMapPropertyLocked(g_state, session_id, g_state.last_loaded_map)) {
      updated_sessions.push_back(session_id);
    }
  }

  if (!kernel_state || updated_sessions.empty()) {
    return;
  }
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return;
  }

  for (uint64_t session_id : updated_sessions) {
    PostSessionPropertiesLocked(kernel_state, g_state, session_id, "loaded map metadata");
  }
}

void XLiveWebClient::ClearLoadedMap(KernelState* kernel_state) {
  std::lock_guard lock(g_state.mutex);
  ClearGameplayPresenceLocked(kernel_state, g_state, "map shutdown");
}

void XLiveWebClient::UpdateAdvertisedSessionInfo(KernelState* kernel_state,
                                                 uint64_t session_id,
                                                 std::string map_name,
                                                 std::string host_name,
                                                 std::string game_type,
                                                 uint32_t clients,
                                                 uint32_t max_clients) {
  if (!session_id) {
    return;
  }

  map_name = Trim(std::move(map_name));
  host_name = Trim(std::move(host_name));
  game_type = Trim(std::move(game_type));

  std::lock_guard lock(g_state.mutex);
  if (IsUnknownMapName(map_name)) {
    if (!g_state.last_loaded_map.empty()) {
      if (REXCVAR_GET(xlive_web_log_requests)) {
        REXSYS_INFO("XLive web replacing advertised map '{}' with active map '{}'",
                    map_name.empty() ? "<empty>" : map_name, g_state.last_loaded_map);
      }
      map_name = g_state.last_loaded_map;
    } else {
      map_name.clear();
    }
  }

  auto& properties = g_state.session_properties[session_id];
  bool changed = false;
  auto set_property = [&properties, &changed](uint32_t id, std::string encoded) {
    if (encoded.empty()) {
      return;
    }
    auto it = properties.find(id);
    if (it == properties.end() || it->second != encoded) {
      properties[id] = std::move(encoded);
      changed = true;
    }
  };

  if (!map_name.empty()) {
    changed = SetSessionMapPropertyLocked(g_state, session_id, map_name) || changed;
  }
  if (!host_name.empty()) {
    set_property(kRexPropertyHostName,
                 SerializeWStringPropertyToBase64(kRexPropertyHostName, host_name));
    set_property(kSystemPropertyGamerHostname,
                 SerializeWStringPropertyToBase64(kSystemPropertyGamerHostname, host_name));
  }
  if (!game_type.empty()) {
    set_property(kRexPropertyGameType,
                 SerializeWStringPropertyToBase64(kRexPropertyGameType, game_type));
  }
  if (clients) {
    set_property(kRexPropertyClientCount,
                 SerializeU32PropertyToBase64(kRexPropertyClientCount, clients));
  }
  if (max_clients) {
    set_property(kRexPropertyMaxClients,
                 SerializeU32PropertyToBase64(kRexPropertyMaxClients, max_clients));
  }

  if (g_state.active_session_id == session_id) {
    DiscordPresenceClient::Get().UpdateSessionInfo(kernel_state, session_id, map_name,
                                                   game_type, clients, max_clients);
  }

  if (!changed || !g_state.created_sessions.contains(session_id) ||
      g_state.deleted_sessions.contains(session_id)) {
    return;
  }

  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return;
  }

  PostSessionPropertiesLocked(kernel_state, g_state, session_id,
                              "System Link advert metadata");
}

bool XLiveWebClient::IsSessionCreatedLocally(uint64_t session_id) {
  std::lock_guard lock(g_state.mutex);
  return g_state.created_sessions.contains(session_id) &&
         !g_state.deleted_sessions.contains(session_id);
}

bool XLiveWebClient::IsInActiveSession() {
  std::lock_guard lock(g_state.mutex);
  return g_state.active_session_id != 0;
}

bool XLiveWebClient::DeleteSession(KernelState* kernel_state, uint64_t session_id) {
  std::lock_guard lock(g_state.mutex);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return false;
  }
  const bool was_active = g_state.active_session_id == session_id;
  const bool deleted = DeleteSessionLocked(kernel_state, g_state, session_id, "session delete");
  if (deleted && was_active) {
    ClearGameplayPresenceLocked(kernel_state, g_state, "session delete");
  }
  return deleted;
}

bool XLiveWebClient::DeleteSessionOnEnd(KernelState* kernel_state, uint64_t session_id) {
  if (!REXCVAR_GET(xlive_web_delete_session_on_end)) {
    return true;
  }

  std::lock_guard lock(g_state.mutex);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return false;
  }

  const bool was_active = g_state.active_session_id == session_id;
  const bool deleted = DeleteSessionLocked(kernel_state, g_state, session_id, "session end delete");
  if (deleted && was_active) {
    ClearGameplayPresenceLocked(kernel_state, g_state, "session end");
  }
  if (!REXCVAR_GET(xlive_web_prune_profile_on_session_end)) {
    return deleted;
  }

  const bool pruned =
      DeleteStaleSessionsLocked(kernel_state, g_state, false, "session end profile prune");
  return deleted || pruned;
}

bool XLiveWebClient::DeleteStaleSessions(KernelState* kernel_state, bool all_for_public_ip) {
  std::lock_guard lock(g_state.mutex);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return false;
  }
  return DeleteStaleSessionsLocked(kernel_state, g_state, all_for_public_ip,
                                   "manual stale session prune");
}

bool XLiveWebClient::CleanupOnShutdown(KernelState* kernel_state) {
  std::lock_guard lock(g_state.mutex);
  if (!EnsureReadyLocked(kernel_state, g_state)) {
    return false;
  }

  ClearGameplayPresenceLocked(kernel_state, g_state, "runtime shutdown");

  bool ok = true;
  if (REXCVAR_GET(xlive_web_delete_created_sessions_on_shutdown)) {
    std::vector<uint64_t> session_ids(g_state.created_sessions.begin(),
                                      g_state.created_sessions.end());
    for (uint64_t session_id : session_ids) {
      ok = DeleteSessionLocked(kernel_state, g_state, session_id,
                               "shutdown created session delete") &&
           ok;
    }
  }

  if (REXCVAR_GET(xlive_web_prune_public_ip_on_shutdown)) {
    ok = DeleteStaleSessionsLocked(kernel_state, g_state, true, "shutdown public IP prune") &&
         ok;
  }

  return ok;
}

}  // namespace rex::system

REXCVAR_DEFINE_COMMAND(xlive_web_status,
                       []() {
                         auto* runtime = rex::Runtime::instance();
                         auto* kernel_state = runtime ? runtime->kernel_state() : nullptr;
                         rex::system::XLiveWebClient::Get().LogStatus(kernel_state, true);
                       },
                       "XLive", "Probe the XLive web service and log connection status");

REXCVAR_DEFINE_COMMAND(
    xlive_web_sessions,
    []() {
      auto* runtime = rex::Runtime::instance();
      auto* kernel_state = runtime ? runtime->kernel_state() : nullptr;
      auto sessions = rex::system::XLiveWebClient::Get().SearchSessions(kernel_state, 0, 16, 1);

      REXSYS_INFO("XLive web sessions visible: {}", sessions.size());
      for (const auto& session : sessions) {
        const uint32_t host = static_cast<uint32_t>(session.info.hostAddress.ina);
        REXSYS_INFO(
            "XLive web session id={:016X} host={}.{}.{}.{}:{} slots open={}/{} private={}/{} "
            "flags={:08X} map='{}' game='{}' clients={}/{}",
            rex::system::XnkidToUint64(session.info.sessionID), (host >> 24) & 0xFF,
            (host >> 16) & 0xFF, (host >> 8) & 0xFF, host & 0xFF,
            static_cast<uint16_t>(session.info.hostAddress.wPortOnline),
            session.open_public_slots, session.public_slots, session.open_private_slots,
            session.private_slots, session.flags, session.map_name, session.game_type,
            session.advertised_clients, session.advertised_max_clients);
      }
    },
    "XLive", "Search the XLive web service and log visible sessions");

REXCVAR_DEFINE_COMMAND(xlive_web_prune_sessions,
                       []() {
                         auto* runtime = rex::Runtime::instance();
                         auto* kernel_state = runtime ? runtime->kernel_state() : nullptr;
                         rex::system::XLiveWebClient::Get().DeleteStaleSessions(kernel_state,
                                                                                 true);
                       },
                       "XLive",
                       "Delete stale XLive web sessions created from this public IP");
