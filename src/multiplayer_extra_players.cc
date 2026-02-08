#include "multiplayer_extra_players.hh"

#include "multiplayer_ball_assignment.hh"
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
    if (player >= g_session.needs_placement.size())
        return false;
    return g_session.needs_placement[player];
}

namespace {

bool controller_single_bit_index(int controllers, unsigned &out_index) {
    if (controllers <= 0)
        return false;
    unsigned mask = static_cast<unsigned>(controllers);
    if ((mask & (mask - 1)) != 0)
        return false;
    unsigned idx = 0;
    while ((mask & 1u) == 0u) {
        mask >>= 1u;
        idx += 1u;
    }
    out_index = idx;
    return true;
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
        // Only players that actually required placement during this level should get
        // placement packets. Players that don't require placement are marked
        // placement_received=true so the host can start immediately, but that does
        // not mean we have a meaningful placement to broadcast.
        if (!placement_required_for_player(player))
            continue;
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
    g_session.needs_placement.clear();
    if (expected_players > 0)
        g_session.needs_placement.assign(expected_players, false);
    if (level_players == 0 || expected_players <= level_players)
        return;
    std::vector<Actor *> actors;
    GetActors(actors);
    if (debug_enabled())
        debug_log("mp extra actors: level_players=%u expected=%u actors=%zu", level_players,
                  expected_players, actors.size());

    rebalance_authored_multi_ball_levels(level_players, expected_players);

    // Track which session players already control a steerable actor after
    // redistribution, to avoid spawning unnecessary duplicates.
    std::vector<bool> has_actor(expected_players, false);
    for (auto *actor : actors) {
        if (!actor || !actor->isSteerable())
            continue;
        int controllers = actor->get_controllers();
        unsigned owner = 0;
        if (!controller_single_bit_index(controllers, owner))
            continue;
        if (owner >= expected_players)
            continue;
        has_actor[owner] = true;
    }

    std::vector<Actor *> base(level_players, nullptr);
    std::vector<Actor *> fallback(level_players, nullptr);
    Actor *any_actor = nullptr;

    for (auto *actor : actors) {
        if (!any_actor && actor->isSteerable())
            any_actor = actor;
        unsigned owner = 0;
        bool has_owner = false;
        if (Value owner_val = actor->getAttr("owner")) {
            int owner_int = owner_val;
            if (owner_int >= 0 && owner_int < static_cast<int>(level_players)) {
                owner = static_cast<unsigned>(owner_int);
                has_owner = true;
            }
        }
        if (!has_owner) {
            unsigned ctrl_owner = 0;
            if (controller_single_bit_index(actor->get_controllers(), ctrl_owner) &&
                ctrl_owner < level_players) {
                owner = ctrl_owner;
                has_owner = true;
            }
        }
        if (!has_owner)
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
        if (player < has_actor.size() && has_actor[player])
            continue;
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
        if (player < g_session.needs_placement.size())
            g_session.needs_placement[player] = true;
    }

    if (debug_enabled()) {
        unsigned have = 0;
        for (bool v : has_actor)
            if (v)
                have += 1;
        unsigned need_place = 0;
        for (bool v : g_session.needs_placement)
            if (v)
                need_place += 1;
        debug_log("mp extra actors: assigned=%u spawned=%u needs_placement=%u", have,
                  (expected_players > have) ? (expected_players - have) : 0, need_place);
    }
}

void rebalance_authored_multi_ball_levels(unsigned level_players, unsigned expected_players) {
    if (level_players == 0 || expected_players <= level_players)
        return;
    std::vector<Actor *> actors;
    GetActors(actors);

    // Redistribution pass: some authored levels (e.g. meditation) give one player
    // multiple steerable actors. If there are enough such actors, distribute them
    // evenly across all session players that share the same color group
    // (player % level_players), instead of spawning new duplicates.
    std::vector<std::vector<Actor *>> candidates(level_players);
    for (auto *actor : actors) {
        if (!actor || !actor->isSteerable())
            continue;

        // For singleplayer-authored levels, treat all steerable, player-controlled
        // actors as belonging to the single base group. Some compatibility modes
        // and conversions (notably Per.Oxyd meditation pearls) may leave "owner"
        // unset or set to a non-zero value, but the level intent is still
        // "controlled by the (single) current player".
        if (level_players == 1) {
            if (actor->get_controllers() == 0) {
                Value owner_val = actor->getAttr("owner");
                Value color_val = actor->getAttr("color");
                if (!owner_val && !color_val)
                    continue;
            }
            candidates[0].push_back(actor);
            continue;
        }

        bool found_base = false;
        unsigned base = 0;

        // Prefer the authored owner if present. This covers many singleplayer
        // multi-ball levels where the engine default sets controllers=CTRL_YINYANG
        // (e.g. pearls), but the level intent is still "owned by player 0".
        if (Value owner_val = actor->getAttr("owner")) {
            int owner_int = owner_val;
            if (owner_int >= 0 && owner_int < static_cast<int>(level_players)) {
                base = static_cast<unsigned>(owner_int);
                found_base = true;
            }
        }

        // Fallback: single-controller actors that don't declare an owner.
        if (!found_base) {
            unsigned ctrl_base = 0;
            if (controller_single_bit_index(actor->get_controllers(), ctrl_base) &&
                ctrl_base < level_players) {
                base = ctrl_base;
                found_base = true;
            }
        }

        // Legacy singleplayer actors sometimes leave owner unset but still default to
        // controllers=CTRL_YINYANG (e.g. pearls created via old API mappings). In
        // singleplayer metadata those are still effectively "owned by player 0".
        if (!found_base && level_players == 1) {
            if ((actor->get_controllers() & 1) != 0) {
                base = 0;
                found_base = true;
            }
        }

        if (!found_base)
            continue;
        candidates[base].push_back(actor);
    }

    for (unsigned base = 0; base < level_players; ++base) {
        // Ensure redistribution is deterministic across platforms/builds.
        // Actor insertion order can differ subtly depending on loader/compat paths,
        // so sort by a stable key before assigning controllers.
        std::sort(candidates[base].begin(), candidates[base].end(),
                  [](const Actor *a, const Actor *b) {
                      if (a == b)
                          return false;
                      const ecl::V2 &pa = a->get_pos();
                      const ecl::V2 &pb = b->get_pos();
                      auto qa0 = static_cast<int64_t>(std::llround(pa[0] * 1000.0));
                      auto qa1 = static_cast<int64_t>(std::llround(pa[1] * 1000.0));
                      auto qb0 = static_cast<int64_t>(std::llround(pb[0] * 1000.0));
                      auto qb1 = static_cast<int64_t>(std::llround(pb[1] * 1000.0));
                      if (qa1 != qb1)
                          return qa1 < qb1;
                      if (qa0 != qb0)
                          return qa0 < qb0;
                      const ecl::V2 &va = a->get_vel();
                      const ecl::V2 &vb = b->get_vel();
                      auto qva0 = static_cast<int64_t>(std::llround(va[0] * 1000.0));
                      auto qva1 = static_cast<int64_t>(std::llround(va[1] * 1000.0));
                      auto qvb0 = static_cast<int64_t>(std::llround(vb[0] * 1000.0));
                      auto qvb1 = static_cast<int64_t>(std::llround(vb[1] * 1000.0));
                      if (qva1 != qvb1)
                          return qva1 < qvb1;
                      if (qva0 != qvb0)
                          return qva0 < qvb0;
                      int kind_cmp = a->getKind().compare(b->getKind());
                      if (kind_cmp != 0)
                          return kind_cmp < 0;
                      int ca = a->get_controllers();
                      int cb = b->get_controllers();
                      if (ca != cb)
                          return ca < cb;
                      Value oa = a->getAttr("owner");
                      Value ob = b->getAttr("owner");
                      int oai = oa ? static_cast<int>(oa) : -1;
                      int obi = ob ? static_cast<int>(ob) : -1;
                      if (oai != obi)
                          return oai < obi;
                      Value cola = a->getAttr("color");
                      Value colb = b->getAttr("color");
                      int cai = cola ? static_cast<int>(cola) : -1;
                      int cbi = colb ? static_cast<int>(colb) : -1;
                      if (cai != cbi)
                          return cai < cbi;
                      // If everything matches, the order does not matter: the actors are
                      // indistinguishable at our digest precision, and redistribution by
                      // position/velocity already yields the same per-player multiset.
                      return false;
                  });

        std::vector<unsigned> group;
        for (unsigned p = base; p < expected_players; p += level_players)
            group.push_back(p);
        if (group.size() <= 1)
            continue;
        if (candidates[base].size() < group.size())
            continue;  // Not enough authored balls; keep legacy duplication behavior.
        if (debug_enabled())
            debug_log("mp rebalance: base=%u candidates=%zu group=%zu", base, candidates[base].size(),
                      group.size());
        std::vector<unsigned> assignment =
            distribute_balls_round_robin(candidates[base].size(), group);
        for (size_t i = 0; i < candidates[base].size(); ++i) {
            Actor *a = candidates[base][i];
            unsigned target = assignment[i];
            a->setAttr("owner", Value(static_cast<int>(target)));
            a->setAttr("controllers", Value(static_cast<int>(1 << target)));
        }
    }
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
