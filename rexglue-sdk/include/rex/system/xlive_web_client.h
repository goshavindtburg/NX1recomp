#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

#include <rex/system/xsession.h>

namespace rex::system {

class KernelState;

struct XLiveSessionContext {
  uint32_t id = 0;
  uint32_t value = 0;
};

struct XLiveSessionProperty {
  uint32_t id = 0;
  uint8_t type = 0xFF;
  std::array<uint8_t, 8> scalar_data = {};
  std::vector<uint8_t> extended_data;
};

struct XLiveSessionSummary {
  XSESSION_INFO info = {};
  uint64_t host_xuid = 0;
  uint32_t flags = 0;
  uint32_t public_slots = 0;
  uint32_t private_slots = 0;
  uint32_t open_public_slots = 0;
  uint32_t open_private_slots = 0;
  uint32_t filled_public_slots = 0;
  uint32_t filled_private_slots = 0;
  std::string host_name;
  std::string map_name;
  std::string game_type;
  uint32_t advertised_clients = 0;
  uint32_t advertised_max_clients = 0;
  std::vector<XLiveSessionContext> contexts;
  std::vector<XLiveSessionProperty> properties;
};

struct XLiveWebStatus {
  bool enabled = false;
  bool connected = false;
  bool registered = false;
  uint32_t http_status = 0;
  uint32_t title_id = 0;
  uint64_t xuid = 0;
  std::string gamertag;
  std::string public_address;
  std::string api_address;
  std::string message;
};

struct XLivePlayerSummary {
  uint64_t xuid = 0;
  uint64_t machine_id = 0;
  uint64_t session_id = 0;
  uint32_t host_address = 0;
  uint16_t port = 0;
  std::array<uint8_t, 6> mac_address = {};
  std::string gamertag;
};

class XLiveWebClient final {
 public:
  static XLiveWebClient& Get();

  XLiveWebStatus Probe(KernelState* kernel_state, bool force_probe = false);
  void LogStatus(KernelState* kernel_state, bool force_probe = false);
  bool CreateSession(KernelState* kernel_state, const XSession& session);
  bool ModifySession(KernelState* kernel_state, const XSession& session);
  std::vector<XLiveSessionSummary> SearchSessions(KernelState* kernel_state,
                                                  uint32_t search_index,
                                                  uint32_t results_count,
                                                  uint32_t num_users,
                                                  bool filter_own_sessions = true);
  bool GetSession(KernelState* kernel_state, uint64_t session_id,
                  XLiveSessionSummary* out_session);
  bool JoinSession(KernelState* kernel_state, uint64_t session_id,
                   const std::vector<uint64_t>& xuids,
                   const std::vector<bool>& private_slots);
  bool PreJoinSession(KernelState* kernel_state, uint64_t session_id,
                      const std::vector<uint64_t>& xuids);
  bool LeaveSession(KernelState* kernel_state, uint64_t session_id,
                    const std::vector<uint64_t>& xuids);
  bool FindPlayerByAddress(KernelState* kernel_state, uint32_t host_order_address,
                           XLivePlayerSummary* out_player);
  bool PostQos(KernelState* kernel_state, uint64_t session_id, const uint8_t* data,
               size_t data_size);
  bool GetQos(KernelState* kernel_state, uint64_t session_id, std::vector<uint8_t>* out_data);
  void SetUserContext(KernelState* kernel_state, uint32_t context_id, uint32_t value);
  void SetUserProperty(KernelState* kernel_state, uint32_t property_id, const uint8_t* value,
                       uint32_t value_size);
  void NoteLoadedMap(KernelState* kernel_state, std::string map_name);
  void ClearLoadedMap(KernelState* kernel_state);
  void UpdateAdvertisedSessionInfo(KernelState* kernel_state, uint64_t session_id,
                                   std::string map_name, std::string host_name,
                                   std::string game_type, uint32_t clients,
                                   uint32_t max_clients);
  bool IsSessionCreatedLocally(uint64_t session_id);
  // True once the local player has joined or is hosting a session (i.e. is in a
  // match). Used to suspend the System Link broadcast bridge's web polling.
  bool IsInActiveSession();
  bool DeleteSession(KernelState* kernel_state, uint64_t session_id);
  bool DeleteSessionOnEnd(KernelState* kernel_state, uint64_t session_id);
  bool DeleteStaleSessions(KernelState* kernel_state, bool all_for_public_ip);
  bool CleanupOnShutdown(KernelState* kernel_state);

 private:
  XLiveWebClient() = default;
};

}  // namespace rex::system
