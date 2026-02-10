#ifndef MULTIPLAYER_WAIT_SETTINGS_HH_INCLUDED
#define MULTIPLAYER_WAIT_SETTINGS_HH_INCLUDED

#include "enet_ver.hh"

#include <cstdint>

namespace enigma {
namespace multiplayer {
namespace wait {

constexpr double kStallDialogDelaySeconds = 2.0;

#ifdef ENET_VER_EQ_GT_13
// When ENet >= 1.3 is available, extend the peer timeout so short/medium
// network outages can recover without triggering a disconnect.
constexpr double kAbortTimeoutSeconds = 60.0;
constexpr std::uint32_t kEnetPeerTimeoutMs = 60000;
#else
// Old ENet cannot adjust peer timeout per connection; keep the UI timeout in
// line with ENet's default maximum timeout window.
constexpr double kAbortTimeoutSeconds = 30.0;
#endif

}  // namespace wait
}  // namespace multiplayer
}  // namespace enigma

#endif  // MULTIPLAYER_WAIT_SETTINGS_HH_INCLUDED

