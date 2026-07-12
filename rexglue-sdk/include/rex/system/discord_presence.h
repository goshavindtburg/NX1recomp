#pragma once

#include <cstdint>
#include <string>

namespace rex::system {

class KernelState;

class DiscordPresenceClient final {
 public:
  static DiscordPresenceClient& Get();

  void SetMenuState(KernelState* kernel_state);
  void NoteLoadedMap(KernelState* kernel_state, std::string map_name);
  void UpdateSessionInfo(KernelState* kernel_state, uint64_t session_id,
                         std::string map_name, std::string game_type,
                         uint32_t players, uint32_t max_players);
  void Shutdown();

 private:
  DiscordPresenceClient();
  ~DiscordPresenceClient();

  DiscordPresenceClient(const DiscordPresenceClient&) = delete;
  DiscordPresenceClient& operator=(const DiscordPresenceClient&) = delete;
};

}  // namespace rex::system
