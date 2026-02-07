#include "multiplayer_config.hh"

#include "options.hh"

#include <cstdlib>
#include <string>

namespace enigma {
namespace multiplayer {

namespace {

bool force_relay_enabled() {
    const char *env = std::getenv("ENIGMA_MP_FORCE_RELAY");
    return env && *env;
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

}  // namespace

MultiplayerConfig LoadMultiplayerConfig() {
    MultiplayerConfig cfg;
    cfg.server_host = sanitize_host_only(options::GetString("MultiplayerLobbyServer"));
    cfg.lobby_port = clamp_port(options::GetInt("MultiplayerInternetLobbyPort"), 12347);
    cfg.udp_relay_port = clamp_port(options::GetInt("MultiplayerInternetUdpRelayPort"), 12348);
    cfg.tcp_relay_port = clamp_port(options::GetInt("MultiplayerInternetTcpRelayPort"), 12349);
    cfg.enable_direct = options::GetBool("MultiplayerEnableDirect");
    cfg.enable_udp_relay = options::GetBool("MultiplayerEnableUdpRelay");
    cfg.enable_tcp_relay = options::GetBool("MultiplayerEnableTcpRelay");
    cfg.force_relay = force_relay_enabled();
    return cfg;
}

InternetServers ResolveInternetServers(const MultiplayerConfig &cfg) {
    InternetServers out;
    if (cfg.server_host.empty() || cfg.server_host == "CHANGEME")
        return out;
    out.lobby = cfg.server_host + ":" + std::to_string(cfg.lobby_port);
    out.udp_relay = cfg.server_host + ":" + std::to_string(cfg.udp_relay_port);
    out.tcp_relay = cfg.server_host + ":" + std::to_string(cfg.tcp_relay_port);
    return out;
}

}  // namespace multiplayer
}  // namespace enigma
