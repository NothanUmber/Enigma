#include "multiplayer.hh"

#include "multiplayer_session.hh"

namespace enigma {
namespace multiplayer {

bool IsActive() {
    return internal::SessionIsActive();
}

bool IsHost() {
    return internal::SessionIsHost();
}

unsigned LocalPlayer() {
    return internal::SessionLocalPlayer();
}

unsigned ExpectedPlayers() {
    return internal::SessionExpectedPlayers();
}

unsigned ConnectedRemotePlayers() {
    return internal::SessionConnectedRemotePlayers();
}

bool HasRemotePeers() {
    return internal::SessionHasRemotePeers();
}

bool ShouldDeferStart() {
    return internal::SessionShouldDeferStart();
}

void NotifyStartRequested() {
    internal::SessionNotifyStartRequested();
}

void NotifyRestart(bool level_restart) {
    internal::SessionNotifyRestart(level_restart);
}

void NotifyLoadLevel(const std::string &pack_name, const std::string &level_id) {
    internal::SessionNotifyLoadLevel(pack_name, level_id);
}

bool IsPaused() {
    return internal::SessionIsPaused();
}

void RequestPause(bool paused) {
    internal::SessionRequestPause(paused);
}

void SetMenuOpen(bool open) {
    internal::SessionSetMenuOpen(open);
}

void RequestAbort() {
    internal::SessionRequestAbort();
}

void SetInputClockFrozen(bool frozen) {
    internal::SessionSetInputClockFrozen(frozen);
}

void PrepareExtraActors() {
    internal::SessionPrepareExtraActors();
}

void SetupExtraPlayerStartPositions() {
    internal::SessionSetupExtraPlayerStartPositions();
}

void PrimeInputQueueForNewLevel() {
    internal::SessionPrimeInputQueueForNewLevel();
}

TransportKind ActiveTransport() {
    return internal::SessionActiveTransport();
}

bool StartHostSession(const protocol::LobbyStart &start) {
    return internal::SessionStartHost(start);
}

bool StartClientSession(const protocol::LobbyStart &start, const std::string &host_ip) {
    return internal::SessionStartClient(start, host_ip);
}

bool BeginClientJoin(const protocol::LobbyStart &start, const std::string &host_ip) {
    return internal::SessionBeginClientJoin(start, host_ip);
}

bool BeginClientJoin(const protocol::LobbyStart &start, const std::vector<std::string> &host_ips) {
    return internal::SessionBeginClientJoin(start, host_ips);
}

ClientJoinStatus PollClientJoin() {
    return internal::SessionPollClientJoin();
}

void CancelClientJoin() {
    internal::SessionCancelClientJoin(nullptr);
}

void CancelClientJoin(const char *reason) {
    internal::SessionCancelClientJoin(reason);
}

void Tick(double dtime) {
    internal::SessionTick(dtime);
}

void Shutdown() {
    internal::SessionShutdown();
}

}  // namespace multiplayer
}  // namespace enigma
