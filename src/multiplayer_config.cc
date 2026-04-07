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
#include "multiplayer_config.hh"

#include "options.hh"

#include <cstdlib>
#include <string>

/* -------------------- Multiplayer config -------------------- */
/*
 * Centralized load/validation for multiplayer configuration.
 *
 * This is shared between UI and runtime to keep behavior consistent across
 * LAN and Internet play, and to make env overrides (debug helpers) explicit.
 */

namespace enigma {
namespace multiplayer {

namespace {

bool force_relay_enabled() {
    const char *env = std::getenv("ENIGMA_MP_FORCE_RELAY");
    if (env && *env)
        return true;
    return options::GetBool("MultiplayerDebugForceRelay");
}

std::uint16_t clamp_port(int value, std::uint16_t fallback) {
    if (value > 0 && value <= 65535)
        return static_cast<std::uint16_t>(value);
    return fallback;
}

std::string sanitize_host_only(const std::string &raw) {
    std::string host = raw;
    if (host.empty())
        host = "CHANGEME";
    // Backwards-compat: if the saved config still contains host:port, strip the port.
    std::string::size_type port_sep = host.rfind(':');
    if (port_sep != std::string::npos)
        host = host.substr(0, port_sep);
    if (host == "localhost" || host == "::1")
        host = "127.0.0.1";
    if (host.empty())
        host = "CHANGEME";
    return host;
}

bool parse_server_endpoint(const std::string &value, std::string &host,
                           std::uint16_t &port, std::uint16_t default_port) {
    host.clear();
    port = default_port;
    if (value.empty())
        return false;

    std::string::size_type pos = value.rfind(':');
    if (pos == std::string::npos) {
        host = value;
    } else {
        host = value.substr(0, pos);
        std::string port_str = value.substr(pos + 1);
        if (!port_str.empty()) {
            char *end = nullptr;
            long parsed = std::strtol(port_str.c_str(), &end, 10);
            if (end && *end == '\0' && parsed > 0 && parsed <= 65535)
                port = static_cast<std::uint16_t>(parsed);
        }
    }

    if (host == "localhost" || host == "::1")
        host = "127.0.0.1";
    return !host.empty();
}

ResolvedInternetUrl parse_internet_url(const std::string &value,
                                       const char *scheme_a, std::uint16_t port_a,
                                       const char *scheme_b, std::uint16_t port_b) {
    ResolvedInternetUrl out;
    out.url = value;
    if (value.empty())
        return out;

    std::string::size_type scheme_pos = value.find("://");
    if (scheme_pos == std::string::npos)
        return out;
    out.scheme = value.substr(0, scheme_pos);
    if (out.scheme != scheme_a && out.scheme != scheme_b)
        return out;

    const std::string rest = value.substr(scheme_pos + 3);
    if (rest.empty())
        return out;

    std::string::size_type path_pos = rest.find('/');
    std::string authority = (path_pos == std::string::npos) ? rest : rest.substr(0, path_pos);
    out.path = (path_pos == std::string::npos) ? "/" : rest.substr(path_pos);
    if (authority.empty() || out.path.empty())
        return out;

    std::string host = authority;
    std::uint16_t port = (out.scheme == scheme_b) ? port_b : port_a;
    std::string::size_type colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        const std::string port_str = authority.substr(colon + 1);
        if (host.empty() || port_str.empty())
            return ResolvedInternetUrl();
        char *end = nullptr;
        long parsed = std::strtol(port_str.c_str(), &end, 10);
        if (!end || *end != '\0' || parsed <= 0 || parsed > 65535)
            return ResolvedInternetUrl();
        port = static_cast<std::uint16_t>(parsed);
    }

    if (host == "localhost" || host == "::1")
        host = "127.0.0.1";
    if (host.empty())
        return ResolvedInternetUrl();

    out.host = host;
    out.port = port;
    out.url = out.scheme + "://" + out.host + ":" + std::to_string(out.port) + out.path;
    return out;
}

ResolvedInternetEndpoint make_configured_endpoint(const std::string &host, std::uint16_t port) {
    ResolvedInternetEndpoint endpoint;
    std::string sanitized = sanitize_host_only(host);
    if (sanitized.empty() || sanitized == "CHANGEME" || port == 0)
        return endpoint;
    endpoint.host = sanitized;
    endpoint.port = port;
    endpoint.server = endpoint.host + ":" + std::to_string(endpoint.port);
    return endpoint;
}

}  // namespace

MultiplayerConfig LoadMultiplayerConfig() {
    MultiplayerConfig cfg;
    cfg.server_host = sanitize_host_only(options::GetString("MultiplayerLobbyServer"));
    cfg.lobby_control_url = options::GetString("MultiplayerLobbyControlUrl");
    cfg.websocket_relay_url = options::GetString("MultiplayerWebSocketRelayUrl");
    cfg.lobby_port = clamp_port(options::GetInt("MultiplayerInternetLobbyPort"), 12347);
    cfg.udp_relay_port = clamp_port(options::GetInt("MultiplayerInternetUdpRelayPort"), 12348);
    cfg.tcp_relay_port = clamp_port(options::GetInt("MultiplayerInternetTcpRelayPort"), 12349);
    cfg.enable_direct = options::GetBool("MultiplayerEnableDirect");
    cfg.enable_udp_relay = options::GetBool("MultiplayerEnableUdpRelay");
    cfg.enable_websocket_relay = options::GetBool("MultiplayerEnableWebSocketRelay");
    cfg.enable_tcp_relay = options::GetBool("MultiplayerEnableTcpRelay");
    cfg.force_relay = force_relay_enabled();
    return cfg;
}

InternetServers ResolveInternetServers(const MultiplayerConfig &cfg) {
    InternetServers out;
    ResolvedInternetServers resolved = ResolveInternetEndpoints(cfg);
    out.lobby = resolved.lobby.server;
    out.lobby_control = resolved.lobby_control.url;
    out.udp_relay = resolved.udp_relay.server;
    out.websocket_relay = resolved.websocket_relay.url;
    out.tcp_relay = resolved.tcp_relay.server;
    return out;
}

ResolvedInternetServers ResolveInternetEndpoints(const MultiplayerConfig &cfg) {
    ResolvedInternetServers out;
    out.lobby_control = ResolveHttpUrl(cfg.lobby_control_url);
    out.websocket_relay = ResolveWebSocketRelayUrl(cfg.websocket_relay_url);
    if (!(cfg.server_host.empty() || cfg.server_host == "CHANGEME")) {
        out.lobby = make_configured_endpoint(cfg.server_host, cfg.lobby_port);
        out.udp_relay = make_configured_endpoint(cfg.server_host, cfg.udp_relay_port);
        out.tcp_relay = make_configured_endpoint(cfg.server_host, cfg.tcp_relay_port);
    }
    return out;
}

ResolvedInternetEndpoint ResolveInternetEndpoint(const std::string &server,
                                                std::uint16_t default_port) {
    ResolvedInternetEndpoint endpoint;
    endpoint.server = server;

    std::string host;
    std::uint16_t port = default_port;
    if (!parse_server_endpoint(server, host, port, default_port) || port == 0)
        return endpoint;

    endpoint.host = host;
    endpoint.port = port;
    endpoint.server = endpoint.host + ":" + std::to_string(endpoint.port);
    return endpoint;
}

ResolvedInternetUrl ResolveHttpUrl(const std::string &url) {
    return parse_internet_url(url, "http", 80, "https", 443);
}

ResolvedInternetUrl ResolveWebSocketRelayUrl(const std::string &url) {
    return parse_internet_url(url, "ws", 80, "wss", 443);
}

}  // namespace multiplayer
}  // namespace enigma
