/*
 * 2026 LLM generated contribution - concept, review and revision by Ferdinand Strixner
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef MULTIPLAYER_SESSION_HH_INCLUDED
#define MULTIPLAYER_SESSION_HH_INCLUDED

/* -------------------- Multiplayer session API -------------------- */
/*
 * Multiplayer session engine.
 *
 * This API is internal. `src/multiplayer.cc` exposes the public entry points and
 * delegates to the session implementation.
 */

#include "multiplayer.hh"
#include "multiplayer_state.hh"

#include <string>
#include <vector>

namespace enigma {
namespace multiplayer {
namespace internal {

// Internal session engine entry points. Public API wrappers live in
// `src/multiplayer.cc` and delegate to these.
bool SessionIsActive();
bool SessionIsHost();
unsigned SessionLocalPlayer();
unsigned SessionExpectedPlayers();
bool SessionHasRemotePeers();
unsigned SessionConnectedRemotePlayers();
TransportKind SessionActiveTransport();

// While connecting/joining, peers must not advance the deterministic simulation. When this returns
// true, the client/UI should show a dedicated "waiting for players" screen and keep pumping SDL
// events + multiplayer transport until the session reaches READY/RUNNING.
	bool SessionShouldDeferStart();
	void SessionNotifyStartRequested();
	void SessionNotifyRestart(bool level_restart);
	void SessionNotifyLoadLevel(const std::string &pack_name, const std::string &level_id);
	bool SessionIsPaused();
	void SessionRequestPause(bool paused);
// Multiplayer pause is derived from "any player has their ESC menu open": the local instance
// reports menu-open changes and the host broadcasts the resulting pause/unpause decision.
void SessionSetMenuOpen(bool open);
void SessionRequestAbort();
// Host -> clients: broadcast the host's current MultiplayerDebug* settings.
// Intended to run before NET_START so all peers use the same session configuration.
void SessionBroadcastDebugOptions();

void SessionPrepareExtraActors();
void SessionSetupExtraPlayerStartPositions();
void SessionPrimeInputQueueForNewLevel();

bool SessionStartHost(const protocol::LobbyStart &start);
bool SessionStartClient(const protocol::LobbyStart &start, const std::string &host_ip);

bool SessionBeginClientJoin(const protocol::LobbyStart &start, const std::string &host_ip);
bool SessionBeginClientJoin(const protocol::LobbyStart &start,
                            const std::vector<std::string> &host_ips);
multiplayer::ClientJoinStatus SessionPollClientJoin();
void SessionCancelClientJoin(const char *reason);

void SessionTick(double dtime);
void SessionShutdown();
void SessionSetInputClockFrozen(bool frozen);
void SessionSetClientDesyncHold(bool enabled);
bool SessionClientDesyncHoldEnabled();
void SessionBuildStatsOverlayLines(std::vector<std::string> &lines,
                                   ::enigma::multiplayer::StatsOverlayPage page);

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma

#endif
