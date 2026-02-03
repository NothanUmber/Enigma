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
void LobbySetSelectedLevel(const std::string &level_id);
std::string LobbySelectedLevel();
bool LobbyPollStart(protocol::LobbyStart &start, std::string &host_ip);
protocol::LobbyStart BuildStartMessage(const std::string &level_id, unsigned expected_players,
                                       unsigned filter_min_players);
void LobbyBroadcastStart(const protocol::LobbyStart &start);
std::string EncodeStartToken(const protocol::LobbyStart &start);
bool DecodeStartToken(const std::string &token, protocol::LobbyStart &start);
bool InternetCreateRoom(const std::string &server, const std::string &room_code,
                        const protocol::LobbyStart &start, std::string &error);
bool InternetJoinRoom(const std::string &server, const std::string &room_code,
                      protocol::LobbyStart &start, std::string &host_ip,
                      unsigned &player_count, std::string &error);
bool InternetStartRoom(const std::string &server, const std::string &room_code,
                       const protocol::LobbyStart &start, std::string &error);
bool InternetPollRoom(const std::string &server, const std::string &room_code,
                      protocol::LobbyStart &start, std::string &host_ip,
                      bool &started, unsigned &player_count, std::string &error);
bool InternetLeaveRoom(const std::string &server, const std::string &room_code,
                       std::string &error);
void SetRelayServer(const std::string &server);

bool StartHostSession(const protocol::LobbyStart &start);
bool StartClientSession(const protocol::LobbyStart &start, const std::string &host_ip);
void Tick(double dtime);
void Shutdown();

}  // namespace multiplayer
}  // namespace enigma

#endif
