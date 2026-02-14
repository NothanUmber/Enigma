/*
 * Copyright (C) 2026 Ferdinand Strixner (LLM collaboration)
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

#include "multiplayer_session.hh"

#include "multiplayer_session_impl.hh"
#include "multiplayer_config.hh"
#include "multiplayer_wait_settings.hh"

#include "enigma.hh"
#include "input.hh"
#include "options.hh"

#include "SDL.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <cerrno>
#endif

/* -------------------- Multiplayer session start -------------------- */
/*
 * Session start/join handshakes.
 *
 * Builds the transport connection (direct or relay) and initializes the
 * deterministic runtime state before unpausing the simulation.
 */

namespace enigma {
namespace multiplayer {
namespace internal {

namespace {
constexpr double kLegacyTickSeconds = 0.01;  // historical tick duration (10ms)

Uint16 read_tick_ms_override() {
    int ms = options::GetInt("MultiplayerDebugTickLengthMs");
    if (ms <= 0)
        ms = 10;
    if (ms < 5)
        ms = 5;
    if (ms > 50)
        ms = 50;
    return static_cast<Uint16>(ms);
}

uint32_t legacy_ticks_to_current_delay_ticks(uint32_t legacy_ticks, double tick_seconds) {
    if (legacy_ticks == 0)
        return 0;
    if (tick_seconds <= 0.0)
        return legacy_ticks;
    // Delay is safety-critical: round up so we don't accidentally under-delay.
    const double desired_s = static_cast<double>(legacy_ticks) * kLegacyTickSeconds;
    uint32_t ticks = static_cast<uint32_t>(std::ceil(desired_s / tick_seconds));
    if (ticks < 1)
        ticks = 1;
    return ticks;
}
}  // namespace

void configure_input_session(unsigned expected_players) {
    // Tick duration must match across peers; it is negotiated by the host and
    // stored on g_session.tick_ms (clients receive it in WELCOME).
    Uint16 tick_ms = g_session.tick_ms ? g_session.tick_ms : read_tick_ms_override();
    g_session.tick_ms = tick_ms;
    const double tick_seconds = static_cast<double>(tick_ms) / 1000.0;

    // The input stream is stamped for a future tick (input delay). A larger
    // delay reduces the odds that a peer reaches a tick before receiving the
    // other players' inputs for that tick.
    // NOTE: These debug options historically assumed 10ms ticks. Now that tick
    // duration is configurable, keep the UX stable by interpreting the values
    // as "legacy ticks" and converting to current ticks.
    uint32_t legacy_delay =
        (g_session.active_transport == TransportKind::TCP_RELAY) ? kInputDelayTcpRelay : kInputDelay;
    int delay_override = options::GetInt("MultiplayerDebugInputDelayTicks");
    if (delay_override > 0) {
        if (delay_override > static_cast<int>(kMaxInputDelayLegacyTicks))
            delay_override = static_cast<int>(kMaxInputDelayLegacyTicks);
        legacy_delay = static_cast<uint32_t>(delay_override);
    }
    g_session.input_delay = legacy_ticks_to_current_delay_ticks(legacy_delay, tick_seconds);
    // Current-tick delay still needs a hard cap to avoid pathological enqueue sizes.
    if (g_session.input_delay > 1024)
        g_session.input_delay = 1024;
    if (debug_enabled())
        debug_log("mp input delay=%u (legacy=%u) transport=%s tick_ms=%u",
                  static_cast<unsigned>(g_session.input_delay),
                  static_cast<unsigned>(legacy_delay),
                  transport_name(g_session.active_transport),
                  static_cast<unsigned>(g_session.tick_ms));
    input::Reset();
    input::SetTickTimestep(static_cast<double>(tick_ms) / 1000.0);
    input::SetNetworked(true);
    input::SetExpectedPlayers(expected_players);
    for (uint32_t tick = 0; tick < g_session.input_delay; ++tick) {
        for (unsigned player = 0; player < expected_players; ++player) {
            input::EnqueueInput(tick, player, input::PlayerInput());
        }
    }
    g_session.next_local_tick = g_session.input_delay;
    g_session.next_send_tick = 0;
    g_session.local_history.clear();
    g_session.late_mouse_valid.fill(false);
    g_session.late_mouse_applied_tick.fill(0);
    g_session.late_mouse_src_tick.fill(0);
    g_session.input_clock_tick = input::CurrentTick();
    g_session.input_clock_accu = 0.0;
    g_session.last_host_resync_broadcast_tick = UINT32_MAX;
    g_session.last_host_world_state_broadcast_tick = UINT32_MAX;
    g_session.last_accepted_resync_tick = 0;
}

namespace {

void log_enet_socket_address(const char *tag, ENetHost *host) {
    if (!debug_enabled() || !host)
        return;
    // ENet 1.0 does not expose enet_socket_get_address(). For debug logging,
    // host->address is good enough across ENet versions.
    ENetAddress a = host->address;
    char ipbuf[64];
    ipbuf[0] = '\0';
    std::string local_ip = address_to_ip_string(a);
    if (local_ip.empty())
        std::snprintf(ipbuf, sizeof(ipbuf), "<unknown>");
    else
        std::snprintf(ipbuf, sizeof(ipbuf), "%s", local_ip.c_str());
    debug_log("mp %s local=%s:%u", tag, ipbuf, static_cast<unsigned>(a.port));
}

struct ConnectStrategy {
    bool enabled;
    const char *disabled_log;
    bool (*attempt)(const protocol::LobbyStart &, const std::string &);
};

enum class JoinPhase {
    IDLE = 0,
    CONNECTING = 1,
    WAIT_WELCOME = 2,
    TCP_CONNECTING = 3,
    TCP_WAIT_WELCOME = 4
};

struct ClientJoinState {
    bool active = false;
    protocol::LobbyStart start;
    // Candidate IPs for direct-connect attempts (multi-homed hosts, VMs).
    std::vector<std::string> host_ips;
    MultiplayerConfig cfg;

    // Ordered list of strategies to try (filtered by cfg).
    std::vector<TransportKind> strategies;
    size_t strategy_index = 0;
    size_t direct_host_index = 0;

    // Current attempt.
    TransportKind attempt_kind = TransportKind::NONE;
    bool relay_connect = false;  // ENet relay HELLO is needed.
    std::string target_host;
    Uint16 target_port = 0;
    JoinPhase phase = JoinPhase::IDLE;
    Uint32 connect_deadline = 0;
    Uint32 welcome_deadline = 0;

    // TCP relay connect attempt state.
    addrinfo *tcp_addrs = nullptr;
    addrinfo *tcp_next = nullptr;

    // Last ENet event observed during the current attempt (CONNECTING/WAIT_WELCOME).
    int last_enet_event = -1;
};

ClientJoinState g_join;

bool is_numeric_private_ipv4(const std::string &host) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(host.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return false;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    return (a == 10) ||
           (a == 172 && b >= 16 && b <= 31) ||
           (a == 192 && b == 168) ||
           (a == 127) ||
           (a == 169 && b == 254);
}

bool should_bind_local_interface_for_enet(const std::string &host) {
    // Binding a UDP socket to a specific local interface can improve behavior on
    // multi-homed machines (VPNs), but it can also break in VM/NAT setups where
    // the OS/network stack rewrites or routes differently than our probe expects.
    // Default: do not bind for LAN/private targets; allow opting in via env var.
    if (std::getenv("ENIGMA_MP_BIND_LOCAL") != nullptr ||
        options::GetBool("MultiplayerDebugBindLocal"))
        return true;

    auto ends_with = [](const std::string &s, const char *suffix) -> bool {
        size_t n = std::strlen(suffix);
        if (s.size() < n)
            return false;
        return s.compare(s.size() - n, n, suffix) == 0;
    };

    if (host == "localhost" || ends_with(host, ".local") || ends_with(host, ".localdomain"))
        return false;
    if (is_numeric_private_ipv4(host))
        return false;
    return true;
}

bool probe_local_bind_ipv4_for_remote(const std::string &remote_host, Uint16 remote_port,
                                      std::string &out_local_ip) {
    out_local_ip.clear();
    if (remote_host.empty() || remote_port == 0)
        return false;

    addrinfo hints;
    ::memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_INET;

    addrinfo *res = nullptr;
    std::string port_str = std::to_string(static_cast<unsigned>(remote_port));
    if (getaddrinfo(remote_host.c_str(), port_str.c_str(), &hints, &res) != 0)
        return false;

    bool ok = false;
    for (addrinfo *ai = res; ai; ai = ai->ai_next) {
        TcpSocket s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (!tcp_socket_valid(s))
            continue;
        if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) != 0) {
            tcp_close(s);
            continue;
        }

        sockaddr_in local_addr;
        ::memset(&local_addr, 0, sizeof(local_addr));
#ifdef WIN32
        int addr_len = static_cast<int>(sizeof(local_addr));
#else
        socklen_t addr_len = static_cast<socklen_t>(sizeof(local_addr));
#endif
        if (getsockname(s, reinterpret_cast<sockaddr *>(&local_addr), &addr_len) == 0) {
            char ipbuf[INET_ADDRSTRLEN];
            ipbuf[0] = '\0';
            if (inet_ntop(AF_INET, &local_addr.sin_addr, ipbuf, sizeof(ipbuf)) != nullptr) {
                out_local_ip = ipbuf;
                ok = !out_local_ip.empty();
            }
        }
        tcp_close(s);
        if (ok)
            break;
    }

    freeaddrinfo(res);
    return ok;
}

void join_clear_network_state() {
    if (g_session.host_handle) {
        enet_host_destroy(g_session.host_handle);
        g_session.host_handle = nullptr;
    }
    g_session.server_peer = nullptr;
    // Host-side relay state is not used for client joins.
    if (g_session.relay_handle) {
        enet_host_destroy(g_session.relay_handle);
        g_session.relay_handle = nullptr;
    }
    g_session.relay_peer = nullptr;
    tcp_close(g_session.tcp_relay_socket);
    g_session.tcp_relay_rx.clear();
    g_session.tcp_relay_frame_len = 0;

    if (g_join.tcp_addrs) {
        freeaddrinfo(g_join.tcp_addrs);
        g_join.tcp_addrs = nullptr;
    }
    g_join.tcp_next = nullptr;
}

void join_fail_current_attempt() {
    join_clear_network_state();
    g_join.phase = JoinPhase::IDLE;
    g_join.relay_connect = false;
    g_join.target_host.clear();
    g_join.target_port = 0;
    g_join.last_enet_event = -1;
}

bool join_begin_enet_attempt(const std::string &host, Uint16 port, bool relay_connect,
                            Uint32 connect_timeout_ms, Uint32 welcome_timeout_ms) {
    join_clear_network_state();
    g_join.relay_connect = relay_connect;
    g_join.target_host = host;
    g_join.target_port = port;
    g_join.last_enet_event = -1;

    // On multi-homed systems, the OS may pick an unexpected source address for
    // outgoing UDP if we bind to ENET_HOST_ANY. We can probe the route and bind
    // ENet to that local interface, but do this only when it is expected to help
    // (see should_bind_local_interface_for_enet()).
    ENetAddress local_bind_addr;
    ENetAddress *local_bind_ptr = nullptr;
    std::string local_bind_ip;
    if (should_bind_local_interface_for_enet(host)) {
        if (probe_local_bind_ipv4_for_remote(host, port, local_bind_ip)) {
            if (enet_address_set_host(&local_bind_addr, local_bind_ip.c_str()) == 0) {
                local_bind_addr.port = 0;  // ephemeral
                local_bind_ptr = &local_bind_addr;
                if (debug_enabled())
                    debug_log("mp client: bind %s for connect to %s:%u", local_bind_ip.c_str(),
                              host.c_str(), static_cast<unsigned>(port));
            }
        }
    }

    g_session.host_handle = enet_host_create(local_bind_ptr, 1,
#ifdef ENET_VER_EQ_GT_13
                                             2 /* channels */,
#endif
                                             0, 0);
    if (g_session.host_handle == nullptr)
        return false;
    log_enet_socket_address("client: enet socket", g_session.host_handle);

    ENetAddress addr;
    if (enet_address_set_host(&addr, host.c_str()) != 0) {
        if (debug_enabled())
            debug_log("mp client: invalid host '%s'", host.c_str());
        join_clear_network_state();
        return false;
    }
    addr.port = port;
    g_session.server_peer = enet_host_connect(g_session.host_handle, &addr, 2
#ifdef ENET_VER_EQ_GT_13
                                              ,
                                              0 /* data */
#endif
    );
    if (g_session.server_peer == nullptr) {
        join_clear_network_state();
        return false;
    }

    Uint32 now = SDL_GetTicks();
    g_join.phase = JoinPhase::CONNECTING;
    g_join.connect_deadline = now + connect_timeout_ms;
    g_join.welcome_deadline = now + welcome_timeout_ms;
    return true;
}

bool join_begin_tcp_relay_attempt(const std::string &host, Uint16 port) {
    join_clear_network_state();
    g_join.relay_connect = false;
    g_join.target_host = host;
    g_join.target_port = port;

    if (host.empty() || port == 0)
        return false;

    addrinfo hints;
    ::memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    addrinfo *res = nullptr;
    std::string port_str = std::to_string(static_cast<unsigned>(port));
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0)
        return false;
    g_join.tcp_addrs = res;
    g_join.tcp_next = res;

    Uint32 now = SDL_GetTicks();
    g_join.phase = JoinPhase::TCP_CONNECTING;
    g_join.connect_deadline = now + 3000;
    g_join.welcome_deadline = now + kJoinTimeoutMs;

    // Start the first address attempt.
    while (g_join.tcp_next) {
        addrinfo *ai = g_join.tcp_next;
        g_join.tcp_next = g_join.tcp_next->ai_next;

        TcpSocket s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (!tcp_socket_valid(s))
            continue;
        if (!tcp_set_nonblocking(s)) {
            tcp_close(s);
            continue;
        }
        int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
#ifdef WIN32
        if (rc == 0) {
            g_session.tcp_relay_socket = s;
            g_join.phase = JoinPhase::TCP_WAIT_WELCOME;
        } else {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
                tcp_close(s);
                continue;
            }
            g_session.tcp_relay_socket = s;
            g_join.phase = JoinPhase::TCP_CONNECTING;
        }
#else
        if (rc == 0) {
            g_session.tcp_relay_socket = s;
            g_join.phase = JoinPhase::TCP_WAIT_WELCOME;
        } else {
            if (errno != EINPROGRESS) {
                tcp_close(s);
                continue;
            }
            g_session.tcp_relay_socket = s;
            g_join.phase = JoinPhase::TCP_CONNECTING;
        }
#endif

        // If we're already connected, send the HELLO immediately.
        if (g_join.phase == JoinPhase::TCP_WAIT_WELCOME) {
            ecl::Buffer hello;
            encode_relay_header(hello, RELAY_HELLO_CLIENT, g_join.start.session_id, 0);
            if (!tcp_send_frame(g_session.tcp_relay_socket, hello.data(), hello.size())) {
                tcp_close(g_session.tcp_relay_socket);
                continue;
            }
        }
        return true;
    }

    join_clear_network_state();
    return false;
}

bool join_begin_next_attempt() {
    auto direct_connect_timeout_ms_for_host = [](const std::string &host) -> Uint32 {
        auto ends_with = [](const std::string &s, const char *suffix) -> bool {
            size_t n = std::strlen(suffix);
            if (s.size() < n)
                return false;
            return s.compare(s.size() - n, n, suffix) == 0;
        };

        if (host == "localhost" || ends_with(host, ".local") || ends_with(host, ".localdomain"))
            return kDirectConnectTimeoutMsLan;

        // If the host is a numeric IPv4, treat public addresses as "Internet" and
        // use a short timeout so we fall back to relays quickly when NAT blocks.
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (std::sscanf(host.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a <= 255 && b <= 255 &&
            c <= 255 && d <= 255) {
            const bool is_private =
                (a == 10) ||
                (a == 172 && b >= 16 && b <= 31) ||
                (a == 192 && b == 168) ||
                (a == 127) ||
                (a == 169 && b == 254);
            return is_private ? kDirectConnectTimeoutMsLan : kDirectConnectTimeoutMsInternet;
        }

        return kDirectConnectTimeoutMsLan;
    };

    while (g_join.strategy_index < g_join.strategies.size()) {
        TransportKind kind = g_join.strategies[g_join.strategy_index];
        g_join.attempt_kind = kind;

        if (kind == TransportKind::DIRECT) {
            while (g_join.direct_host_index < g_join.host_ips.size()) {
                const std::string &host = g_join.host_ips[g_join.direct_host_index++];
                if (host.empty())
                    continue;
                if (debug_enabled())
                    debug_log("mp client: connect %s:%u relay=0", host.c_str(),
                              static_cast<unsigned>(g_join.start.host_port));
                Uint32 connect_timeout_ms = direct_connect_timeout_ms_for_host(host);
                // If we have multiple host IP candidates (multi-homed/VPN/VM),
                // cycle through them quickly. A long ENet connect timeout on a
                // single unreachable interface makes LAN joins feel stuck.
                if (g_join.host_ips.size() > 1 &&
                    connect_timeout_ms == kDirectConnectTimeoutMsLan &&
                    is_numeric_private_ipv4(host)) {
                    connect_timeout_ms = 5000;
                }
                if (join_begin_enet_attempt(host, g_join.start.host_port, false,
                                            connect_timeout_ms,
                                            kJoinTimeoutMs)) {
                    return true;
                }
            }
            g_join.strategy_index++;
            continue;
        }
        if (kind == TransportKind::UDP_RELAY) {
            g_join.strategy_index++;
            if (g_relay_server.empty())
                continue;
            if (debug_enabled())
                debug_log("mp client: relay server=%s", g_relay_server.c_str());
            std::string relay_host;
            Uint16 relay_port = 0;
            if (!parse_host_port(g_relay_server, relay_host, relay_port))
                continue;
            if (join_begin_enet_attempt(relay_host, relay_port, true, 2000, kJoinTimeoutMs)) {
                return true;
            }
            continue;
        }
        if (kind == TransportKind::TCP_RELAY) {
            g_join.strategy_index++;
            if (g_tcp_relay_server.empty())
                continue;
            if (debug_enabled())
                debug_log("mp client: tcp relay server=%s", g_tcp_relay_server.c_str());
            std::string relay_host;
            Uint16 relay_port = 0;
            if (!parse_host_port(g_tcp_relay_server, relay_host, relay_port))
                continue;
            if (join_begin_tcp_relay_attempt(relay_host, relay_port)) {
                return true;
            }
            continue;
        }
    }
    return false;
}

void join_build_strategy_list() {
    g_join.strategies.clear();
    const bool enable_direct = g_join.cfg.enable_direct;
    const bool enable_udp_relay = g_join.cfg.enable_udp_relay;
    const bool enable_tcp_relay = g_join.cfg.enable_tcp_relay;
    const bool force_relay = g_join.cfg.force_relay;

    if (enable_direct && !force_relay && !g_join.host_ips.empty())
        g_join.strategies.push_back(TransportKind::DIRECT);
    if (enable_udp_relay)
        g_join.strategies.push_back(TransportKind::UDP_RELAY);
    if (enable_tcp_relay)
        g_join.strategies.push_back(TransportKind::TCP_RELAY);

    if (debug_enabled()) {
        if (!enable_direct)
            debug_log("mp client: direct connect disabled");
        else if (force_relay)
            debug_log("mp client: force relay enabled");
        if (!enable_udp_relay)
            debug_log("mp client: udp relay disabled");
        if (!enable_tcp_relay)
            debug_log("mp client: tcp relay disabled");
    }
}

// Session startup helpers keep SessionStartHost/SessionStartClient short by isolating
// transport-specific bootstrapping/handshake code paths.
void reset_session_bootstrap(const protocol::LobbyStart &start, unsigned expected_players,
                             bool is_host, bool local_player_known, unsigned local_player) {
    if (g_session.host_handle) {
        enet_host_destroy(g_session.host_handle);
        g_session.host_handle = nullptr;
    }
    if (g_session.relay_handle) {
        enet_host_destroy(g_session.relay_handle);
        g_session.relay_handle = nullptr;
    }
    g_session.server_peer = nullptr;
    g_session.relay_peer = nullptr;
    tcp_close(g_session.tcp_relay_socket);

    g_session = SessionState();
    g_session.active = true;
    g_session.host = is_host;
    g_session.local_player_known = local_player_known;
    g_session.local_player = local_player;
    g_session.expected_players = expected_players;
    g_session.session_id = start.session_id;
    g_session.seed = start.seed;
    g_session.level_pack_name = start.pack_name;
    g_session.level_id = start.level_id;
    g_session.tick_ms = read_tick_ms_override();
    g_session.menu_open.assign(expected_players, false);
    g_session.player_in_use.assign(expected_players, false);
    if (is_host && expected_players > 0)
        g_session.player_in_use[0] = true;

    enigma::Randomize(start.seed, true);
    configure_input_session(expected_players);

    // READY/START must be keyed to a nonzero load_id so clients can never signal
    // readiness while still on an arbitrary previously-loaded level.
    if (is_host && !start.level_id.empty()) {
        g_session.load_id = 1;
    }
}

bool host_open_direct_listener(Uint16 port, unsigned expected_players) {
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;
    unsigned peer_capacity = 0;
    if (expected_players > 1) {
        // Allow a few extra peers so clients can retry with alternate host IPs
        // without the host getting "stuck" on a half-connected attempt.
        peer_capacity = std::max<unsigned>(expected_players - 1, 8);
    }
    g_session.host_handle = enet_host_create(&address, peer_capacity,
#ifdef ENET_VER_EQ_GT_13
                                             2 /* channels */,
#endif
                                             0, 0);
    if (g_session.host_handle == nullptr) {
        if (debug_enabled())
            debug_log("mp host: failed to open direct listener port=%u",
                      static_cast<unsigned>(port));
        return false;
    }
    if (debug_enabled())
        debug_log("mp host: direct listener port=%u", static_cast<unsigned>(port));
    log_enet_socket_address("host: direct listener", g_session.host_handle);
    // Do not tweak ENet-managed sockets. ENet already configures non-blocking
    // mode and buffering; overriding this can break connect/handshake on some
    // platform builds.
    return true;
}

void host_try_connect_udp_relay() {
    multiplayer::MultiplayerConfig cfg = multiplayer::LoadMultiplayerConfig();
    if (!cfg.enable_udp_relay || g_relay_server.empty())
        return;
    if (debug_enabled())
        debug_log("mp host: relay server=%s", g_relay_server.c_str());

    std::string relay_host;
    Uint16 relay_port = 0;
    if (!parse_host_port(g_relay_server, relay_host, relay_port))
        return;

    if (g_session.relay_handle)
        enet_host_destroy(g_session.relay_handle);
    g_session.relay_handle = enet_host_create(nullptr, 1,
#ifdef ENET_VER_EQ_GT_13
                                              2 /* channels */,
#endif
                                              0, 0);
    if (!g_session.relay_handle)
        return;

    ENetAddress relay_addr;
    enet_address_set_host(&relay_addr, relay_host.c_str());
    relay_addr.port = relay_port;
    g_session.relay_peer = enet_host_connect(g_session.relay_handle, &relay_addr, 2
#ifdef ENET_VER_EQ_GT_13
                                             ,
                                             0 /* data */
#endif
    );
    if (!g_session.relay_peer) {
        debug_log("mp host: relay connect failed %s:%u", relay_host.c_str(),
                  static_cast<unsigned>(relay_port));
        enet_host_destroy(g_session.relay_handle);
        g_session.relay_handle = nullptr;
        return;
    }

    ENetEvent event;
    if (enet_host_service(g_session.relay_handle, &event, 3000) > 0 &&
        event.type == ENET_EVENT_TYPE_CONNECT) {
        g_session.relay_peer = event.peer;
#ifdef ENET_VER_EQ_GT_13
        enet_peer_timeout(g_session.relay_peer, ENET_PEER_TIMEOUT_LIMIT,
                          static_cast<enet_uint32>(multiplayer::wait::kEnetPeerTimeoutMs),
                          static_cast<enet_uint32>(multiplayer::wait::kEnetPeerTimeoutMs));
#endif
        ecl::Buffer buf;
        encode_relay_header(buf, RELAY_HELLO_HOST, g_session.session_id, 0);
        ENetPacket *packet = enet_packet_create(buf.data(), buf.size(),
                                                ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(g_session.relay_peer, 0, packet);
        enet_host_flush(g_session.relay_handle);
        debug_log("mp host: connected to relay %s:%u", relay_host.c_str(),
                  static_cast<unsigned>(relay_port));
        return;
    }

    debug_log("mp host: relay connect timeout %s:%u", relay_host.c_str(),
              static_cast<unsigned>(relay_port));
    enet_host_destroy(g_session.relay_handle);
    g_session.relay_handle = nullptr;
    g_session.relay_peer = nullptr;
}

void host_try_connect_tcp_relay() {
    multiplayer::MultiplayerConfig cfg = multiplayer::LoadMultiplayerConfig();
    if (!cfg.enable_tcp_relay || g_tcp_relay_server.empty())
        return;
    if (debug_enabled())
        debug_log("mp host: tcp relay server=%s", g_tcp_relay_server.c_str());

    std::string relay_host;
    Uint16 relay_port = 0;
    if (!parse_host_port(g_tcp_relay_server, relay_host, relay_port))
        return;

    TcpSocket sock;
    if (!tcp_connect_timeout(relay_host, relay_port, 3000, sock)) {
        debug_log("mp host: tcp relay connect failed %s:%u", relay_host.c_str(),
                  static_cast<unsigned>(relay_port));
        return;
    }

    g_session.tcp_relay_socket = sock;
    ecl::Buffer buf;
    encode_relay_header(buf, RELAY_HELLO_HOST, g_session.session_id, 0);
    if (tcp_send_frame(g_session.tcp_relay_socket, buf.data(), buf.size())) {
        debug_log("mp host: connected to tcp relay %s:%u", relay_host.c_str(),
                  static_cast<unsigned>(relay_port));
        return;
    }

    debug_log("mp host: tcp relay hello failed %s:%u", relay_host.c_str(),
              static_cast<unsigned>(relay_port));
    tcp_close(g_session.tcp_relay_socket);
}

bool client_connect_and_wait_enet(const std::string &target_host, Uint16 target_port,
                                  bool relay_connect, Uint32 connect_timeout_ms,
                                  Uint32 welcome_timeout_ms, Uint32 session_id) {
    if (debug_enabled())
        debug_log("mp client: connect %s:%u relay=%d", target_host.c_str(),
                  static_cast<unsigned>(target_port), relay_connect ? 1 : 0);
    if (g_session.host_handle) {
        enet_host_destroy(g_session.host_handle);
        g_session.host_handle = nullptr;
    }
    g_session.server_peer = nullptr;
    ENetAddress local_bind_addr;
    ENetAddress *local_bind_ptr = nullptr;
    std::string local_bind_ip;
    if (probe_local_bind_ipv4_for_remote(target_host, target_port, local_bind_ip)) {
        if (enet_address_set_host(&local_bind_addr, local_bind_ip.c_str()) == 0) {
            local_bind_addr.port = 0;  // ephemeral
            local_bind_ptr = &local_bind_addr;
            if (debug_enabled())
                debug_log("mp client: bind %s for connect to %s:%u", local_bind_ip.c_str(),
                          target_host.c_str(), static_cast<unsigned>(target_port));
        }
    }

    g_session.host_handle = enet_host_create(local_bind_ptr, 1,
#ifdef ENET_VER_EQ_GT_13
                                             2 /* channels */,
#endif
                                             0, 0);
    if (g_session.host_handle == nullptr)
        return false;
    log_enet_socket_address("client: enet socket", g_session.host_handle);

    ENetAddress addr;
    if (enet_address_set_host(&addr, target_host.c_str()) != 0) {
        if (debug_enabled())
            debug_log("mp client: invalid host '%s'", target_host.c_str());
        enet_host_destroy(g_session.host_handle);
        g_session.host_handle = nullptr;
        return false;
    }
    addr.port = target_port;
    g_session.server_peer = enet_host_connect(g_session.host_handle, &addr, 2
#ifdef ENET_VER_EQ_GT_13
                                              ,
                                              0 /* data */
#endif
    );
    if (g_session.server_peer == nullptr)
        return false;

    ENetEvent event;
    const int serviced = enet_host_service(g_session.host_handle, &event, connect_timeout_ms);
    if (!(serviced > 0 && event.type == ENET_EVENT_TYPE_CONNECT)) {
        if (debug_enabled())
            debug_log("mp client: connect failed %s:%u service=%d event=%d", target_host.c_str(),
                      static_cast<unsigned>(target_port), serviced,
                      serviced > 0 ? static_cast<int>(event.type) : -1);
        enet_host_destroy(g_session.host_handle);
        g_session.host_handle = nullptr;
        g_session.server_peer = nullptr;
        return false;
    }
    g_session.server_peer = event.peer;
#ifdef ENET_VER_EQ_GT_13
    enet_peer_timeout(g_session.server_peer, ENET_PEER_TIMEOUT_LIMIT,
                      static_cast<enet_uint32>(multiplayer::wait::kEnetPeerTimeoutMs),
                      static_cast<enet_uint32>(multiplayer::wait::kEnetPeerTimeoutMs));
#endif
    if (relay_connect) {
        ecl::Buffer buf;
        encode_relay_header(buf, RELAY_HELLO_CLIENT, session_id, 0);
        ENetPacket *packet = enet_packet_create(buf.data(), buf.size(),
                                                ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(g_session.server_peer, 0, packet);
        enet_host_flush(g_session.host_handle);
    }

    Uint32 start_wait = SDL_GetTicks();
    while (SDL_GetTicks() - start_wait < welcome_timeout_ms) {
        int res = enet_host_service(g_session.host_handle, &event, 100);
        if (res <= 0)
            continue;
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            const char *data = reinterpret_cast<const char *>(event.packet->data);
            ecl::Buffer buf;
            buf.assign(const_cast<char *>(data), event.packet->dataLength);
            Uint8 player_id = 0;
            Uint8 expected_players = 0;
            Uint32 seed = 0;
            if (protocol::decode_welcome(buf, player_id, expected_players, seed)) {
                if (debug_enabled())
                    debug_log("mp client: welcome player=%u expected=%u seed=%u",
                              static_cast<unsigned>(player_id),
                              static_cast<unsigned>(expected_players),
                              static_cast<unsigned>(seed));
                g_session.local_player = player_id;
                g_session.local_player_known = true;
                g_session.expected_players = expected_players;
                g_session.seed = seed;
                input::SetExpectedPlayers(expected_players);
                g_session.active_transport =
                    relay_connect ? TransportKind::UDP_RELAY : TransportKind::DIRECT;
                if (debug_enabled())
                    debug_log("mp client: transport=%s",
                              transport_name(g_session.active_transport));
                enet_packet_destroy(event.packet);
                return true;
            }
            enet_packet_destroy(event.packet);
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            break;
        }
    }

    if (debug_enabled())
        debug_log("mp client: welcome timeout %s:%u", target_host.c_str(),
                  static_cast<unsigned>(target_port));
    if (g_session.server_peer) {
        enet_peer_reset(g_session.server_peer);
        g_session.server_peer = nullptr;
    }
    if (g_session.host_handle) {
        enet_host_destroy(g_session.host_handle);
        g_session.host_handle = nullptr;
    }
    return false;
}

bool client_try_connect_direct(const protocol::LobbyStart &start, const std::string &host_ip) {
    // Keep the actual connect timeout short, but allow a longer welcome window
    // in case the host is still loading/binding when the client attempts to join.
    Uint32 connect_timeout_ms = kDirectConnectTimeoutMsLan;
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(host_ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a <= 255 && b <= 255 &&
        c <= 255 && d <= 255) {
        const bool is_private =
            (a == 10) ||
            (a == 172 && b >= 16 && b <= 31) ||
            (a == 192 && b == 168) ||
            (a == 127) ||
            (a == 169 && b == 254);
        connect_timeout_ms = is_private ? kDirectConnectTimeoutMsLan : kDirectConnectTimeoutMsInternet;
    }
    return client_connect_and_wait_enet(host_ip, start.host_port, false, connect_timeout_ms,
                                        kJoinTimeoutMs, start.session_id);
}

bool client_try_connect_udp_relay(Uint32 session_id) {
    if (!g_relay_server.empty()) {
        if (debug_enabled())
            debug_log("mp client: relay server=%s", g_relay_server.c_str());
        std::string relay_host;
        Uint16 relay_port = 0;
        if (parse_host_port(g_relay_server, relay_host, relay_port)) {
            if (client_connect_and_wait_enet(relay_host, relay_port, true, 2000, kJoinTimeoutMs,
                                             session_id)) {
                return true;
            }
        }
    }
    return false;
}

bool client_try_connect_udp_relay(const protocol::LobbyStart &start, const std::string &host_ip) {
    static_cast<void>(host_ip);
    return client_try_connect_udp_relay(start.session_id);
}

bool client_try_connect_tcp_relay(Uint32 session_id) {
    if (g_tcp_relay_server.empty())
        return false;
    if (debug_enabled())
        debug_log("mp client: tcp relay server=%s", g_tcp_relay_server.c_str());

    // Ensure stale ENet state from previous attempts does not interfere with
    // TCP relay operation (client_send_payload prefers server_peer if set).
    if (g_session.host_handle) {
        enet_host_destroy(g_session.host_handle);
        g_session.host_handle = nullptr;
    }
    g_session.server_peer = nullptr;

    std::string relay_host;
    Uint16 relay_port = 0;
    if (!parse_host_port(g_tcp_relay_server, relay_host, relay_port))
        return false;

    TcpSocket sock;
    if (!tcp_connect_timeout(relay_host, relay_port, 3000, sock)) {
        if (debug_enabled())
            debug_log("mp client: connect failed %s:%u (tcp relay)", relay_host.c_str(),
                      static_cast<unsigned>(relay_port));
        return false;
    }
    g_session.tcp_relay_socket = sock;

    ecl::Buffer hello;
    encode_relay_header(hello, RELAY_HELLO_CLIENT, session_id, 0);
    if (!tcp_send_frame(g_session.tcp_relay_socket, hello.data(), hello.size())) {
        tcp_close(g_session.tcp_relay_socket);
        return false;
    }

    Uint32 start_wait = SDL_GetTicks();
    std::vector<uint8_t> frame;
    while (SDL_GetTicks() - start_wait < kJoinTimeoutMs) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_session.tcp_relay_socket, &rfds);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000;
#ifdef WIN32
        int sel = ::select(0, &rfds, nullptr, nullptr, &tv);
#else
        int sel = ::select(g_session.tcp_relay_socket + 1, &rfds, nullptr, nullptr, &tv);
#endif
        if (sel <= 0)
            continue;
        if (!tcp_pump_recv(g_session.tcp_relay_socket, g_session.tcp_relay_rx)) {
            tcp_close(g_session.tcp_relay_socket);
            break;
        }
        while (tcp_try_extract_frame(g_session.tcp_relay_rx, g_session.tcp_relay_frame_len,
                                     frame)) {
            const char *data = reinterpret_cast<const char *>(frame.data());
            ecl::Buffer buf;
            buf.assign(const_cast<char *>(data), frame.size());
            Uint8 player_id = 0;
            Uint8 expected = 0;
            Uint32 seed = 0;
            if (protocol::decode_welcome(buf, player_id, expected, seed)) {
                if (debug_enabled())
                    debug_log("mp client: welcome (tcp relay) player=%u expected=%u seed=%u",
                              static_cast<unsigned>(player_id), static_cast<unsigned>(expected),
                              static_cast<unsigned>(seed));
                g_session.local_player = player_id;
                g_session.local_player_known = true;
                g_session.expected_players = expected;
                g_session.seed = seed;
                input::SetExpectedPlayers(expected);
                g_session.active_transport = TransportKind::TCP_RELAY;
                if (debug_enabled())
                    debug_log("mp client: transport=%s",
                              transport_name(g_session.active_transport));
                return true;
            }
        }
    }

    if (debug_enabled())
        debug_log("mp client: welcome timeout %s:%u (tcp relay)", relay_host.c_str(),
                  static_cast<unsigned>(relay_port));
    tcp_close(g_session.tcp_relay_socket);
    return false;
}

bool client_try_connect_tcp_relay(const protocol::LobbyStart &start, const std::string &host_ip) {
    static_cast<void>(host_ip);
    return client_try_connect_tcp_relay(start.session_id);
}

}  // namespace

bool SessionStartHost(const protocol::LobbyStart &start) {
    if (g_session.active)
        return false;
    if (debug_enabled())
        debug_log("mp debug enabled");
    unsigned expected_players = std::min<unsigned>(start.expected_players, input::kMaxPlayers);
    if (expected_players < 1)
        return false;
    reset_session_bootstrap(start, expected_players, true, true, 0);
    if (!host_open_direct_listener(start.host_port, expected_players)) {
        SessionShutdown();
        return false;
    }

    host_try_connect_udp_relay();
    host_try_connect_tcp_relay();
    return true;
}

bool SessionStartClient(const protocol::LobbyStart &start, const std::string &host_ip) {
    if (g_session.active)
        return false;
    if (debug_enabled())
        debug_log("mp debug enabled");
    unsigned expected_players = std::min<unsigned>(start.expected_players, input::kMaxPlayers);
    if (expected_players < 1)
        return false;
    reset_session_bootstrap(start, expected_players, false, false, 1);
    multiplayer::MultiplayerConfig cfg = multiplayer::LoadMultiplayerConfig();
    const bool enable_direct = cfg.enable_direct;
    const bool enable_udp_relay = cfg.enable_udp_relay;
    const bool enable_tcp_relay = cfg.enable_tcp_relay;
    const bool force_relay = cfg.force_relay;
    const char *direct_disabled_log =
        !enable_direct ? "mp client: direct connect disabled"
                       : (force_relay ? "mp client: force relay enabled" : nullptr);

    const ConnectStrategy strategies[] = {
        {enable_direct && !force_relay, direct_disabled_log, client_try_connect_direct},
        {enable_udp_relay, "mp client: udp relay disabled",
         static_cast<bool (*)(const protocol::LobbyStart &, const std::string &)>(
             client_try_connect_udp_relay)},
        {enable_tcp_relay, "mp client: tcp relay disabled",
         static_cast<bool (*)(const protocol::LobbyStart &, const std::string &)>(
             client_try_connect_tcp_relay)},
    };

    auto try_strategies = [&](const ConnectStrategy *begin,
                              const ConnectStrategy *end) -> bool {
        for (const ConnectStrategy *it = begin; it != end; ++it) {
            if (!it->enabled) {
                if (debug_enabled() && it->disabled_log)
                    debug_log("%s", it->disabled_log);
                continue;
            }
            if (it->attempt(start, host_ip))
                return true;
        }
        return false;
    };

    if (try_strategies(std::begin(strategies), std::end(strategies)))
        return true;

    SessionShutdown();
    return false;
}

bool SessionBeginClientJoin(const protocol::LobbyStart &start, const std::string &host_ip) {
    std::vector<std::string> hosts;
    if (!host_ip.empty())
        hosts.push_back(host_ip);
    return SessionBeginClientJoin(start, hosts);
}

bool SessionBeginClientJoin(const protocol::LobbyStart &start,
                            const std::vector<std::string> &host_ips) {
    if (g_session.active || g_join.active)
        return false;
    if (debug_enabled())
        debug_log("mp debug enabled");
    unsigned expected_players = std::min<unsigned>(start.expected_players, input::kMaxPlayers);
    if (expected_players < 1)
        return false;

    reset_session_bootstrap(start, expected_players, false, false, 1);

    g_join = ClientJoinState();
    g_join.active = true;
    g_join.start = start;
    g_join.host_ips = host_ips;
    g_join.cfg = multiplayer::LoadMultiplayerConfig();
    if (debug_enabled()) {
        std::string hosts;
        for (size_t i = 0; i < g_join.host_ips.size(); ++i) {
            if (i)
                hosts += ",";
            hosts += g_join.host_ips[i];
        }
        debug_log("mp client: join begin session=%u port=%u hosts=%s",
                  static_cast<unsigned>(start.session_id),
                  static_cast<unsigned>(start.host_port),
                  hosts.c_str());
    }
    join_build_strategy_list();
    g_join.strategy_index = 0;
    g_join.direct_host_index = 0;

    if (!join_begin_next_attempt()) {
        SessionShutdown();
        g_join = ClientJoinState();
        return false;
    }
    return true;
}

multiplayer::ClientJoinStatus SessionPollClientJoin() {
    if (!g_join.active)
        return multiplayer::ClientJoinStatus::IDLE;

    Uint32 now = SDL_GetTicks();

    if (g_join.phase == JoinPhase::TCP_CONNECTING || g_join.phase == JoinPhase::TCP_WAIT_WELCOME) {
        if (tcp_socket_valid(g_session.tcp_relay_socket)) {
            if (g_join.phase == JoinPhase::TCP_CONNECTING) {
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(g_session.tcp_relay_socket, &wfds);
                timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 0;
#ifdef WIN32
                int sel = ::select(0, nullptr, &wfds, nullptr, &tv);
#else
                int sel = ::select(g_session.tcp_relay_socket + 1, nullptr, &wfds, nullptr, &tv);
#endif
                if (sel > 0) {
                    int err = 0;
#ifdef WIN32
                    int errlen = sizeof(err);
#else
                    socklen_t errlen = sizeof(err);
#endif
                    if (::getsockopt(g_session.tcp_relay_socket, SOL_SOCKET, SO_ERROR,
                                     reinterpret_cast<char *>(&err), &errlen) == 0 &&
                        err == 0) {
                        ecl::Buffer hello;
                        encode_relay_header(hello, RELAY_HELLO_CLIENT, g_join.start.session_id, 0);
                        if (tcp_send_frame(g_session.tcp_relay_socket, hello.data(), hello.size()))
                            g_join.phase = JoinPhase::TCP_WAIT_WELCOME;
                        else
                            join_fail_current_attempt();
                    } else {
                        join_fail_current_attempt();
                    }
                }
            }

            if (g_join.phase == JoinPhase::TCP_WAIT_WELCOME) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(g_session.tcp_relay_socket, &rfds);
                timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 0;
#ifdef WIN32
                int sel = ::select(0, &rfds, nullptr, nullptr, &tv);
#else
                int sel = ::select(g_session.tcp_relay_socket + 1, &rfds, nullptr, nullptr, &tv);
#endif
                if (sel > 0) {
                    if (!tcp_pump_recv(g_session.tcp_relay_socket, g_session.tcp_relay_rx)) {
                        join_fail_current_attempt();
                    } else {
                        std::vector<uint8_t> frame;
                        while (tcp_try_extract_frame(g_session.tcp_relay_rx,
                                                     g_session.tcp_relay_frame_len, frame)) {
                            const char *data = reinterpret_cast<const char *>(frame.data());
                            ecl::Buffer buf;
                            buf.assign(const_cast<char *>(data), frame.size());
                            Uint8 player_id = 0;
                            Uint8 expected_players = 0;
                            Uint32 seed = 0;
                            if (protocol::decode_welcome(buf, player_id, expected_players, seed)) {
                                if (debug_enabled())
                                    debug_log("mp client: welcome (tcp relay) player=%u expected=%u seed=%u",
                                              static_cast<unsigned>(player_id),
                                              static_cast<unsigned>(expected_players),
                                              static_cast<unsigned>(seed));
                                g_session.local_player = player_id;
                                g_session.local_player_known = true;
                                g_session.expected_players = expected_players;
                                g_session.seed = seed;
                                input::SetExpectedPlayers(expected_players);
                                g_session.active_transport = TransportKind::TCP_RELAY;
                                if (debug_enabled())
                                    debug_log("mp client: transport=%s",
                                              transport_name(g_session.active_transport));
                                g_join.active = false;
                                return multiplayer::ClientJoinStatus::JOINED;
                            }
                        }
                    }
                }
            }
        }

        if (g_join.phase == JoinPhase::TCP_CONNECTING && now > g_join.connect_deadline) {
            if (debug_enabled())
                debug_log("mp client: connect failed %s:%u (tcp relay)", g_join.target_host.c_str(),
                          static_cast<unsigned>(g_join.target_port));
            join_fail_current_attempt();
        } else if (g_join.phase == JoinPhase::TCP_WAIT_WELCOME && now > g_join.welcome_deadline) {
            if (debug_enabled())
                debug_log("mp client: welcome timeout %s:%u (tcp relay)", g_join.target_host.c_str(),
                          static_cast<unsigned>(g_join.target_port));
            join_fail_current_attempt();
        }

        if (g_join.phase == JoinPhase::IDLE) {
            if (join_begin_next_attempt())
                return multiplayer::ClientJoinStatus::CONNECTING;
            SessionShutdown();
            g_join = ClientJoinState();
            return multiplayer::ClientJoinStatus::FAILED;
        }

        return multiplayer::ClientJoinStatus::CONNECTING;
    }

    ENetEvent event;
    while (g_session.host_handle && enet_host_service(g_session.host_handle, &event, 0) > 0) {
        g_join.last_enet_event = static_cast<int>(event.type);
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            g_session.server_peer = event.peer;
#ifdef ENET_VER_EQ_GT_13
            enet_peer_timeout(g_session.server_peer, ENET_PEER_TIMEOUT_LIMIT,
                              static_cast<enet_uint32>(multiplayer::wait::kEnetPeerTimeoutMs),
                              static_cast<enet_uint32>(multiplayer::wait::kEnetPeerTimeoutMs));
#endif
            if (debug_enabled())
            {
                char rip[64];
                rip[0] = '\0';
                std::string remote_ip = address_to_ip_string(event.peer->address);
                if (remote_ip.empty())
                    std::snprintf(rip, sizeof(rip), "<unknown>");
                else
                    std::snprintf(rip, sizeof(rip), "%s", remote_ip.c_str());
                debug_log("mp client: connected target=%s:%u remote=%s:%u relay=%d",
                          g_join.target_host.c_str(),
                          static_cast<unsigned>(g_join.target_port),
                          rip,
                          static_cast<unsigned>(event.peer->address.port),
                          g_join.relay_connect ? 1 : 0);
            }
            if (g_join.relay_connect) {
                ecl::Buffer buf;
                encode_relay_header(buf, RELAY_HELLO_CLIENT, g_join.start.session_id, 0);
                ENetPacket *packet = enet_packet_create(buf.data(), buf.size(),
                                                        ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(g_session.server_peer, 0, packet);
                enet_host_flush(g_session.host_handle);
            }
            g_join.phase = JoinPhase::WAIT_WELCOME;
        } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            const char *data = reinterpret_cast<const char *>(event.packet->data);
            ecl::Buffer buf;
            buf.assign(const_cast<char *>(data), event.packet->dataLength);
            Uint8 player_id = 0;
            Uint8 expected_players = 0;
            Uint32 seed = 0;
            if (protocol::decode_welcome(buf, player_id, expected_players, seed)) {
                if (seed != g_join.start.seed) {
                    if (debug_enabled())
                        debug_log("mp client: welcome ignored (seed mismatch remote=%u local=%u)",
                                  static_cast<unsigned>(seed),
                                  static_cast<unsigned>(g_join.start.seed));
                    enet_packet_destroy(event.packet);
                    continue;
                }
                if (debug_enabled())
                    debug_log("mp client: welcome player=%u expected=%u seed=%u",
                              static_cast<unsigned>(player_id),
                              static_cast<unsigned>(expected_players),
                              static_cast<unsigned>(seed));
                g_session.local_player = player_id;
                g_session.local_player_known = true;
                g_session.expected_players = expected_players;
                g_session.seed = seed;
                input::SetExpectedPlayers(expected_players);
                g_session.active_transport =
                    g_join.relay_connect ? TransportKind::UDP_RELAY : TransportKind::DIRECT;
                if (debug_enabled())
                    debug_log("mp client: transport=%s",
                              transport_name(g_session.active_transport));

                enet_packet_destroy(event.packet);
                g_join.active = false;
                return multiplayer::ClientJoinStatus::JOINED;
            }
            enet_packet_destroy(event.packet);
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            break;
        }
    }

    if (g_join.phase == JoinPhase::CONNECTING && now > g_join.connect_deadline) {
        if (debug_enabled())
            debug_log("mp client: connect failed %s:%u last_event=%d", g_join.target_host.c_str(),
                      static_cast<unsigned>(g_join.target_port), g_join.last_enet_event);
        join_fail_current_attempt();
    } else if (g_join.phase == JoinPhase::WAIT_WELCOME && now > g_join.welcome_deadline) {
        if (debug_enabled())
            debug_log("mp client: welcome timeout %s:%u", g_join.target_host.c_str(),
                      static_cast<unsigned>(g_join.target_port));
        join_fail_current_attempt();
    }

    if (g_join.phase == JoinPhase::IDLE) {
        if (join_begin_next_attempt())
            return multiplayer::ClientJoinStatus::CONNECTING;
        SessionShutdown();
        g_join = ClientJoinState();
        return multiplayer::ClientJoinStatus::FAILED;
    }

    return multiplayer::ClientJoinStatus::CONNECTING;
}

void SessionCancelClientJoin() {
    if (!g_join.active)
        return;
    join_fail_current_attempt();
    SessionShutdown();
    g_join = ClientJoinState();
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
