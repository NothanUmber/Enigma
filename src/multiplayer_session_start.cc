#include "multiplayer_session.hh"

#include "multiplayer_session_impl.hh"
#include "multiplayer_config.hh"

#include "enigma.hh"
#include "input.hh"
#include "options.hh"

#include "SDL.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <cerrno>
#endif

namespace enigma {
namespace multiplayer {
namespace internal {

void configure_input_session(unsigned expected_players) {
    input::Reset();
    input::SetNetworked(true);
    input::SetExpectedPlayers(expected_players);
    for (uint32_t tick = 0; tick < kInputDelay; ++tick) {
        for (unsigned player = 0; player < expected_players; ++player) {
            input::EnqueueInput(tick, player, input::PlayerInput());
        }
    }
    g_session.next_local_tick = kInputDelay;
    g_session.next_send_tick = 0;
    g_session.local_history.clear();
    g_session.input_clock_tick = input::CurrentTick();
    g_session.input_clock_accu = 0.0;
}

namespace {

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
    std::string host_ip;
    MultiplayerConfig cfg;

    // Ordered list of strategies to try (filtered by cfg).
    std::vector<TransportKind> strategies;
    size_t strategy_index = 0;

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
};

ClientJoinState g_join;

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
}

bool join_begin_enet_attempt(const std::string &host, Uint16 port, bool relay_connect,
                            Uint32 connect_timeout_ms, Uint32 welcome_timeout_ms) {
    join_clear_network_state();
    g_join.relay_connect = relay_connect;
    g_join.target_host = host;
    g_join.target_port = port;

    g_session.host_handle = enet_host_create(nullptr, 1,
#ifdef ENET_VER_EQ_GT_13
                                             2 /* channels */,
#endif
                                             0, 0);
    if (g_session.host_handle == nullptr)
        return false;

    ENetAddress addr;
    enet_address_set_host(&addr, host.c_str());
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
    while (g_join.strategy_index < g_join.strategies.size()) {
        TransportKind kind = g_join.strategies[g_join.strategy_index++];
        g_join.attempt_kind = kind;

        if (kind == TransportKind::DIRECT) {
            if (debug_enabled())
                debug_log("mp client: connect %s:%u relay=0", g_join.host_ip.c_str(),
                          static_cast<unsigned>(g_join.start.host_port));
            if (join_begin_enet_attempt(g_join.host_ip, g_join.start.host_port, false, 3000,
                                        kJoinTimeoutMs)) {
                return true;
            }
            continue;
        }
        if (kind == TransportKind::UDP_RELAY) {
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

    if (enable_direct && !force_relay)
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
    g_session.menu_open.assign(expected_players, false);

    enigma::Randomize(start.seed, true);
    configure_input_session(expected_players);
}

bool host_open_direct_listener(Uint16 port, unsigned expected_players) {
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;
    g_session.host_handle = enet_host_create(&address, expected_players - 1,
#ifdef ENET_VER_EQ_GT_13
                                             2 /* channels */,
#endif
                                             0, 0);
    if (g_session.host_handle == nullptr)
        return false;
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
    g_session.host_handle = enet_host_create(nullptr, 1,
#ifdef ENET_VER_EQ_GT_13
                                             2 /* channels */,
#endif
                                             0, 0);
    if (g_session.host_handle == nullptr)
        return false;

    ENetAddress addr;
    enet_address_set_host(&addr, target_host.c_str());
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
    if (!(enet_host_service(g_session.host_handle, &event, connect_timeout_ms) > 0 &&
          event.type == ENET_EVENT_TYPE_CONNECT)) {
        if (debug_enabled())
            debug_log("mp client: connect failed %s:%u", target_host.c_str(),
                      static_cast<unsigned>(target_port));
        enet_host_destroy(g_session.host_handle);
        g_session.host_handle = nullptr;
        g_session.server_peer = nullptr;
        return false;
    }
    g_session.server_peer = event.peer;
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
    return client_connect_and_wait_enet(host_ip, start.host_port, false, 3000, kJoinTimeoutMs,
                                        start.session_id);
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
    g_join.host_ip = host_ip;
    g_join.cfg = multiplayer::LoadMultiplayerConfig();
    join_build_strategy_list();
    g_join.strategy_index = 0;

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
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            g_session.server_peer = event.peer;
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
            debug_log("mp client: connect failed %s:%u", g_join.target_host.c_str(),
                      static_cast<unsigned>(g_join.target_port));
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
