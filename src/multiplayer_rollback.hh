#ifndef MULTIPLAYER_ROLLBACK_HH_INCLUDED
#define MULTIPLAYER_ROLLBACK_HH_INCLUDED

#include "input.hh"

#include <cstdint>

namespace enigma {
namespace multiplayer {
namespace protocol {
struct ResyncState;
}  // namespace protocol
namespace rollback {

// Experimental rollback/replay to reduce jitter when inputs arrive late under
// unreliable transport (UDP relay). Intended for debug experimentation.

bool Enabled();
bool IsReplaying();
uint32_t EarliestTick(uint32_t current_tick);

void Reset();
void ClearPendingReconcile();

// Record a (tick, player) input sample as received/known. If this input arrives
// for an already simulated tick, rollback may be requested.
void RecordInput(uint32_t tick, unsigned player, const input::PlayerInput &pi);
bool GetRecordedInput(uint32_t tick, unsigned player, input::PlayerInput &out);

// Capture a pre-tick snapshot for the given simulation tick.
void OnBeforeSimTick(uint32_t tick);

// Experimental: reconcile simulation to an authoritative host resync snapshot by
// restoring a past local snapshot at `state.tick`, applying the authoritative
// state at that tick, and replaying forward.
//
// Returns true if the reconcile was queued and the caller should NOT apply the
// resync state directly.
bool TryQueueReconcileResyncState(const protocol::ResyncState &state);

// If rollback is pending, restore a snapshot and replay forward to the current
// tick. Call from the simulation thread (server::gametick).
void MaybeRollback(double timestep);

}  // namespace rollback
}  // namespace multiplayer
}  // namespace enigma

#endif
