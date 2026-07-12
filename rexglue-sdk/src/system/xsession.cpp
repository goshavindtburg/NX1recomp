/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <random>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xsession.h>

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#elif REX_PLATFORM_LINUX
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

REXCVAR_DECLARE(uint32_t, systemlink_base_port);
REXCVAR_DECLARE(uint32_t, systemlink_port_offset);

namespace rex::system {

namespace {

constexpr uint8_t kXnkidSystemLink = 0x00;
constexpr uint32_t kSessionFlagHost = 1u << 0;
constexpr uint32_t kSessionFlagPeerNetwork = 1u << 5;

uint16_t AdvertisedSystemLinkPort() {
  const uint32_t advertised_port =
      REXCVAR_GET(systemlink_base_port) + REXCVAR_GET(systemlink_port_offset);
  return advertised_port <= 65535 ? static_cast<uint16_t>(advertised_port)
                                  : static_cast<uint16_t>(REXCVAR_GET(systemlink_base_port));
}

uint64_t GenerateSessionId(uint8_t type) {
  std::random_device random;
  std::uniform_int_distribution<uint64_t> distribution(1, 0x00FFFFFFFFFFFFFFull);
  return (uint64_t(type) << 56) | (distribution(random) & 0x00FFFFFFFFFFFFFFull);
}

void GenerateExchangeKey(XNKEY* key) {
  for (size_t i = 0; i < sizeof(key->ab); ++i) {
    key->ab[i] = static_cast<uint8_t>(i);
  }
}

void GetLocalMacAddress(KernelState* kernel_state, uint8_t* out_mac) {
  uint64_t xuid = 0;
  if (kernel_state && kernel_state->user_profile()) {
    xuid = kernel_state->user_profile()->xuid();
  }

  out_mac[0] = 0x02;
  out_mac[1] = static_cast<uint8_t>((xuid >> 32) & 0xFF);
  out_mac[2] = static_cast<uint8_t>((xuid >> 24) & 0xFF);
  out_mac[3] = static_cast<uint8_t>((xuid >> 16) & 0xFF);
  out_mac[4] = static_cast<uint8_t>((xuid >> 8) & 0xFF);
  out_mac[5] = static_cast<uint8_t>(xuid & 0xFF);

  const uint32_t port_offset = REXCVAR_GET(systemlink_port_offset);
  if (port_offset) {
    out_mac[1] ^= static_cast<uint8_t>((port_offset >> 8) & 0xFF);
    out_mac[2] ^= static_cast<uint8_t>(port_offset & 0xFF);
  }
}

uint32_t GetLocalIpv4HostOrder() {
  static bool initialized = false;
  static uint32_t cached_addr = 0x7F000001;
  if (initialized) {
    return cached_addr;
  }

  initialized = true;
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
    cached_addr = host_order;
    break;
  }

  freeaddrinfo(results);
  return cached_addr;
}

}  // namespace

uint64_t XnkidToUint64(const XNKID& session_id) {
  uint64_t value = 0;
  std::memcpy(&value, session_id.ab, sizeof(value));
  return rex::byte_swap(value);
}

void Uint64ToXnkid(uint64_t value, XNKID* session_id) {
  value = rex::byte_swap(value);
  std::memcpy(session_id->ab, &value, sizeof(value));
}

XSession::XSession(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSession::~XSession() = default;

X_STATUS XSession::Initialize() {
  auto native_object = CreateNative<X_KSESSION>();
  if (!native_object) {
    return X_STATUS_NO_MEMORY;
  }

  native_object->handle = handle();
  return X_STATUS_SUCCESS;
}

X_STATUS XSession::CreateHostSession(uint32_t flags, uint32_t public_slots,
                                     uint32_t private_slots, XSESSION_INFO* session_info,
                                     uint64_t* nonce) {
  flags_ = flags;
  max_public_slots_ = public_slots;
  max_private_slots_ = private_slots;
  available_public_slots_ = public_slots;
  available_private_slots_ = private_slots;
  actual_member_count_ = 0;
  state_ = 0;  // Lobby.

  const uint64_t incoming_session_id =
      session_info ? XnkidToUint64(session_info->sessionID) : 0;
  const bool preserve_remote_session =
      (flags & kSessionFlagPeerNetwork) && !(flags & kSessionFlagHost) &&
      incoming_session_id;
  if (preserve_remote_session) {
    session_info_ = *session_info;
    nonce_ = incoming_session_id ^ 0x024E58310001584Eull;
    if (nonce) {
      *nonce = nonce_;
    }
    REXSYS_INFO("XSession preserved remote peer session {:016X}", incoming_session_id);
    return X_STATUS_SUCCESS;
  }

  std::memset(&session_info_, 0, sizeof(session_info_));
  const uint64_t session_id = GenerateSessionId(kXnkidSystemLink);
  nonce_ = session_id ^ 0x024E58310001584Eull;

  Uint64ToXnkid(session_id, &session_info_.sessionID);
  session_info_.hostAddress.ina = GetLocalIpv4HostOrder();
  session_info_.hostAddress.inaOnline = GetLocalIpv4HostOrder();
  session_info_.hostAddress.wPortOnline = AdvertisedSystemLinkPort();
  GetLocalMacAddress(kernel_state(), session_info_.hostAddress.abEnet);
  GenerateExchangeKey(&session_info_.keyExchangeKey);

  if (session_info) {
    *session_info = session_info_;
  }
  if (nonce) {
    *nonce = nonce_;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XSession::DeleteSession() {
  state_ = 4;  // Deleted.
  return X_STATUS_SUCCESS;
}

X_STATUS XSession::JoinLocal(uint32_t user_count, uint32_t user_index_array,
                             uint32_t private_slots_array) {
  (void)user_index_array;
  (void)private_slots_array;
  actual_member_count_ = std::max<uint32_t>(1, user_count);
  if (available_public_slots_ >= actual_member_count_) {
    available_public_slots_ -= actual_member_count_;
  } else {
    available_public_slots_ = 0;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XSession::JoinRemote(uint32_t user_count, const std::vector<bool>& private_slots) {
  actual_member_count_ += user_count;

  for (uint32_t i = 0; i < user_count; ++i) {
    const bool wants_private = i < private_slots.size() && private_slots[i];
    if (wants_private && available_private_slots_ > 0) {
      --available_private_slots_;
    } else if (available_public_slots_ > 0) {
      --available_public_slots_;
    } else if (available_private_slots_ > 0) {
      --available_private_slots_;
    }
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSession::Leave(uint32_t user_count) {
  actual_member_count_ =
      actual_member_count_ > user_count ? actual_member_count_ - user_count : 0;
  available_public_slots_ =
      std::min(max_public_slots_, available_public_slots_ + user_count);
  return X_STATUS_SUCCESS;
}

X_STATUS XSession::Modify(uint32_t flags, uint32_t public_slots, uint32_t private_slots) {
  flags_ = flags;
  max_public_slots_ = public_slots;
  max_private_slots_ = private_slots;
  available_public_slots_ = std::min(available_public_slots_, max_public_slots_);
  available_private_slots_ = std::min(available_private_slots_, max_private_slots_);
  return X_STATUS_SUCCESS;
}

X_STATUS XSession::Start() {
  state_ = 2;  // In game.
  return X_STATUS_SUCCESS;
}

X_STATUS XSession::End() {
  state_ = 3;  // Reporting.
  return X_STATUS_SUCCESS;
}

X_STATUS XSession::GetDetails(XSESSION_LOCAL_DETAILS* details, uint32_t details_size) const {
  if (!details || details_size < sizeof(XSESSION_LOCAL_DETAILS)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  std::memset(details, 0, sizeof(XSESSION_LOCAL_DETAILS));
  details->user_index_host = 0;
  details->flags = flags_;
  details->max_public_slots = max_public_slots_;
  details->max_private_slots = max_private_slots_;
  details->available_public_slots = available_public_slots_;
  details->available_private_slots = available_private_slots_;
  details->actual_member_count = actual_member_count_;
  details->returned_member_count = actual_member_count_;
  details->state = state_;
  details->nonce = nonce_;
  details->session_info = session_info_;
  details->arbitration_id = session_info_.sessionID;
  details->session_members_ptr = 0;
  return X_STATUS_SUCCESS;
}

uint64_t XSession::session_id() const {
  return XnkidToUint64(session_info_.sessionID);
}

}  // namespace rex::system
