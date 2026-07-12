/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/kernel/xam/module.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xlive_web_client.h>
#include <rex/system/xsocket.h>
// #include <rex/system/xnet.h>

#include <rex/net/socket.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <natupnp.h>
#include <WS2tcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

REXCVAR_DEFINE_BOOL(xlive_web_bridge_systemlink_broadcast, true, "XLive",
                    "Forward System Link broadcast probes to XLive web sessions");
REXCVAR_DEFINE_UINT32(xlive_web_bridge_broadcast_cache_ms, 2000, "XLive",
                      "Milliseconds to cache XLive web sessions for broadcast forwarding")
    .range(250, 30000);
REXCVAR_DEFINE_UINT32(xlive_web_bridge_broadcast_max_hosts, 32, "XLive",
                      "Maximum XLive web sessions to fan out each System Link broadcast to")
    .range(1, 128);
REXCVAR_DEFINE_BOOL(xlive_web_bridge_log_packets, false, "XLive",
                    "Log System Link broadcast bridge packet fan-out");
REXCVAR_DEFINE_BOOL(xlive_web_bridge_synthesize_lan_info, true, "XLive",
                    "Synthesize LAN infoResponse packets from XLive web sessions");
REXCVAR_DEFINE_BOOL(xlive_web_bridge_loopback_same_public_ip, true, "XLive",
                    "Use 127.0.0.1 for synthesized System Link sessions from the same public IP");
REXCVAR_DEFINE_UINT32(systemlink_base_port, 1001, "XLive",
                      "Guest System Link query port advertised through XLive web sessions")
    .range(1, 65535);
REXCVAR_DEFINE_UINT32(systemlink_port_offset, 0, "XLive",
                      "Offset added to native UDP bind ports for multiple local instances")
    .range(0, 60000);
REXCVAR_DEFINE_BOOL(systemlink_upnp_enabled, true, "XLive",
                    "Use UPnP to map native System Link UDP ports when sockets bind");
REXCVAR_DEFINE_BOOL(systemlink_upnp_log, true, "XLive",
                    "Log UPnP System Link port mapping attempts");
REXCVAR_DEFINE_STRING(systemlink_upnp_description, "ReXGlue System Link UDP", "XLive",
                      "Description used for UPnP System Link UDP port mappings");

namespace rex::system {

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

namespace {

using Clock = std::chrono::steady_clock;

struct BroadcastBridgeCache {
  std::mutex mutex;
  bool valid = false;
  // Set while a background refresh thread is in flight so we never spawn more
  // than one at a time, and so the packet-send path can serve stale results
  // instead of blocking on the web request.
  bool refresh_in_flight = false;
  Clock::time_point refreshed_at = Clock::time_point::min();
  std::vector<XLiveSessionSummary> sessions;
};

BroadcastBridgeCache g_broadcast_bridge_cache;

// Shutdown guard for the background refresh threads. Once set, no new refreshes
// are spawned; the counter lets shutdown wait for an in-flight refresh to drain
// before runtime globals (KernelState, the web client) are destroyed.
std::atomic<bool> g_broadcast_bridge_shutting_down{false};
std::atomic<int> g_broadcast_bridge_refreshes_active{0};

struct UpnpMappingState {
  std::mutex mutex;
  std::map<uint16_t, uint32_t> ref_counts;
};

UpnpMappingState g_upnp_mapping_state;

std::string Ipv4HostOrderToString(uint32_t host_order_addr);

int GetLastSocketError() {
#if REX_PLATFORM_WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool EnableDatagramPortReuse(uint64_t native_handle) {
  int reuse = 1;
  if (setsockopt(native_handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse)) < 0) {
    return false;
  }

#if !REX_PLATFORM_WIN32 && defined(SO_REUSEPORT)
  // SO_REUSEADDR is enough for WinSock, but POSIX systems commonly need
  // SO_REUSEPORT for multiple sockets to share the same UDP port.
  setsockopt(native_handle, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));
#endif

  return true;
}

bool DisableUdpConnectionReset(uint64_t native_handle) {
#if REX_PLATFORM_WIN32
  BOOL report_connection_reset = FALSE;
  DWORD bytes_returned = 0;
  return WSAIoctl(static_cast<SOCKET>(native_handle), SIO_UDP_CONNRESET,
                  &report_connection_reset, sizeof(report_connection_reset), nullptr, 0,
                  &bytes_returned, nullptr, nullptr) != SOCKET_ERROR;
#else
  (void)native_handle;
  return true;
#endif
}

#if REX_PLATFORM_WIN32
template <typename T>
class ComReleasePtr {
 public:
  ~ComReleasePtr() { Reset(); }
  T** Out() {
    Reset();
    return &ptr_;
  }
  T* Get() const { return ptr_; }
  void Reset() {
    if (ptr_) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }

 private:
  T* ptr_ = nullptr;
};

class ScopedBstr {
 public:
  explicit ScopedBstr(const wchar_t* value) : value_(SysAllocString(value)) {}
  explicit ScopedBstr(const std::wstring& value) : value_(SysAllocString(value.c_str())) {}
  ~ScopedBstr() {
    if (value_) {
      SysFreeString(value_);
    }
  }
  BSTR Get() const { return value_; }
  bool valid() const { return value_ != nullptr; }

 private:
  BSTR value_ = nullptr;
};

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int count =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0) {
    return {};
  }
  std::wstring wide(static_cast<size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
  return wide;
}

bool IsUsableLocalIpv4(uint32_t host_order_addr) {
  return host_order_addr != 0 && host_order_addr != 0x7F000001u &&
         (host_order_addr >> 24) != 0x7Fu;
}

std::string DetectLocalIpv4ForUpnp() {
  SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (probe != INVALID_SOCKET) {
    sockaddr_in remote = {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
    if (connect(probe, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) !=
        SOCKET_ERROR) {
      sockaddr_in local = {};
      int local_len = sizeof(local);
      if (getsockname(probe, reinterpret_cast<sockaddr*>(&local), &local_len) != SOCKET_ERROR) {
        const uint32_t host_order_addr = ntohl(local.sin_addr.s_addr);
        closesocket(probe);
        if (IsUsableLocalIpv4(host_order_addr)) {
          return Ipv4HostOrderToString(host_order_addr);
        }
      }
    }
    closesocket(probe);
  }

  char hostname[256] = {};
  if (gethostname(hostname, sizeof(hostname)) == SOCKET_ERROR) {
    return {};
  }

  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* results = nullptr;
  if (getaddrinfo(hostname, nullptr, &hints, &results) != 0) {
    return {};
  }

  std::string address;
  for (addrinfo* result = results; result; result = result->ai_next) {
    auto* addr = reinterpret_cast<sockaddr_in*>(result->ai_addr);
    const uint32_t host_order_addr = ntohl(addr->sin_addr.s_addr);
    if (IsUsableLocalIpv4(host_order_addr)) {
      address = Ipv4HostOrderToString(host_order_addr);
      break;
    }
  }
  freeaddrinfo(results);
  return address;
}

HRESULT GetUpnpPortMappings(ComReleasePtr<IUPnPNAT>* nat_out,
                            ComReleasePtr<IStaticPortMappingCollection>* mappings_out) {
  HRESULT hr = CoCreateInstance(CLSID_UPnPNAT, nullptr, CLSCTX_ALL, IID_IUPnPNAT,
                                reinterpret_cast<void**>(nat_out->Out()));
  if (FAILED(hr)) {
    return hr;
  }
  return nat_out->Get()->get_StaticPortMappingCollection(mappings_out->Out());
}

bool AddUpnpUdpMapping(uint16_t native_port) {
  const std::string local_ip = DetectLocalIpv4ForUpnp();
  if (local_ip.empty()) {
    REXSYS_WARN("System Link UPnP could not detect a local IPv4 address for UDP port {}",
                native_port);
    return false;
  }

  HRESULT coinit_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool should_uninitialize = SUCCEEDED(coinit_result);
  if (FAILED(coinit_result) && coinit_result != RPC_E_CHANGED_MODE) {
    REXSYS_WARN("System Link UPnP COM init failed for UDP port {}: 0x{:08X}", native_port,
                static_cast<uint32_t>(coinit_result));
    return false;
  }

  bool mapped = false;
  ComReleasePtr<IUPnPNAT> nat;
  ComReleasePtr<IStaticPortMappingCollection> mappings;
  HRESULT hr = GetUpnpPortMappings(&nat, &mappings);
  if (SUCCEEDED(hr) && mappings.Get()) {
    ScopedBstr protocol(L"UDP");
    ScopedBstr internal_client(Utf8ToWide(local_ip));
    ScopedBstr description(Utf8ToWide(REXCVAR_GET(systemlink_upnp_description)));
    if (protocol.valid() && internal_client.valid() && description.valid()) {
      ComReleasePtr<IStaticPortMapping> mapping;
      hr = mappings.Get()->Add(native_port, protocol.Get(), native_port, internal_client.Get(),
                               VARIANT_TRUE, description.Get(), mapping.Out());
      if (FAILED(hr)) {
        mappings.Get()->Remove(native_port, protocol.Get());
        mapping.Reset();
        hr = mappings.Get()->Add(native_port, protocol.Get(), native_port, internal_client.Get(),
                                 VARIANT_TRUE, description.Get(), mapping.Out());
      }
      mapped = SUCCEEDED(hr);
    } else {
      hr = E_OUTOFMEMORY;
    }
  }

  if (mapped) {
    REXSYS_INFO("System Link UPnP mapped UDP {} -> {}:{}", native_port, local_ip, native_port);
  } else if (REXCVAR_GET(systemlink_upnp_log)) {
    REXSYS_WARN("System Link UPnP failed to map UDP port {} to {}: 0x{:08X}", native_port,
                local_ip, static_cast<uint32_t>(hr));
  }

  if (should_uninitialize) {
    CoUninitialize();
  }
  return mapped;
}

void RemoveUpnpUdpMapping(uint16_t native_port) {
  HRESULT coinit_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool should_uninitialize = SUCCEEDED(coinit_result);
  if (FAILED(coinit_result) && coinit_result != RPC_E_CHANGED_MODE) {
    if (REXCVAR_GET(systemlink_upnp_log)) {
      REXSYS_WARN("System Link UPnP COM init failed while removing UDP port {}: 0x{:08X}",
                  native_port, static_cast<uint32_t>(coinit_result));
    }
    return;
  }

  ComReleasePtr<IUPnPNAT> nat;
  ComReleasePtr<IStaticPortMappingCollection> mappings;
  HRESULT hr = GetUpnpPortMappings(&nat, &mappings);
  if (SUCCEEDED(hr) && mappings.Get()) {
    ScopedBstr protocol(L"UDP");
    if (protocol.valid()) {
      hr = mappings.Get()->Remove(native_port, protocol.Get());
    }
  }

  if (SUCCEEDED(hr)) {
    REXSYS_INFO("System Link UPnP removed UDP mapping for port {}", native_port);
  } else if (REXCVAR_GET(systemlink_upnp_log)) {
    REXSYS_WARN("System Link UPnP failed to remove UDP mapping for port {}: 0x{:08X}",
                native_port, static_cast<uint32_t>(hr));
  }

  if (should_uninitialize) {
    CoUninitialize();
  }
}

bool AcquireUpnpUdpMapping(uint16_t native_port) {
  if (!REXCVAR_GET(systemlink_upnp_enabled) || !native_port) {
    return false;
  }

  std::lock_guard<std::mutex> lock(g_upnp_mapping_state.mutex);
  auto& ref_count = g_upnp_mapping_state.ref_counts[native_port];
  if (ref_count) {
    ++ref_count;
    return true;
  }

  if (!AddUpnpUdpMapping(native_port)) {
    g_upnp_mapping_state.ref_counts.erase(native_port);
    return false;
  }

  ref_count = 1;
  return true;
}

void ReleaseUpnpUdpMapping(uint16_t native_port) {
  if (!native_port) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_upnp_mapping_state.mutex);
  auto it = g_upnp_mapping_state.ref_counts.find(native_port);
  if (it == g_upnp_mapping_state.ref_counts.end()) {
    return;
  }
  if (it->second > 1) {
    --it->second;
    return;
  }

  g_upnp_mapping_state.ref_counts.erase(it);
  RemoveUpnpUdpMapping(native_port);
}
#else
bool AcquireUpnpUdpMapping(uint16_t native_port) {
  (void)native_port;
  return false;
}

void ReleaseUpnpUdpMapping(uint16_t native_port) {
  (void)native_port;
}
#endif

bool IsBroadcastAddress(uint32_t host_order_addr) {
  return host_order_addr == 0xFFFFFFFFu || host_order_addr == 0 ||
         (host_order_addr & 0xFFu) == 0xFFu;
}

std::string Ipv4HostOrderToString(uint32_t host_order_addr) {
  return fmt::format("{}.{}.{}.{}", (host_order_addr >> 24) & 0xFF,
                     (host_order_addr >> 16) & 0xFF, (host_order_addr >> 8) & 0xFF,
                     host_order_addr & 0xFF);
}

bool ParseIpv4HostOrder(std::string_view text, uint32_t* out_addr) {
  if (!out_addr || text.empty()) {
    return false;
  }

  const std::string text_string(text);
  in_addr addr = {};
  if (inet_pton(AF_INET, text_string.c_str(), &addr) != 1) {
    return false;
  }

  *out_addr = ntohl(addr.s_addr);
  return true;
}

int SendToNative(uint64_t native_handle, const uint8_t* buf, uint32_t buf_len, uint32_t flags,
                 const N_XSOCKADDR_IN* to, uint32_t to_len) {
  sockaddr_in nto = {};
  if (to) {
    nto.sin_addr.s_addr = htonl(static_cast<uint32_t>(to->sin_addr));
    nto.sin_family = to->sin_family;
    nto.sin_port = htons(static_cast<uint16_t>(to->sin_port));
  }

  return sendto(native_handle, reinterpret_cast<const char*>(buf), buf_len, flags,
                to ? reinterpret_cast<sockaddr*>(&nto) : nullptr, to_len);
}

bool ParseGenericSockaddrIn(const N_XSOCKADDR* name, int name_len, N_XSOCKADDR_IN* out) {
  if (!name || !out || name_len < static_cast<int>(sizeof(N_XSOCKADDR_IN)) ||
      name->address_family != 2) {
    return false;
  }

  const auto* data = reinterpret_cast<const uint8_t*>(name->sa_data);
  out->sin_family = name->address_family;
  out->sin_port = static_cast<uint16_t>((uint16_t(data[0]) << 8) | uint16_t(data[1]));
  out->sin_addr = (uint32_t(data[2]) << 24) | (uint32_t(data[3]) << 16) |
                  (uint32_t(data[4]) << 8) | uint32_t(data[5]);
  std::memset(out->x_sin_zero, 0, sizeof(out->x_sin_zero));
  return true;
}

int ConnectNative(uint64_t native_handle, const N_XSOCKADDR_IN* to, uint32_t to_len) {
  sockaddr_in nto = {};
  if (to) {
    nto.sin_addr.s_addr = htonl(static_cast<uint32_t>(to->sin_addr));
    nto.sin_family = to->sin_family;
    nto.sin_port = htons(static_cast<uint16_t>(to->sin_port));
  }

  return connect(native_handle, reinterpret_cast<sockaddr*>(&nto), to_len);
}

// Performs the actual (blocking) web search + local-session filtering. Never
// touches the cache; callers decide whether to run this synchronously or on a
// background thread.
std::vector<XLiveSessionSummary> FetchBroadcastBridgeSessions(KernelState* kernel_state) {
  auto sessions = XLiveWebClient::Get().SearchSessions(
      kernel_state, 0, REXCVAR_GET(xlive_web_bridge_broadcast_max_hosts), 1, false);
  const size_t before_filter = sessions.size();
  sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                [](const XLiveSessionSummary& session) {
                                  return XLiveWebClient::Get().IsSessionCreatedLocally(
                                      XnkidToUint64(session.info.sessionID));
                                }),
                 sessions.end());
  if (before_filter != sessions.size() && REXCVAR_GET(xlive_web_bridge_log_packets)) {
    REXSYS_INFO("XLive web bridge filtered {} local-process System Link session(s)",
                before_filter - sessions.size());
  }
  if (REXCVAR_GET(xlive_web_bridge_log_packets)) {
    if (sessions.empty()) {
      REXSYS_INFO("XLive web bridge found no remote System Link sessions");
    } else {
      REXSYS_INFO("XLive web bridge cached {} System Link session(s)", sessions.size());
    }
  }
  return sessions;
}

// Publishes freshly fetched sessions into the cache and clears the in-flight
// flag. Always call this to close out a refresh (sync or async) so the flag can
// never get stuck true.
void StoreBroadcastBridgeSessions(std::vector<XLiveSessionSummary> sessions) {
  std::lock_guard lock(g_broadcast_bridge_cache.mutex);
  g_broadcast_bridge_cache.valid = true;
  g_broadcast_bridge_cache.refreshed_at = Clock::now();
  g_broadcast_bridge_cache.sessions = std::move(sessions);
  g_broadcast_bridge_cache.refresh_in_flight = false;
}

std::vector<XLiveSessionSummary> GetBroadcastBridgeSessions(KernelState* kernel_state) {
  const auto now = Clock::now();
  const auto cache_duration =
      std::chrono::milliseconds(REXCVAR_GET(xlive_web_bridge_broadcast_cache_ms));

  bool need_sync_fetch = false;
  bool kick_async_refresh = false;
  std::vector<XLiveSessionSummary> snapshot;
  {
    std::lock_guard lock(g_broadcast_bridge_cache.mutex);
    if (g_broadcast_bridge_cache.valid) {
      snapshot = g_broadcast_bridge_cache.sessions;
      const bool fresh = now - g_broadcast_bridge_cache.refreshed_at < cache_duration;
      if (fresh) {
        if (REXCVAR_GET(xlive_web_bridge_log_packets)) {
          REXSYS_INFO("XLive web bridge using cached {} System Link session(s)",
                      snapshot.size());
        }
        return snapshot;
      }
      // Stale but usable. Serve it immediately (stale-while-revalidate) and kick
      // a background refresh -- unless one is already running, or the local
      // player is already in a match, in which case we suspend the bridge's web
      // polling entirely and just keep serving the last known session list. The
      // advertised host/port of a joined session does not change mid-match, so
      // the cached data stays correct for port rewriting.
      if (!g_broadcast_bridge_cache.refresh_in_flight &&
          !g_broadcast_bridge_shutting_down.load(std::memory_order_acquire) &&
          !XLiveWebClient::Get().IsInActiveSession()) {
        g_broadcast_bridge_cache.refresh_in_flight = true;
        kick_async_refresh = true;
      }
    } else {
      // Cold cache: nothing to serve yet, so fetch synchronously this once. This
      // only happens while browsing System Link (before a match starts), never
      // on the in-match packet-send path, so it does not cause the hitch.
      need_sync_fetch = true;
      g_broadcast_bridge_cache.refresh_in_flight = true;
    }
  }

  if (kick_async_refresh) {
    if (REXCVAR_GET(xlive_web_bridge_log_packets)) {
      REXSYS_INFO("XLive web bridge refreshing System Link session cache (background)");
    }
    g_broadcast_bridge_refreshes_active.fetch_add(1, std::memory_order_acq_rel);
    try {
      std::thread([kernel_state]() {
        StoreBroadcastBridgeSessions(FetchBroadcastBridgeSessions(kernel_state));
        g_broadcast_bridge_refreshes_active.fetch_sub(1, std::memory_order_acq_rel);
      }).detach();
    } catch (...) {
      // Thread creation failed; undo the bookkeeping so we retry next time
      // instead of getting stuck with refresh_in_flight pinned true.
      g_broadcast_bridge_refreshes_active.fetch_sub(1, std::memory_order_acq_rel);
      std::lock_guard lock(g_broadcast_bridge_cache.mutex);
      g_broadcast_bridge_cache.refresh_in_flight = false;
    }
    return snapshot;
  }

  if (need_sync_fetch) {
    if (REXCVAR_GET(xlive_web_bridge_log_packets)) {
      REXSYS_INFO("XLive web bridge refreshing System Link session cache");
    }
    snapshot = FetchBroadcastBridgeSessions(kernel_state);
    StoreBroadcastBridgeSessions(snapshot);
    return snapshot;
  }

  // Stale cache with a refresh already in flight, or bridge suspended in-match:
  // serve the last known sessions without blocking.
  return snapshot;
}

uint32_t SessionHostAddress(const XLiveSessionSummary& session) {
  const uint32_t online = static_cast<uint32_t>(session.info.hostAddress.inaOnline);
  return online ? online : static_cast<uint32_t>(session.info.hostAddress.ina);
}

uint16_t SessionHostPort(const XLiveSessionSummary& session, uint16_t fallback_port) {
  const uint16_t session_port = static_cast<uint16_t>(session.info.hostAddress.wPortOnline);
  return session_port ? session_port : fallback_port;
}

uint16_t NativePortForGuestPort(uint16_t guest_port) {
  if (!guest_port) {
    return 0;
  }

  const uint32_t native_port = uint32_t(guest_port) + REXCVAR_GET(systemlink_port_offset);
  if (native_port > 65535) {
    REXSYS_WARN("System Link port offset would move guest port {} beyond 65535; using guest port",
                guest_port);
    return guest_port;
  }
  return static_cast<uint16_t>(native_port);
}

bool IsGuestSystemLinkPort(uint16_t guest_port) {
  const uint32_t base_port = REXCVAR_GET(systemlink_base_port);
  if (!base_port || base_port > 65535) {
    return false;
  }

  return guest_port == base_port || (base_port > 1 && guest_port == base_port - 1) ||
         (base_port <= 65523 && guest_port == base_port + 12);
}

uint32_t LocalPublicAddress(KernelState* kernel_state) {
  const auto status = XLiveWebClient::Get().Probe(kernel_state);
  uint32_t public_address = 0;
  if (!status.public_address.empty()) {
    ParseIpv4HostOrder(status.public_address, &public_address);
  }
  return public_address;
}

uint32_t BridgeAddressForClient(uint32_t host_address, uint32_t local_public_address) {
  if (REXCVAR_GET(xlive_web_bridge_loopback_same_public_ip) && local_public_address &&
      host_address == local_public_address) {
    return 0x7F000001u;
  }
  return host_address;
}

bool ShouldRewriteSamePublicUnicast(KernelState* kernel_state, const N_XSOCKADDR_IN* to,
                                    uint32_t destination) {
  if (!to || !REXCVAR_GET(xlive_web_bridge_systemlink_broadcast) ||
      !REXCVAR_GET(xlive_web_bridge_loopback_same_public_ip) ||
      IsBroadcastAddress(destination) || destination == 0x7F000001u) {
    return false;
  }

  const uint32_t port = static_cast<uint16_t>(to->sin_port);
  if (!IsGuestSystemLinkPort(static_cast<uint16_t>(port))) {
    return false;
  }

  return destination == LocalPublicAddress(kernel_state);
}

bool TryRewriteSystemLinkPortForSession(KernelState* kernel_state, N_XSOCKADDR_IN* to,
                                        uint32_t original_destination) {
  if (!to || !REXCVAR_GET(xlive_web_bridge_systemlink_broadcast) ||
      IsBroadcastAddress(static_cast<uint32_t>(to->sin_addr))) {
    return false;
  }

  const uint16_t guest_port = static_cast<uint16_t>(to->sin_port);
  if (!IsGuestSystemLinkPort(guest_port)) {
    return false;
  }

  const uint32_t base_port = REXCVAR_GET(systemlink_base_port);
  if (!base_port || base_port > 65535) {
    return false;
  }

  const uint32_t destination = static_cast<uint32_t>(to->sin_addr);
  const uint32_t local_public_address = LocalPublicAddress(kernel_state);
  const uint32_t lookup_address = original_destination ? original_destination : destination;

  for (const auto& session : GetBroadcastBridgeSessions(kernel_state)) {
    const uint32_t host_address = SessionHostAddress(session);
    const bool matches_destination = host_address && host_address == lookup_address;
    const bool matches_same_public_loopback =
        destination == 0x7F000001u && local_public_address && host_address == local_public_address;
    if (!matches_destination && !matches_same_public_loopback) {
      continue;
    }

    const uint16_t advertised_port =
        SessionHostPort(session, static_cast<uint16_t>(base_port));
    if (advertised_port <= base_port) {
      continue;
    }

    const uint32_t remote_offset = uint32_t(advertised_port) - base_port;
    const uint32_t native_port = uint32_t(guest_port) + remote_offset;
    if (native_port > 65535) {
      REXSYS_WARN(
          "XLive web bridge cannot rewrite loopback System Link port {} with offset {}; "
          "native port exceeds 65535",
          guest_port, remote_offset);
      return false;
    }

    to->sin_port = static_cast<uint16_t>(native_port);
    if (REXCVAR_GET(xlive_web_bridge_log_packets)) {
      REXSYS_INFO(
          "XLive web bridge rewrote System Link port {} -> {} using session {:016X} "
          "advertised at {}:{}",
          guest_port, native_port, XnkidToUint64(session.info.sessionID),
          Ipv4HostOrderToString(original_destination ? original_destination : host_address),
          advertised_port);
    }
    return true;
  }

  return false;
}

bool PayloadContains(std::string_view haystack, std::string_view needle) {
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) !=
         haystack.end();
}

bool IsSystemLinkInfoProbe(const uint8_t* buf, uint32_t buf_len) {
  if (!buf || !buf_len) {
    return false;
  }
  const std::string_view payload(reinterpret_cast<const char*>(buf), buf_len);
  return PayloadContains(payload, "getinfo");
}

bool IsSystemLinkInfoResponse(const uint8_t* buf, uint32_t buf_len) {
  if (!buf || !buf_len) {
    return false;
  }
  const std::string_view payload(reinterpret_cast<const char*>(buf), buf_len);
  return PayloadContains(payload, "infoResponse") && PayloadContains(payload, "\\xnkid\\");
}

std::string CleanInfoValue(std::string_view value) {
  while (!value.empty() &&
         (value.back() == '\0' || value.back() == '\n' || value.back() == '\r')) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

std::map<std::string, std::string> ParseInfoValues(std::string_view payload) {
  std::map<std::string, std::string> values;
  size_t pos = payload.find('\\');
  while (pos != std::string_view::npos && pos + 1 < payload.size()) {
    const size_t key_start = pos + 1;
    const size_t key_end = payload.find('\\', key_start);
    if (key_end == std::string_view::npos || key_end == key_start) {
      break;
    }

    const size_t value_start = key_end + 1;
    size_t value_end = payload.find('\\', value_start);
    if (value_end == std::string_view::npos) {
      value_end = payload.find('\n', value_start);
    }
    if (value_end == std::string_view::npos) {
      value_end = payload.size();
    }

    values.emplace(std::string(payload.substr(key_start, key_end - key_start)),
                   CleanInfoValue(payload.substr(value_start, value_end - value_start)));
    pos = value_end;
  }
  return values;
}

bool ParseHexU64(std::string_view text, uint64_t* out_value) {
  if (!out_value) {
    return false;
  }
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

uint32_t ParseU32Decimal(std::string_view text, uint32_t fallback = 0) {
  uint32_t parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
  return result.ec == std::errc() ? parsed : fallback;
}

std::string InfoValue(const std::map<std::string, std::string>& values, std::string_view key) {
  const auto it = values.find(std::string(key));
  return it == values.end() ? std::string() : it->second;
}

void PublishSystemLinkInfoResponse(KernelState* kernel_state, const uint8_t* buf,
                                   uint32_t buf_len) {
  if (!IsSystemLinkInfoResponse(buf, buf_len)) {
    return;
  }

  const std::string_view payload(reinterpret_cast<const char*>(buf), buf_len);
  const auto values = ParseInfoValues(payload);

  uint64_t session_id = 0;
  if (!ParseHexU64(InfoValue(values, "xnkid"), &session_id)) {
    return;
  }

  XLiveWebClient::Get().UpdateAdvertisedSessionInfo(
      kernel_state, session_id, InfoValue(values, "mapname"),
      InfoValue(values, "hostname"), InfoValue(values, "gametype"),
      ParseU32Decimal(InfoValue(values, "clients")),
      ParseU32Decimal(InfoValue(values, "sv_maxclients")));
}

void StoreU16BE(std::array<uint8_t, 36>& bytes, size_t offset, uint16_t value) {
  bytes[offset + 0] = static_cast<uint8_t>((value >> 8) & 0xFF);
  bytes[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

void StoreU32BE(std::array<uint8_t, 36>& bytes, size_t offset, uint32_t value) {
  bytes[offset + 0] = static_cast<uint8_t>((value >> 24) & 0xFF);
  bytes[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
  bytes[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
  bytes[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

std::string HexWordsFromBytes(const uint8_t* bytes, size_t byte_count) {
  std::string value;
  value.reserve((byte_count / 4) * 8);
  for (size_t i = 0; i + 3 < byte_count; i += 4) {
    const uint32_t word = (uint32_t(bytes[i + 0]) << 24) | (uint32_t(bytes[i + 1]) << 16) |
                          (uint32_t(bytes[i + 2]) << 8) | uint32_t(bytes[i + 3]);
    value += fmt::format("{:08x}", word);
  }
  return value;
}

std::string XnAddrToInfoString(const XLiveSessionSummary& session, uint32_t host_address,
                               uint16_t host_port) {
  std::array<uint8_t, 36> bytes = {};
  StoreU32BE(bytes, 0, host_address);
  StoreU32BE(bytes, 4, host_address);
  StoreU16BE(bytes, 8, host_port);
  std::memcpy(bytes.data() + 10, session.info.hostAddress.abEnet,
              sizeof(session.info.hostAddress.abEnet));
  std::memcpy(bytes.data() + 16, session.info.hostAddress.abOnline,
              sizeof(session.info.hostAddress.abOnline));
  return HexWordsFromBytes(bytes.data(), bytes.size());
}

std::string XnKeyToInfoString(const XLiveSessionSummary& session) {
  return HexWordsFromBytes(session.info.keyExchangeKey.ab, sizeof(session.info.keyExchangeKey.ab));
}

std::string XnkidToInfoString(const XLiveSessionSummary& session) {
  return fmt::format("{:016x}", XnkidToUint64(session.info.sessionID));
}

void AppendInfoValue(std::string& info, std::string_view key, std::string_view value) {
  info.push_back('\\');
  info.append(key);
  info.push_back('\\');
  info.append(value);
}

uint32_t SaturatingSub(uint32_t lhs, uint32_t rhs) {
  return lhs >= rhs ? lhs - rhs : 0;
}

std::vector<uint8_t> BuildSyntheticInfoResponse(const XLiveSessionSummary& session,
                                                uint32_t host_address, uint16_t host_port) {
  const uint32_t filled_slots =
      session.advertised_clients
          ? session.advertised_clients
          : session.filled_public_slots + session.filled_private_slots;
  const uint32_t max_slots =
      session.advertised_max_clients ? session.advertised_max_clients
                                     : session.public_slots + session.private_slots;
  const uint32_t open_public = session.open_public_slots;
  const uint32_t open_private = session.open_private_slots;
  const uint64_t session_id = XnkidToUint64(session.info.sessionID);
  const std::string host_name =
      session.host_name.empty() ? fmt::format("NX1 Netplay {:04X}", session_id & 0xFFFF)
                                : session.host_name;
  const std::string map_name = session.map_name.empty() ? "mp_unknown" : session.map_name;
  const std::string game_type = session.game_type.empty() ? "war" : session.game_type;

  std::string info;
  info.reserve(768);
  AppendInfoValue(info, "protocol", "147");
  AppendInfoValue(info, "hostname", host_name);
  AppendInfoValue(info, "mapname", map_name);
  AppendInfoValue(info, "clients", fmt::format("{}", std::max<uint32_t>(1, filled_slots)));
  AppendInfoValue(info, "sv_maxclients", fmt::format("{}", std::max<uint32_t>(1, max_slots)));
  AppendInfoValue(info, "gametype", game_type);
  AppendInfoValue(info, "game", "");
  AppendInfoValue(info, "nettype", "1");
  AppendInfoValue(info, "minping", "0");
  AppendInfoValue(info, "maxping", "999");
  AppendInfoValue(info, "xnaddr", XnAddrToInfoString(session, host_address, host_port));
  AppendInfoValue(info, "xnkey", XnKeyToInfoString(session));
  AppendInfoValue(info, "xnkid", XnkidToInfoString(session));
  AppendInfoValue(info, "pslots", fmt::format("{}", session.public_slots));
  AppendInfoValue(info, "pused", fmt::format("{}", SaturatingSub(session.public_slots, open_public)));
  AppendInfoValue(info, "prslots", fmt::format("{}", session.private_slots));
  AppendInfoValue(info, "prused",
                  fmt::format("{}", SaturatingSub(session.private_slots, open_private)));
  AppendInfoValue(info, "nonce", fmt::format("{:016x}", session_id ^ 0x024E58310001584Eull));
  AppendInfoValue(info, "mod", "0");
  AppendInfoValue(info, "hw", "1");
  AppendInfoValue(info, "st", "0");
  AppendInfoValue(info, "kc", "0");
  AppendInfoValue(info, "ff", "0");

  std::string payload = "\xFF\xFF\xFF\xFF";
  payload += "infoResponse\n";
  payload += info;
  payload.push_back('\n');
  return std::vector<uint8_t>(payload.begin(), payload.end());
}

}  // namespace

void ShutdownBroadcastBridge() {
  g_broadcast_bridge_shutting_down.store(true, std::memory_order_release);

  // Wait (bounded) for any in-flight background refresh to finish touching the
  // web client / KernelState. The web request is capped by xlive_web_timeout_ms
  // (default 2s); we give it a little longer before giving up so we don't hang
  // shutdown if the network stack wedges.
  using namespace std::chrono_literals;
  for (int i = 0; i < 500 &&
                  g_broadcast_bridge_refreshes_active.load(std::memory_order_acquire) > 0;
       ++i) {
    std::this_thread::sleep_for(10ms);
  }
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (type_ == Type(2) && !EnableDatagramPortReuse(native_handle_)) {
    REXSYS_WARN("Unable to enable UDP port reuse; multiple local instances may not bind: {}",
                GetLastSocketError());
  }
  if (type_ == Type(2) && !DisableUdpConnectionReset(native_handle_)) {
    REXSYS_WARN("Unable to disable UDP connection reset notifications: {}", GetLastSocketError());
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  if (upnp_mapping_active_) {
    ReleaseUpnpUdpMapping(upnp_native_port_);
    upnp_mapping_active_ = false;
    upnp_native_port_ = 0;
  }

  int ret = rex::net::socket_close(native_handle_);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  // Xbox SO_BROADCAST lives at SOL_SOCKET(0xFFFF)/SO_BROADCAST(0x0020), but
  // WinSock's SOL_SOCKET value is different. Treat it as a bridge hint even if
  // the native option fails so synthetic LAN replies still work.
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
    int ret =
        setsockopt(native_handle_, SOL_SOCKET, SO_BROADCAST, (char*)optval_ptr, optlen);
    if (ret < 0) {
      REXSYS_WARN("Unable to enable native UDP broadcast: {}", GetLastSocketError());
    }
    return X_STATUS_SUCCESS;
  }

  int ret = setsockopt(native_handle_, level, optname, (char*)optval_ptr, optlen);
  if (ret < 0) {
    REXSYS_WARN("Socket setsockopt failed level={} opt={} error={}", level, optname,
                GetLastSocketError());
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  int ret = rex::net::socket_ioctl(native_handle_, cmd, arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  N_XSOCKADDR_IN parsed_to = {};
  if (ParseGenericSockaddrIn(name, name_len, &parsed_to)) {
    const bool bridge_udp_socket =
        type_ == Type(2) && (proto_ == Protocol(17) || proto_ == Protocol(254));
    const uint32_t destination = static_cast<uint32_t>(parsed_to.sin_addr);
    N_XSOCKADDR_IN native_to = parsed_to;

    if (bridge_udp_socket && ShouldRewriteSamePublicUnicast(kernel_state_, &parsed_to, destination)) {
      native_to.sin_addr = 0x7F000001u;
      TryRewriteSystemLinkPortForSession(kernel_state_, &native_to, destination);
      if (REXCVAR_GET(xlive_web_bridge_log_packets)) {
        REXSYS_INFO("XLive web bridge rewrote System Link connect {}:{} to 127.0.0.1:{}",
                    Ipv4HostOrderToString(destination), static_cast<uint16_t>(parsed_to.sin_port),
                    static_cast<uint16_t>(native_to.sin_port));
      }
    } else if (bridge_udp_socket) {
      TryRewriteSystemLinkPortForSession(kernel_state_, &native_to, destination);
      if (REXCVAR_GET(xlive_web_bridge_log_packets)) {
        REXSYS_INFO("XLive web bridge UDP connect to {}:{} native {}:{}",
                    Ipv4HostOrderToString(destination), static_cast<uint16_t>(parsed_to.sin_port),
                    Ipv4HostOrderToString(static_cast<uint32_t>(native_to.sin_addr)),
                    static_cast<uint16_t>(native_to.sin_port));
      }
    }

    int ret = ConnectNative(native_handle_, &native_to, name_len);
    if (ret < 0) {
      REXSYS_WARN("Socket connect failed for {}:{} error={}",
                  Ipv4HostOrderToString(destination), static_cast<uint16_t>(parsed_to.sin_port),
                  GetLastSocketError());
      return X_STATUS_UNSUCCESSFUL;
    }

    connected_ = true;
    connected_to_ = native_to;
    connected_send_log_count_ = 0;
    return X_STATUS_SUCCESS;
  }

  int ret = connect(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  connected_ = true;
  connected_send_log_count_ = 0;
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  N_XSOCKADDR_IN native_name = *name;
  const uint16_t guest_port = static_cast<uint16_t>(name->sin_port);
  const bool bridge_udp_socket =
      type_ == Type(2) && (proto_ == Protocol(17) || proto_ == Protocol(254));
  if (type_ == Type(2)) {
    native_name.sin_port = NativePortForGuestPort(guest_port);
  }
  if (bridge_udp_socket && IsGuestSystemLinkPort(guest_port) &&
      static_cast<uint32_t>(native_name.sin_addr) == 0x7F000001u) {
    native_name.sin_addr = 0;
    if (REXCVAR_GET(xlive_web_bridge_log_packets)) {
      REXSYS_INFO("XLive web bridge widened System Link bind 127.0.0.1:{} to 0.0.0.0:{}",
                  guest_port, static_cast<uint16_t>(native_name.sin_port));
    }
  }

  int ret = bind(native_handle_, (sockaddr*)&native_name, name_len);
  if (ret < 0) {
    REXSYS_WARN("Socket bind failed for guest port {} native port {}: {}",
                name ? uint16_t(name->sin_port) : 0,
                name ? uint16_t(native_name.sin_port) : 0, GetLastSocketError());
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = guest_port;

  if (type_ == Type(2) && bound_port_) {
    if (uint16_t(native_name.sin_port) != bound_port_) {
      REXSYS_INFO("UDP socket bound to guest port {} on native port {}", bound_port_,
                  uint16_t(native_name.sin_port));
    } else {
      REXSYS_INFO("UDP socket bound to port {}", bound_port_);
    }
  }

  if (bridge_udp_socket && IsGuestSystemLinkPort(guest_port) && uint16_t(native_name.sin_port)) {
    upnp_native_port_ = uint16_t(native_name.sin_port);
    upnp_mapping_active_ = AcquireUpnpUdpMapping(upnp_native_port_);
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr n_sockaddr;
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(native_handle_, &n_sockaddr, &n_name_len);
  if (ret == -1) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  return shutdown(native_handle_, how);
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      if (from) {
        from->sin_family = 2;
        from->sin_addr = pkt->src_ip;
        from->sin_port = pkt->src_port;
        std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
      }
      if (from_len) {
        *from_len = sizeof(N_XSOCKADDR_IN);
      }

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }

  sockaddr_in nfrom;
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nfrom, &nfromlen);
  if (ret < 0) {
#if REX_PLATFORM_WIN32
    if (type_ == Type(2) && WSAGetLastError() == WSAECONNRESET) {
      WSASetLastError(WSAEWOULDBLOCK);
    }
#endif
    return ret;
  }

  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);  // BE <- BE
    from->sin_port = ntohs(nfrom.sin_port);
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  const int ret = send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
  const bool bridge_udp_socket =
      connected_ && type_ == Type(2) && (proto_ == Protocol(17) || proto_ == Protocol(254));
  if (bridge_udp_socket && REXCVAR_GET(xlive_web_bridge_log_packets)) {
    ++connected_send_log_count_;
    if (connected_send_log_count_ <= 32 || (connected_send_log_count_ % 256) == 0 || ret < 0) {
      REXSYS_INFO("XLive web bridge connected UDP send {} byte(s) to {}:{} result={}",
                  buf_len, Ipv4HostOrderToString(static_cast<uint32_t>(connected_to_.sin_addr)),
                  static_cast<uint16_t>(connected_to_.sin_port), ret);
    }
  }
  return ret;
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  const bool bridge_udp_socket =
      to && type_ == Type(2) && (proto_ == Protocol(17) || proto_ == Protocol(254));
  const uint32_t destination = to ? static_cast<uint32_t>(to->sin_addr) : 0;
  const uint16_t destination_port = to ? static_cast<uint16_t>(to->sin_port) : 0;
  const bool info_probe = IsSystemLinkInfoProbe(buf, buf_len);
  if (bridge_udp_socket) {
    PublishSystemLinkInfoResponse(kernel_state_, buf, buf_len);
  }
  N_XSOCKADDR_IN rewritten_to = {};
  N_XSOCKADDR_IN* native_to = to;
  if (bridge_udp_socket && ShouldRewriteSamePublicUnicast(kernel_state_, to, destination)) {
    rewritten_to = *to;
    rewritten_to.sin_addr = 0x7F000001u;
    TryRewriteSystemLinkPortForSession(kernel_state_, &rewritten_to, destination);
    native_to = &rewritten_to;
    if (REXCVAR_GET(xlive_web_bridge_log_packets)) {
      REXSYS_INFO("XLive web bridge rewrote System Link unicast {}:{} to 127.0.0.1:{}",
                  Ipv4HostOrderToString(destination), static_cast<uint16_t>(to->sin_port),
                  static_cast<uint16_t>(rewritten_to.sin_port));
    }
  } else if (bridge_udp_socket && destination == 0x7F000001u) {
    rewritten_to = *to;
    if (TryRewriteSystemLinkPortForSession(kernel_state_, &rewritten_to, destination)) {
      native_to = &rewritten_to;
    }
  } else if (bridge_udp_socket && IsGuestSystemLinkPort(destination_port)) {
    rewritten_to = *to;
    if (TryRewriteSystemLinkPortForSession(kernel_state_, &rewritten_to, destination)) {
      native_to = &rewritten_to;
    }
  }

  const int ret = SendToNative(native_handle_, buf, buf_len, flags, native_to, to_len);
  if (bridge_udp_socket && !info_probe && REXCVAR_GET(xlive_web_bridge_log_packets)) {
    const uint32_t native_destination = native_to ? static_cast<uint32_t>(native_to->sin_addr) : 0;
    const uint16_t native_port = native_to ? static_cast<uint16_t>(native_to->sin_port) : 0;
    const uint32_t base_port = REXCVAR_GET(systemlink_base_port);
    if (destination_port == 3074 || destination_port >= base_port ||
        IsGuestSystemLinkPort(destination_port)) {
      ++sendto_log_count_;
      if (sendto_log_count_ <= 64 || (sendto_log_count_ % 256) == 0 || ret < 0) {
        REXSYS_INFO(
            "XLive web bridge UDP sendto {} byte(s) to {}:{} native {}:{} result={} "
            "bound_port={} broadcast={}",
            buf_len, Ipv4HostOrderToString(destination), destination_port,
            Ipv4HostOrderToString(native_destination), native_port, ret, bound_port_,
            broadcast_socket_);
      }
    }
  }

  if (!REXCVAR_GET(xlive_web_bridge_systemlink_broadcast) || !bridge_udp_socket) {
    return ret;
  }

  if (info_probe && REXCVAR_GET(xlive_web_bridge_log_packets)) {
    REXSYS_INFO(
        "XLive web bridge saw System Link probe to {}.{}.{}.{}:{} broadcast_socket={}",
        (destination >> 24) & 0xFF, (destination >> 16) & 0xFF,
        (destination >> 8) & 0xFF, destination & 0xFF,
        static_cast<uint16_t>(to->sin_port), broadcast_socket_);
  }
  if (!broadcast_socket_ && !IsBroadcastAddress(destination)) {
    return ret;
  }

  const uint16_t fallback_port = static_cast<uint16_t>(to->sin_port);
  const auto sessions = GetBroadcastBridgeSessions(kernel_state_);
  const uint32_t local_public_address = LocalPublicAddress(kernel_state_);
  if (REXCVAR_GET(xlive_web_bridge_synthesize_lan_info) && info_probe) {
    uint32_t synthesized_count = 0;
    for (const auto& session : sessions) {
      const uint32_t host_address = SessionHostAddress(session);
      if (!host_address || IsBroadcastAddress(host_address) || host_address == destination) {
        continue;
      }

      const uint16_t host_port = SessionHostPort(session, fallback_port);
      const auto response = BuildSyntheticInfoResponse(session, host_address, host_port);
      if (QueuePacket(host_address, host_port, response.data(), response.size())) {
        ++synthesized_count;
      }
    }
    if (synthesized_count && REXCVAR_GET(xlive_web_bridge_log_packets)) {
      REXSYS_INFO("XLive web bridge synthesized {} LAN infoResponse packet(s)",
                  synthesized_count);
    }
    if (synthesized_count) {
      return ret;
    }
  }

  uint32_t forwarded_count = 0;
  const uint32_t session_count = static_cast<uint32_t>(sessions.size());
  for (const auto& session : sessions) {
    const uint32_t host_address = SessionHostAddress(session);
    if (!host_address || IsBroadcastAddress(host_address) || host_address == destination) {
      continue;
    }

    N_XSOCKADDR_IN remote = *to;
    remote.sin_addr = BridgeAddressForClient(host_address, local_public_address);
    remote.sin_port = SessionHostPort(session, fallback_port);
    if (SendToNative(native_handle_, buf, buf_len, flags, &remote, to_len) >= 0) {
      ++forwarded_count;
      if (REXCVAR_GET(xlive_web_bridge_log_packets)) {
        REXSYS_INFO("XLive web bridge sent {} byte System Link probe to {}.{}.{}.{}:{}",
                    buf_len, (uint32_t(remote.sin_addr) >> 24) & 0xFF,
                    (uint32_t(remote.sin_addr) >> 16) & 0xFF,
                    (uint32_t(remote.sin_addr) >> 8) & 0xFF,
                    uint32_t(remote.sin_addr) & 0xFF, uint16_t(remote.sin_port));
      }
    }
  }

  if (forwarded_count) {
    REXSYS_INFO("XLive web bridged System Link broadcast to {} host(s)", forwarded_count);
  } else if (REXCVAR_GET(xlive_web_bridge_log_packets) && session_count) {
    REXSYS_INFO("XLive web bridge had {} session(s), but no eligible remote host for {} bytes",
                session_count, buf_len);
  }
  return ret;
}

bool XSocket::HasQueuedPackets() const {
  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  return !incoming_packets_.empty();
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
