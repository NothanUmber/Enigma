#ifndef MULTIPLAYER_ROLLBACK_HH_INCLUDED
#define MULTIPLAYER_ROLLBACK_HH_INCLUDED

#include "input.hh"

#include <cstdint>

namespace enigma {
namespace multiplayer {
namespace rollback {

// Experimental rollback/replay to reduce jitter when inputs arrive late under
// unreliable transport (UDP relay). Intended for debug experimentation.

bool Enabled();
bool IsReplaying();
uint32_t EarliestTick(uint32_t current_tick);

void Reset();

// Record a (tick, player) input sample as received/known. If this input arrives
// for an already simulated tick, rollback may be requested.
void RecordInput(uint32_t tick, unsigned player, const input::PlayerInput &pi);

// Capture a pre-tick snapshot for the given simulation tick.
void OnBeforeSimTick(uint32_t tick);

// If rollback is pending, restore a snapshot and replay forward to the current
// tick. Call from the simulation thread (server::gametick).
void MaybeRollback(double timestep);

}  // namespace rollback
}  // namespace multiplayer
}  // namespace enigma

#endif
