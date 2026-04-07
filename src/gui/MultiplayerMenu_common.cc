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

#include "gui/MultiplayerMenu_internal.hh"

#include "errors.hh"
#include "multiplayer_config.hh"
#include "nls.hh"

#include "lev/Index.hh"
#include "lev/VolatileIndex.hh"

#include <string>
#include <vector>
#include <cstdint>

/* -------------------- Multiplayer menu common -------------------- */
/*
 * Multiplayer menu helpers that are not part of the public GUI API.
 *
 * Kept in a separate TU to keep `MultiplayerMenu.cc` readable.
 */

namespace enigma {
namespace gui {
namespace mp_menu {

namespace {
lev::VolatileIndex *s_lobby_index = nullptr;
}  // namespace

lev::VolatileIndex *ensure_lobby_index() {
    if (s_lobby_index != nullptr)
        return s_lobby_index;
    std::vector<std::string> empty;
    s_lobby_index = new lev::VolatileIndex("Multiplayer Lobby", "Network Levels", "Multiplayer",
                                           empty, INDEX_DEFAULT_PACK_LOCATION);
    lev::Index::registerIndex(s_lobby_index);
    return s_lobby_index;
}

lev::Index *find_pack_for_level_id(const std::string &level_id, lev::Index *lobby_index) {
    std::vector<std::string> groups = lev::Index::getGroupNames();
    for (const auto &group : groups) {
        std::vector<lev::Index *> *indices = lev::Index::getGroup(group);
        if (!indices)
            continue;
        for (auto *index : *indices) {
            if (!index || index == lobby_index)
                continue;
            if (index->hasNormLevelPath(level_id))
                return index;
        }
    }
    return nullptr;
}

InternetEndpoints resolve_internet_servers(const std::string &server) {
    static_cast<void>(server);
    multiplayer::MultiplayerConfig cfg = multiplayer::LoadMultiplayerConfig();
    return multiplayer::ResolveInternetEndpoints(cfg);
}

std::string multiplayer_server_host_from_options() {
    multiplayer::MultiplayerConfig cfg = multiplayer::LoadMultiplayerConfig();
    return cfg.server_host.empty() ? std::string("CHANGEME") : cfg.server_host;
}

bool proxy_player_info(lev::Proxy *proxy, unsigned &players, bool &optimized) {
    if (!proxy)
        return false;
    try {
        proxy->loadMetadata(true);
    } catch (XLevelLoading &) {
        return false;
    }
    bool network = proxy->hasNetworkMode();
    bool single = proxy->hasSingleMode();
    if (!network && !single)
        return false;
    optimized = network;
    players = network ? proxy->getNetworkPlayers() : 1;
    if (players < 1)
        players = 1;
    return true;
}

std::string no_level_message(unsigned min_players, unsigned desired_players) {
    if (min_players == desired_players) {
        return ecl::strf(_("No %u player levels are in this level pack"), desired_players);
    }
    return ecl::strf(_("No %u-%u player levels are in this level pack"), min_players,
                     desired_players);
}

std::string resolved_lobby_server(const std::string &server, const InternetEndpoints &servers) {
    if (servers.lobby.is_valid())
        return servers.lobby.server;
    if (servers.lobby_control.is_valid())
        return servers.lobby_control.url;
    return server;
}

}  // namespace mp_menu
}  // namespace gui
}  // namespace enigma
