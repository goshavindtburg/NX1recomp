/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xmemory.h>
#include <rex/system/xevent.h>
#include <rex/system/xlive_web_client.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#include <WS2tcpip.h>
#elif REX_PLATFORM_LINUX
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <unistd.h>
#include <sys/socket.h>
#endif

REXCVAR_DECLARE(uint32_t, systemlink_base_port);
REXCVAR_DECLARE(uint32_t, systemlink_port_offset);
REXCVAR_DEFINE_UINT32(xlive_web_qos_rtt_min_ms, 35, "XLive",
                      "Minimum RTT reported by XNetQosLookup for XLive web System Link peers")
    .range(1, 1000);
REXCVAR_DEFINE_UINT32(xlive_web_qos_rtt_median_ms, 70, "XLive",
                      "Median RTT reported by XNetQosLookup for XLive web System Link peers")
    .range(1, 1000);
REXCVAR_DEFINE_UINT32(xlive_web_qos_up_bits_per_second, 8 * 1024 * 1024, "XLive",
                      "Upload bandwidth reported by XNetQosLookup when the title provides no value")
    .range(64000, 1000000000);
REXCVAR_DEFINE_UINT32(xlive_web_qos_down_bits_per_second, 8 * 1024 * 1024, "XLive",
                      "Download bandwidth reported by XNetQosLookup when the title provides no value")
    .range(64000, 1000000000);

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
typedef struct {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;                    // IP address (zero if not static/DHCP)
  in_addr inaOnline;              // Online IP address (zero if not online)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
} XNADDR;

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

typedef struct {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
} XNQOSINFO;

struct XNKID {
  uint8_t ab[8];
};
static_assert_size(XNKID, 0x8);

struct XNKEY {
  uint8_t ab[16];
};
static_assert_size(XNKEY, 0x10);

typedef struct {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
} XNQOS;

typedef struct {
  rex::be<uint32_t> size_of_struct;
  rex::be<uint32_t> requests_received_count;
  rex::be<uint32_t> probes_received_count;
  rex::be<uint32_t> slots_full_discards_count;
  rex::be<uint32_t> data_replies_sent_count;
  rex::be<uint32_t> data_reply_bytes_sent;
  rex::be<uint32_t> probe_replies_sent_count;
} XNQOSLISTENSTATS;

struct Xsockaddr_t {
  rex::be<uint16_t> sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = memory::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = memory::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      memory::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      memory::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      memory::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

XNetStartupParams xnet_startup_params = {};
uint32_t xnet_reserved_memory = 0;
uint64_t xnet_registered_systemlink_id = 0;

struct CachedXNetAddress {
  XNADDR address = {};
  uint64_t session_id = 0;
};

std::mutex xnet_address_cache_mutex;
std::unordered_map<uint32_t, CachedXNetAddress> xnet_address_cache;
uint32_t xnet_connect_log_count = 0;
uint32_t xnet_connect_status_log_count = 0;

constexpr uint32_t kXNetConnectStatusConnected = 0x02;
constexpr uint8_t kXnkidSystemLink = 0x00;

enum XNetQosListenFlags : uint32_t {
  kXNetQosListenEnable = 0x01,
  kXNetQosListenDisable = 0x02,
  kXNetQosListenSetData = 0x04,
  kXNetQosListenSetBitsPerSec = 0x08,
  kXNetQosListenRelease = 0x10,
};

enum XNetQosInfoFlags : uint8_t {
  kXNetQosInfoComplete = 0x01,
  kXNetQosInfoTargetContacted = 0x02,
  kXNetQosInfoTargetDisabled = 0x04,
  kXNetQosInfoDataReceived = 0x08,
  kXNetQosInfoPartialComplete = 0x10,
};

struct CachedQosListenData {
  bool enabled = false;
  uint32_t bits_per_second = 1024 * 1024;
  std::vector<uint8_t> data;
};

std::mutex xnet_qos_mutex;
std::unordered_map<uint64_t, CachedQosListenData> xnet_qos_listen_data;

uint16_t AdvertisedSystemLinkPort() {
  const uint32_t advertised_port =
      REXCVAR_GET(systemlink_base_port) + REXCVAR_GET(systemlink_port_offset);
  return advertised_port <= 65535 ? static_cast<uint16_t>(advertised_port)
                                  : static_cast<uint16_t>(REXCVAR_GET(systemlink_base_port));
}

uint64_t XnkidToUint64(const XNKID* session_key) {
  if (!session_key) {
    return 0;
  }
  uint64_t value = 0;
  std::memcpy(&value, session_key->ab, sizeof(value));
  return rex::byte_swap(value);
}

void Uint64ToXnkid(uint64_t value, XNKID* session_key) {
  if (!session_key) {
    return;
  }
  value = rex::byte_swap(value);
  std::memcpy(session_key->ab, &value, sizeof(value));
}

uint64_t GenerateSessionId(uint8_t type) {
  std::random_device random;
  std::uniform_int_distribution<uint64_t> distribution(1, 0x00FFFFFFFFFFFFFFull);
  return (uint64_t(type) << 56) | (distribution(random) & 0x00FFFFFFFFFFFFFFull);
}

void GenerateExchangeKey(XNKEY* exchange_key) {
  if (!exchange_key) {
    return;
  }
  for (size_t i = 0; i < sizeof(exchange_key->ab); ++i) {
    exchange_key->ab[i] = static_cast<uint8_t>(i);
  }
}

bool IsSystemLinkSessionId(uint64_t session_id) {
  return session_id && ((session_id >> 56) & 0xFF) == kXnkidSystemLink;
}

bool ParseIpv4HostOrder(std::string_view text, uint32_t* out_addr) {
  if (!out_addr || text.empty()) {
    return false;
  }

  in_addr addr = {};
  const std::string text_string(text);
  if (inet_pton(AF_INET, text_string.c_str(), &addr) != 1) {
    return false;
  }

  *out_addr = ntohl(addr.s_addr);
  return true;
}

void StoreResolvedXNetAddress(uint32_t host_order_addr, const XNADDR& address,
                              uint64_t session_id) {
  if (!host_order_addr) {
    return;
  }

  std::lock_guard lock(xnet_address_cache_mutex);
  xnet_address_cache[host_order_addr] = {address, session_id};
}

bool LoadResolvedXNetAddress(uint32_t host_order_addr, XNADDR* address, uint64_t* session_id) {
  std::lock_guard lock(xnet_address_cache_mutex);
  const auto it = xnet_address_cache.find(host_order_addr);
  if (it == xnet_address_cache.end()) {
    return false;
  }

  if (address) {
    *address = it->second.address;
  }
  if (session_id) {
    *session_id = it->second.session_id;
  }
  return true;
}

std::string MacToString(const uint8_t* mac) {
  if (!mac) {
    return {};
  }
  return fmt::format("{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}", mac[0], mac[1], mac[2],
                     mac[3], mac[4], mac[5]);
}

uint64_t CurrentProfileXuid() {
  auto* kernel_state = REX_KERNEL_STATE();
  const auto* profile = kernel_state ? kernel_state->user_profile() : nullptr;
  return profile ? profile->xuid() : 0;
}

void GetLocalMacAddress(uint8_t* out_mac) {
  uint64_t xuid = 0;
  auto* kernel_state = REX_KERNEL_STATE();
  if (kernel_state && kernel_state->user_profile()) {
    xuid = kernel_state->user_profile()->xuid();
  }

  out_mac[0] = 0x02;
  out_mac[1] = static_cast<uint8_t>((xuid >> 32) & 0xFF);
  out_mac[2] = static_cast<uint8_t>((xuid >> 24) & 0xFF);
  out_mac[3] = static_cast<uint8_t>((xuid >> 16) & 0xFF);
  out_mac[4] = static_cast<uint8_t>((xuid >> 8) & 0xFF);
  out_mac[5] = static_cast<uint8_t>(xuid & 0xFF);
}

uint64_t GetLocalMachineId() {
  uint8_t mac_address[6] = {};
  GetLocalMacAddress(mac_address);

  uint64_t machine_id = 0;
  for (uint8_t byte : mac_address) {
    machine_id = (machine_id << 8) | byte;
  }
  return machine_id;
}

uint32_t GetLocalIpv4NetworkOrder() {
  static bool initialized = false;
  static uint32_t cached_addr = 0;
  if (initialized) {
    return cached_addr;
  }

  initialized = true;
  cached_addr = htonl(INADDR_LOOPBACK);

  char host_name[256] = {};
  if (gethostname(host_name, sizeof(host_name)) != 0) {
    return cached_addr;
  }

  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;

  addrinfo* results = nullptr;
  if (getaddrinfo(host_name, nullptr, &hints, &results) != 0) {
    return cached_addr;
  }

  for (addrinfo* result = results; result; result = result->ai_next) {
    auto* addr = reinterpret_cast<sockaddr_in*>(result->ai_addr);
    const uint32_t host_order = ntohl(addr->sin_addr.s_addr);
    const uint8_t first_octet = static_cast<uint8_t>(host_order >> 24);
    if (first_octet == 127 || first_octet == 0) {
      continue;
    }
    cached_addr = addr->sin_addr.s_addr;
    break;
  }

  freeaddrinfo(results);
  return cached_addr;
}

void PopulateLocalXnAddr(XNADDR* addr_ptr) {
  std::memset(addr_ptr, 0, sizeof(XNADDR));
  const uint32_t local_addr = GetLocalIpv4NetworkOrder();
  addr_ptr->ina.s_addr = local_addr;
  addr_ptr->inaOnline.s_addr = local_addr;
  addr_ptr->wPortOnline = AdvertisedSystemLinkPort();
  GetLocalMacAddress(addr_ptr->abEnet);
}

bool IsLocalMacAddress(const uint8_t* mac) {
  if (!mac) {
    return false;
  }

  uint8_t local_mac[6] = {};
  GetLocalMacAddress(local_mac);
  return std::memcmp(local_mac, mac, sizeof(local_mac)) == 0;
}

bool FillXnAddrFromPlayer(const XLivePlayerSummary& player, XNADDR* addr_ptr,
                          uint64_t* session_id) {
  if (!addr_ptr || !player.host_address) {
    return false;
  }

  std::memset(addr_ptr, 0, sizeof(XNADDR));
  addr_ptr->ina.s_addr = htonl(player.host_address);
  addr_ptr->inaOnline.s_addr = htonl(player.host_address);
  addr_ptr->wPortOnline = AdvertisedSystemLinkPort();
  std::memcpy(addr_ptr->abEnet, player.mac_address.data(), player.mac_address.size());
  if (session_id) {
    *session_id = player.session_id ? player.session_id : xnet_registered_systemlink_id;
  }
  return true;
}

bool ResolveWebPlayerXnAddr(uint32_t lookup_host_order_addr, XNADDR* addr_ptr,
                            uint64_t* session_id) {
  XLivePlayerSummary player = {};
  if (!XLiveWebClient::Get().FindPlayerByAddress(REX_KERNEL_STATE(), lookup_host_order_addr,
                                                 &player)) {
    return false;
  }

  if (player.session_id) {
    XLiveSessionSummary session = {};
    if (XLiveWebClient::Get().GetSession(REX_KERNEL_STATE(), player.session_id, &session)) {
      const uint32_t host_address =
          static_cast<uint32_t>(session.info.hostAddress.inaOnline)
              ? static_cast<uint32_t>(session.info.hostAddress.inaOnline)
              : static_cast<uint32_t>(session.info.hostAddress.ina);
      std::memset(addr_ptr, 0, sizeof(XNADDR));
      addr_ptr->ina.s_addr = htonl(host_address);
      addr_ptr->inaOnline.s_addr = htonl(host_address);
      addr_ptr->wPortOnline = static_cast<uint16_t>(session.info.hostAddress.wPortOnline);
      std::memcpy(addr_ptr->abEnet, session.info.hostAddress.abEnet,
                  sizeof(session.info.hostAddress.abEnet));
      if (session_id) {
        *session_id = player.session_id;
      }
      return true;
    }
  }

  if (player.xuid == CurrentProfileXuid()) {
    return false;
  }

  return FillXnAddrFromPlayer(player, addr_ptr, session_id);
}

uint32_t GuestPointerFromArray(mapped_u32 guest_pointer_array, uint32_t index) {
  if (!guest_pointer_array) {
    return 0;
  }
  return static_cast<uint32_t>(guest_pointer_array.host_address()[index]);
}

uint64_t SessionIdFromPointerArray(mapped_u32 session_id_pointer_array, uint32_t index) {
  const uint32_t session_key_guest = GuestPointerFromArray(session_id_pointer_array, index);
  if (!session_key_guest) {
    return 0;
  }
  return XnkidToUint64(REX_KERNEL_MEMORY()->TranslateVirtual<XNKID*>(session_key_guest));
}

bool CopyCachedQosData(uint64_t session_id, std::vector<uint8_t>* out_data) {
  if (!session_id || !out_data) {
    return false;
  }

  std::lock_guard lock(xnet_qos_mutex);
  const auto it = xnet_qos_listen_data.find(session_id);
  if (it == xnet_qos_listen_data.end()) {
    return false;
  }
  *out_data = it->second.data;
  return true;
}

void FillSuccessfulQosInfo(XNQOSINFO* info, const std::vector<uint8_t>& data,
                           uint32_t bits_per_second) {
  const uint32_t rtt_min =
      std::clamp<uint32_t>(REXCVAR_GET(xlive_web_qos_rtt_min_ms), 1, 0xFFFF);
  const uint32_t rtt_median = std::clamp<uint32_t>(
      std::max(REXCVAR_GET(xlive_web_qos_rtt_median_ms), rtt_min), 1, 0xFFFF);
  const uint32_t fallback_up = REXCVAR_GET(xlive_web_qos_up_bits_per_second);
  const uint32_t fallback_down = REXCVAR_GET(xlive_web_qos_down_bits_per_second);

  std::memset(info, 0, sizeof(*info));
  info->flags = kXNetQosInfoComplete | kXNetQosInfoTargetContacted;
  info->probes_xmit = 4;
  info->probes_recv = 4;
  info->rtt_min_in_msecs = static_cast<uint16_t>(rtt_min);
  info->rtt_med_in_msecs = static_cast<uint16_t>(rtt_median);
  info->up_bits_per_sec = bits_per_second ? bits_per_second : fallback_up;
  info->down_bits_per_sec = bits_per_second ? bits_per_second : fallback_down;

  if (!data.empty()) {
    const uint32_t data_guest =
        REX_KERNEL_MEMORY()->SystemHeapAlloc(static_cast<uint32_t>(data.size()));
    std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual(data_guest), data.data(), data.size());
    info->data_ptr = data_guest;
    info->data_len = static_cast<uint16_t>(std::min<size_t>(data.size(), 0xFFFF));
    info->flags |= kXNetQosInfoDataReceived;
  }
}

void SignalXNetEvent(uint32_t event_handle) {
  if (!event_handle) {
    return;
  }
  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
  assert_not_null(ev);
  ev->Set(0, false);
}

u32 NetDll_XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  if (!xnet_reserved_memory) {
    // Retail XNet startup consumes title-visible system memory. Some games
    // measure that delta and feed it into their own memory tracker.
    xnet_reserved_memory =
        REX_KERNEL_MEMORY()->SystemHeapAlloc(64 * 1024, 4096, memory::kSystemHeapPhysical);
  }

  return 0;
}

u32 NetDll_XNetCleanup_entry(u32 caller, mapped_void params) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  if (xnet_reserved_memory) {
    REX_KERNEL_MEMORY()->SystemHeapFree(xnet_reserved_memory);
    xnet_reserved_memory = 0;
  }

  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  // For now, constant values.
  // This makes replicating things easier.
  std::memset(buffer_ptr, 0xBB, length);

  return 0;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version;
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  // DEBUG
  /*
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}

u32 NetDll_WSACleanup_entry(u32 caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket, ppc_ptr_t<XWSABUF> buffers_ptr,
                             u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                             ppc_ptr_t<XSOCKADDR_IN> from_addr,
                             mapped_u32 from_len_ptr,
                             ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                             mapped_void completion_routine_ptr) {
  if (overlapped_ptr || completion_routine_ptr) {
    REXKRNL_WARN("WSARecvFrom overlapped completion is not implemented yet");
  }

  auto socket_obj = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket);
  if (!socket_obj) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    return -1;
  }

  uint32_t total_size = 0;
  for (uint32_t i = 0; i < buffer_count; ++i) {
    total_size += buffers_ptr[i].len;
  }

  std::vector<uint8_t> combined(total_size);
  N_XSOCKADDR_IN native_from;
  uint32_t native_from_len = from_len_ptr ? from_len_ptr.value() : sizeof(XSOCKADDR_IN);
  const uint32_t flags = flags_ptr ? flags_ptr.value() : 0;
  int ret = socket_obj->RecvFrom(combined.data(), total_size, flags, &native_from,
                                 from_addr ? &native_from_len : nullptr);

  if (ret == -1) {
#if REX_PLATFORM_WIN32
    XThread::SetLastError(WSAGetLastError());
#else
    XThread::SetLastError(0x0);
#endif
    if (num_bytes_recv) {
      *num_bytes_recv = 0;
    }
    return -1;
  }

  uint32_t copied = 0;
  for (uint32_t i = 0; i < buffer_count && copied < static_cast<uint32_t>(ret); ++i) {
    const uint32_t buffer_len = buffers_ptr[i].len;
    const uint32_t chunk = std::min<uint32_t>(buffer_len, static_cast<uint32_t>(ret) - copied);
    std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual(buffers_ptr[i].buf_ptr),
                combined.data() + copied, chunk);
    copied += chunk;
  }

  if (num_bytes_recv) {
    *num_bytes_recv = copied;
  }
  if (from_addr) {
    from_addr->sin_family = native_from.sin_family;
    from_addr->sin_port = native_from.sin_port;
    from_addr->sin_addr = native_from.sin_addr;
    std::memset(from_addr->x_sin_zero, 0, sizeof(from_addr->x_sin_zero));
  }
  if (from_len_ptr) {
    *from_len_ptr = native_from_len;
  }
  return 0;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                           u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  assert(!completion_routine);

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // Our sockets implementation doesn't support multiple buffers, so we need
  // to combine the buffers the game has given us!
  std::vector<uint8_t> combined_buffer_mem;
  uint32_t combined_buffer_size = 0;
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    uint8_t* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  int result = socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to,
                              to_len);
  if (result == -1) {
#if REX_PLATFORM_WIN32
    XThread::SetLastError(WSAGetLastError());
#else
    XThread::SetLastError(0x0);
#endif
    if (overlapped) {
      overlapped->internal = static_cast<uint32_t>(-1);
      if (overlapped->event_handle) {
        xboxkrnl::xeNtSetEvent(overlapped->event_handle, nullptr);
      }
    }
    if (num_bytes_sent) {
      *num_bytes_sent = 0;
    }
    return -1;
  }

  if (num_bytes_sent) {
    *num_bytes_sent = static_cast<uint32_t>(result);
  }
  if (overlapped) {
    overlapped->internal = static_cast<uint32_t>(result);
    if (overlapped->event_handle) {
      xboxkrnl::xeNtSetEvent(overlapped->event_handle, nullptr);
    }
  }

  return 0;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  uint64_t timeout_wait = (uint64_t)timeout;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, wait_all, 1, alertable,
                                                    timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  return 0;
}

u32 NetDll_WSACreateEvent_entry() {
  XEvent* ev = new XEvent(REX_KERNEL_STATE());
  ev->Initialize(true, false);
  return ev->handle();
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static const uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  PopulateLocalXnAddr(addr_ptr);

  // TODO(gibbed): A proper mac address.
  // RakNet's 360 version appears to depend on abEnet to create "random" 64-bit
  // numbers. A zero value will cause RakPeer::Startup to fail. This causes
  // 58411436 to crash on startup.
  // The 360-specific code is scrubbed from the RakNet repo, but there's still
  // traces of what it's doing which match the game code.
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L382
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4527
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4467
  // "Mac address is a poor solution because you can't have multiple connections
  // from the same system"
  return XnAddrStatus::XNET_GET_XNADDR_ETHERNET | XnAddrStatus::XNET_GET_XNADDR_STATIC |
         XnAddrStatus::XNET_GET_XNADDR_GATEWAY | XnAddrStatus::XNET_GET_XNADDR_DNS |
         XnAddrStatus::XNET_GET_XNADDR_ONLINE;
}

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr,
                                       mapped_u64 id_ptr) {
  if (!addr_ptr || !id_ptr) {
    return 0x2726;  // WSAEINVAL
  }

  uint64_t machine_id = 0;
  for (uint8_t byte : addr_ptr->abEnet) {
    machine_id = (machine_id << 8) | byte;
  }
  *id_ptr = machine_id ? machine_id : GetLocalMachineId();
  return X_ERROR_SUCCESS;
}

void NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, mapped_string string_out,
                                     u32 string_size) {
  struct in_addr addr = {};
  addr.s_addr = htonl(in_addr);
  rex::string::rex_strcpy(string_out, string_size, inet_ntoa(addr));
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                    mapped_u32 in_addr) {
  if (!xn_addr || !in_addr) {
    return 0x2726;  // WSAEINVAL
  }

  uint32_t network_addr = xn_addr->inaOnline.s_addr ? xn_addr->inaOnline.s_addr : xn_addr->ina.s_addr;
  if (!network_addr) {
    network_addr = GetLocalIpv4NetworkOrder();
  }

  uint64_t session_id = 0;
  if (xid) {
    session_id = XnkidToUint64(xid.as<XNKID*>());
  }

  const uint32_t host_order_addr = ntohl(network_addr);
  REXKRNL_INFO(
      "XNetXnAddrToInAddr input ip={:08X} online={:08X} port={} mac={} xid={:016X}",
      ntohl(xn_addr->ina.s_addr), ntohl(xn_addr->inaOnline.s_addr),
      static_cast<uint16_t>(xn_addr->wPortOnline), MacToString(xn_addr->abEnet), session_id);

  if (IsLocalMacAddress(xn_addr->abEnet)) {
    *in_addr = INADDR_LOOPBACK;
    StoreResolvedXNetAddress(INADDR_LOOPBACK, *xn_addr, session_id);
    REXKRNL_INFO("XNetXnAddrToInAddr resolved local MAC to loopback port={}",
                 static_cast<uint16_t>(xn_addr->wPortOnline));
    return X_ERROR_SUCCESS;
  }

  uint32_t public_addr = 0;
  const auto status = XLiveWebClient::Get().Probe(REX_KERNEL_STATE());
  if (!status.public_address.empty()) {
    ParseIpv4HostOrder(status.public_address, &public_addr);
  }
  if (public_addr && host_order_addr == public_addr) {
    *in_addr = INADDR_LOOPBACK;
    StoreResolvedXNetAddress(INADDR_LOOPBACK, *xn_addr, session_id);
    REXKRNL_INFO("XNetXnAddrToInAddr resolved same-public peer to loopback port={} mac={}",
                 static_cast<uint16_t>(xn_addr->wPortOnline), MacToString(xn_addr->abEnet));
    return X_ERROR_SUCCESS;
  }

  *in_addr = host_order_addr;
  StoreResolvedXNetAddress(host_order_addr, *xn_addr, session_id);
  return X_ERROR_SUCCESS;
}

// Does the reverse of the above.
u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, u32 in_addr, ppc_ptr_t<XNADDR> xn_addr,
                                    mapped_void xid) {
  if (!xn_addr) {
    return 0x2726;  // WSAEINVAL
  }

  XNADDR resolved_addr = {};
  uint64_t resolved_session_id = 0;
  if (LoadResolvedXNetAddress(in_addr, &resolved_addr, &resolved_session_id)) {
    *xn_addr = resolved_addr;
    if (xid) {
      Uint64ToXnkid(resolved_session_id ? resolved_session_id : xnet_registered_systemlink_id,
                    xid.as<XNKID*>());
    }
    REXKRNL_INFO("XNetInAddrToXnAddr resolved {:08X} from cache", in_addr);
    return X_ERROR_SUCCESS;
  }

  if (in_addr == INADDR_LOOPBACK) {
    uint32_t public_addr = 0;
    const auto status = XLiveWebClient::Get().Probe(REX_KERNEL_STATE());
    if (!status.public_address.empty()) {
      ParseIpv4HostOrder(status.public_address, &public_addr);
    }
    if (public_addr && ResolveWebPlayerXnAddr(public_addr, &resolved_addr, &resolved_session_id)) {
      *xn_addr = resolved_addr;
      StoreResolvedXNetAddress(in_addr, resolved_addr, resolved_session_id);
      if (xid) {
        Uint64ToXnkid(resolved_session_id ? resolved_session_id : xnet_registered_systemlink_id,
                      xid.as<XNKID*>());
      }
      REXKRNL_INFO("XNetInAddrToXnAddr resolved loopback through XLive web player");
      return X_ERROR_SUCCESS;
    }
  } else if (in_addr != INADDR_BROADCAST && in_addr != ntohl(GetLocalIpv4NetworkOrder()) &&
             ResolveWebPlayerXnAddr(in_addr, &resolved_addr, &resolved_session_id)) {
    *xn_addr = resolved_addr;
    StoreResolvedXNetAddress(in_addr, resolved_addr, resolved_session_id);
    if (xid) {
      Uint64ToXnkid(resolved_session_id ? resolved_session_id : xnet_registered_systemlink_id,
                    xid.as<XNKID*>());
    }
    REXKRNL_INFO("XNetInAddrToXnAddr resolved remote {:08X} through XLive web player", in_addr);
    return X_ERROR_SUCCESS;
  }

  if (in_addr == INADDR_BROADCAST || in_addr == INADDR_LOOPBACK ||
      in_addr == ntohl(GetLocalIpv4NetworkOrder())) {
    PopulateLocalXnAddr(xn_addr);
  } else {
    PopulateLocalXnAddr(xn_addr);
    xn_addr->ina.s_addr = htonl(in_addr);
    xn_addr->inaOnline.s_addr = htonl(in_addr);
  }

  if (xid) {
    auto session_key = xid.as<XNKID*>();
    Uint64ToXnkid(xnet_registered_systemlink_id, session_key);
  }

  return X_ERROR_SUCCESS;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
u32 NetDll_XNetSetSystemLinkPort_entry(u32 caller, u32 port) {
  return 1;
}

u32 NetDll_XNetGetSystemLinkPort_entry(u32 caller) {
  const uint32_t port = REXCVAR_GET(systemlink_base_port);
  return port <= 65535 ? port : 1001;
}

u32 NetDll_XNetCreateKey_entry(u32 caller, ppc_ptr_t<XNKID> session_key,
                               ppc_ptr_t<XNKEY> exchange_key) {
  if (!session_key || !exchange_key) {
    return 0x2726;  // WSAEINVAL
  }

  const uint64_t session_id = GenerateSessionId(kXnkidSystemLink);
  Uint64ToXnkid(session_id, session_key);
  GenerateExchangeKey(exchange_key);
  REXKRNL_INFO("XNetCreateKey generated systemlink session {:016X}", session_id);
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetRegisterKey_entry(u32 caller, ppc_ptr_t<XNKID> session_key,
                                 ppc_ptr_t<XNKEY> exchange_key) {
  if (!session_key || !exchange_key) {
    return 0x2726;  // WSAEINVAL
  }

  const uint64_t session_id = XnkidToUint64(session_key.host_address());
  if (IsSystemLinkSessionId(session_id)) {
    xnet_registered_systemlink_id = session_id;
    REXKRNL_INFO("XNetRegisterKey registered systemlink session {:016X}", session_id);
  } else {
    REXKRNL_INFO("XNetRegisterKey registered non-systemlink session {:016X}", session_id);
  }

  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetUnregisterKey_entry(u32 caller, ppc_ptr_t<XNKID> session_key) {
  if (!session_key) {
    return 0x2726;  // WSAEINVAL
  }

  const uint64_t session_id = XnkidToUint64(session_key.host_address());
  if (!session_id || session_id == xnet_registered_systemlink_id) {
    xnet_registered_systemlink_id = 0;
  }
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetConnect_entry(u32 caller, u32 in_addr) {
  ++xnet_connect_log_count;
  if (xnet_connect_log_count <= 64 || (xnet_connect_log_count % 256) == 0) {
    REXKRNL_INFO("XNetConnect caller={} in_addr={:08X}", caller, in_addr);
  }
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetGetConnectStatus_entry(u32 caller, u32 in_addr) {
  ++xnet_connect_status_log_count;
  if (xnet_connect_status_log_count <= 64 || (xnet_connect_status_log_count % 256) == 0) {
    REXKRNL_INFO("XNetGetConnectStatus caller={} in_addr={:08X} status={}", caller, in_addr,
                 kXNetConnectStatusConnected);
  }
  return kXNetConnectStatusConnected;
}

u32 NetDll_XNetServerToInAddr_entry(u32 caller, u32 server_addr, u32 service_id,
                                    mapped_u32 in_addr) {
  if (!server_addr || !service_id || !in_addr) {
    return 0x2726;  // WSAEINVAL
  }

  *in_addr = server_addr;
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetInAddrToServer_entry(u32 caller, u32 server_addr, mapped_u32 in_addr) {
  if (!server_addr || !in_addr) {
    return 0x2726;  // WSAEINVAL
  }

  *in_addr = server_addr;
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetTsAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> ts_addr, u32 service_id,
                                    mapped_void session_key, mapped_u32 in_addr) {
  if (!ts_addr || !service_id || !session_key || !in_addr) {
    return 0x2726;  // WSAEINVAL
  }

  const uint32_t network_addr =
      ts_addr->inaOnline.s_addr ? ts_addr->inaOnline.s_addr : ts_addr->ina.s_addr;
  *in_addr = ntohl(network_addr);
  return X_ERROR_SUCCESS;
}

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static const uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  return XEthernetStatus::XNET_ETHERNET_LINK_ACTIVE |
         XEthernetStatus::XNET_ETHERNET_LINK_100MBPS |
         XEthernetStatus::XNET_ETHERNET_LINK_FULL_DUPLEX;
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle, mapped_u32 pdns) {
  // TODO(gibbed): actually implement this
  if (pdns) {
    auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
    dns->status = 1;  // non-zero = error
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetDnsRelease_entry(u32 caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
  // Set pqos as some games will try accessing it despite non-successful result
  if (pqos) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    SignalXNetEvent(event_handle);
  }
  return 0;
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

u32 NetDll_XNetQosListen_entry(u32 caller, mapped_void id, mapped_void data, u32 data_size,
                               u32 bits_per_second, u32 flags) {
  const uint64_t session_id = id ? XnkidToUint64(id.as<XNKID*>()) : xnet_registered_systemlink_id;
  REXKRNL_INFO("XNetQosListen session={:016X} data={} bps={} flags={:08X}", session_id,
               data_size, bits_per_second, flags);

  if (!session_id) {
    return X_ERROR_SUCCESS;
  }

  std::vector<uint8_t> qos_data;
  if (data && data_size) {
    qos_data.resize(data_size);
    std::memcpy(qos_data.data(), data.host_address(), data_size);
  }

  {
    std::lock_guard lock(xnet_qos_mutex);
    auto& cached = xnet_qos_listen_data[session_id];
    if (flags & kXNetQosListenEnable) {
      cached.enabled = true;
    }
    if (flags & kXNetQosListenDisable) {
      cached.enabled = false;
    }
    if (flags & kXNetQosListenSetBitsPerSec) {
      cached.bits_per_second = bits_per_second;
    }
    if (flags & kXNetQosListenSetData) {
      cached.data = qos_data;
    }
    if (flags & kXNetQosListenRelease) {
      xnet_qos_listen_data.erase(session_id);
    }
  }

  if ((flags & kXNetQosListenSetData) && data && data_size) {
    XLiveWebClient::Get().PostQos(REX_KERNEL_STATE(), session_id, qos_data.data(),
                                  qos_data.size());
  }

  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetQosLookup_entry(u32 caller, u32 num_remote_consoles,
                               mapped_u32 remote_addresses_ptrs, mapped_u32 session_ids_ptrs,
                               mapped_u32 remote_keys_ptrs, u32 num_gateways,
                               mapped_u32 gateways_ptrs, mapped_u32 service_ids_ptrs,
                               u32 probes_count, u32 bits_per_second, u32 flags,
                               u32 event_handle, mapped_u32 pqos) {
  (void)remote_keys_ptrs;
  (void)num_gateways;
  (void)gateways_ptrs;
  (void)service_ids_ptrs;
  (void)probes_count;
  (void)flags;

  if (!pqos) {
    return 0x2726;  // WSAEINVAL
  }

  const uint32_t count = num_remote_consoles;
  const uint32_t info_count = std::max<uint32_t>(1, count);
  const uint32_t qos_size = sizeof(XNQOS) + (info_count - 1) * sizeof(XNQOSINFO);
  const uint32_t qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(qos_size);
  auto* qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
  std::memset(qos, 0, qos_size);
  qos->count = count;
  qos->count_pending = 0;
  *pqos = qos_guest;

  REXKRNL_INFO("XNetQosLookup remotes={} gateways={} probes={} bps={} flags={:08X}", count,
               num_gateways, probes_count, bits_per_second, flags);

  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t session_id = SessionIdFromPointerArray(session_ids_ptrs, i);
    std::vector<uint8_t> qos_data;
    if (!CopyCachedQosData(session_id, &qos_data)) {
      XLiveWebClient::Get().GetQos(REX_KERNEL_STATE(), session_id, &qos_data);
    }

    uint32_t remote_host = 0;
    uint16_t remote_port = 0;
    const uint32_t remote_guest = GuestPointerFromArray(remote_addresses_ptrs, i);
    if (remote_guest) {
      const auto* remote_addr = REX_KERNEL_MEMORY()->TranslateVirtual<XNADDR*>(remote_guest);
      remote_host =
          ntohl(remote_addr->inaOnline.s_addr ? remote_addr->inaOnline.s_addr
                                              : remote_addr->ina.s_addr);
      remote_port = static_cast<uint16_t>(remote_addr->wPortOnline);
    }

    FillSuccessfulQosInfo(&qos->info[i], qos_data, bits_per_second);
    REXKRNL_INFO(
        "XNetQosLookup result[{}] session={:016X} host={:08X}:{} qos_bytes={} flags={:02X}",
        i, session_id, remote_host, remote_port, qos_data.size(), qos->info[i].flags);
  }

  SignalXNetEvent(event_handle);
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetQosGetListenStats_entry(u32 caller, ppc_ptr_t<XNKID> session_id_ptr,
                                       ppc_ptr_t<XNQOSLISTENSTATS> stats_ptr) {
  const uint64_t session_id = session_id_ptr ? XnkidToUint64(session_id_ptr.host_address()) : 0;
  REXKRNL_INFO("XNetQosGetListenStats session={:016X}", session_id);
  if (stats_ptr) {
    stats_ptr->requests_received_count = 1;
    stats_ptr->probes_received_count = 1;
    stats_ptr->slots_full_discards_count = 0;
    stats_ptr->data_replies_sent_count = 1;
    stats_ptr->data_reply_bytes_sent = 1;
    stats_ptr->probe_replies_sent_count = 1;
  }
  return X_ERROR_SUCCESS;
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
  XSocket* socket = new XSocket(REX_KERNEL_STATE());
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->Release();

    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return -1;
  }

  return socket->handle();
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }
  return ret;
}

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  return XSUCCEEDED(status) ? 0 : -1;
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_void arg_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  // TODO
  return 0;
}

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> addr_ptr,
                        mapped_u32 addrlen_ptr) {
  if (!addr_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    return new_socket->handle();
  } else {
    return -1;
  }
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      assert_not_null(socket);
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }

  uint32_t KeepQueuedPackets() {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (socket->HasQueuedPackets()) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
    return new_count;
  }
};

i32 NetDll_select_entry(i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  host_set host_readfds = {};
  fd_set native_readfds = {};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {};
  fd_set native_writefds = {};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {};
  fd_set native_exceptfds = {};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }

  if (readfds) {
    host_set queued_readfds = host_readfds;
    const uint32_t queued_count = queued_readfds.KeepQueuedPackets();
    if (queued_count) {
      queued_readfds.Store(readfds);
      if (writefds) {
        host_writefds.count = 0;
        host_writefds.Store(writefds);
      }
      if (exceptfds) {
        host_exceptfds.count = 0;
        host_exceptfds.Store(exceptfds);
      }
      return queued_count;
    }
  }

  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  int ret = select(nfds, readfds ? &native_readfds : nullptr, writefds ? &native_writefds : nullptr,
                   exceptfds ? &native_exceptfds : nullptr, timeout_in);
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  // TODO(gibbed): modify ret to be what's actually copied to the guest fd_sets?
  return ret;
}

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  return socket->Recv(buf_ptr, buf_len, flags);
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret =
      socket->RecvFrom(buf_ptr, buf_len, flags, &native_from, fromlen_ptr ? &native_fromlen : 0);

  if (ret != -1 && from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (ret != -1 && fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
// TODO: Better way of getting the error code
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }

  return ret;
}

u32 NetDll_send_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  return socket->Send(buf_ptr, buf_len, flags);
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags,
                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  return socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
REX_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
REX_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
REX_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
REX_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
REX_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
REX_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
REX_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
REX_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
REX_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
REX_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
REX_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
REX_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
REX_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
REX_EXPORT(__imp__NetDll_XNetCreateKey, rex::kernel::xam::NetDll_XNetCreateKey_entry)
REX_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterKey, rex::kernel::xam::NetDll_XNetUnregisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetServerToInAddr, rex::kernel::xam::NetDll_XNetServerToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetConnect, rex::kernel::xam::NetDll_XNetConnect_entry)
REX_EXPORT(__imp__NetDll_XNetGetConnectStatus,
           rex::kernel::xam::NetDll_XNetGetConnectStatus_entry)
REX_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
REX_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
REX_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
REX_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
REX_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
REX_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
REX_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
REX_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
REX_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
REX_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
REX_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
REX_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
REX_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
REX_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
REX_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
REX_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
REX_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
REX_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
REX_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
REX_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)

REX_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
REX_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
REX_EXPORT_STUB(__imp__NetDll_UpnpStartup);
REX_EXPORT_STUB(__imp__NetDll_WSACancelOverlappedIO);
REX_EXPORT_STUB(__imp__NetDll_WSAEventSelect);
REX_EXPORT_STUB(__imp__NetDll_WSAGetOverlappedResult);
REX_EXPORT_STUB(__imp__NetDll_WSARecv);
REX_EXPORT_STUB(__imp__NetDll_WSASend);
REX_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_XHttpConnect);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
REX_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpen);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpReadData);
REX_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
REX_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
REX_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
REX_EXPORT_STUB(__imp__NetDll_XHttpStartup);
REX_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
REX_EXPORT(__imp__NetDll_XNetGetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetGetSystemLinkPort_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
REX_EXPORT(__imp__NetDll_XNetInAddrToServer, rex::kernel::xam::NetDll_XNetInAddrToServer_entry)
REX_EXPORT(__imp__NetDll_XNetQosGetListenStats,
           rex::kernel::xam::NetDll_XNetQosGetListenStats_entry)
REX_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
REX_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
REX_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
REX_EXPORT(__imp__NetDll_XNetTsAddrToInAddr, rex::kernel::xam::NetDll_XNetTsAddrToInAddr_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetUnregisterInAddr);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
REX_EXPORT_STUB(__imp__NetDll_XnpCapture);
REX_EXPORT_STUB(__imp__NetDll_XnpConfig);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
REX_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
REX_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
REX_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
REX_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
REX_EXPORT_STUB(__imp__NetDll_getpeername);
REX_EXPORT_STUB(__imp__NetDll_getsockname);
REX_EXPORT_STUB(__imp__NetDll_getsockopt);
