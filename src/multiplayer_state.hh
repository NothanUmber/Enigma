#ifndef MULTIPLAYER_STATE_HH_INCLUDED
#define MULTIPLAYER_STATE_HH_INCLUDED

namespace enigma {
namespace multiplayer {

bool IsActive();
bool IsHost();
unsigned LocalPlayer();
unsigned ExpectedPlayers();
void PrimeInputQueueForNewLevel();
void PrepareExtraActors();
void SetupExtraPlayerStartPositions();
bool ShouldDeferStart();
void NotifyStartRequested();
void NotifyRestart(bool level_restart);
bool IsPaused();
void RequestPause(bool paused);
void SetMenuOpen(bool open);
void RequestAbort();

}  // namespace multiplayer
}  // namespace enigma

#endif
