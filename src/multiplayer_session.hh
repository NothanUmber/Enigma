#ifndef MULTIPLAYER_SESSION_HH_INCLUDED
#define MULTIPLAYER_SESSION_HH_INCLUDED

#include "multiplayer.hh"
#include "multiplayer_state.hh"

#include <string>

namespace enigma {
namespace multiplayer {
namespace internal {

// Internal session engine entry points. Public API wrappers live in
// `src/multiplayer.cc` and delegate to these.
bool SessionIsActive();
bool SessionIsHost();
unsigned SessionLocalPlayer();
unsigned SessionExpectedPlayers();
TransportKind SessionActiveTransport();

// While connecting/joining, peers must not advance the deterministic simulation. When this returns
// true, the client/UI should show a dedicated "waiting for players" screen and keep pumping SDL
// events + multiplayer transport until the session reaches READY/RUNNING.
bool SessionShouldDeferStart();
void SessionNotifyStartRequested();
void SessionNotifyRestart(bool level_restart);
bool SessionIsPaused();
void SessionRequestPause(bool paused);
// Multiplayer pause is derived from "any player has their ESC menu open": the local instance
// reports menu-open changes and the host broadcasts the resulting pause/unpause decision.
void SessionSetMenuOpen(bool open);
void SessionRequestAbort();

void SessionPrepareExtraActors();
void SessionSetupExtraPlayerStartPositions();
void SessionPrimeInputQueueForNewLevel();

bool SessionStartHost(const protocol::LobbyStart &start);
bool SessionStartClient(const protocol::LobbyStart &start, const std::string &host_ip);

bool SessionBeginClientJoin(const protocol::LobbyStart &start, const std::string &host_ip);
multiplayer::ClientJoinStatus SessionPollClientJoin();
void SessionCancelClientJoin();

void SessionTick(double dtime);
void SessionShutdown();

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma

#endif
