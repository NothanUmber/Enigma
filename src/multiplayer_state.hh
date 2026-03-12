#ifndef MULTIPLAYER_STATE_HH_INCLUDED
#define MULTIPLAYER_STATE_HH_INCLUDED

#include <string>

namespace enigma {

class Actor;

namespace multiplayer {

enum class VisualPredictionActorMode {
    Truth = 0,
    LocalOwned = 1,
    BlendToTruth = 2,
};

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
void SetInputClockFrozen(bool frozen);
void VisualPredictionOnBeforeSimTick(double timestep);
void VisualPredictionOnAfterSimTick();
void VisualPredictionBeginRender();
void VisualPredictionEndRender();
bool VisualPredictionRenderActive();
bool VisualPredictionSimulationActive();
void VisualPredictionInvalidate();
bool VisualPredictionEnabled();
VisualPredictionActorMode VisualPredictionGetActorMode(const Actor &actor);
double VisualPredictionGetActorBlendAlpha(const Actor &actor);

}  // namespace multiplayer
}  // namespace enigma

#endif
