#ifndef GUI_MULTIPLAYER_MENU_INTERNAL_HH_INCLUDED
#define GUI_MULTIPLAYER_MENU_INTERNAL_HH_INCLUDED

#include "multiplayer_config.hh"

#include <string>

namespace enigma {
namespace lev {
class Proxy;
class Index;
class VolatileIndex;
}  // namespace lev
}  // namespace enigma

namespace enigma {
namespace gui {
namespace mp_menu {
using InternetServers = enigma::multiplayer::InternetServers;

// Lazily-created synthetic index used for the "Multiplayer Lobby" view.
lev::VolatileIndex *ensure_lobby_index();

// Used for syncing level-pack choice by looking up which index owns a level id.
lev::Index *find_pack_for_level_id(const std::string &level_id, lev::Index *lobby_index);

InternetServers resolve_internet_servers(const std::string &server);
std::string multiplayer_server_host_from_options();

// Loads level metadata and returns the designed player count and whether the map is
// "optimized" (network-mode) for that player count.
bool proxy_player_info(lev::Proxy *proxy, unsigned &players, bool &optimized);

std::string no_level_message(unsigned min_players, unsigned desired_players);
std::string resolved_lobby_server(const std::string &server, const InternetServers &servers);

}  // namespace mp_menu
}  // namespace gui
}  // namespace enigma

#endif
