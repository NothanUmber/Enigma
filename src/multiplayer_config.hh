#ifndef MULTIPLAYER_CONFIG_HH_INCLUDED
#define MULTIPLAYER_CONFIG_HH_INCLUDED

#include <cstdint>
#include <string>

namespace enigma {
namespace multiplayer {

struct InternetServers {
    std::string lobby;      // host:port
    std::string udp_relay;  // host:port
    std::string tcp_relay;  // host:port
};

struct MultiplayerConfig {
    std::string server_host;  // host only (no port)

    std::uint16_t lobby_port = 12347;
    std::uint16_t udp_relay_port = 12348;
    std::uint16_t tcp_relay_port = 12349;

    bool enable_direct = true;
    bool enable_udp_relay = true;
    bool enable_tcp_relay = true;

    bool force_relay = false;  // env override
};

// Loads the effective multiplayer configuration from the persisted options plus
// env overrides (for debugging).
MultiplayerConfig LoadMultiplayerConfig();

// Derives "host:port" endpoints for the lobby and relays from the config.
InternetServers ResolveInternetServers(const MultiplayerConfig &cfg);

}  // namespace multiplayer
}  // namespace enigma

#endif

