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
#ifndef MULTIPLAYER_CONFIG_HH_INCLUDED
#define MULTIPLAYER_CONFIG_HH_INCLUDED

/* -------------------- Multiplayer config -------------------- */
/*
 * Multiplayer configuration shared by UI and runtime.
 *
 * The config is stored in Options and can be overridden via environment for
 * debugging (see `LoadMultiplayerConfig()`).
 */

#include <cstdint>
#include <string>

namespace enigma {
namespace multiplayer {

struct InternetServers {
    std::string lobby;      // host:port
    std::string lobby_control;  // http:// or https:// URL
    std::string udp_relay;  // host:port
    std::string websocket_relay;  // ws:// or wss:// URL
    std::string tcp_relay;  // host:port
};

struct ResolvedInternetEndpoint {
    std::string server;  // canonical host:port when valid, otherwise original input
    std::string host;
    std::uint16_t port = 0;

    bool empty() const {
        return server.empty();
    }

    bool is_valid() const {
        return !host.empty() && port != 0;
    }
};

struct ResolvedInternetUrl {
    std::string url;
    std::string scheme;
    std::string host;
    std::uint16_t port = 0;
    std::string path;

    bool empty() const {
        return url.empty();
    }

    bool secure() const {
        return scheme == "https" || scheme == "wss";
    }

    bool is_valid() const {
        return !url.empty() && !scheme.empty() && !host.empty() && port != 0 &&
               !path.empty();
    }
};

struct ResolvedInternetServers {
    ResolvedInternetEndpoint lobby;
    ResolvedInternetUrl lobby_control;
    ResolvedInternetEndpoint udp_relay;
    ResolvedInternetUrl websocket_relay;
    ResolvedInternetEndpoint tcp_relay;
};

struct MultiplayerConfig {
    std::string server_host;  // host only (no port)
    std::string lobby_control_url;
    std::string websocket_relay_url;

    std::uint16_t lobby_port = 12347;
    std::uint16_t udp_relay_port = 12348;
    std::uint16_t tcp_relay_port = 12349;

    bool enable_direct = true;
    bool enable_udp_relay = true;
    bool enable_websocket_relay = true;
    bool enable_tcp_relay = true;

    bool force_relay = false;  // env override
};

// Loads the effective multiplayer configuration from the persisted options plus
// env overrides (for debugging).
MultiplayerConfig LoadMultiplayerConfig();

// Derives "host:port" endpoints for the lobby and relays from the config.
InternetServers ResolveInternetServers(const MultiplayerConfig &cfg);

// Resolves the lobby and relay endpoints into canonical host/port bundles.
ResolvedInternetServers ResolveInternetEndpoints(const MultiplayerConfig &cfg);

// Parses a single runtime override or host:port string into a canonical endpoint.
ResolvedInternetEndpoint ResolveInternetEndpoint(const std::string &server,
                                                std::uint16_t default_port = 0);

ResolvedInternetUrl ResolveHttpUrl(const std::string &url);

ResolvedInternetUrl ResolveWebSocketRelayUrl(const std::string &url);

}  // namespace multiplayer
}  // namespace enigma

#endif
