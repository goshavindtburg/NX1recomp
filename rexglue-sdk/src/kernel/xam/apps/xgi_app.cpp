/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/logging.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xlive_web_client.h>
#include <rex/system/xsession.h>
#include <rex/thread.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

namespace {

constexpr uint32_t kSessionFlagHost = 1u << 0;
constexpr uint32_t kSessionFlagPeerNetwork = 1u << 5;
constexpr X_HRESULT X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER =
    static_cast<X_HRESULT>(0x80155207L);

object_ref<XSession> LookupSessionByGuestObject(KernelState* kernel_state, memory::Memory* memory,
                                                uint32_t session_ptr) {
  if (!session_ptr) {
    return nullptr;
  }

  auto session_object = memory->TranslateVirtual<X_KSESSION*>(session_ptr);
  if (!session_object) {
    return nullptr;
  }

  return kernel_state->object_table()->LookupObject<XSession>(session_object->handle);
}

void StoreEmptySearchResults(memory::Memory* memory, uint32_t search_results_ptr) {
  if (!search_results_ptr) {
    return;
  }
  auto results = memory->TranslateVirtual(search_results_ptr);
  memory::store_and_swap<uint32_t>(results + 0, 0);
  memory::store_and_swap<uint32_t>(results + 4, 0);
}

uint32_t RequiredSearchResultsSize(uint32_t max_results) {
  // Xenia reports the result payload size here, while the search header itself
  // still lives at the supplied pointer. Accept that convention and allocate the
  // result array separately if the title did not leave inline room after it.
  return std::max<uint32_t>(sizeof(XSESSION_SEARCHRESULT_HEADER),
                            max_results * sizeof(XSESSION_SEARCHRESULT));
}

bool EnsureSearchResultsBuffer(uint8_t* buffer, uint32_t results_size_offset,
                               uint32_t search_results_ptr, uint32_t results_buffer_size,
                               uint32_t max_results) {
  if (search_results_ptr && results_buffer_size >= sizeof(XSESSION_SEARCHRESULT_HEADER)) {
    return true;
  }

  const uint32_t required_size = RequiredSearchResultsSize(max_results);
  memory::store_and_swap<uint32_t>(buffer + results_size_offset, required_size);
  return false;
}

std::string Ipv4ToString(uint32_t address) {
  return fmt::format("{}.{}.{}.{}", (address >> 24) & 0xFF, (address >> 16) & 0xFF,
                     (address >> 8) & 0xFF, address & 0xFF);
}

uint32_t SessionHostAddress(const XLiveSessionSummary& session) {
  const uint32_t online = static_cast<uint32_t>(session.info.hostAddress.inaOnline);
  return online ? online : static_cast<uint32_t>(session.info.hostAddress.ina);
}

void StoreSessionContexts(memory::Memory* memory, XSESSION_SEARCHRESULT* result,
                          const XLiveSessionSummary& session) {
  if (!memory || !result || session.contexts.empty()) {
    return;
  }

  const uint32_t count = static_cast<uint32_t>(session.contexts.size());
  const uint32_t contexts_ptr = memory->SystemHeapAlloc(count * sizeof(XUSER_CONTEXT));
  if (!contexts_ptr) {
    REXKRNL_WARN("XSessionSearch could not allocate {} web context(s)", count);
    return;
  }

  auto contexts = memory->TranslateVirtual<XUSER_CONTEXT*>(contexts_ptr);
  std::memset(contexts, 0, count * sizeof(XUSER_CONTEXT));
  for (uint32_t i = 0; i < count; ++i) {
    contexts[i].context_id = session.contexts[i].id;
    contexts[i].value = session.contexts[i].value;
  }

  result->contexts_count = count;
  result->contexts_ptr = contexts_ptr;
}

void StoreSessionProperties(memory::Memory* memory, XSESSION_SEARCHRESULT* result,
                            const XLiveSessionSummary& session) {
  if (!memory || !result || session.properties.empty()) {
    return;
  }

  const uint32_t count = static_cast<uint32_t>(session.properties.size());
  const uint32_t properties_ptr = memory->SystemHeapAlloc(count * sizeof(XUSER_PROPERTY));
  if (!properties_ptr) {
    REXKRNL_WARN("XSessionSearch could not allocate {} web propert(y/ies)", count);
    return;
  }

  auto properties = memory->TranslateVirtual<XUSER_PROPERTY*>(properties_ptr);
  std::memset(properties, 0, count * sizeof(XUSER_PROPERTY));
  for (uint32_t i = 0; i < count; ++i) {
    const auto& property = session.properties[i];
    properties[i].property_id = property.id;
    properties[i].data.type = static_cast<X_USER_DATA_TYPE>(property.type);

    if (property.type == static_cast<uint8_t>(X_USER_DATA_TYPE::WSTRING) ||
        property.type == static_cast<uint8_t>(X_USER_DATA_TYPE::BINARY)) {
      const uint32_t data_size = static_cast<uint32_t>(property.extended_data.size());
      uint32_t data_ptr = 0;
      if (data_size) {
        data_ptr = memory->SystemHeapAlloc(data_size);
        if (data_ptr) {
          std::memcpy(memory->TranslateVirtual(data_ptr), property.extended_data.data(),
                      data_size);
        } else {
          REXKRNL_WARN("XSessionSearch could not allocate {} byte property payload", data_size);
        }
      }

      if (property.type == static_cast<uint8_t>(X_USER_DATA_TYPE::WSTRING)) {
        properties[i].data.data.unicode.size = data_ptr ? data_size : 0;
        properties[i].data.data.unicode.ptr = data_ptr;
      } else {
        properties[i].data.data.binary.size = data_ptr ? data_size : 0;
        properties[i].data.data.binary.ptr = data_ptr;
      }
    } else {
      std::memcpy(&properties[i].data.data, property.scalar_data.data(),
                  property.scalar_data.size());
    }
  }

  result->properties_count = count;
  result->properties_ptr = properties_ptr;
}

void StoreSearchResults(memory::Memory* memory, uint32_t search_results_ptr,
                        uint32_t results_buffer_size,
                        const std::vector<XLiveSessionSummary>& sessions,
                        uint32_t max_results) {
  StoreEmptySearchResults(memory, search_results_ptr);
  if (!search_results_ptr || sessions.empty() ||
      results_buffer_size < sizeof(XSESSION_SEARCHRESULT_HEADER)) {
    return;
  }

  const uint32_t inline_capacity =
      (results_buffer_size - sizeof(XSESSION_SEARCHRESULT_HEADER)) /
      sizeof(XSESSION_SEARCHRESULT);
  const uint32_t count = std::min<uint32_t>(
      static_cast<uint32_t>(sessions.size()), max_results);
  if (!count) {
    return;
  }

  auto base = memory->TranslateVirtual(search_results_ptr);
  std::memset(base, 0, sizeof(XSESSION_SEARCHRESULT_HEADER));

  const bool use_inline_results = inline_capacity >= count;
  const uint32_t results_ptr = use_inline_results
                                   ? search_results_ptr + sizeof(XSESSION_SEARCHRESULT_HEADER)
                                   : memory->SystemHeapAlloc(count * sizeof(XSESSION_SEARCHRESULT));
  if (!results_ptr) {
    REXKRNL_WARN("XSessionSearch could not allocate {} web result(s)", count);
    return;
  }

  memory::store_and_swap<uint32_t>(base + 0, count);
  memory::store_and_swap<uint32_t>(base + 4, results_ptr);

  auto results = memory->TranslateVirtual<XSESSION_SEARCHRESULT*>(results_ptr);
  std::memset(results, 0, count * sizeof(XSESSION_SEARCHRESULT));
  for (uint32_t i = 0; i < count; ++i) {
    const auto& session = sessions[i];
    results[i].info = session.info;
    results[i].open_public_slots = session.open_public_slots;
    results[i].open_private_slots = session.open_private_slots;
    results[i].filled_public_slots = session.filled_public_slots;
    results[i].filled_private_slots = session.filled_private_slots;
    results[i].properties_count = 0;
    results[i].contexts_count = 0;
    results[i].properties_ptr = 0;
    results[i].contexts_ptr = 0;
    StoreSessionProperties(memory, &results[i], session);
    StoreSessionContexts(memory, &results[i], session);
  }

  if (!use_inline_results) {
    REXKRNL_INFO(
        "XSessionSearch stored {} web result(s) in system heap; caller buffer={} bytes, "
        "inline_capacity={}",
        count, results_buffer_size, inline_capacity);
  } else {
    REXKRNL_INFO("XSessionSearch stored {} web result(s) inline", count);
  }
  for (uint32_t i = 0; i < count; ++i) {
    const auto& session = sessions[i];
    REXKRNL_INFO(
        "XSessionSearch result[{}] id={:016X} host={}:{} open={}/{} filled={}/{} "
        "properties={} contexts={} map='{}' game='{}'",
                 i, XnkidToUint64(session.info.sessionID),
                 Ipv4ToString(SessionHostAddress(session)),
                 static_cast<uint16_t>(session.info.hostAddress.wPortOnline),
                 session.open_public_slots, session.open_private_slots,
                 session.filled_public_slots, session.filled_private_slots,
                 session.properties.size(), session.contexts.size(), session.map_name,
                 session.game_type);
  }
}

std::vector<bool> ReadPrivateSlots(memory::Memory* memory, uint32_t private_slots_array,
                                   uint32_t user_count) {
  std::vector<bool> private_slots(user_count, false);
  if (!private_slots_array) {
    return private_slots;
  }

  auto slots = memory->TranslateVirtual<rex::be<uint32_t>*>(private_slots_array);
  for (uint32_t i = 0; i < user_count; ++i) {
    private_slots[i] = static_cast<uint32_t>(slots[i]) != 0;
  }
  return private_slots;
}

std::vector<uint64_t> ReadRemoteXuids(memory::Memory* memory, uint32_t xuid_array,
                                      uint32_t user_count) {
  std::vector<uint64_t> xuids;
  if (!xuid_array) {
    return xuids;
  }

  auto xuid_values = memory->TranslateVirtual<rex::be<uint64_t>*>(xuid_array);
  xuids.reserve(user_count);
  for (uint32_t i = 0; i < user_count; ++i) {
    xuids.push_back(static_cast<uint64_t>(xuid_values[i]));
  }
  return xuids;
}

std::vector<uint64_t> ReadLocalJoinXuids(KernelState* kernel_state, memory::Memory* memory,
                                         uint32_t user_index_array, uint32_t user_count) {
  std::vector<uint64_t> xuids;
  xuids.reserve(user_count);

  auto indices =
      user_index_array ? memory->TranslateVirtual<rex::be<uint32_t>*>(user_index_array) : nullptr;
  for (uint32_t i = 0; i < user_count; ++i) {
    const uint32_t user_index = indices ? static_cast<uint32_t>(indices[i]) : 0;
    if (user_index == 0 && kernel_state->user_profile()) {
      xuids.push_back(kernel_state->user_profile()->xuid());
    }
  }

  if (xuids.empty() && kernel_state->user_profile()) {
    xuids.push_back(kernel_state->user_profile()->xuid());
  }
  return xuids;
}

std::vector<uint64_t> ReadManageXuids(KernelState* kernel_state, memory::Memory* memory,
                                      uint32_t xuid_array, uint32_t user_index_array,
                                      uint32_t user_count) {
  return xuid_array ? ReadRemoteXuids(memory, xuid_array, user_count)
                    : ReadLocalJoinXuids(kernel_state, memory, user_index_array, user_count);
}

std::vector<XLiveSessionSummary> LookupRemoteSessionsByIds(KernelState* kernel_state,
                                                           memory::Memory* memory,
                                                           uint32_t num_session_ids,
                                                           uint32_t session_ids_ptr) {
  std::vector<XLiveSessionSummary> sessions;
  if (!num_session_ids || !session_ids_ptr) {
    return sessions;
  }

  auto session_ids = memory->TranslateVirtual<XNKID*>(session_ids_ptr);
  sessions.reserve(num_session_ids);
  for (uint32_t i = 0; i < num_session_ids; ++i) {
    XLiveSessionSummary session;
    if (XLiveWebClient::Get().GetSession(kernel_state, XnkidToUint64(session_ids[i]), &session)) {
      sessions.push_back(session);
    }
  }
  return sessions;
}

X_HRESULT ResultFromStatus(X_STATUS status) {
  return XSUCCEEDED(status) ? X_E_SUCCESS : X_E_FAIL;
}

}  // namespace

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length || buffer_length == 24);
      // dword r3 user index
      // dword (unwritten?)
      // qword 0
      // dword r4 context enum
      // dword r5 value
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t context_value = memory::load_and_swap<uint32_t>(buffer + 20);
      REXKRNL_DEBUG("XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                    context_value);
      XLiveWebClient::Get().SetUserContext(kernel_state_, context_id, context_value);
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t property_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t value_ptr = memory::load_and_swap<uint32_t>(buffer + 24);
      REXKRNL_DEBUG("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index, property_id,
                    value_size, value_ptr);
      const uint8_t* value =
          value_ptr ? memory_->TranslateVirtual<uint8_t*>(value_ptr) : nullptr;
      XLiveWebClient::Get().SetUserProperty(kernel_state_, property_id, value, value_size);
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t achievement_count = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t achievements_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      REXKRNL_DEBUG("XGIUserWriteAchievements({:08X}, {:08X})", achievement_count,
                    achievements_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      assert_true(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_slots_public = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t num_slots_private = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t user_xuid = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t nonce_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG(
          "XGISessionCreateImpl({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, "
          "{:08X})",
          session_ptr, flags, num_slots_public, num_slots_private, user_xuid, session_info_ptr,
          nonce_ptr);

      auto session = LookupSessionByGuestObject(kernel_state_, memory_, session_ptr);
      if (!session) {
        return X_E_FAIL;
      }

      auto session_info = session_info_ptr ? memory_->TranslateVirtual<XSESSION_INFO*>(session_info_ptr)
                                           : nullptr;
      const uint64_t incoming_session_id =
          session_info ? XnkidToUint64(session_info->sessionID) : 0;
      const bool creates_remote_peer_session =
          (flags & kSessionFlagPeerNetwork) && !(flags & kSessionFlagHost) &&
          incoming_session_id;
      uint64_t nonce = 0;
      X_STATUS status = session->CreateHostSession(flags, num_slots_public, num_slots_private,
                                                   session_info, nonce_ptr ? &nonce : nullptr);
      if (nonce_ptr) {
        memory::store_and_swap<uint64_t>(memory_->TranslateVirtual(nonce_ptr), nonce);
      }
      if (XSUCCEEDED(status)) {
        if ((flags & kSessionFlagHost) != 0) {
          XLiveWebClient::Get().CreateSession(kernel_state_, *session);
        } else if (creates_remote_peer_session &&
                   session->session_id() == incoming_session_id) {
          const auto xuids = ReadLocalJoinXuids(kernel_state_, memory_, 0, 1);
          const std::vector<bool> private_slots(xuids.size(), false);
          XLiveWebClient::Get().JoinSession(kernel_state_, session->session_id(), xuids,
                                            private_slots);
          REXKRNL_INFO("XGISessionCreateImpl marked local player joined web session {:016X}",
                       session->session_id());
        }
      }
      return ResultFromStatus(status);
    }
    case 0x000B0011: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XGISessionDelete({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      auto session = LookupSessionByGuestObject(kernel_state_, memory_, obj_ptr);
      if (!session) {
        return X_E_FAIL;
      }

      const uint64_t session_id = session->session_id();
      const bool is_host = (session->flags() & kSessionFlagHost) != 0;
      X_STATUS status = session->DeleteSession();
      if (XSUCCEEDED(status)) {
        if (is_host) {
          XLiveWebClient::Get().DeleteSessionOnEnd(kernel_state_, session_id);
        } else {
          XLiveWebClient::Get().LeaveSession(
              kernel_state_, session_id,
              ReadLocalJoinXuids(kernel_state_, memory_, 0, 1));
        }
      }
      return ResultFromStatus(status);
    }
    case 0x000B0012: {
      assert_true(!buffer_length || buffer_length == 20);
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t user_index_array = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t private_slots_array = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XGISessionJoin({:08X}, {}, {:08X}, {:08X}, {:08X})", session_ptr, user_count,
                    xuid_array, user_index_array, private_slots_array);
      auto session = LookupSessionByGuestObject(kernel_state_, memory_, session_ptr);
      if (!session) {
        return X_E_FAIL;
      }

      const auto private_slots =
          ReadPrivateSlots(memory_, private_slots_array, user_count);
      if (xuid_array) {
        const auto xuids = ReadRemoteXuids(memory_, xuid_array, user_count);
        X_STATUS status = session->JoinRemote(user_count, private_slots);
        if (XSUCCEEDED(status)) {
          XLiveWebClient::Get().JoinSession(kernel_state_, session->session_id(), xuids,
                                            private_slots);
        }
        return ResultFromStatus(status);
      }

      X_STATUS status = session->JoinLocal(user_count, user_index_array, private_slots_array);
      if (XSUCCEEDED(status)) {
        const auto xuids =
            ReadLocalJoinXuids(kernel_state_, memory_, user_index_array, user_count);
        if ((session->flags() & kSessionFlagHost) != 0) {
          XLiveWebClient::Get().JoinSession(kernel_state_, session->session_id(), xuids,
                                            private_slots);
        } else {
          XLiveWebClient::Get().JoinSession(kernel_state_, session->session_id(), xuids,
                                            private_slots);
        }
      }
      return ResultFromStatus(status);
    }
    case 0x000B0013: {
      assert_true(!buffer_length || buffer_length == 20);
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t user_index_array = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t private_slots_array = memory::load_and_swap<uint32_t>(buffer + 16);
      (void)private_slots_array;

      REXKRNL_DEBUG("XGISessionLeave({:08X}, {}, {:08X}, {:08X}, {:08X})", session_ptr,
                    user_count, xuid_array, user_index_array, private_slots_array);
      auto session = LookupSessionByGuestObject(kernel_state_, memory_, session_ptr);
      if (!session) {
        return X_E_FAIL;
      }

      const auto xuids =
          ReadManageXuids(kernel_state_, memory_, xuid_array, user_index_array, user_count);
      X_STATUS status = session->Leave(user_count);
      if (XSUCCEEDED(status)) {
        XLiveWebClient::Get().LeaveSession(kernel_state_, session->session_id(), xuids);
      }
      return ResultFromStatus(status);
    }
    case 0x000B0014: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XSessionStart({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      auto session = LookupSessionByGuestObject(kernel_state_, memory_, obj_ptr);
      return session ? ResultFromStatus(session->Start()) : X_E_FAIL;
    }
    case 0x000B0015: {
      // send high scores?
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XSessionEnd({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      auto session = LookupSessionByGuestObject(kernel_state_, memory_, obj_ptr);
      if (!session) {
        return X_E_FAIL;
      }

      const uint64_t session_id = session->session_id();
      const bool is_host = (session->flags() & kSessionFlagHost) != 0;
      X_STATUS status = session->End();
      if (XSUCCEEDED(status) && is_host) {
        XLiveWebClient::Get().DeleteSessionOnEnd(kernel_state_, session_id);
      }
      return ResultFromStatus(status);
    }
    case 0x000B0016: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearch({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X})", proc_index,
                    user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr);
      if (!EnsureSearchResultsBuffer(buffer, 24, search_results_ptr, results_buffer_size,
                                     num_results)) {
        REXKRNL_INFO("XSessionSearch needs {} bytes for {} result(s)",
                     RequiredSearchResultsSize(num_results), num_results);
        return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
      }

      const auto sessions =
          XLiveWebClient::Get().SearchSessions(kernel_state_, proc_index, num_results, 1);
      StoreSearchResults(memory_, search_results_ptr, results_buffer_size, sessions, num_results);
      return X_E_SUCCESS;
    }
    case 0x000B0018: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t maxPublicSlots = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t maxPrivateSlots = memory::load_and_swap<uint16_t>(buffer + 12);

      REXKRNL_DEBUG("XSessionModify({:08X}, {:08X}, {:08X}, {:08X})", obj_ptr, flags,
                    maxPublicSlots, maxPrivateSlots);

      auto session = LookupSessionByGuestObject(kernel_state_, memory_, obj_ptr);
      if (!session) {
        return X_E_FAIL;
      }

      X_STATUS status = session->Modify(flags, maxPublicSlots, maxPrivateSlots);
      if (XSUCCEEDED(status) && (session->flags() & kSessionFlagHost) != 0) {
        XLiveWebClient::Get().ModifySession(kernel_state_, *session);
      }
      return ResultFromStatus(status);
    }
    case 0x000B001C: {
      assert_true(!buffer_length || buffer_length == 36);

      // session_search
      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      //
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 32);

      REXKRNL_DEBUG("XSessionSearchEx({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X}, {})",
                    proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr, num_users);
      if (!EnsureSearchResultsBuffer(buffer, 24, search_results_ptr, results_buffer_size,
                                     num_results)) {
        REXKRNL_INFO("XSessionSearchEx needs {} bytes for {} result(s)",
                     RequiredSearchResultsSize(num_results), num_results);
        return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
      }

      const auto sessions =
          XLiveWebClient::Get().SearchSessions(kernel_state_, proc_index, num_results, num_users);
      StoreSearchResults(memory_, search_results_ptr, results_buffer_size, sessions, num_results);
      return X_E_SUCCESS;
    }
    case 0x000B001D: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t details_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_details_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionGetDetails({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr,
                    details_buffer_size, session_details_ptr, reserved1, reserved2, reserved3);

      auto session = LookupSessionByGuestObject(kernel_state_, memory_, obj_ptr);
      auto details = session_details_ptr
                         ? memory_->TranslateVirtual<XSESSION_LOCAL_DETAILS*>(session_details_ptr)
                         : nullptr;
      return session ? ResultFromStatus(session->GetDetails(details, details_buffer_size))
                     : X_E_FAIL;
    }
    case 0x000B001E: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionMigrateHost({:08X}, {:08X}, {}, {}, {}, {})", obj_ptr,
                    session_info_ptr, user_index, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0019: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XSessionGetInvitationData - unimplemented({}, {:08X})", user_index,
                    session_info_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001A: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);
      uint32_t session_duration_sec = memory::load_and_swap<uint32_t>(buffer + 16);  // 300
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XSessionArbitrationRegister({:08X}, {:08X}, {:016X}, {:08X}, {:08X}, {:08X})",
                    obj_ptr, flags, session_nonce, session_duration_sec, results_buffer_size,
                    results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001B: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByID({}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);
      if (!EnsureSearchResultsBuffer(buffer, 12, search_results_ptr, results_buffer_size,
                                     num_session_ids)) {
        REXKRNL_INFO("XSessionSearchByID needs {} bytes for {} result(s)",
                     RequiredSearchResultsSize(num_session_ids), num_session_ids);
        return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
      }

      const auto sessions =
          LookupRemoteSessionsByIds(kernel_state_, memory_, num_session_ids, session_ids_ptr);
      StoreSearchResults(memory_, search_results_ptr, results_buffer_size, sessions,
                         num_session_ids);
      return X_E_SUCCESS;
    }
    case 0x000B001F: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t array_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionModifySkill({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr, array_count,
                    xuid_array_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0020: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t view_id = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XUserResetStatsView({:08X}, {})", user_index, view_id);

      return X_E_SUCCESS;
    }
    case 0x000B0021: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t title_id = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t xuids_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t specs_count = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t specs_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t results_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XUserReadStats({}, {}, {:08X}, {}, {:08X}, {}, {:08X})", title_id, xuids_count,
                    xuids_ptr, specs_count, specs_ptr, results_size, results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0025: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionWriteStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0026: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionFlushStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      REXKRNL_DEBUG("XInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t AnId_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t AnId_buffer_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t block = memory::load_and_swap<uint32_t>(buffer + 12);

      REXKRNL_DEBUG("XUserGetANID({:08X}, {:08X}, {:08X}, {:08X})", user_index, AnId_buffer_size,
                    AnId_buffer_ptr, block);

      return X_E_SUCCESS;
    }
    case 0x000B0041: {
      assert_true(!buffer_length || buffer_length == 32);
      // 00000000 2789fecc 00000000 00000000 200491e0 00000000 200491f0 20049340
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      auto context = context_ptr ? memory_->TranslateVirtual(context_ptr) : nullptr;
      uint32_t context_id = context ? memory::load_and_swap<uint32_t>(context + 0) : 0;
      REXKRNL_DEBUG("XGIUserGetContext({:08X}, {:08X}, {:08X}))", user_index, context_ptr,
                    context_id);
      uint32_t value = 0;
      if (context) {
        memory::store_and_swap<uint32_t>(context + 4, value);
      }
      return X_E_FAIL;
    }
    case 0x000B0060: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByIds({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);
      if (!EnsureSearchResultsBuffer(buffer, 12, search_results_ptr, results_buffer_size,
                                     num_session_ids)) {
        REXKRNL_INFO("XSessionSearchByIds needs {} bytes for {} result(s)",
                     RequiredSearchResultsSize(num_session_ids), num_session_ids);
        return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
      }

      const auto sessions =
          LookupRemoteSessionsByIds(kernel_state_, memory_, num_session_ids, session_ids_ptr);
      StoreSearchResults(memory_, search_results_ptr, results_buffer_size, sessions,
                         num_session_ids);
      return X_E_SUCCESS;
    }
    case 0x000B0065: {
      assert_true(!buffer_length || buffer_length == 52);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_weighted_properties = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_weighted_contexts = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 24);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 26);
      uint32_t non_weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      uint32_t non_weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 32);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 36);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 40);
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 44);
      uint32_t weighted_search = memory::load_and_swap<uint32_t>(buffer + 48);

      REXKRNL_DEBUG(
          "XSessionSearchWeighted({:08X}, {:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {}, {}, {:08X}, "
          "{:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
          proc_index, user_index, num_results, num_weighted_properties, num_weighted_contexts,
          weighted_search_properties_ptr, weighted_search_contexts_ptr, num_props, num_ctx,
          non_weighted_search_properties_ptr, non_weighted_search_contexts_ptr, results_buffer_size,
          search_results_ptr, num_users, weighted_search);
      if (!EnsureSearchResultsBuffer(buffer, 36, search_results_ptr, results_buffer_size,
                                     num_results)) {
        REXKRNL_INFO("XSessionSearchWeighted needs {} bytes for {} result(s)",
                     RequiredSearchResultsSize(num_results), num_results);
        return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
      }

      const auto sessions =
          XLiveWebClient::Get().SearchSessions(kernel_state_, proc_index, num_results, num_users);
      StoreSearchResults(memory_, search_results_ptr, results_buffer_size, sessions, num_results);
      return X_E_SUCCESS;
    }
    case 0x000B0071: {
      REXKRNL_DEBUG("XGI 0x000B0071, unimplemented");
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
