#ifndef MULTIPLAYER_STATE_HH_INCLUDED
#define MULTIPLAYER_STATE_HH_INCLUDED

#include <string>

namespace enigma {
namespace multiplayer {

bool IsActive();
bool IsHost();
unsigned LocalPlayer();
unsigned ExpectedPlayers();
bool HasRemotePeers();
unsigned ConnectedRemotePlayers();
void PrimeInputQueueForNewLevel();
void PrepareExtraActors();
void SetupExtraPlayerStartPositions();
bool ShouldDeferStart();
void NotifyStartRequested();
void NotifyRestart(bool level_restart);
void NotifyLoadLevel(const std::string &pack_name, const std::string &level_id);
bool IsPaused();
void RequestPause(bool paused);
void SetMenuOpen(bool open);
void RequestAbort();

}  // namespace multiplayer
}  // namespace enigma

#endif
