#ifndef MULTIPLAYER_HH_INCLUDED
#define MULTIPLAYER_HH_INCLUDED

#include "multiplayer_protocol.hh"
#include "multiplayer_state.hh"

#include <string>
#include <vector>

namespace enigma {
namespace multiplayer {

struct LobbyPeer {
    std::string id;
    std::string name;
    std::string level_id;
    std::string address;
    bool is_self;
};

void LobbyStart();
void LobbyStop();
void LobbyTick(double dtime);
std::vector<LobbyPeer> LobbyPeers();
unsigned LobbySize();
std::string LobbyLocalName();
void LobbySetSelectedLevel(const std::string &level_id);
std::string LobbySelectedLevel();
bool LobbyPollStart(protocol::LobbyStart &start, std::string &host_ip);
bool LobbyPollStart(protocol::LobbyStart &start, std::vector<std::string> &host_ips);
protocol::LobbyStart BuildStartMessage(const std::string &level_id, unsigned expected_players,
                                       const std::string &pack_name, unsigned filter_min_players);
void LobbyBroadcastStart(const protocol::LobbyStart &start);
std::string EncodeStartToken(const protocol::LobbyStart &start);
bool DecodeStartToken(const std::string &token, protocol::LobbyStart &start);
bool InternetCreateRoom(const std::string &server, const std::string &room_code,
                        const protocol::LobbyStart &start, std::string &error);
bool InternetJoinRoom(const std::string &server, const std::string &room_code,
                      protocol::LobbyStart &start, std::string &host_ip,
                      unsigned &player_count, std::vector<LobbyPeer> &peers,
                      std::string &error);
bool InternetStartRoom(const std::string &server, const std::string &room_code,
                       const protocol::LobbyStart &start, std::string &error);
bool InternetPollRoom(const std::string &server, const std::string &room_code,
                      protocol::LobbyStart &start, std::string &host_ip,
                      bool &started, unsigned &player_count,
                      std::vector<LobbyPeer> &peers, std::string &error);
bool InternetLeaveRoom(const std::string &server, const std::string &room_code,
                       std::string &error);
void InternetLeaveTrackedRoomOnShutdown();
void SetRelayServer(const std::string &server);
void SetTcpRelayServer(const std::string &server);

enum class TransportKind {
    NONE = 0,
    DIRECT = 1,
    UDP_RELAY = 2,
    TCP_RELAY = 3
};

// For clients: which transport is currently used to talk to the host.
// For hosts: returns NONE.
TransportKind ActiveTransport();

bool StartHostSession(const protocol::LobbyStart &start);
bool StartClientSession(const protocol::LobbyStart &start, const std::string &host_ip);

// Async client-join helper used by the lobby UI.
//
// `StartClientSession()` performs a blocking connect + welcome handshake. That is fine
// when the user explicitly starts a session, but it can stall the lobby UI when the
// host is unreachable (for example after a crash or abrupt disconnect).
//
// The async variant splits the join into begin/poll steps so the UI remains responsive
// while timeouts elapse.
enum class ClientJoinStatus {
    IDLE = 0,
    CONNECTING = 1,
    JOINED = 2,
    FAILED = 3
};

bool BeginClientJoin(const protocol::LobbyStart &start, const std::string &host_ip);
bool BeginClientJoin(const protocol::LobbyStart &start, const std::vector<std::string> &host_ips);
ClientJoinStatus PollClientJoin();
void CancelClientJoin();

void Tick(double dtime);
void Shutdown();
void SetInputClockFrozen(bool frozen);

}  // namespace multiplayer
}  // namespace enigma

#endif
