#include "multiplayer.hh"

#include "client.hh"
#include "display.hh"
#include "enigma.hh"
#include "errors.hh"
#include "input.hh"
#include "main.hh"
#include "player.hh"
#include "server.hh"
#include "world.hh"
#include "lev/Proxy.hh"

#include "enet/enet.h"
#include "SDL.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <limits>
#include <random>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace enigma {
namespace multiplayer {

namespace {

constexpr Uint16 kLobbyPort = 12346;
constexpr Uint16 kGamePort = 12345;
constexpr Uint16 kInternetLobbyPort = 12347;
constexpr double kAnnounceInterval = 0.5;
constexpr double kPeerTimeout = 2.0;
constexpr uint32_t kInputDelay = 4;
constexpr uint32_t kMaxInputLead = 32;
constexpr double kInputTimestep = 0.01;
constexpr double kSyncInterval = 0.5;
constexpr double kResyncCooldown = 1.0;
constexpr unsigned kResyncMaxAttempts = 3;
constexpr size_t kChecksumHistory = 512;
constexpr float kSyncPosEpsilon = 0.05f;
constexpr double kPlacementPromptSeconds = 3600.0;

constexpr Uint32 kInternetMagic = 0x52494E45;  // "ENIR"
constexpr Uint8 kInternetVersion = 1;
constexpr Uint32 kRelayMagic = 0x4C524E45;  // "ENRL"
constexpr Uint8 kRelayVersion = 1;

enum InternetMessageType : Uint8 {
    INET_CREATE = 1,
    INET_JOIN = 2,
    INET_LEAVE = 3,
    INET_START = 4,
    INET_POLL = 5,
    INET_CREATE_OK = 101,
    INET_JOIN_OK = 102,
    INET_POLL_OK = 103,
    INET_START_OK = 104,
    INET_ERROR = 105
};

enum RelayMessageType : Uint8 {
    RELAY_HELLO_HOST = 1,
    RELAY_HELLO_CLIENT = 2,
    RELAY_CLIENT_CONNECT = 3,
    RELAY_CLIENT_DISCONNECT = 4,
    RELAY_SEND = 5,
    RELAY_DATA = 6,
    RELAY_ERROR = 7
};

bool debug_enabled() {
    const char *env = std::getenv("ENIGMA_MP_DEBUG");
    return env && *env;
}

bool force_relay_enabled() {
    const char *env = std::getenv("ENIGMA_MP_FORCE_RELAY");
    return env && *env;
}

void debug_log(const char *fmt, ...) {
    if (!debug_enabled())
        return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

struct LobbyPeerEntry {
    LobbyPeer peer;
    double last_seen;
};

struct LobbyState {
    bool active = false;
    ENetSocket socket = ENET_SOCKET_NULL;
    double time = 0.0;
    double announce_timer = 0.0;
    std::string local_id;
    std::string local_name;
    std::string selected_level;
    std::unordered_map<std::string, LobbyPeerEntry> peers;
    bool has_pending_start = false;
    protocol::LobbyStart pending_start;
    std::string pending_host_ip;
    Uint32 last_session_id = 0;
};

struct SessionState {
    bool active = false;
    bool host = false;
    bool local_player_known = false;
    unsigned local_player = 0;
    unsigned expected_players = 1;
    Uint32 session_id = 0;
    Uint32 seed = 0;
    Uint32 input_epoch = 0;
    Uint32 restart_id = 0;
    Uint32 last_restart_id = 0;
    ENetHost *host_handle = nullptr;
    ENetPeer *server_peer = nullptr;
    ENetHost *relay_handle = nullptr;
    ENetPeer *relay_peer = nullptr;
    std::unordered_map<ENetPeer *, unsigned> peer_players;
    std::unordered_map<ENetPeer *, bool> peer_ready;
    std::unordered_map<Uint32, unsigned> relay_players;
    std::unordered_map<Uint32, bool> relay_ready;
    unsigned next_player_id = 1;
    uint32_t next_local_tick = 0;
    uint32_t next_send_tick = 0;
    std::unordered_map<uint32_t, input::PlayerInput> local_history;
    uint32_t input_clock_tick = 0;
    double input_clock_accu = 0.0;
    double sync_timer = 0.0;
    bool desync_reported = false;
    bool start_requested = false;
    bool start_allowed = false;
    bool ready_sent = false;
    double ready_timer = 0.0;
    bool has_pending_sync = false;
    protocol::SyncPacket pending_sync;
    struct ChecksumSample {
        uint32_t tick = 0;
        uint64_t world_checksum = 0;
        uint64_t actor_checksum = 0;
        uint32_t random_state = 0;
        float p0_x = 0.0f;
        float p0_y = 0.0f;
        float p1_x = 0.0f;
        float p1_y = 0.0f;
        bool world_valid = false;
    };
    std::deque<ChecksumSample> checksum_history;
    uint32_t last_checksum_tick = UINT32_MAX;
    uint64_t last_world_checksum = 0;
    bool resync_inflight = false;
    double resync_cooldown = 0.0;
    unsigned resync_attempts = 0;
    unsigned level_players = 0;
    std::vector<bool> placement_received;
};

LobbyState g_lobby;
SessionState g_session;
std::string g_relay_server;

void record_checksum_sample();
void send_ready_to_host();
bool local_can_send_ready();
bool has_remote_peers();
void send_relay_payload(Uint32 client_id, const ecl::Buffer &payload);
void broadcast_relay_payload(const ecl::Buffer &payload, Uint32 exclude_client_id);

unsigned compute_level_players() {
    unsigned players = 1;
    lev::Proxy *proxy = server::LoadedProxy;
    if (!proxy)
        return players;
    try {
        proxy->loadMetadata(true);
    } catch (XLevelLoading &) {
        return players;
    }
    if (proxy->hasNetworkMode())
        return proxy->getNetworkPlayers();
    if (proxy->hasSingleMode())
        return 1;
    return players;
}

bool placement_required_for_player(unsigned player) {
    if (g_session.expected_players <= g_session.level_players)
        return false;
    if (g_session.level_players == 0)
        return false;
    return player >= g_session.level_players;
}

bool screen_tiles(int &out_x, int &out_y) {
    const ecl::Rect &area = display::GetGameArea();
    int tile = video_engine->GetTileset()->tilesize;
    if (tile <= 0)
        return false;
    out_x = std::max(1, area.w / tile);
    out_y = std::max(1, area.h / tile);
    return true;
}

bool screen_bounds_for_base(const GridPos &base_pos, int &origin_x, int &origin_y,
                            int &tiles_x, int &tiles_y) {
    if (!screen_tiles(tiles_x, tiles_y))
        return false;
    origin_x = (base_pos.x / tiles_x) * tiles_x;
    origin_y = (base_pos.y / tiles_y) * tiles_y;
    return true;
}

bool is_passable_for_sight(const GridPos &pos) {
    if (!IsInsideLevel(pos))
        return false;
    Floor *floor = GetFloor(pos);
    if (!floor)
        return false;
    const std::string kind = floor->getKind();
    if (kind == "fl_abyss" || kind == "fl_water" || kind == "fl_space" || kind == "fl_space_force")
        return false;
    if (GetStone(pos) != nullptr)
        return false;
    if (GetItem(pos) != nullptr)
        return false;
    return true;
}

bool is_free_floor(const GridPos &pos);

bool is_safe_floor(const GridPos &pos) {
    if (!is_free_floor(pos))
        return false;
    Floor *floor = GetFloor(pos);
    if (!floor)
        return false;
    const std::string kind = floor->getKind();
    if (kind == "fl_swamp" || kind == "fl_thief")
        return false;
    return true;
}

bool has_clear_sight(const GridPos &from, const GridPos &to) {
    int x0 = from.x;
    int y0 = from.y;
    int x1 = to.x;
    int y1 = to.y;
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    int x = x0;
    int y = y0;
    while (true) {
        if (!(x == x0 && y == y0) && !(x == x1 && y == y1)) {
            if (!is_passable_for_sight(GridPos(x, y)))
                return false;
        }
        if (x == x1 && y == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
    return true;
}

bool is_free_floor(const GridPos &pos) {
    if (!IsInsideLevel(pos))
        return false;
    Floor *floor = GetFloor(pos);
    if (!floor)
        return false;
    const std::string kind = floor->getKind();
    if (kind == "fl_abyss" || kind == "fl_water" || kind == "fl_space" || kind == "fl_space_force")
        return false;
    if (GetStone(pos) != nullptr)
        return false;
    if (GetItem(pos) != nullptr)
        return false;
    std::vector<Actor *> actors;
    GetActorsInsideField(pos, actors);
    if (!actors.empty())
        return false;
    return true;
}

bool is_valid_placement(const GridPos &pos, unsigned player) {
    if (!is_free_floor(pos))
        return false;
    if (!placement_required_for_player(player))
        return true;
    unsigned base_player = player % g_session.level_players;
    Actor *base_actor = player::GetMainActor(base_player);
    if (!base_actor)
        return true;
    int tiles_x = 0;
    int tiles_y = 0;
    int origin_x = 0;
    int origin_y = 0;
    GridPos base_pos(base_actor->get_startpos());
    if (!screen_bounds_for_base(base_pos, origin_x, origin_y, tiles_x, tiles_y))
        return true;
    if (pos.x < origin_x || pos.x >= origin_x + tiles_x)
        return false;
    if (pos.y < origin_y || pos.y >= origin_y + tiles_y)
        return false;
    return true;
}

void apply_placement(unsigned player, const GridPos &pos);
void broadcast_placement(const protocol::PlacementPacket &msg);

bool find_clear_floor_near_base(const GridPos &base_pos, GridPos &out_pos) {
    int tiles_x = 0;
    int tiles_y = 0;
    int origin_x = 0;
    int origin_y = 0;
    if (!screen_bounds_for_base(base_pos, origin_x, origin_y, tiles_x, tiles_y))
        return false;
    int best_dist = std::numeric_limits<int>::max();
    bool found = false;
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = origin_y; y < origin_y + tiles_y; ++y) {
            for (int x = origin_x; x < origin_x + tiles_x; ++x) {
                GridPos pos(x, y);
                if (pass == 0) {
                    if (!is_safe_floor(pos))
                        continue;
                } else {
                    if (!is_free_floor(pos))
                        continue;
                }
                if (!has_clear_sight(base_pos, pos))
                    continue;
                int dx = x - base_pos.x;
                int dy = y - base_pos.y;
                int dist = dx * dx + dy * dy;
                if (dist < best_dist) {
                    best_dist = dist;
                    out_pos = pos;
                    found = true;
                }
            }
        }
        if (found)
            return true;
    }
    return false;
}

bool find_random_free_tile(const GridPos &base_pos, GridPos &out_pos) {
    int tiles_x = 0;
    int tiles_y = 0;
    int origin_x = 0;
    int origin_y = 0;
    std::vector<GridPos> free_tiles;
    if (screen_bounds_for_base(base_pos, origin_x, origin_y, tiles_x, tiles_y)) {
        for (int y = origin_y; y < origin_y + tiles_y; ++y) {
            for (int x = origin_x; x < origin_x + tiles_x; ++x) {
                GridPos pos(x, y);
                if (is_free_floor(pos))
                    free_tiles.push_back(pos);
            }
        }
        if (!free_tiles.empty()) {
            int idx = enigma::IntegerRand(0, static_cast<int>(free_tiles.size() - 1), true);
            out_pos = free_tiles[static_cast<size_t>(idx)];
            return true;
        }
    }
    free_tiles.clear();
    int width = Width();
    int height = Height();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            GridPos pos(x, y);
            if (is_free_floor(pos))
                free_tiles.push_back(pos);
        }
    }
    if (free_tiles.empty())
        return false;
    int idx = enigma::IntegerRand(0, static_cast<int>(free_tiles.size() - 1), true);
    out_pos = free_tiles[static_cast<size_t>(idx)];
    return true;
}

void auto_place_extra_players() {
    if (!g_session.host)
        return;
    if (g_session.level_players == 0)
        return;
    for (unsigned player = g_session.level_players; player < g_session.expected_players; ++player) {
        if (!placement_required_for_player(player))
            continue;
        unsigned base_index = player % g_session.level_players;
        Actor *base_actor = player::GetMainActor(base_index);
        if (!base_actor)
            continue;
        GridPos base_pos(base_actor->get_startpos());
        GridPos pos;
        bool found = find_clear_floor_near_base(base_pos, pos);
        if (!found)
            found = find_random_free_tile(base_pos, pos);
        if (!found)
            continue;
        protocol::PlacementPacket msg;
        msg.restart_id = g_session.restart_id;
        msg.player = static_cast<Uint8>(player);
        msg.x = static_cast<Uint16>(pos.x);
        msg.y = static_cast<Uint16>(pos.y);
        apply_placement(player, pos);
        if (g_session.placement_received.size() > player)
            g_session.placement_received[player] = true;
        broadcast_placement(msg);
    }
}

void apply_placement(unsigned player, const GridPos &pos) {
    Actor *actor = player::GetMainActor(player);
    if (!actor)
        return;
    ecl::V2 center = pos.center();
    WarpActor(actor, center[0], center[1], false);
    actor->set_respawnpos(center);
}

void broadcast_placement(const protocol::PlacementPacket &msg) {
    if (!g_session.host || !has_remote_peers())
        return;
    ecl::Buffer buf;
    protocol::encode_place(buf, msg);
    if (!g_session.peer_players.empty()) {
        ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
        enet_host_broadcast(g_session.host_handle, 0, packet);
    }
    if (!g_session.relay_players.empty())
        broadcast_relay_payload(buf, 0);
}

void send_placement_to_peer(ENetPeer *peer, const protocol::PlacementPacket &msg) {
    if (!peer)
        return;
    ecl::Buffer buf;
    protocol::encode_place(buf, msg);
    ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, packet);
}

void send_existing_placements_to_peer(ENetPeer *peer) {
    if (!g_session.host || !peer)
        return;
    if (g_session.expected_players <= g_session.level_players || g_session.level_players == 0)
        return;
    if (g_session.placement_received.size() < g_session.expected_players)
        return;
    for (unsigned player = g_session.level_players; player < g_session.expected_players; ++player) {
        if (!g_session.placement_received[player])
            continue;
        Actor *actor = player::GetMainActor(player);
        if (!actor)
            continue;
        GridPos pos = actor->get_gridpos();
        protocol::PlacementPacket msg;
        msg.restart_id = g_session.restart_id;
        msg.player = static_cast<Uint8>(player);
        msg.x = static_cast<Uint16>(pos.x);
        msg.y = static_cast<Uint16>(pos.y);
        send_placement_to_peer(peer, msg);
    }
}

void send_existing_placements_to_relay(Uint32 client_id) {
    if (!g_session.host || !g_session.relay_peer)
        return;
    if (g_session.expected_players <= g_session.level_players || g_session.level_players == 0)
        return;
    if (g_session.placement_received.size() < g_session.expected_players)
        return;
    for (unsigned player = g_session.level_players; player < g_session.expected_players; ++player) {
        if (!g_session.placement_received[player])
            continue;
        Actor *actor = player::GetMainActor(player);
        if (!actor)
            continue;
        GridPos pos = actor->get_gridpos();
        protocol::PlacementPacket msg;
        msg.restart_id = g_session.restart_id;
        msg.player = static_cast<Uint8>(player);
        msg.x = static_cast<Uint16>(pos.x);
        msg.y = static_cast<Uint16>(pos.y);
        ecl::Buffer buf;
        protocol::encode_place(buf, msg);
        send_relay_payload(client_id, buf);
    }
}

void send_placement_to_host(const GridPos &pos) {
    if (!g_session.active)
        return;
    protocol::PlacementPacket msg;
    msg.restart_id = g_session.restart_id;
    msg.player = static_cast<Uint8>(g_session.local_player);
    msg.x = static_cast<Uint16>(pos.x);
    msg.y = static_cast<Uint16>(pos.y);
    if (g_session.host) {
        if (placement_required_for_player(msg.player) && is_valid_placement(pos, msg.player)) {
            apply_placement(msg.player, pos);
            if (g_session.placement_received.size() > msg.player)
                g_session.placement_received[msg.player] = true;
            broadcast_placement(msg);
        }
        return;
    }
    if (!g_session.server_peer)
        return;
    ecl::Buffer buf;
    protocol::encode_place(buf, msg);
    ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(g_session.server_peer, 0, packet);
    enet_host_flush(g_session.host_handle);
}

void add_extra_actors(unsigned level_players, unsigned expected_players) {
    if (level_players == 0 || expected_players <= level_players)
        return;
    std::vector<Actor *> actors;
    GetActors(actors);

    std::vector<Actor *> base(level_players, nullptr);
    std::vector<Actor *> fallback(level_players, nullptr);
    Actor *any_actor = nullptr;

    for (auto *actor : actors) {
        if (!any_actor && actor->isSteerable())
            any_actor = actor;
        Value owner_val = actor->getAttr("owner");
        if (owner_val.getType() == Value::NIL)
            continue;
        int owner = owner_val;
        if (owner < 0 || owner >= static_cast<int>(level_players))
            continue;
        if (!fallback[owner])
            fallback[owner] = actor;
        if (!base[owner]) {
            int controllers = actor->get_controllers();
            if ((controllers & (1 << owner)) != 0 && actor->isSteerable())
                base[owner] = actor;
        }
    }

    for (unsigned i = 0; i < level_players; ++i) {
        if (!base[i])
            base[i] = fallback[i];
    }

    if (!any_actor) {
        for (auto *candidate : base) {
            if (candidate) {
                any_actor = candidate;
                break;
            }
        }
    }

    for (unsigned player = level_players; player < expected_players; ++player) {
        unsigned base_index = (level_players > 0) ? (player % level_players) : 0;
        Actor *base_actor = base_index < base.size() ? base[base_index] : nullptr;
        if (!base_actor)
            base_actor = any_actor;
        if (!base_actor)
            continue;
        Actor *extra = MakeActor(base_actor->getKind().c_str());
        if (!extra)
            continue;
        extra->setAttr("owner", Value(static_cast<int>(player)));
        extra->setAttr("controllers", Value(static_cast<int>(1 << player)));
        extra->setAttr("essential", Value(0));
        Value color = base_actor->getAttr("color");
        if (color.getType() != Value::NIL)
            extra->setAttr("color", color);
        ecl::V2 pos = base_actor->get_pos();
        AddActor(pos[0], pos[1], extra);
    }
}

bool local_can_send_ready() {
    if (!g_session.active || g_session.host)
        return false;
    if (!g_session.local_player_known)
        return false;
    return true;
}

bool bind_lobby_socket(ENetSocket socket, Uint16 port) {
    int opt = 1;
#ifdef WIN32
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(socket, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char *>(&opt), sizeof(opt));
#endif
#else
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(socket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
#endif

    sockaddr_in sin;
    std::memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = ENET_HOST_TO_NET_16(port);
    sin.sin_addr.s_addr = ENET_HOST_ANY;
    return bind(socket, reinterpret_cast<sockaddr *>(&sin), sizeof(sin)) == 0;
}

std::string make_id() {
    std::random_device rd;
    uint64_t value = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    std::ostringstream out;
    out << std::hex << value;
    return out.str();
}

bool parse_host_port(const std::string &value, std::string &host, Uint16 &port) {
    host.clear();
    port = kInternetLobbyPort;
    if (value.empty())
        return false;
    std::string::size_type pos = value.rfind(':');
    if (pos == std::string::npos) {
        host = value;
        return true;
    }
    host = value.substr(0, pos);
    std::string port_str = value.substr(pos + 1);
    if (!port_str.empty()) {
        char *end = nullptr;
        long parsed = std::strtol(port_str.c_str(), &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 65535)
            port = static_cast<Uint16>(parsed);
    }
    if (host == "localhost" || host == "::1")
        host = "127.0.0.1";
    return !host.empty();
}

bool internet_exchange(const std::string &server, const ecl::Buffer &request,
                       ecl::Buffer &response, std::string &error) {
    std::string host;
    Uint16 port = 0;
    if (!parse_host_port(server, host, port)) {
        error = "Invalid server address.";
        return false;
    }

    ENetAddress addr;
    if (enet_address_set_host(&addr, host.c_str()) != 0) {
        error = "Failed to resolve server.";
        return false;
    }
    addr.port = port;

    ENetSocket socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM, nullptr);
    if (socket == ENET_SOCKET_NULL) {
        error = "Failed to open socket.";
        return false;
    }
    ENetBuffer eb;
    eb.data = const_cast<char *>(request.data());
    eb.dataLength = request.size();
    if (enet_socket_send(socket, &addr, &eb, 1) <= 0) {
        enet_socket_destroy(socket);
        error = "Failed to send request.";
        return false;
    }

    char data[1024];
    ENetBuffer rb;
    rb.data = data;
    rb.dataLength = sizeof(data);
    Uint32 start = SDL_GetTicks();
    while (SDL_GetTicks() - start < 1500) {
        enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
        enet_socket_wait(socket, &condition, 50);
        if ((condition & ENET_SOCKET_WAIT_RECEIVE) == 0)
            continue;
        ENetAddress src;
        int received = enet_socket_receive(socket, &src, &rb, 1);
        if (received > 0) {
            response.assign(data, static_cast<ecl::Buffer::size_t>(received));
            enet_socket_destroy(socket);
            return true;
        }
    }

    enet_socket_destroy(socket);
    error = "No response from server.";
    return false;
}

void encode_relay_header(ecl::Buffer &buf, RelayMessageType type, Uint32 session_id,
                         Uint32 client_id) {
    buf << Uint32(kRelayMagic) << Uint8(kRelayVersion) << Uint8(type)
        << Uint32(session_id) << Uint32(client_id);
}

bool decode_relay_header(const char *data, size_t len, RelayMessageType &type,
                         Uint32 &session_id, Uint32 &client_id,
                         const char *&payload, size_t &payload_len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 raw_type = 0;
    if (!(buf >> magic >> version >> raw_type >> session_id >> client_id))
        return false;
    if (magic != kRelayMagic || version != kRelayVersion)
        return false;
    type = static_cast<RelayMessageType>(raw_type);
    size_t offset = static_cast<size_t>(buf.get_rpos());
    if (offset > len)
        return false;
    payload = data + offset;
    payload_len = len - offset;
    return true;
}

std::string encode_hex(const char *data, size_t len) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        out.push_back(hex[(c >> 4) & 0xF]);
        out.push_back(hex[c & 0xF]);
    }
    return out;
}

bool decode_hex(const std::string &text, ecl::Buffer &out) {
    if (text.empty() || (text.size() % 2) != 0)
        return false;
    out.clear();
    for (size_t i = 0; i < text.size(); i += 2) {
        char hi = text[i];
        char lo = text[i + 1];
        auto hexval = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'A' && c <= 'F')
                return 10 + (c - 'A');
            if (c >= 'a' && c <= 'f')
                return 10 + (c - 'a');
            return -1;
        };
        int h = hexval(hi);
        int l = hexval(lo);
        if (h < 0 || l < 0)
            return false;
        char byte = static_cast<char>((h << 4) | l);
        out.write(byte);
    }
    return true;
}

std::string resolve_local_name() {
    std::string name = app.state->getString("UserName");
    if (name.empty())
        name = "Player";
    return name;
}

void ensure_lobby_identity() {
    if (!g_lobby.local_id.empty())
        return;
    g_lobby.local_id = make_id();
    g_lobby.local_name = resolve_local_name();
}

void send_lobby_announce() {
    if (!g_lobby.active || g_lobby.socket == ENET_SOCKET_NULL)
        return;
    protocol::LobbyAnnounce msg;
    msg.id = g_lobby.local_id;
    msg.name = g_lobby.local_name;
    msg.level_id = g_lobby.selected_level;
    msg.player_count = static_cast<Uint8>(g_lobby.peers.size() + 1);

    ecl::Buffer buf;
    protocol::encode_lobby_announce(buf, msg);

    ENetAddress addr;
    addr.host = ENET_HOST_BROADCAST;
    addr.port = kLobbyPort;
    ENetBuffer eb;
    eb.data = const_cast<char *>(buf.data());
    eb.dataLength = buf.size();
    enet_socket_send(g_lobby.socket, &addr, &eb, 1);
}

void poll_lobby_socket() {
    if (!g_lobby.active || g_lobby.socket == ENET_SOCKET_NULL)
        return;
    ENetAddress src;
    char data[512];
    ENetBuffer buffer;
    buffer.data = data;
    buffer.dataLength = sizeof(data);

    while (true) {
        int received = enet_socket_receive(g_lobby.socket, &src, &buffer, 1);
        if (received <= 0)
            break;

        char ip[64] = "";
        enet_address_get_host(&src, ip, sizeof(ip));

        ecl::Buffer buf;
        buf.assign(data, static_cast<ecl::Buffer::size_t>(received));
        protocol::LobbyAnnounce announce;
        if (protocol::decode_lobby_announce(buf, announce)) {
            if (announce.id == g_lobby.local_id)
                continue;
            LobbyPeerEntry &entry = g_lobby.peers[announce.id];
            entry.peer.id = announce.id;
            entry.peer.name = announce.name;
            entry.peer.level_id = announce.level_id;
            entry.peer.address = ip;
            entry.peer.is_self = false;
            entry.last_seen = g_lobby.time;
            continue;
        }

        buf.assign(data, static_cast<ecl::Buffer::size_t>(received));
        protocol::LobbyStart start;
        if (protocol::decode_lobby_start(buf, start)) {
            if (start.host_id == g_lobby.local_id)
                continue;
            if (g_session.active)
                continue;
            if (start.session_id == g_lobby.last_session_id)
                continue;
            g_lobby.pending_start = start;
            g_lobby.pending_host_ip = ip;
            g_lobby.has_pending_start = true;
            g_lobby.last_session_id = start.session_id;
        }
    }
}

void cleanup_lobby_peers() {
    for (auto it = g_lobby.peers.begin(); it != g_lobby.peers.end();) {
        if (g_lobby.time - it->second.last_seen > kPeerTimeout)
            it = g_lobby.peers.erase(it);
        else
            ++it;
    }
}

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

bool has_remote_peers() {
    return !g_session.peer_players.empty() || !g_session.relay_players.empty();
}

void send_relay_payload(Uint32 client_id, const char *data, size_t len) {
    if (!g_session.relay_peer)
        return;
    ecl::Buffer buf;
    encode_relay_header(buf, RELAY_SEND, g_session.session_id, client_id);
    buf.write(data, len);
    ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(g_session.relay_peer, 0, packet);
    if (g_session.relay_handle)
        enet_host_flush(g_session.relay_handle);
}

void send_relay_payload(Uint32 client_id, const ecl::Buffer &payload) {
    send_relay_payload(client_id, payload.data(), payload.size());
}

void broadcast_relay_payload(const ecl::Buffer &payload, Uint32 exclude_client_id) {
    for (const auto &entry : g_session.relay_players) {
        if (entry.first == exclude_client_id)
            continue;
        send_relay_payload(entry.first, payload);
    }
}

void send_input_to_peer(ENetPeer *peer, const protocol::InputPacket &pkt) {
    ecl::Buffer buf;
    protocol::encode_input(buf, pkt);
    ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, packet);
}

void broadcast_input(const protocol::InputPacket &pkt, ENetPeer *exclude, Uint32 exclude_relay) {
    for (const auto &entry : g_session.peer_players) {
        if (entry.first == exclude)
            continue;
        send_input_to_peer(entry.first, pkt);
    }
    ecl::Buffer buf;
    protocol::encode_input(buf, pkt);
    broadcast_relay_payload(buf, exclude_relay);
}

void send_sync_to_peers() {
    if (!g_session.host || !has_remote_peers())
        return;
    protocol::SyncPacket sync;
    sync.epoch = g_session.input_epoch;
    sync.tick = input::CurrentTick();
    sync.random_state = server::RandomState;
    Actor *p0 = player::GetMainActor(0);
    Actor *p1 = player::GetMainActor(1);
    sync.p0_x = p0 ? static_cast<float>(p0->get_pos()[0]) : 0.0f;
    sync.p0_y = p0 ? static_cast<float>(p0->get_pos()[1]) : 0.0f;
    sync.p1_x = p1 ? static_cast<float>(p1->get_pos()[0]) : 0.0f;
    sync.p1_y = p1 ? static_cast<float>(p1->get_pos()[1]) : 0.0f;
    sync.world_checksum = WorldChecksum();
    sync.actor_checksum = ActorChecksum();

    ecl::Buffer buf;
    protocol::encode_sync(buf, sync);
    if (!g_session.peer_players.empty()) {
        ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
        enet_host_broadcast(g_session.host_handle, 0, packet);
    }
    if (!g_session.relay_players.empty())
        broadcast_relay_payload(buf, 0);
}

void send_resync_request() {
    if (!g_session.server_peer)
        return;
    protocol::ResyncRequest req;
    req.epoch = g_session.input_epoch;
    req.tick = input::CurrentTick();
    ecl::Buffer buf;
    protocol::encode_resync_request(buf, req);
    ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(g_session.server_peer, 0, packet);
}

void send_resync_state(ENetPeer *peer) {
    if (!peer)
        return;
    protocol::ResyncState state;
    state.epoch = g_session.input_epoch;
    state.tick = input::CurrentTick();
    state.random_state = server::RandomState;
    state.actors.clear();
    std::vector<Actor *> actors;
    GetActors(actors);
    state.actors.reserve(static_cast<size_t>(actors.size()));
    for (Actor *actor : actors) {
        protocol::ResyncActorState entry;
        entry.object_id = static_cast<Uint32>(actor->getId());
        entry.actor_id = static_cast<Uint16>(get_id(actor));
        Value owner = actor->getAttr("owner");
        if (owner.getType() != Value::NIL)
            entry.owner = static_cast<Uint16>(static_cast<int>(owner));
        else
            entry.owner = static_cast<Uint16>(0xFFFF);
        entry.x = static_cast<float>(actor->get_pos()[0]);
        entry.y = static_cast<float>(actor->get_pos()[1]);
        entry.vx = static_cast<float>(actor->get_vel()[0]);
        entry.vy = static_cast<float>(actor->get_vel()[1]);
        state.actors.push_back(entry);
    }
    ecl::Buffer buf;
    protocol::encode_resync_state(buf, state);
    ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, packet);
}

void send_resync_state_to_relay(Uint32 client_id) {
    if (!g_session.relay_peer)
        return;
    protocol::ResyncState state;
    state.epoch = g_session.input_epoch;
    state.tick = input::CurrentTick();
    state.random_state = server::RandomState;
    state.actors.clear();
    std::vector<Actor *> actors;
    GetActors(actors);
    state.actors.reserve(static_cast<size_t>(actors.size()));
    for (Actor *actor : actors) {
        protocol::ResyncActorState entry;
        entry.object_id = static_cast<Uint32>(actor->getId());
        entry.actor_id = static_cast<Uint16>(get_id(actor));
        Value owner = actor->getAttr("owner");
        if (owner.getType() != Value::NIL)
            entry.owner = static_cast<Uint16>(static_cast<int>(owner));
        else
            entry.owner = static_cast<Uint16>(0xFFFF);
        entry.x = static_cast<float>(actor->get_pos()[0]);
        entry.y = static_cast<float>(actor->get_pos()[1]);
        entry.vx = static_cast<float>(actor->get_vel()[0]);
        entry.vy = static_cast<float>(actor->get_vel()[1]);
        state.actors.push_back(entry);
    }
    ecl::Buffer buf;
    protocol::encode_resync_state(buf, state);
    send_relay_payload(client_id, buf);
}

void apply_resync_state(const protocol::ResyncState &state) {
    if (!g_session.active)
        return;
    if (state.epoch != g_session.input_epoch)
        return;
    server::RandomState = state.random_state;

    std::unordered_map<Uint32, Actor *> by_id;
    std::vector<Actor *> actors;
    GetActors(actors);
    by_id.reserve(actors.size());
    for (Actor *actor : actors)
        by_id[static_cast<Uint32>(actor->getId())] = actor;

    for (const auto &entry : state.actors) {
        Actor *actor = nullptr;
        auto it = by_id.find(entry.object_id);
        if (it != by_id.end()) {
            actor = it->second;
        } else {
            int desired_owner = (entry.owner == 0xFFFF)
                                    ? -1
                                    : static_cast<int>(entry.owner);
            for (Actor *candidate : actors) {
                if (get_id(candidate) == entry.actor_id) {
                    Value owner = candidate->getAttr("owner");
                    int candidate_owner = (owner.getType() != Value::NIL)
                                              ? static_cast<int>(owner)
                                              : -1;
                    if (candidate_owner == desired_owner) {
                        actor = candidate;
                        break;
                    }
                }
            }
            if (!actor) {
                for (Actor *candidate : actors) {
                    if (get_id(candidate) == entry.actor_id) {
                        actor = candidate;
                        break;
                    }
                }
            }
        }
        if (!actor)
            continue;
        WarpActor(actor, entry.x, entry.y, true);
        actor->get_actorinfo()->vel = ecl::V2(entry.vx, entry.vy);
    }

    g_session.resync_inflight = false;
    g_session.resync_cooldown = 0.0;
    g_session.resync_attempts = 0;
    g_session.desync_reported = false;
    record_checksum_sample();
}

void send_ready_to_host() {
    if (!g_session.server_peer)
        return;
    if (debug_enabled())
        debug_log("mp send ready");
    ecl::Buffer buf;
    protocol::encode_ready(buf);
    ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(g_session.server_peer, 0, packet);
}

void send_start_to_peers() {
    if (!g_session.host || !has_remote_peers())
        return;
    ecl::Buffer buf;
    protocol::encode_start(buf, g_session.input_epoch);
    if (!g_session.peer_players.empty()) {
        ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
        enet_host_broadcast(g_session.host_handle, 0, packet);
    }
    if (!g_session.relay_players.empty())
        broadcast_relay_payload(buf, 0);
}

void send_restart_to_peers(bool level_restart) {
    if (!g_session.host || !has_remote_peers())
        return;
    protocol::RestartPacket msg;
    msg.restart_id = g_session.restart_id;
    msg.level_restart = level_restart ? 1 : 0;
    ecl::Buffer buf;
    protocol::encode_restart(buf, msg);
    if (!g_session.peer_players.empty()) {
        ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
        enet_host_broadcast(g_session.host_handle, 0, packet);
    }
    if (!g_session.relay_players.empty())
        broadcast_relay_payload(buf, 0);
}

bool host_ready_to_start() {
    if (!g_session.host)
        return false;
    if (g_session.expected_players <= 1)
        return true;
    if (g_session.peer_players.size() + g_session.relay_players.size() + 1 < g_session.expected_players)
        return false;
    for (const auto &entry : g_session.peer_players) {
        auto it = g_session.peer_ready.find(entry.first);
        if (it == g_session.peer_ready.end() || !it->second)
            return false;
    }
    for (const auto &entry : g_session.relay_players) {
        auto it = g_session.relay_ready.find(entry.first);
        if (it == g_session.relay_ready.end() || !it->second)
            return false;
    }
    if (g_session.expected_players > g_session.level_players && g_session.level_players > 0) {
        if (g_session.placement_received.size() < g_session.expected_players)
            return false;
        for (unsigned player = g_session.level_players; player < g_session.expected_players; ++player) {
            if (!g_session.placement_received[player])
                return false;
        }
    }
    return true;
}

bool ready_to_start() {
    if (!g_session.active)
        return true;
    return g_session.start_allowed;
}

bool lookup_checksum_sample(uint32_t tick, SessionState::ChecksumSample &out) {
    for (const auto &entry : g_session.checksum_history) {
        if (entry.tick == tick) {
            out = entry;
            return true;
        }
    }
    return false;
}

void update_checksum_sample_world(uint32_t tick, uint64_t world_checksum) {
    for (auto &entry : g_session.checksum_history) {
        if (entry.tick == tick) {
            entry.world_checksum = world_checksum;
            entry.world_valid = true;
            break;
        }
    }
}

void handle_sync_current(const protocol::SyncPacket &sync) {
    if (sync.epoch != g_session.input_epoch) {
        debug_log("mp sync skip: epoch=%u local epoch=%u",
                  sync.epoch, g_session.input_epoch);
        return;
    }
    uint32_t local_tick = input::CurrentTick();
    if (sync.tick != local_tick) {
        debug_log("mp sync skip: sync tick=%u local tick=%u", sync.tick, local_tick);
        return;
    }
    uint64_t local_checksum = WorldChecksum();
    update_checksum_sample_world(local_tick, local_checksum);
    g_session.last_world_checksum = local_checksum;
    bool checksum_mismatch = (sync.world_checksum != 0 && sync.world_checksum != local_checksum);
    if (checksum_mismatch) {
        debug_log("mp checksum mismatch: tick=%u local=%llu remote=%llu",
                  sync.tick,
                  static_cast<unsigned long long>(local_checksum),
                  static_cast<unsigned long long>(sync.world_checksum));
    }
    uint64_t local_actor_checksum = ActorChecksum();
    bool actor_mismatch = (sync.actor_checksum != 0 && sync.actor_checksum != local_actor_checksum);
    if (actor_mismatch) {
        debug_log("mp actor mismatch: tick=%u local=%llu remote=%llu",
                  sync.tick,
                  static_cast<unsigned long long>(local_actor_checksum),
                  static_cast<unsigned long long>(sync.actor_checksum));
    }
    Actor *p0 = player::GetMainActor(0);
    Actor *p1 = player::GetMainActor(1);
    float p0_x = p0 ? static_cast<float>(p0->get_pos()[0]) : 0.0f;
    float p0_y = p0 ? static_cast<float>(p0->get_pos()[1]) : 0.0f;
    float p1_x = p1 ? static_cast<float>(p1->get_pos()[0]) : 0.0f;
    float p1_y = p1 ? static_cast<float>(p1->get_pos()[1]) : 0.0f;

    auto diff = [](float a, float b) { return fabs(a - b); };
    bool rand_mismatch = sync.random_state != server::RandomState;
    bool pos_mismatch = diff(sync.p0_x, p0_x) > kSyncPosEpsilon ||
                        diff(sync.p0_y, p0_y) > kSyncPosEpsilon ||
                        diff(sync.p1_x, p1_x) > kSyncPosEpsilon ||
                        diff(sync.p1_y, p1_y) > kSyncPosEpsilon;
    if (pos_mismatch || rand_mismatch || actor_mismatch || checksum_mismatch) {
        debug_log("mp desync: tick=%u rand local=%u remote=%u "
                  "p0(%.2f,%.2f)->(%.2f,%.2f) p1(%.2f,%.2f)->(%.2f,%.2f)",
                  sync.tick, static_cast<unsigned>(server::RandomState),
                  static_cast<unsigned>(sync.random_state),
                  p0_x, p0_y, sync.p0_x, sync.p0_y,
                  p1_x, p1_y, sync.p1_x, sync.p1_y);
        if (!pos_mismatch && rand_mismatch && !checksum_mismatch && !actor_mismatch) {
            server::RandomState = sync.random_state;
            debug_log("mp rng resynced to %u", static_cast<unsigned>(sync.random_state));
            return;
        }
    }

    if (checksum_mismatch || actor_mismatch || pos_mismatch) {
        if (!g_session.resync_inflight && g_session.resync_cooldown <= 0.0) {
            send_resync_request();
            g_session.resync_inflight = true;
            g_session.resync_attempts += 1;
            g_session.resync_cooldown = kResyncCooldown;
        }
        if (g_session.resync_attempts >= kResyncMaxAttempts && !g_session.desync_reported) {
            g_session.desync_reported = true;
            client::Msg_ShowText("World desync detected. Please restart.", true, 4.0);
        }
    } else {
        g_session.resync_attempts = 0;
        g_session.resync_inflight = false;
    }
}

void handle_sync_sample(const protocol::SyncPacket &sync, const SessionState::ChecksumSample &sample) {
    if (sync.epoch != g_session.input_epoch) {
        debug_log("mp sync skip: epoch=%u local epoch=%u",
                  sync.epoch, g_session.input_epoch);
        return;
    }
    bool checksum_mismatch = false;
    if (sample.world_valid && sync.world_checksum != 0)
        checksum_mismatch = sync.world_checksum != sample.world_checksum;
    bool actor_mismatch = (sync.actor_checksum != 0 && sync.actor_checksum != sample.actor_checksum);
    bool rand_mismatch = sync.random_state != sample.random_state;

    auto diff = [](float a, float b) { return fabs(a - b); };
    bool pos_mismatch = diff(sync.p0_x, sample.p0_x) > kSyncPosEpsilon ||
                        diff(sync.p0_y, sample.p0_y) > kSyncPosEpsilon ||
                        diff(sync.p1_x, sample.p1_x) > kSyncPosEpsilon ||
                        diff(sync.p1_y, sample.p1_y) > kSyncPosEpsilon;

    if (checksum_mismatch) {
        debug_log("mp checksum mismatch: tick=%u local=%llu remote=%llu",
                  sync.tick,
                  static_cast<unsigned long long>(sample.world_checksum),
                  static_cast<unsigned long long>(sync.world_checksum));
    }
    if (actor_mismatch) {
        debug_log("mp actor mismatch: tick=%u local=%llu remote=%llu",
                  sync.tick,
                  static_cast<unsigned long long>(sample.actor_checksum),
                  static_cast<unsigned long long>(sync.actor_checksum));
    }
    if (pos_mismatch || rand_mismatch || actor_mismatch || checksum_mismatch) {
        debug_log("mp desync (late): tick=%u rand local=%u remote=%u "
                  "p0(%.2f,%.2f)->(%.2f,%.2f) p1(%.2f,%.2f)->(%.2f,%.2f)",
                  sync.tick,
                  static_cast<unsigned>(sample.random_state),
                  static_cast<unsigned>(sync.random_state),
                  sample.p0_x, sample.p0_y, sync.p0_x, sync.p0_y,
                  sample.p1_x, sample.p1_y, sync.p1_x, sync.p1_y);
    }

    if (checksum_mismatch || actor_mismatch || pos_mismatch || rand_mismatch) {
        if (!g_session.resync_inflight && g_session.resync_cooldown <= 0.0) {
            send_resync_request();
            g_session.resync_inflight = true;
            g_session.resync_attempts += 1;
            g_session.resync_cooldown = kResyncCooldown;
        }
        if (g_session.resync_attempts >= kResyncMaxAttempts && !g_session.desync_reported) {
            g_session.desync_reported = true;
            client::Msg_ShowText("World desync detected. Please restart.", true, 4.0);
        }
    } else {
        g_session.resync_attempts = 0;
        g_session.resync_inflight = false;
    }
}

void record_checksum_sample() {
    uint32_t tick = input::CurrentTick();
    g_session.last_checksum_tick = tick;
    SessionState::ChecksumSample sample;
    sample.tick = tick;
    sample.actor_checksum = ActorChecksum();
    sample.world_checksum = 0;
    sample.world_valid = false;
    sample.random_state = server::RandomState;
    Actor *p0 = player::GetMainActor(0);
    Actor *p1 = player::GetMainActor(1);
    sample.p0_x = p0 ? static_cast<float>(p0->get_pos()[0]) : 0.0f;
    sample.p0_y = p0 ? static_cast<float>(p0->get_pos()[1]) : 0.0f;
    sample.p1_x = p1 ? static_cast<float>(p1->get_pos()[0]) : 0.0f;
    sample.p1_y = p1 ? static_cast<float>(p1->get_pos()[1]) : 0.0f;
    if (!g_session.checksum_history.empty() && g_session.checksum_history.back().tick == tick) {
        bool preserve_world = g_session.checksum_history.back().world_valid;
        uint64_t preserved_checksum = g_session.checksum_history.back().world_checksum;
        g_session.checksum_history.back() = sample;
        if (preserve_world) {
            g_session.checksum_history.back().world_valid = true;
            g_session.checksum_history.back().world_checksum = preserved_checksum;
        }
        return;
    }
    g_session.checksum_history.push_back(sample);
    while (g_session.checksum_history.size() > kChecksumHistory)
        g_session.checksum_history.pop_front();
}

bool handle_host_packet(const char *data, size_t len, ENetPeer *peer,
                        bool via_relay, Uint32 relay_client_id) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::InputPacket input_msg;
    if (protocol::decode_input(buf, input_msg)) {
        if (input_msg.epoch != g_session.input_epoch) {
            debug_log("mp drop input: tick=%u epoch=%u local epoch=%u",
                      input_msg.tick, input_msg.epoch, g_session.input_epoch);
            return true;
        }
        if (input_msg.tick < 20) {
            debug_log("mp recv input: tick=%u player=%u mouse=(%.2f,%.2f) rot=%d act=%u",
                      input_msg.tick, input_msg.player, input_msg.mouse_x, input_msg.mouse_y,
                      static_cast<int>(input_msg.rotate_steps),
                      static_cast<unsigned>(input_msg.activate_count));
        }
        if (input_msg.tick < input::CurrentTick())
            return true;
        unsigned player_id = 0;
        bool found = false;
        if (via_relay) {
            auto it = g_session.relay_players.find(relay_client_id);
            if (it != g_session.relay_players.end()) {
                player_id = it->second;
                found = true;
            }
        } else {
            auto it = g_session.peer_players.find(peer);
            if (it != g_session.peer_players.end()) {
                player_id = it->second;
                found = true;
            }
        }
        if (!found)
            return true;
        input::PlayerInput pi;
        pi.mouse_force = ecl::V2(input_msg.mouse_x, input_msg.mouse_y);
        pi.rotate_steps = input_msg.rotate_steps;
        pi.activate_count = input_msg.activate_count;
        input::EnqueueInput(input_msg.tick, player_id, pi);
        protocol::InputPacket forward = input_msg;
        forward.player = static_cast<Uint8>(player_id);
        broadcast_input(forward, via_relay ? nullptr : peer, via_relay ? relay_client_id : 0);
        return true;
    }

    buf.assign(const_cast<char *>(data), len);
    protocol::ResyncRequest req;
    if (protocol::decode_resync_request(buf, req)) {
        if (req.epoch == g_session.input_epoch) {
            if (via_relay)
                send_resync_state_to_relay(relay_client_id);
            else
                send_resync_state(peer);
        }
        return true;
    }

    buf.assign(const_cast<char *>(data), len);
    if (protocol::decode_ready(buf)) {
        unsigned player_id = 0;
        bool found = false;
        if (via_relay) {
            auto it = g_session.relay_players.find(relay_client_id);
            if (it != g_session.relay_players.end()) {
                player_id = it->second;
                found = true;
            }
        } else {
            auto it = g_session.peer_players.find(peer);
            if (it != g_session.peer_players.end()) {
                player_id = it->second;
                found = true;
            }
        }
        if (found) {
            if (debug_enabled())
                debug_log("mp host: ready player=%u via_relay=%d", player_id, via_relay ? 1 : 0);
            if (via_relay)
                g_session.relay_ready[relay_client_id] = true;
            else
                g_session.peer_ready[peer] = true;
            debug_log("mp host: peer ready");
        }
        return true;
    }

    buf.assign(const_cast<char *>(data), len);
    protocol::PlacementPacket place;
    if (protocol::decode_place(buf, place)) {
        if (place.restart_id == g_session.restart_id &&
            place.player < g_session.expected_players) {
            GridPos pos(static_cast<int>(place.x), static_cast<int>(place.y));
            if (placement_required_for_player(place.player) &&
                is_valid_placement(pos, place.player)) {
                apply_placement(place.player, pos);
                if (g_session.placement_received.size() > place.player)
                    g_session.placement_received[place.player] = true;
                broadcast_placement(place);
            }
        }
        return true;
    }

    return false;
}

void process_network_events() {
    if (!g_session.active || g_session.host_handle == nullptr)
        return;
    ENetEvent event;
    while (enet_host_service(g_session.host_handle, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            if (g_session.host) {
                if (g_session.next_player_id >= g_session.expected_players) {
                    enet_peer_disconnect(event.peer, 0);
                    break;
                }
                unsigned player_id = g_session.next_player_id++;
                g_session.peer_players[event.peer] = player_id;
                g_session.peer_ready[event.peer] = false;
                event.peer->data = reinterpret_cast<void *>(static_cast<uintptr_t>(player_id));
                debug_log("mp host: peer connected -> player %u", player_id);
                ecl::Buffer buf;
                protocol::encode_welcome(buf, static_cast<Uint8>(player_id),
                                         static_cast<Uint8>(g_session.expected_players),
                                         g_session.seed);
                ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(event.peer, 0, packet);
                send_existing_placements_to_peer(event.peer);
            }
            break;
        case ENET_EVENT_TYPE_RECEIVE: {
            const char *data = reinterpret_cast<const char *>(event.packet->data);
            ecl::Buffer buf;
            if (g_session.host) {
                if (handle_host_packet(data, event.packet->dataLength, event.peer, false, 0)) {
                    enet_packet_destroy(event.packet);
                    break;
                }
            }
            buf.assign(const_cast<char *>(data), event.packet->dataLength);
            protocol::InputPacket input_msg;
            if (!g_session.host && protocol::decode_input(buf, input_msg)) {
                if (input_msg.epoch != g_session.input_epoch) {
                    debug_log("mp drop input: tick=%u epoch=%u local epoch=%u",
                              input_msg.tick, input_msg.epoch, g_session.input_epoch);
                    enet_packet_destroy(event.packet);
                    break;
                }
                if (input_msg.tick < 20) {
                    debug_log("mp recv input: tick=%u player=%u mouse=(%.2f,%.2f) rot=%d act=%u",
                              input_msg.tick, input_msg.player, input_msg.mouse_x, input_msg.mouse_y,
                              static_cast<int>(input_msg.rotate_steps),
                              static_cast<unsigned>(input_msg.activate_count));
                }
                if (input_msg.tick < input::CurrentTick()) {
                    enet_packet_destroy(event.packet);
                    break;
                }
                input::PlayerInput pi;
                pi.mouse_force = ecl::V2(input_msg.mouse_x, input_msg.mouse_y);
                pi.rotate_steps = input_msg.rotate_steps;
                pi.activate_count = input_msg.activate_count;
                input::EnqueueInput(input_msg.tick, input_msg.player, pi);
                enet_packet_destroy(event.packet);
                break;
            }

            buf.assign(const_cast<char *>(data), event.packet->dataLength);
            Uint8 player_id = 0;
            Uint8 expected_players = 0;
            Uint32 seed = 0;
            if (!g_session.host && protocol::decode_welcome(buf, player_id, expected_players, seed)) {
                g_session.local_player = player_id;
                g_session.local_player_known = true;
                g_session.expected_players = expected_players;
                g_session.seed = seed;
                input::SetExpectedPlayers(expected_players);
                debug_log("mp client: welcome player=%u expected=%u seed=%u",
                          player_id, expected_players, seed);
                if (g_session.start_requested && !g_session.ready_sent) {
                    send_ready_to_host();
                    g_session.ready_sent = true;
                    g_session.ready_timer = 0.0;
                }
                enet_packet_destroy(event.packet);
                break;
            }

            buf.assign(const_cast<char *>(data), event.packet->dataLength);
            protocol::SyncPacket sync;
            if (!g_session.host && protocol::decode_sync(buf, sync)) {
                uint32_t local_tick = input::CurrentTick();
                if (local_tick < sync.tick) {
                    g_session.pending_sync = sync;
                    g_session.has_pending_sync = true;
                } else if (local_tick == sync.tick) {
                    handle_sync_current(sync);
                } else {
                    SessionState::ChecksumSample sample;
                    if (lookup_checksum_sample(sync.tick, sample)) {
                        handle_sync_sample(sync, sample);
                    } else {
                        debug_log("mp sync skip: sync tick=%u local tick=%u (no history)",
                                  sync.tick, local_tick);
                    }
                }
                enet_packet_destroy(event.packet);
                break;
            }

            buf.assign(const_cast<char *>(data), event.packet->dataLength);
            if (!g_session.host) {
                protocol::ResyncState state;
                if (protocol::decode_resync_state(buf, state)) {
                    apply_resync_state(state);
                    enet_packet_destroy(event.packet);
                    break;
                }
            }

            buf.assign(const_cast<char *>(data), event.packet->dataLength);
            if (!g_session.host) {
                Uint32 epoch = 0;
                if (protocol::decode_start(buf, epoch)) {
                    g_session.input_epoch = epoch;
                    configure_input_session(g_session.expected_players);
                    g_session.start_allowed = true;
                    if (debug_enabled())
                        debug_log("mp client: start allowed");
                    debug_log("mp client: start allowed");
                    enet_packet_destroy(event.packet);
                    break;
                }
            }

            buf.assign(const_cast<char *>(data), event.packet->dataLength);
            if (!g_session.host) {
                protocol::RestartPacket restart;
                if (protocol::decode_restart(buf, restart)) {
                    if (restart.restart_id > g_session.last_restart_id) {
                        g_session.last_restart_id = restart.restart_id;
                        g_session.restart_id = restart.restart_id;
                        if (restart.level_restart)
                            server::RestartLevelFromNetwork();
                        else
                            server::Msg_RestartGameFromNetwork();
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
            }

            buf.assign(const_cast<char *>(data), event.packet->dataLength);
            if (!g_session.host) {
                protocol::PlacementPacket place;
                if (protocol::decode_place(buf, place)) {
                    if (place.restart_id == g_session.restart_id &&
                        place.player < g_session.expected_players) {
                        GridPos pos(static_cast<int>(place.x), static_cast<int>(place.y));
                        apply_placement(place.player, pos);
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
            }

            enet_packet_destroy(event.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
            if (g_session.host) {
                g_session.peer_players.erase(event.peer);
                g_session.peer_ready.erase(event.peer);
                if (g_session.active) {
                    if (!client::AbortGameP())
                        client::Msg_ShowText("Player disconnected. Ending session.", true, 3.0);
                    client::Msg_Command("abort");
                    Shutdown();
                    return;
                }
            } else {
                if (!client::AbortGameP())
                    client::Msg_ShowText("Disconnected from host.", true, 3.0);
                client::Msg_Command("abort");
                Shutdown();
                return;
            }
            break;
        default:
            break;
        }
    }

    if (g_session.host && g_session.relay_handle) {
        while (enet_host_service(g_session.relay_handle, &event, 0) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_RECEIVE: {
                const char *data = reinterpret_cast<const char *>(event.packet->data);
                RelayMessageType type = RELAY_ERROR;
                Uint32 session_id = 0;
                Uint32 client_id = 0;
                const char *payload = nullptr;
                size_t payload_len = 0;
                if (!decode_relay_header(data, event.packet->dataLength, type,
                                         session_id, client_id, payload, payload_len)) {
                    enet_packet_destroy(event.packet);
                    break;
                }
                if (session_id != g_session.session_id) {
                    enet_packet_destroy(event.packet);
                    break;
                }
                if (type == RELAY_CLIENT_CONNECT) {
                    if (g_session.next_player_id >= g_session.expected_players) {
                        enet_packet_destroy(event.packet);
                        break;
                    }
                    unsigned player_id = g_session.next_player_id++;
                    g_session.relay_players[client_id] = player_id;
                    g_session.relay_ready[client_id] = false;
                    debug_log("mp host: relay client -> player %u", player_id);
                    ecl::Buffer buf;
                    protocol::encode_welcome(buf, static_cast<Uint8>(player_id),
                                             static_cast<Uint8>(g_session.expected_players),
                                             g_session.seed);
                    send_relay_payload(client_id, buf);
                    send_existing_placements_to_relay(client_id);
                    enet_packet_destroy(event.packet);
                    break;
                }
                if (type == RELAY_CLIENT_DISCONNECT) {
                    g_session.relay_players.erase(client_id);
                    g_session.relay_ready.erase(client_id);
                    if (g_session.active) {
                        if (!client::AbortGameP())
                            client::Msg_ShowText("Player disconnected. Ending session.", true, 3.0);
                        client::Msg_Command("abort");
                        Shutdown();
                        enet_packet_destroy(event.packet);
                        return;
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
                if (type == RELAY_DATA) {
                    handle_host_packet(payload, payload_len, nullptr, true, client_id);
                    enet_packet_destroy(event.packet);
                    break;
                }
                enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
                if (g_session.relay_peer == event.peer) {
                    g_session.relay_peer = nullptr;
                    if (!g_session.relay_players.empty()) {
                        if (!client::AbortGameP())
                            client::Msg_ShowText("Relay disconnected. Ending session.", true, 3.0);
                        client::Msg_Command("abort");
                        Shutdown();
                        return;
                    }
                }
                break;
            default:
                break;
            }
        }
    }
}

void send_local_inputs() {
    if (!g_session.active || !g_session.local_player_known)
        return;
    uint32_t current_tick = input::CurrentTick();
    uint32_t target_tick = current_tick + kInputDelay;
    if (g_session.input_clock_tick > current_tick) {
        uint32_t extra = g_session.input_clock_tick - current_tick;
        if (extra > kMaxInputLead)
            extra = kMaxInputLead;
        target_tick += extra;
    }
    input::PlayerInput pending = input::DrainLocalPending(g_session.local_player);
    bool applied = false;
    bool sent_packets = false;

    while (g_session.next_local_tick <= target_tick) {
        input::PlayerInput send = applied ? input::PlayerInput() : pending;
        if (!send.empty()) {
            float fx = static_cast<float>(send.mouse_force[0]);
            float fy = static_cast<float>(send.mouse_force[1]);
            send.mouse_force = ecl::V2(fx, fy);
        }
        protocol::InputPacket pkt;
        pkt.epoch = g_session.input_epoch;
        pkt.tick = g_session.next_local_tick;
        pkt.player = static_cast<Uint8>(g_session.local_player);
        pkt.mouse_x = static_cast<float>(send.mouse_force[0]);
        pkt.mouse_y = static_cast<float>(send.mouse_force[1]);
        pkt.rotate_steps = static_cast<int16_t>(send.rotate_steps);
        pkt.activate_count = static_cast<Uint8>(send.activate_count);

        input::EnqueueInput(pkt.tick, g_session.local_player, send);
        g_session.local_history[pkt.tick] = send;
        applied = true;
        ++g_session.next_local_tick;
    }

    if (g_session.host) {
        if (!has_remote_peers())
            return;
    } else if (!g_session.server_peer) {
        return;
    }

    uint32_t send_tick = g_session.next_send_tick;
    if (send_tick < current_tick)
        send_tick = current_tick;
    while (send_tick < g_session.next_local_tick) {
        protocol::InputPacket pkt;
        pkt.epoch = g_session.input_epoch;
        pkt.tick = send_tick;
        pkt.player = static_cast<Uint8>(g_session.local_player);
        auto it = g_session.local_history.find(pkt.tick);
        if (it != g_session.local_history.end()) {
            const input::PlayerInput &queued = it->second;
            pkt.mouse_x = static_cast<float>(queued.mouse_force[0]);
            pkt.mouse_y = static_cast<float>(queued.mouse_force[1]);
            pkt.rotate_steps = static_cast<int16_t>(queued.rotate_steps);
            pkt.activate_count = static_cast<Uint8>(queued.activate_count);
            g_session.local_history.erase(it);
        } else {
            pkt.mouse_x = 0.0f;
            pkt.mouse_y = 0.0f;
            pkt.rotate_steps = 0;
            pkt.activate_count = 0;
        }
        if (pkt.tick < 20) {
            debug_log("mp send input: tick=%u player=%u mouse=(%.2f,%.2f) rot=%d act=%u",
                      pkt.tick, pkt.player, pkt.mouse_x, pkt.mouse_y,
                      static_cast<int>(pkt.rotate_steps),
                      static_cast<unsigned>(pkt.activate_count));
        }
        if (g_session.host)
            broadcast_input(pkt, nullptr, 0);
        else
            send_input_to_peer(g_session.server_peer, pkt);
        sent_packets = true;
        ++send_tick;
    }
    g_session.next_send_tick = send_tick;

    if (sent_packets)
        enet_host_flush(g_session.host_handle);
}

}  // namespace

bool IsActive() {
    return g_session.active;
}

bool IsHost() {
    return g_session.host;
}

unsigned LocalPlayer() {
    return g_session.local_player;
}

unsigned ExpectedPlayers() {
    return g_session.expected_players;
}

bool ShouldDeferStart() {
    if (!g_session.active)
        return false;
    if (debug_enabled())
        debug_log("mp should defer start: %d", ready_to_start() ? 0 : 1);
    return !ready_to_start();
}

void NotifyStartRequested() {
    if (!g_session.active)
        return;
    if (debug_enabled())
        debug_log("mp notify start requested (host=%d local=%u expected=%u)",
                  g_session.host ? 1 : 0, g_session.local_player, g_session.expected_players);
    g_session.start_requested = true;
    if (!g_session.host && local_can_send_ready() && !g_session.ready_sent) {
        send_ready_to_host();
        g_session.ready_sent = true;
        g_session.ready_timer = 0.0;
    }
}

void NotifyRestart(bool level_restart) {
    if (!g_session.active || !g_session.host)
        return;
    g_session.restart_id += 1;
    g_session.last_restart_id = g_session.restart_id;
    send_restart_to_peers(level_restart);
}

void PrepareExtraActors() {
    if (!g_session.active)
        return;
    g_session.level_players = compute_level_players();
    if (g_session.level_players < 1)
        g_session.level_players = 1;
    add_extra_actors(g_session.level_players, g_session.expected_players);
}

void SetupExtraPlayerStartPositions() {
    if (!g_session.active)
        return;
    if (g_session.level_players == 0)
        g_session.level_players = compute_level_players();
    if (g_session.level_players < 1)
        g_session.level_players = 1;
    g_session.placement_received.clear();
    if (g_session.expected_players > 0) {
        g_session.placement_received.resize(g_session.expected_players, false);
        for (unsigned player = 0; player < g_session.expected_players; ++player) {
            if (!placement_required_for_player(player))
                g_session.placement_received[player] = true;
        }
    }
    auto_place_extra_players();
}

void PrimeInputQueueForNewLevel() {
    if (!g_session.active)
        return;
    configure_input_session(g_session.expected_players);
    g_session.desync_reported = false;
    g_session.start_requested = false;
    g_session.start_allowed = false;
    g_session.ready_sent = false;
    g_session.ready_timer = 0.0;
    g_session.has_pending_sync = false;
    g_session.last_world_checksum = 0;
    g_session.checksum_history.clear();
    g_session.last_checksum_tick = UINT32_MAX;
    g_session.resync_inflight = false;
    g_session.resync_cooldown = 0.0;
    g_session.resync_attempts = 0;
    g_session.ready_timer = 0.0;
    g_session.level_players = 0;
    g_session.placement_received.clear();
    if (g_session.host) {
        for (auto &entry : g_session.peer_ready)
            entry.second = false;
        for (auto &entry : g_session.relay_ready)
            entry.second = false;
    }
}

void LobbyStart() {
    if (g_lobby.active)
        return;
    g_lobby.active = true;
    g_lobby.local_id = make_id();
    g_lobby.local_name = resolve_local_name();
    g_lobby.time = 0.0;
    g_lobby.announce_timer = 0.0;
    g_lobby.peers.clear();
    g_lobby.has_pending_start = false;
    g_lobby.pending_host_ip.clear();
    g_lobby.last_session_id = 0;

    g_lobby.socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM, nullptr);
    if (g_lobby.socket == ENET_SOCKET_NULL) {
        g_lobby.active = false;
        return;
    }
    if (!bind_lobby_socket(g_lobby.socket, kLobbyPort)) {
        enet_socket_destroy(g_lobby.socket);
        g_lobby.socket = ENET_SOCKET_NULL;
        g_lobby.active = false;
        return;
    }
    send_lobby_announce();
}

void LobbyStop() {
    if (!g_lobby.active)
        return;
    if (g_lobby.socket != ENET_SOCKET_NULL) {
        enet_socket_destroy(g_lobby.socket);
        g_lobby.socket = ENET_SOCKET_NULL;
    }
    g_lobby.active = false;
    g_lobby.peers.clear();
}

void LobbyTick(double dtime) {
    if (!g_lobby.active)
        return;
    g_lobby.time += dtime;
    g_lobby.announce_timer += dtime;
    if (g_lobby.announce_timer >= kAnnounceInterval) {
        g_lobby.announce_timer = 0.0;
        send_lobby_announce();
    }
    poll_lobby_socket();
    cleanup_lobby_peers();
}

std::vector<LobbyPeer> LobbyPeers() {
    std::vector<LobbyPeer> result;
    if (!g_lobby.active)
        return result;
    LobbyPeer self;
    self.id = g_lobby.local_id;
    self.name = g_lobby.local_name;
    self.level_id = g_lobby.selected_level;
    self.address = "localhost";
    self.is_self = true;
    result.push_back(self);
    for (const auto &entry : g_lobby.peers) {
        result.push_back(entry.second.peer);
    }
    return result;
}

unsigned LobbySize() {
    if (!g_lobby.active)
        return 0;
    return static_cast<unsigned>(g_lobby.peers.size() + 1);
}

void LobbySetSelectedLevel(const std::string &level_id) {
    g_lobby.selected_level = level_id;
}

std::string LobbySelectedLevel() {
    return g_lobby.selected_level;
}

bool LobbyPollStart(protocol::LobbyStart &start, std::string &host_ip) {
    if (!g_lobby.has_pending_start)
        return false;
    start = g_lobby.pending_start;
    host_ip = g_lobby.pending_host_ip;
    g_lobby.has_pending_start = false;
    return true;
}

protocol::LobbyStart BuildStartMessage(const std::string &level_id, unsigned expected_players,
                                       unsigned filter_min_players) {
    protocol::LobbyStart start;
    std::random_device rd;
    Uint32 seed = static_cast<Uint32>(rd() ^ SDL_GetTicks());
    Uint32 session_id = static_cast<Uint32>((rd() << 16) ^ SDL_GetTicks());
    start.session_id = session_id;
    start.level_id = level_id;
    start.seed = seed;
    start.expected_players = static_cast<Uint8>(expected_players);
    start.host_port = kGamePort;
    start.host_id = g_lobby.local_id;
    if (filter_min_players < 1)
        filter_min_players = 1;
    if (filter_min_players > expected_players)
        filter_min_players = expected_players;
    start.filter_optimized = static_cast<Uint8>(filter_min_players);
    return start;
}

void LobbyBroadcastStart(const protocol::LobbyStart &start) {
    if (!g_lobby.active || g_lobby.socket == ENET_SOCKET_NULL)
        return;
    ecl::Buffer buf;
    protocol::encode_lobby_start(buf, start);
    ENetAddress addr;
    addr.host = ENET_HOST_BROADCAST;
    addr.port = kLobbyPort;
    ENetBuffer eb;
    eb.data = const_cast<char *>(buf.data());
    eb.dataLength = buf.size();
    enet_socket_send(g_lobby.socket, &addr, &eb, 1);
}

std::string EncodeStartToken(const protocol::LobbyStart &start) {
    ecl::Buffer buf;
    protocol::encode_lobby_start(buf, start);
    return encode_hex(buf.data(), buf.size());
}

bool DecodeStartToken(const std::string &token, protocol::LobbyStart &start) {
    ecl::Buffer buf;
    if (!decode_hex(token, buf))
        return false;
    return protocol::decode_lobby_start(buf, start);
}

bool InternetCreateRoom(const std::string &server, const std::string &room_code,
                        const protocol::LobbyStart &start, std::string &error) {
    ensure_lobby_identity();
    ecl::Buffer request;
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_CREATE)
            << room_code << Uint32(start.session_id) << Uint32(start.seed)
            << Uint8(start.expected_players) << Uint16(start.host_port)
            << Uint8(start.filter_optimized) << start.level_id << start.host_id;

    ecl::Buffer response;
    if (!internet_exchange(server, request, response, error))
        return false;

    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 type = 0;
    if (!(response >> magic >> version >> type))
        return false;
    if (magic != kInternetMagic || version != kInternetVersion)
        return false;
    if (type == INET_ERROR) {
        std::string msg;
        response >> msg;
        error = msg.empty() ? "Server error." : msg;
        return false;
    }
    if (type != INET_CREATE_OK)
        return false;
    std::string host_ip;
    std::string server_room;
    if (!(response >> server_room >> host_ip))
        return false;
    (void)host_ip;
    return true;
}

bool InternetJoinRoom(const std::string &server, const std::string &room_code,
                      protocol::LobbyStart &start, std::string &host_ip,
                      unsigned &player_count, std::string &error) {
    ensure_lobby_identity();
    player_count = 0;
    ecl::Buffer request;
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_JOIN)
            << room_code << g_lobby.local_id;

    ecl::Buffer response;
    if (!internet_exchange(server, request, response, error))
        return false;

    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 type = 0;
    if (!(response >> magic >> version >> type))
        return false;
    if (magic != kInternetMagic || version != kInternetVersion)
        return false;
    if (type == INET_ERROR) {
        std::string msg;
        response >> msg;
        error = msg.empty() ? "Server error." : msg;
        return false;
    }
    if (type != INET_JOIN_OK)
        return false;
    if (!(response >> start.session_id >> start.level_id >> start.seed
          >> start.expected_players >> start.host_port >> start.host_id
          >> start.filter_optimized >> host_ip))
        return false;
    if (response.get_rpos() < static_cast<std::ptrdiff_t>(response.size())) {
        Uint8 count = 0;
        if (response >> count)
            player_count = count;
    }
    return true;
}

bool InternetStartRoom(const std::string &server, const std::string &room_code,
                       const protocol::LobbyStart &start, std::string &error) {
    ensure_lobby_identity();
    ecl::Buffer request;
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_START)
            << room_code << Uint32(start.session_id) << Uint32(start.seed)
            << Uint8(start.expected_players) << Uint16(start.host_port)
            << Uint8(start.filter_optimized) << start.level_id << start.host_id;

    ecl::Buffer response;
    if (!internet_exchange(server, request, response, error))
        return false;

    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 type = 0;
    if (!(response >> magic >> version >> type))
        return false;
    if (magic != kInternetMagic || version != kInternetVersion)
        return false;
    if (type == INET_ERROR) {
        std::string msg;
        response >> msg;
        error = msg.empty() ? "Server error." : msg;
        return false;
    }
    if (type != INET_START_OK)
        return false;
    return true;
}

bool InternetPollRoom(const std::string &server, const std::string &room_code,
                      protocol::LobbyStart &start, std::string &host_ip,
                      bool &started, unsigned &player_count, std::string &error) {
    ensure_lobby_identity();
    started = false;
    player_count = 0;
    ecl::Buffer request;
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_POLL)
            << room_code;

    ecl::Buffer response;
    if (!internet_exchange(server, request, response, error))
        return false;

    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 type = 0;
    if (!(response >> magic >> version >> type))
        return false;
    if (magic != kInternetMagic || version != kInternetVersion)
        return false;
    if (type == INET_ERROR) {
        std::string msg;
        response >> msg;
        error = msg.empty() ? "Server error." : msg;
        return false;
    }
    if (type != INET_POLL_OK)
        return false;
    Uint8 started_flag = 0;
    Uint8 count = 0;
    if (!(response >> started_flag >> count))
        return false;
    started = (started_flag != 0);
    player_count = count;
    if (!started)
        return true;
    if (!(response >> start.session_id >> start.level_id >> start.seed
          >> start.expected_players >> start.host_port >> start.host_id
          >> start.filter_optimized >> host_ip))
        return false;
    return true;
}

bool InternetLeaveRoom(const std::string &server, const std::string &room_code,
                       std::string &error) {
    ensure_lobby_identity();
    ecl::Buffer request;
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_LEAVE)
            << room_code << g_lobby.local_id;

    ecl::Buffer response;
    if (!internet_exchange(server, request, response, error))
        return false;

    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 type = 0;
    if (!(response >> magic >> version >> type))
        return false;
    if (magic != kInternetMagic || version != kInternetVersion)
        return false;
    if (type == INET_ERROR) {
        std::string msg;
        response >> msg;
        error = msg.empty() ? "Server error." : msg;
        return false;
    }
    return true;
}

void SetRelayServer(const std::string &server) {
    g_relay_server = server;
}

bool StartHostSession(const protocol::LobbyStart &start) {
    if (g_session.active)
        return false;
    if (debug_enabled())
        debug_log("mp debug enabled");
    unsigned expected_players = std::min<unsigned>(start.expected_players, input::kMaxPlayers);
    if (expected_players < 1)
        return false;
    g_session.active = true;
    g_session.host = true;
    g_session.local_player_known = true;
    g_session.local_player = 0;
    g_session.expected_players = expected_players;
    g_session.session_id = start.session_id;
    g_session.seed = start.seed;
    g_session.input_epoch = 0;
    g_session.restart_id = 0;
    g_session.last_restart_id = 0;
    g_session.next_player_id = 1;
    g_session.next_local_tick = kInputDelay;
    g_session.next_send_tick = 0;
    g_session.local_history.clear();
    g_session.sync_timer = 0.0;
    g_session.desync_reported = false;
    g_session.start_requested = false;
    g_session.start_allowed = false;
    g_session.ready_sent = false;
    g_session.ready_timer = 0.0;
    g_session.checksum_history.clear();
    g_session.last_checksum_tick = UINT32_MAX;
    g_session.peer_players.clear();
    g_session.peer_ready.clear();
    g_session.relay_players.clear();
    g_session.relay_ready.clear();
    g_session.relay_peer = nullptr;
    g_session.relay_handle = nullptr;

    enigma::Randomize(start.seed, true);
    configure_input_session(expected_players);

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = start.host_port;
    g_session.host_handle = enet_host_create(&address, expected_players - 1, 0, 0);
    if (g_session.host_handle == nullptr) {
        Shutdown();
        return false;
    }
    if (!g_relay_server.empty()) {
        if (debug_enabled())
            debug_log("mp host: relay server=%s", g_relay_server.c_str());
        std::string relay_host;
        Uint16 relay_port = 0;
        if (parse_host_port(g_relay_server, relay_host, relay_port)) {
            if (g_session.relay_handle)
                enet_host_destroy(g_session.relay_handle);
            g_session.relay_handle = enet_host_create(nullptr, 1, 0, 0);
            if (g_session.relay_handle) {
                ENetAddress relay_addr;
                enet_address_set_host(&relay_addr, relay_host.c_str());
                relay_addr.port = relay_port;
                g_session.relay_peer = enet_host_connect(g_session.relay_handle, &relay_addr, 2);
                if (g_session.relay_peer) {
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
                        debug_log("mp host: connected to relay %s:%u",
                                  relay_host.c_str(), static_cast<unsigned>(relay_port));
                    } else {
                        debug_log("mp host: relay connect timeout %s:%u",
                                  relay_host.c_str(), static_cast<unsigned>(relay_port));
                        enet_host_destroy(g_session.relay_handle);
                        g_session.relay_handle = nullptr;
                        g_session.relay_peer = nullptr;
                    }
                } else {
                    debug_log("mp host: relay connect failed %s:%u",
                              relay_host.c_str(), static_cast<unsigned>(relay_port));
                    enet_host_destroy(g_session.relay_handle);
                    g_session.relay_handle = nullptr;
                }
            }
        }
    }
    return true;
}

bool StartClientSession(const protocol::LobbyStart &start, const std::string &host_ip) {
    if (g_session.active)
        return false;
    if (debug_enabled())
        debug_log("mp debug enabled");
    unsigned expected_players = std::min<unsigned>(start.expected_players, input::kMaxPlayers);
    if (expected_players < 1)
        return false;
    g_session.active = true;
    g_session.host = false;
    g_session.local_player_known = false;
    g_session.local_player = 1;
    g_session.expected_players = expected_players;
    g_session.session_id = start.session_id;
    g_session.seed = start.seed;
    g_session.input_epoch = 0;
    g_session.restart_id = 0;
    g_session.last_restart_id = 0;
    g_session.next_local_tick = kInputDelay;
    g_session.next_send_tick = 0;
    g_session.local_history.clear();
    g_session.sync_timer = 0.0;
    g_session.desync_reported = false;
    g_session.start_requested = false;
    g_session.start_allowed = false;
    g_session.ready_sent = false;
    g_session.checksum_history.clear();
    g_session.last_checksum_tick = UINT32_MAX;

    enigma::Randomize(start.seed, true);
    configure_input_session(expected_players);
    auto connect_and_wait = [&](const std::string &target_host, Uint16 target_port,
                                bool relay_connect, Uint32 connect_timeout_ms,
                                Uint32 welcome_timeout_ms) -> bool {
        if (debug_enabled())
            debug_log("mp client: connect %s:%u relay=%d",
                      target_host.c_str(), static_cast<unsigned>(target_port),
                      relay_connect ? 1 : 0);
        if (g_session.host_handle) {
            enet_host_destroy(g_session.host_handle);
            g_session.host_handle = nullptr;
        }
        g_session.server_peer = nullptr;
        g_session.host_handle = enet_host_create(nullptr, 1, 0, 0);
        if (g_session.host_handle == nullptr)
            return false;

        ENetAddress addr;
        enet_address_set_host(&addr, target_host.c_str());
        addr.port = target_port;
        g_session.server_peer = enet_host_connect(g_session.host_handle, &addr, 2);
        if (g_session.server_peer == nullptr)
            return false;

        ENetEvent event;
        if (!(enet_host_service(g_session.host_handle, &event, connect_timeout_ms) > 0 &&
              event.type == ENET_EVENT_TYPE_CONNECT)) {
            if (debug_enabled())
                debug_log("mp client: connect failed %s:%u", target_host.c_str(),
                          static_cast<unsigned>(target_port));
            return false;
        }
        g_session.server_peer = event.peer;
        if (relay_connect) {
            ecl::Buffer buf;
            encode_relay_header(buf, RELAY_HELLO_CLIENT, start.session_id, 0);
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
        return false;
    };

    if (!force_relay_enabled()) {
        if (connect_and_wait(host_ip, start.host_port, false, 3000, 3000))
            return true;
    } else if (debug_enabled()) {
        debug_log("mp client: force relay enabled");
    }

    if (!g_relay_server.empty()) {
        if (debug_enabled())
            debug_log("mp client: relay server=%s", g_relay_server.c_str());
        std::string relay_host;
        Uint16 relay_port = 0;
        if (parse_host_port(g_relay_server, relay_host, relay_port)) {
            if (connect_and_wait(relay_host, relay_port, true, 2000, 2000))
                return true;
        }
    }

    Shutdown();
    return false;
}

void Tick(double dtime) {
    if (!g_session.active)
        return;
    g_session.input_clock_accu += dtime;
    if (g_session.input_clock_accu > 1.0)
        g_session.input_clock_accu = 1.0;
    while (g_session.input_clock_accu >= kInputTimestep) {
        g_session.input_clock_accu -= kInputTimestep;
        g_session.input_clock_tick += 1;
    }
    process_network_events();
    if (!g_session.active)
        return;
    if (g_session.has_pending_sync) {
        uint32_t local_tick = input::CurrentTick();
        if (local_tick == g_session.pending_sync.tick) {
            protocol::SyncPacket sync = g_session.pending_sync;
            g_session.has_pending_sync = false;
            handle_sync_current(sync);
        } else if (local_tick > g_session.pending_sync.tick) {
            protocol::SyncPacket sync = g_session.pending_sync;
            SessionState::ChecksumSample sample;
            if (lookup_checksum_sample(sync.tick, sample))
                handle_sync_sample(sync, sample);
            g_session.has_pending_sync = false;
        }
    }
    record_checksum_sample();
    if (g_session.resync_cooldown > 0.0) {
        g_session.resync_cooldown -= dtime;
        if (g_session.resync_cooldown < 0.0)
            g_session.resync_cooldown = 0.0;
    }
    if (!g_session.host && g_session.start_requested && !g_session.start_allowed) {
        g_session.ready_timer += dtime;
        if (g_session.ready_timer >= 0.5) {
            if (!g_session.ready_sent && local_can_send_ready()) {
                send_ready_to_host();
                g_session.ready_sent = true;
            }
            g_session.ready_timer = 0.0;
        }
    }
    if (debug_enabled() && g_session.local_player_known && input::IsNetworked()) {
        static uint32_t last_missing_tick = UINT32_MAX;
        uint32_t tick = input::CurrentTick();
        if (tick != last_missing_tick) {
            bool missing = false;
            for (unsigned player = 0; player < g_session.expected_players; ++player) {
                if (!input::HasInput(tick, player)) {
                    debug_log("mp missing input: tick=%u player=%u", tick, player);
                    missing = true;
                }
            }
            if (missing)
                last_missing_tick = tick;
        }
    }
    if (g_session.start_requested) {
        if (g_session.host) {
            if (host_ready_to_start()) {
                if (debug_enabled())
                    debug_log("mp host: start allowed");
                g_session.start_requested = false;
                g_session.start_allowed = true;
                g_session.input_epoch += 1;
                configure_input_session(g_session.expected_players);
                send_start_to_peers();
                server::Msg_StartGame();
            }
        } else if (g_session.start_allowed) {
            g_session.start_requested = false;
            server::Msg_StartGame();
        }
    }
    send_local_inputs();
    if (g_session.host) {
        g_session.sync_timer += dtime;
        if (g_session.sync_timer >= kSyncInterval) {
            g_session.sync_timer = 0.0;
            send_sync_to_peers();
        }
    }
}

void Shutdown() {
    if (!g_session.active)
        return;
    if (g_session.relay_peer) {
        enet_peer_disconnect(g_session.relay_peer, 0);
        g_session.relay_peer = nullptr;
    }
    if (g_session.relay_handle) {
        enet_host_destroy(g_session.relay_handle);
        g_session.relay_handle = nullptr;
    }
    if (g_session.host_handle) {
        if (g_session.host) {
            for (const auto &entry : g_session.peer_players)
                enet_peer_disconnect(entry.first, 0);
        } else if (g_session.server_peer) {
            enet_peer_disconnect(g_session.server_peer, 0);
        }
        enet_host_destroy(g_session.host_handle);
    }
    g_session = SessionState();
    input::Reset();
}

}  // namespace multiplayer
}  // namespace enigma
