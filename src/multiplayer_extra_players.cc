#include "multiplayer_extra_players.hh"

#include "multiplayer_transport.hh"

#include "display.hh"
#include "enigma.hh"
#include "errors.hh"
#include "player.hh"
#include "server.hh"
#include "video.hh"
#include "lev/Proxy.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace enigma {
namespace multiplayer {
namespace internal {

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

namespace {

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
    if (kind == "fl_abyss" || kind == "fl_water" || kind == "fl_space" ||
        kind == "fl_space_force") {
        return false;
    }
    if (GetStone(pos) != nullptr)
        return false;
    if (GetItem(pos) != nullptr)
        return false;
    return true;
}

bool is_free_floor(const GridPos &pos) {
    if (!IsInsideLevel(pos))
        return false;
    Floor *floor = GetFloor(pos);
    if (!floor)
        return false;
    const std::string kind = floor->getKind();
    if (kind == "fl_abyss" || kind == "fl_water" || kind == "fl_space" ||
        kind == "fl_space_force") {
        return false;
    }
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

template <class Sender>
void for_each_existing_placement(Sender send) {
    if (!g_session.host)
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
        send(msg);
    }
}

void send_placement_to_peer(ENetPeer *peer, const protocol::PlacementPacket &msg) {
    if (!peer)
        return;
    ecl::Buffer buf;
    protocol::encode_place(buf, msg);
    ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, packet);
}

}  // namespace

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

void apply_placement(unsigned player, const GridPos &pos) {
    Actor *actor = player::GetMainActor(player);
    if (!actor)
        return;
    ecl::V2 center = pos.center();
    WarpActor(actor, center[0], center[1], false);
    actor->set_respawnpos(center);
}

void broadcast_placement(const protocol::PlacementPacket &msg) {
    if (!g_session.host)
        return;
    ecl::Buffer buf;
    protocol::encode_place(buf, msg);
    g_transport.HostBroadcast(buf);
}

void send_existing_placements_to_peer(ENetPeer *peer) {
    if (!peer)
        return;
    for_each_existing_placement([peer](const protocol::PlacementPacket &msg) {
        send_placement_to_peer(peer, msg);
    });
}

void send_existing_placements_to_relay(Uint32 client_id) {
    for_each_existing_placement([client_id](const protocol::PlacementPacket &msg) {
        ecl::Buffer buf;
        protocol::encode_place(buf, msg);
        g_transport.HostSendUdpRelay(client_id, buf);
    });
}

void send_existing_placements_to_tcp_relay(Uint32 client_id) {
    for_each_existing_placement([client_id](const protocol::PlacementPacket &msg) {
        ecl::Buffer buf;
        protocol::encode_place(buf, msg);
        g_transport.HostSendTcpRelay(client_id, buf);
    });
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
    ecl::Buffer buf;
    protocol::encode_place(buf, msg);
    g_transport.ClientSend(buf);
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

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
