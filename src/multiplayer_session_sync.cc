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

#include "multiplayer_session_impl.hh"

#include "multiplayer_transport.hh"

#include "client.hh"
#include "enigma.hh"
#include "input.hh"
#include "options.hh"
#include "player.hh"
#include "server.hh"
#include "world.hh"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

/* -------------------- Multiplayer session sync -------------------- */
/*
 * Deterministic sync checks and recovery.
 *
 * Maintains periodic checksums, detects divergence, and triggers soft-resyncs
 * (authoritative actor state snapshots) when the simulation drifts.
 */

namespace enigma {
namespace multiplayer {
namespace internal {

const char *transport_name(TransportKind t) {
    switch (t) {
    case TransportKind::DIRECT:
        return "direct";
    case TransportKind::UDP_RELAY:
        return "udp-relay";
    case TransportKind::TCP_RELAY:
        return "tcp-relay";
    default:
        return "none";
    }
}

const char *host_source_name(HostSource s) {
    switch (s) {
    case HostSource::DIRECT:
        return "direct";
    case HostSource::UDP_RELAY:
        return "udp-relay";
    case HostSource::TCP_RELAY:
        return "tcp-relay";
    default:
        return "unknown";
    }
}

namespace {

int64_t quantize_for_dump(double v) {
    return static_cast<int64_t>(std::llround(v * 100.0));
}

uint32_t stable_name_hash(Actor *actor) {
    if (!actor)
        return 0;
    Value name = actor->getAttr("name");
    if (name.getType() != Value::STRING)
        return 0;
    const std::string &s = name.get_string();
    if (s.empty())
        return 0;
    // FNV-1a 32-bit.
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= static_cast<uint32_t>(c);
        h *= 16777619u;
    }
    return h ? h : 1u;
}

Actor *sync_reference_actor(unsigned player, uint32_t tick);

struct ActorSortKey {
    uint32_t name_hash = 0;
    std::string kind;
    Uint32 object_id = 0;
};

ActorSortKey actor_sort_key(Actor *a) {
    ActorSortKey k;
    k.name_hash = stable_name_hash(a);
    k.kind = a ? a->getKind() : std::string();
    k.object_id = a ? static_cast<Uint32>(a->getId()) : 0u;
    return k;
}

bool actor_key_less(const ActorSortKey &a, const ActorSortKey &b) {
    // Prefer actors with a stable authored name (e.g. meditation pearls: pearl%N).
    // Unnamed actors sort after named ones.
    uint32_t an = a.name_hash ? a.name_hash : 0xFFFFFFFFu;
    uint32_t bn = b.name_hash ? b.name_hash : 0xFFFFFFFFu;
    if (an != bn)
        return an < bn;
    if (a.kind != b.kind)
        return a.kind < b.kind;
    return a.object_id < b.object_id;
}

void dump_actor_digest_once(const char *reason, uint32_t tick, const protocol::SyncPacket *sync) {
    if (!debug_enabled() || !dump_state_enabled())
        return;
    if (g_session.debug_state_dumped)
        return;
    g_session.debug_state_dumped = true;

    debug_log("mp dump: reason=%s epoch=%u tick=%u host=%d transport=%s expected=%u local_player=%u seed=%u",
              reason ? reason : "unknown",
              static_cast<unsigned>(g_session.input_epoch),
              static_cast<unsigned>(tick),
              g_session.host ? 1 : 0,
              transport_name(g_session.active_transport),
              static_cast<unsigned>(g_session.expected_players),
              static_cast<unsigned>(g_session.local_player),
              static_cast<unsigned>(g_session.seed));
    debug_log("mp dump: local rand=%u", static_cast<unsigned>(server::RandomState));

    if (server::LoadedProxy) {
        std::string level_path =
            (server::LoadedProxy->getNormPathType() == lev::Proxy::pt_resource)
                ? server::LoadedProxy->getAbsLevelPath()
                : server::LoadedProxy->getNormFilePath();
        debug_log("mp dump: level id=%s path=%s difficult=%d compat=%d",
                  server::LoadedProxy->getId().c_str(),
                  level_path.c_str(),
                  server::IsDifficult ? 1 : 0,
                  static_cast<int>(server::GameCompatibility));
    }

    if (Actor *p0 = sync_reference_actor(0, tick)) {
        const ecl::V2 &pos = p0->get_pos();
        const ecl::V2 &vel = p0->get_vel();
        Value name = p0->getAttr("name");
        const char *name_str = (name.getType() == Value::STRING) ? name.get_string() : "";
        debug_log("mp dump: ref p0 kind=%s obj=%u ctrl=%d name=%s pos=%.2f,%.2f vel=%.2f,%.2f",
                  p0->getKind().c_str(),
                  static_cast<unsigned>(p0->getId()),
                  p0->get_controllers(),
                  name_str,
                  static_cast<double>(pos[0]),
                  static_cast<double>(pos[1]),
                  static_cast<double>(vel[0]),
                  static_cast<double>(vel[1]));
    }
    if (Actor *p1 = sync_reference_actor(1, tick)) {
        const ecl::V2 &pos = p1->get_pos();
        const ecl::V2 &vel = p1->get_vel();
        Value name = p1->getAttr("name");
        const char *name_str = (name.getType() == Value::STRING) ? name.get_string() : "";
        debug_log("mp dump: ref p1 kind=%s obj=%u ctrl=%d name=%s pos=%.2f,%.2f vel=%.2f,%.2f",
                  p1->getKind().c_str(),
                  static_cast<unsigned>(p1->getId()),
                  p1->get_controllers(),
                  name_str,
                  static_cast<double>(pos[0]),
                  static_cast<double>(pos[1]),
                  static_cast<double>(vel[0]),
                  static_cast<double>(vel[1]));
    }

    if (sync) {
        debug_log("mp dump: sync tick=%u rand=%u world=%llu actor=%llu p0=(%.2f,%.2f) p1=(%.2f,%.2f)",
                  static_cast<unsigned>(sync->tick),
                  static_cast<unsigned>(sync->random_state),
                  static_cast<unsigned long long>(sync->world_checksum),
                  static_cast<unsigned long long>(sync->actor_checksum),
                  static_cast<double>(sync->p0_x),
                  static_cast<double>(sync->p0_y),
                  static_cast<double>(sync->p1_x),
                  static_cast<double>(sync->p1_y));
    }

    struct Digest {
        uint32_t kind_id = 0;
        std::string kind;
        uint32_t object_id = 0;
        int owner = -1;
        int controllers = 0;
        int color = -1;
        int64_t x = 0;
        int64_t y = 0;
        int64_t vx = 0;
        int64_t vy = 0;
        std::string name;
    };
    std::vector<Digest> digs;
    std::vector<Actor *> actors;
    GetActors(actors);
    digs.reserve(actors.size());
    for (Actor *a : actors) {
        if (!a)
            continue;
        Digest d;
        d.kind_id = static_cast<uint32_t>(get_id(a));
        d.kind = a->getKind();
        d.object_id = static_cast<uint32_t>(a->getId());
        Value owner = a->getAttr("owner");
        if (owner.getType() != Value::NIL)
            d.owner = static_cast<int>(owner);
        d.controllers = a->get_controllers();
        Value color = a->getAttr("color");
        if (color.getType() != Value::NIL)
            d.color = static_cast<int>(color);
        Value name = a->getAttr("name");
        if (name.getType() == Value::STRING)
            d.name = name.get_string();
        const ecl::V2 &pos = a->get_pos();
        const ecl::V2 &vel = a->get_vel();
        d.x = quantize_for_dump(pos[0]);
        d.y = quantize_for_dump(pos[1]);
        d.vx = quantize_for_dump(vel[0]);
        d.vy = quantize_for_dump(vel[1]);
        digs.push_back(d);
    }
    std::sort(digs.begin(), digs.end(), [](const Digest &a, const Digest &b) {
        if (a.kind_id != b.kind_id)
            return a.kind_id < b.kind_id;
        if (a.owner != b.owner)
            return a.owner < b.owner;
        if (a.controllers != b.controllers)
            return a.controllers < b.controllers;
        if (a.color != b.color)
            return a.color < b.color;
        if (a.x != b.x)
            return a.x < b.x;
        if (a.y != b.y)
            return a.y < b.y;
        if (a.vx != b.vx)
            return a.vx < b.vx;
        if (a.vy != b.vy)
            return a.vy < b.vy;
        if (a.name != b.name)
            return a.name < b.name;
        if (a.kind != b.kind)
            return a.kind < b.kind;
        return a.object_id < b.object_id;
    });

    debug_log("mp dump: actors=%u", static_cast<unsigned>(digs.size()));
    for (const auto &d : digs) {
        debug_log("mp dump: actor kind_id=%u kind=%s obj=%u owner=%d ctrl=%d color=%d name=%s pos=%lld,%lld vel=%lld,%lld",
                  static_cast<unsigned>(d.kind_id),
                  d.kind.c_str(),
                  static_cast<unsigned>(d.object_id),
                  d.owner,
                  d.controllers,
                  d.color,
                  d.name.c_str(),
                  static_cast<long long>(d.x),
                  static_cast<long long>(d.y),
                  static_cast<long long>(d.vx),
                  static_cast<long long>(d.vy));
    }
}

constexpr uint32_t kSyncProbeStrideTicks =
    static_cast<uint32_t>(kSyncInterval / kInputTimestep + 0.5);

Actor *sync_reference_actor(unsigned player, uint32_t tick) {
    // The legacy engine defines "main actor" as the first actor in the player's
    // actor list. For authored multi-ball levels (e.g. meditation pearls) that
    // ordering can differ across peers even when the physical state matches,
    // which would cause spurious position mismatches in sync packets.
    //
    // Pick a deterministic steerable actor controlled by this player instead.
    //
    // IMPORTANT: Do not use position-based ordering. When a player controls
    // multiple marbles that can cross, "top-left" can flip between peers (or
    // across ticks) while still being physically consistent, causing endless
    // false "pos mismatch" reports and soft-resync churn.
    //
    // Also, do not always pick the same controlled actor. If the chosen probe
    // happens to be in a "stable sink" (e.g. trapped in a hole), its position
    // can stay identical while other controlled marbles diverge. Cycle the
    // probe deterministically so all controlled actors eventually get sampled.
    std::vector<Actor *> actors;
    GetActors(actors);
    std::vector<Actor *> candidates;
    std::vector<ActorSortKey> keys;
    for (Actor *a : actors) {
        if (!a || !a->isSteerable())
            continue;
        if (!a->controlled_by(static_cast<int>(player)))
            continue;
        candidates.push_back(a);
        keys.push_back(actor_sort_key(a));
    }

    if (!candidates.empty()) {
        std::vector<size_t> order(candidates.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t ia, size_t ib) {
            return actor_key_less(keys[ia], keys[ib]);
        });
        uint32_t stride = kSyncProbeStrideTicks ? kSyncProbeStrideTicks : 1u;
        size_t idx = static_cast<size_t>((tick / stride) % static_cast<uint32_t>(order.size()));
        return candidates[order[idx]];
    }

    return player::GetMainActor(player);
}

}  // namespace

namespace {

void apply_resync_metadata(Actor *actor, Uint16 owner, Uint32 controllers, Uint16 color) {
    if (!actor)
        return;
    if (owner != 0xFFFF)
        actor->setAttr("owner", Value(static_cast<int>(owner)));

    actor->setAttr("controllers", Value(static_cast<int>(controllers)));

    if (color != 0xFFFF)
        actor->setAttr("color", Value(static_cast<int>(color)));
}

}  // namespace

void send_sync_to_peers() {
    if (!g_session.host || !has_remote_peers())
        return;
    protocol::SyncPacket sync;
    sync.epoch = g_session.input_epoch;
    sync.tick = input::CurrentTick();
    sync.random_state = server::RandomState;
    Actor *p0 = sync_reference_actor(0, sync.tick);
    Actor *p1 = sync_reference_actor(1, sync.tick);
    sync.p0_x = p0 ? static_cast<float>(p0->get_pos()[0]) : 0.0f;
    sync.p0_y = p0 ? static_cast<float>(p0->get_pos()[1]) : 0.0f;
    sync.p1_x = p1 ? static_cast<float>(p1->get_pos()[0]) : 0.0f;
    sync.p1_y = p1 ? static_cast<float>(p1->get_pos()[1]) : 0.0f;
    sync.world_checksum = WorldGridChecksum();
    sync.actor_checksum = ActorChecksum();
    sync.grid_kind_checksum = WorldGridKindChecksum();
    sync.grid_state_checksum = WorldGridStateChecksum();

    ecl::Buffer buf;
    protocol::encode_sync(buf, sync);
    g_transport.HostBroadcast(buf);
}

namespace {
protocol::ResyncState build_resync_state_snapshot();
}  // namespace

void broadcast_resync_state_unreliable() {
    if (!g_session.host || !has_remote_peers())
        return;
    protocol::ResyncState state = build_resync_state_snapshot();
    ecl::Buffer buf;
    protocol::encode_resync_state(buf, state);
    g_transport.HostBroadcastUnreliable(buf);
}

namespace {
void broadcast_world_state_snapshot(bool reliable) {
    if (!g_session.host || !has_remote_peers())
        return;
    const int w = Width();
    const int h = Height();
    if (w <= 0 || h <= 0)
        return;
    protocol::WorldStatePacket pkt;
    pkt.epoch = g_session.input_epoch;
    pkt.tick = input::CurrentTick();
    pkt.width = static_cast<Uint16>(w);
    pkt.height = static_cast<Uint16>(h);
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    pkt.floor_state.assign(count, 0xFFFF);
    pkt.stone_state.assign(count, 0xFFFF);
    pkt.item_state.assign(count, 0xFFFF);

    auto encode_state = [](Object *obj) -> Uint16 {
        if (!obj)
            return 0xFFFF;
        Value v = obj->getAttr("state");
        if (!v)
            return 0;
        int s = static_cast<int>(v);
        if (s < 0)
            s = 0;
        if (s > 0xFFFE)
            s = 0xFFFE;
        return static_cast<Uint16>(s);
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            GridPos p(x, y);
            pkt.floor_state[idx] = encode_state(GetFloor(p));
            pkt.stone_state[idx] = encode_state(GetStone(p));
            pkt.item_state[idx] = encode_state(GetItem(p));
        }
    }

    pkt.movable_stones.clear();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            GridPos p(x, y);
            Stone *st = GetStone(p);
            if (!st)
                continue;
            if (!st->is_movable())
                continue;
            protocol::WorldStatePacket::MovableStone entry;
            entry.object_id = static_cast<Uint32>(st->getId());
            entry.x = static_cast<Uint16>(x);
            entry.y = static_cast<Uint16>(y);
            pkt.movable_stones.push_back(entry);
        }
    }

    ecl::Buffer buf;
    protocol::encode_world_state(buf, pkt);
    if (reliable)
        g_transport.HostBroadcast(buf);
    else
        g_transport.HostBroadcastUnreliable(buf);
}
}  // namespace

void broadcast_world_state_unreliable() {
    broadcast_world_state_snapshot(false);
}

void broadcast_world_state_reliable() {
    broadcast_world_state_snapshot(true);
}

namespace {

void send_resync_request() {
    protocol::ResyncRequest req;
    req.epoch = g_session.input_epoch;
    req.tick = input::CurrentTick();
    debug_log("mp resync request: tick=%u attempt=%u via=%s", req.tick,
              static_cast<unsigned>(g_session.resync_attempts + 1),
              transport_name(g_session.active_transport));
    g_session.telemetry.resync_requests_sent += 1;
    ecl::Buffer buf;
    protocol::encode_resync_request(buf, req);
    g_transport.ClientSend(buf);
}

void send_world_state_request() {
    protocol::WorldStateRequest req;
    req.epoch = g_session.input_epoch;
    req.tick = input::CurrentTick();
    if (debug_enabled())
        debug_log("mp world-state request: tick=%u via=%s", req.tick, transport_name(g_session.active_transport));
    ecl::Buffer buf;
    protocol::encode_world_state_request(buf, req);
    g_transport.ClientSend(buf);
}

protocol::ResyncState build_resync_state_snapshot() {
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
        entry.controllers = static_cast<Uint32>(actor->get_controllers());
        Value color = actor->getAttr("color");
        if (color.getType() != Value::NIL)
            entry.color = static_cast<Uint16>(static_cast<int>(color));
        else
            entry.color = static_cast<Uint16>(0xFFFF);
        entry.name_hash = stable_name_hash(actor);
        entry.x = static_cast<float>(actor->get_pos()[0]);
        entry.y = static_cast<float>(actor->get_pos()[1]);
        entry.vx = static_cast<float>(actor->get_vel()[0]);
        entry.vy = static_cast<float>(actor->get_vel()[1]);
        state.actors.push_back(entry);
    }
    return state;
}

void update_checksum_sample_world(uint32_t tick, uint64_t world_checksum, uint64_t grid_kind_checksum,
                                  uint64_t grid_state_checksum) {
    for (auto &entry : g_session.checksum_history) {
        if (entry.tick == tick) {
            entry.world_checksum = world_checksum;
            entry.grid_kind_checksum = grid_kind_checksum;
            entry.grid_state_checksum = grid_state_checksum;
            entry.world_valid = true;
            break;
        }
    }
}

}  // namespace

void send_resync_state(ENetPeer *peer) {
    if (!peer)
        return;
    dump_actor_digest_once("host_send_resync_state_direct", input::CurrentTick(), nullptr);
    protocol::ResyncState state = build_resync_state_snapshot();
    ecl::Buffer buf;
    protocol::encode_resync_state(buf, state);
    g_transport.HostSendDirect(peer, buf);
}

void send_resync_state_to_relay(Uint32 client_id) {
    if (!g_session.relay_peer)
        return;
    dump_actor_digest_once("host_send_resync_state_udp_relay", input::CurrentTick(), nullptr);
    protocol::ResyncState state = build_resync_state_snapshot();
    ecl::Buffer buf;
    protocol::encode_resync_state(buf, state);
    g_transport.HostSendUdpRelay(client_id, buf);
}

void send_resync_state_to_tcp_relay(Uint32 client_id) {
    if (!tcp_socket_valid(g_session.tcp_relay_socket))
        return;
    dump_actor_digest_once("host_send_resync_state_tcp_relay", input::CurrentTick(), nullptr);
    protocol::ResyncState state = build_resync_state_snapshot();
    ecl::Buffer buf;
    protocol::encode_resync_state(buf, state);
    g_transport.HostSendTcpRelay(client_id, buf);
}

void apply_resync_state(const protocol::ResyncState &state) {
    if (!g_session.active)
        return;
    if (state.epoch != g_session.input_epoch)
        return;
    g_session.telemetry.resync_responses_recv += 1;
    uint32_t local_tick = input::CurrentTick();
    uint32_t delta_ticks = (local_tick > state.tick) ? (local_tick - state.tick) : 0;
    // Resync snapshots can arrive a few ticks late, but velocity projection is
    // unreliable in high-acceleration physics (rubberbands, collisions) and
    // tends to amplify post-resync divergence. Apply snapshots as-is.
    float dt = 0.0f;
    if (debug_enabled())
        debug_log("mp resync recv: state tick=%u local tick=%u delta=%u actors=%u", state.tick,
                  local_tick, delta_ticks, static_cast<unsigned>(state.actors.size()));
    server::RandomState = state.random_state;

    std::unordered_map<Uint32, Actor *> by_object_id;
    std::vector<Actor *> actors;
    GetActors(actors);
    by_object_id.reserve(actors.size());
    for (Actor *actor : actors)
        by_object_id[static_cast<Uint32>(actor->getId())] = actor;
    std::unordered_map<Actor *, bool> used;
    used.reserve(actors.size());

    // Applying a resync snapshot must not trigger gameplay side-effects like
    // floor/item enter/leave callbacks (which can diverge further across peers).
    // We only update physics state and the spatial index via DidMoveActor().
    auto resync_teleport = [](Actor *actor, float x, float y, float vx, float vy) {
        ActorInfo *ai = actor->get_actorinfo();
        ai->pos = ecl::V2(x, y);
        DidMoveActor(actor);
        ai->last_gridpos = ai->gridpos;
        ai->vel = ecl::V2(vx, vy);
        ai->pos_force = ai->pos;
        ai->forceacc = ecl::V2();
        ai->force = ecl::V2();
        ai->collforce = ecl::V2();
        ai->friction = 0.0;
        ai->contacts = ai->contacts_a;
        ai->last_contacts = ai->contacts_b;
        ai->contacts_count = 0;
        ai->last_contacts_count = 0;
    };

    auto should_skip_actor = [](Actor *actor) -> bool {
        if (!actor)
            return false;
        if (!options::GetBool("MultiplayerDebugSkipLocalResync"))
            return false;
        if (!g_session.local_player_known)
            return false;
        if (g_session.local_player >= g_session.expected_players)
            return false;
        return actor->controlled_by(static_cast<int>(g_session.local_player));
    };

    unsigned applied = 0;
    unsigned object_id_matches = 0;
    float max_pos_delta = 0.0f;
    // First, apply by object_id when possible (best fidelity).
    for (const auto &entry : state.actors) {
        auto it = by_object_id.find(entry.object_id);
        if (it == by_object_id.end())
            continue;
        Actor *actor = it->second;
        if (!actor || used[actor])
            continue;
        float x = entry.x + entry.vx * dt;
        float y = entry.y + entry.vy * dt;
        ActorInfo *ai = actor->get_actorinfo();
        float dx = static_cast<float>(ai->pos[0]) - x;
        float dy = static_cast<float>(ai->pos[1]) - y;
        float d = std::sqrt(dx * dx + dy * dy);
        if (d > max_pos_delta)
            max_pos_delta = d;
        if (!should_skip_actor(actor))
            resync_teleport(actor, x, y, entry.vx, entry.vy);
        apply_resync_metadata(actor, entry.owner, entry.controllers, entry.color);
        used[actor] = true;
        applied += 1;
        object_id_matches += 1;
    }

    // For the remaining entries, match by (kind, owner, position) instead of
    // object_id. This avoids fragile resync mapping in levels with multiple
    // identical marbles (e.g. meditation) where object ids can diverge across
    // peers due to non-deterministic scripting/creation order.
    std::vector<uint8_t> snap_used(state.actors.size(), 0);
    std::vector<size_t> remaining_snaps;
    remaining_snaps.reserve(state.actors.size());
    for (size_t i = 0; i < state.actors.size(); ++i) {
        const auto &entry = state.actors[i];
        auto it = by_object_id.find(entry.object_id);
        if (it != by_object_id.end()) {
            Actor *actor = it->second;
            if (actor && used[actor]) {
                snap_used[i] = 1;
                continue;
            }
        }
        remaining_snaps.push_back(i);
    }

    // Pass 0: match by stable name hash when available (best effort for scripted
    // multi-ball levels where object ids can differ across peers).
    if (!remaining_snaps.empty()) {
        std::unordered_map<uint64_t, std::vector<Actor *>> local_by_name;
        local_by_name.reserve(actors.size());
        for (Actor *a : actors) {
            if (!a || used[a])
                continue;
            uint32_t nh = stable_name_hash(a);
            if (!nh)
                continue;
            uint16_t kind = static_cast<uint16_t>(get_id(a));
            uint64_t key = (static_cast<uint64_t>(kind) << 32) | static_cast<uint64_t>(nh);
            local_by_name[key].push_back(a);
        }

        for (size_t idx : remaining_snaps) {
            if (snap_used[idx])
                continue;
            const auto &s = state.actors[idx];
            if (!s.name_hash)
                continue;
            uint64_t key = (static_cast<uint64_t>(s.actor_id) << 32) | static_cast<uint64_t>(s.name_hash);
            auto it = local_by_name.find(key);
            if (it == local_by_name.end() || it->second.empty())
                continue;

            // Choose the closest remaining actor to the snapshot position.
            Actor *best = nullptr;
            double best_d2 = 0.0;
            for (Actor *cand : it->second) {
                if (!cand || used[cand])
                    continue;
                const ecl::V2 &p = cand->get_pos();
                double dx = static_cast<double>(p[0]) - static_cast<double>(s.x);
                double dy = static_cast<double>(p[1]) - static_cast<double>(s.y);
                double d2 = dx * dx + dy * dy;
                if (!best || d2 < best_d2) {
                    best = cand;
                    best_d2 = d2;
                }
            }
            if (!best)
                continue;
            float x = s.x + s.vx * dt;
            float y = s.y + s.vy * dt;
            ActorInfo *ai = best->get_actorinfo();
            float dx = static_cast<float>(ai->pos[0]) - x;
            float dy = static_cast<float>(ai->pos[1]) - y;
            float d = std::sqrt(dx * dx + dy * dy);
            if (d > max_pos_delta)
                max_pos_delta = d;
            if (!should_skip_actor(best))
                resync_teleport(best, x, y, s.vx, s.vy);
            apply_resync_metadata(best, s.owner, s.controllers, s.color);
            used[best] = true;
            snap_used[idx] = 1;
            applied += 1;
        }

        // Rebuild remaining_snaps to exclude those already matched by name.
        std::vector<size_t> tmp;
        tmp.reserve(remaining_snaps.size());
        for (size_t idx : remaining_snaps) {
            if (!snap_used[idx])
                tmp.push_back(idx);
        }
        remaining_snaps.swap(tmp);
    }

    auto local_owner = [](Actor *a) -> int {
        Value owner = a->getAttr("owner");
        if (owner.getType() != Value::NIL)
            return static_cast<int>(owner);
        return -1;
    };

    auto match_groups = [&](bool include_owner) -> unsigned {
        std::unordered_map<uint32_t, std::vector<size_t>> snap_groups;
        std::unordered_map<uint32_t, std::vector<Actor *>> local_groups;
        snap_groups.reserve(remaining_snaps.size());
        local_groups.reserve(actors.size());

        auto make_key = [&](Uint16 kind, int owner) -> uint32_t {
            if (!include_owner)
                return static_cast<uint32_t>(kind);
            uint32_t o = static_cast<uint32_t>(owner + 1);
            if (o > 0xFFFFu)
                o = 0xFFFFu;
            return (static_cast<uint32_t>(kind) << 16) | o;
        };

        for (size_t idx : remaining_snaps) {
            if (snap_used[idx])
                continue;
            const auto &s = state.actors[idx];
            int owner = (s.owner == 0xFFFF) ? -1 : static_cast<int>(s.owner);
            snap_groups[make_key(s.actor_id, owner)].push_back(idx);
        }
        for (Actor *a : actors) {
            if (!a || used[a])
                continue;
            int owner = local_owner(a);
            uint16_t kind = static_cast<uint16_t>(get_id(a));
            local_groups[make_key(kind, owner)].push_back(a);
        }

        auto sort_snap = [&](size_t ai, size_t bi) {
            const auto &a = state.actors[ai];
            const auto &b = state.actors[bi];
            if (a.x != b.x)
                return a.x < b.x;
            return a.y < b.y;
        };
        auto sort_local = [&](const Actor *a, const Actor *b) {
            const ecl::V2 &pa = a->get_pos();
            const ecl::V2 &pb = b->get_pos();
            if (pa[0] != pb[0])
                return pa[0] < pb[0];
            return pa[1] < pb[1];
        };

        unsigned matched = 0;
        for (auto &kv : snap_groups) {
            auto it_local = local_groups.find(kv.first);
            if (it_local == local_groups.end())
                continue;
            auto &snap_list = kv.second;
            auto &local_list = it_local->second;
            if (snap_list.empty() || local_list.empty())
                continue;
            std::sort(snap_list.begin(), snap_list.end(), sort_snap);
            std::sort(local_list.begin(), local_list.end(), sort_local);
            size_t n = std::min(snap_list.size(), local_list.size());
            for (size_t i = 0; i < n; ++i) {
                size_t snap_idx = snap_list[i];
                Actor *actor = local_list[i];
                if (!actor || used[actor] || snap_used[snap_idx])
                    continue;
                const auto &s = state.actors[snap_idx];
                float x = s.x + s.vx * dt;
                float y = s.y + s.vy * dt;
                ActorInfo *ai = actor->get_actorinfo();
                float dx = static_cast<float>(ai->pos[0]) - x;
                float dy = static_cast<float>(ai->pos[1]) - y;
                float d = std::sqrt(dx * dx + dy * dy);
                if (d > max_pos_delta)
                    max_pos_delta = d;
                if (!should_skip_actor(actor))
                    resync_teleport(actor, x, y, s.vx, s.vy);
                apply_resync_metadata(actor, s.owner, s.controllers, s.color);
                used[actor] = true;
                snap_used[snap_idx] = 1;
                applied += 1;
                matched += 1;
            }
        }
        return matched;
    };

    unsigned matched_by_group = 0;
    // Pass 1: match by (kind, owner) when possible.
    matched_by_group += match_groups(true);
    // Pass 2: match by kind only for remaining (ownerless / ambiguous) actors.
    // Rebuild remaining_snaps to exclude those already matched.
    if (!remaining_snaps.empty()) {
        std::vector<size_t> tmp;
        tmp.reserve(remaining_snaps.size());
        for (size_t idx : remaining_snaps) {
            if (!snap_used[idx])
                tmp.push_back(idx);
        }
        remaining_snaps.swap(tmp);
    }
    if (!remaining_snaps.empty())
        matched_by_group += match_groups(false);
    // Rubberband "violation" flags can diverge across peers and then keep
    // applying different forces even after actor positions are snapped. After
    // teleporting, recompute these flags from the current anchor positions to
    // reduce persistent post-resync drift in meditation levels.
    std::vector<Rubberband *> rubbers;
    GetRubberbands(rubbers);
    for (Rubberband *rb : rubbers) {
        if (!rb)
            continue;
        SendMessage(rb, "_mp_resync_flags");
    }
    if (debug_enabled())
        debug_log("mp resync applied: actors=%u (obj_id=%u group=%u) max_pos_delta=%.3f",
                  applied, object_id_matches, matched_by_group,
                  static_cast<double>(max_pos_delta));
    g_session.telemetry.resync_applied += 1;

    // Mark the resync response as received. Do NOT reset attempts here: if the
    // resync does not actually fix the divergence, resetting attempts would
    // cause endless "jumpy" re-resync loops and prevent reaching the "failed
    // resync" user-facing warning.
    g_session.resync_inflight = false;
    g_session.resync_inflight_timer = 0.0;
    g_session.resync_cooldown = kResyncCooldown;
    g_session.desync_streak = 0;
    record_checksum_sample();
}

void send_ready_to_host() {
    debug_log("mp send ready via=%s session=%u epoch=%u load=%u",
              transport_name(g_session.active_transport),
              static_cast<unsigned>(g_session.session_id),
              static_cast<unsigned>(g_session.input_epoch),
              static_cast<unsigned>(g_session.last_load_id));
    ecl::Buffer buf;
    protocol::encode_ready(buf, g_session.session_id, g_session.input_epoch, g_session.last_load_id);
    g_transport.ClientSend(buf);
}

void send_pause_to_host(bool paused) {
    debug_log("mp send pause=%d via=%s", paused ? 1 : 0, transport_name(g_session.active_transport));
    ecl::Buffer buf;
    protocol::encode_pause(buf, g_session.input_epoch, paused);
    g_transport.ClientSend(buf);
}

void send_menu_to_host(bool open) {
    ecl::Buffer buf;
    protocol::encode_menu(buf, g_session.input_epoch, static_cast<Uint8>(g_session.local_player), open);
    g_transport.ClientSend(buf);
}

void send_abort_to_host() {
    ecl::Buffer buf;
    protocol::encode_abort(buf, g_session.input_epoch);
    g_transport.ClientSend(buf);
    g_transport.Flush();
}

void send_start_to_peers() {
    ecl::Buffer buf;
    protocol::encode_start(buf, g_session.input_epoch, g_session.load_id);
    g_transport.HostBroadcast(buf);
    g_transport.Flush();
}

void send_pause_to_peers(bool paused) {
    ecl::Buffer buf;
    protocol::encode_pause(buf, g_session.input_epoch, paused);
    g_transport.HostBroadcast(buf);
    g_transport.Flush();
}

void send_restart_to_peers(bool level_restart) {
    protocol::RestartPacket msg;
    msg.restart_id = g_session.restart_id;
    msg.level_restart = level_restart ? 1 : 0;
    ecl::Buffer buf;
    protocol::encode_restart(buf, msg);
    g_transport.HostBroadcast(buf);
    g_transport.Flush();
}

void send_load_level_to_peers(const std::string &pack_name, const std::string &level_id) {
    protocol::LoadLevelPacket msg;
    msg.load_id = g_session.load_id;
    msg.pack_name = pack_name;
    msg.level_id = level_id;
    ecl::Buffer buf;
    protocol::encode_load_level(buf, msg);
    g_transport.HostBroadcast(buf);
    g_transport.Flush();
}

void send_abort_to_peers() {
    ecl::Buffer buf;
    protocol::encode_abort(buf, g_session.input_epoch);
    g_transport.HostBroadcast(buf);
    g_transport.Flush();
}

bool host_ready_to_start() {
    if (!g_session.host)
        return false;
    if (g_session.expected_players <= 1)
        return true;
    if (g_session.peer_players.size() + g_session.relay_players.size() +
            g_session.tcp_relay_players.size() + 1 <
        g_session.expected_players) {
        return false;
    }
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
    for (const auto &entry : g_session.tcp_relay_players) {
        auto it = g_session.tcp_relay_ready.find(entry.first);
        if (it == g_session.tcp_relay_ready.end() || !it->second)
            return false;
    }
    if (g_session.expected_players > g_session.level_players && g_session.level_players > 0) {
        if (g_session.placement_received.size() < g_session.expected_players)
            return false;
        for (unsigned player = g_session.level_players; player < g_session.expected_players;
             ++player) {
            if (!g_session.placement_received[player])
                return false;
        }
    }
    return true;
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

void handle_sync_current(const protocol::SyncPacket &sync) {
    if (g_session.abort_pending)
        return;
    if (sync.epoch != g_session.input_epoch) {
        debug_log("mp sync skip: epoch=%u local epoch=%u", sync.epoch, g_session.input_epoch);
        return;
    }
    g_session.telemetry.sync_current_total += 1;
    uint32_t local_tick = input::CurrentTick();
    if (sync.tick != local_tick) {
        debug_log("mp sync skip: sync tick=%u local tick=%u", sync.tick, local_tick);
        return;
    }
    uint64_t local_checksum = WorldGridChecksum();
    uint64_t local_kind_checksum = WorldGridKindChecksum();
    uint64_t local_state_checksum = WorldGridStateChecksum();
    update_checksum_sample_world(local_tick, local_checksum, local_kind_checksum, local_state_checksum);
    g_session.last_world_checksum = local_checksum;
    bool checksum_mismatch =
        (sync.world_checksum != 0 && sync.world_checksum != local_checksum);
    if (checksum_mismatch) {
        debug_log("mp checksum mismatch: tick=%u local=%llu remote=%llu", sync.tick,
                  static_cast<unsigned long long>(local_checksum),
                  static_cast<unsigned long long>(sync.world_checksum));
    }
    bool kind_mismatch =
        (sync.grid_kind_checksum != 0 && sync.grid_kind_checksum != local_kind_checksum);
    bool state_mismatch =
        (sync.grid_state_checksum != 0 && sync.grid_state_checksum != local_state_checksum);
    if (kind_mismatch || state_mismatch) {
        debug_log("mp grid mismatch: tick=%u kind(local=%llu remote=%llu) state(local=%llu remote=%llu)",
                  sync.tick,
                  static_cast<unsigned long long>(local_kind_checksum),
                  static_cast<unsigned long long>(sync.grid_kind_checksum),
                  static_cast<unsigned long long>(local_state_checksum),
                  static_cast<unsigned long long>(sync.grid_state_checksum));
    }
    uint64_t local_actor_checksum = ActorChecksum();
    bool actor_mismatch =
        (sync.actor_checksum != 0 && sync.actor_checksum != local_actor_checksum);
    if (actor_mismatch) {
        debug_log("mp actor mismatch: tick=%u local=%llu remote=%llu", sync.tick,
                  static_cast<unsigned long long>(local_actor_checksum),
                  static_cast<unsigned long long>(sync.actor_checksum));
    }
    Actor *p0 = sync_reference_actor(0, sync.tick);
    Actor *p1 = sync_reference_actor(1, sync.tick);
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
    if (pos_mismatch)
        g_session.telemetry.mismatch_pos += 1;
    if (rand_mismatch)
        g_session.telemetry.mismatch_rand += 1;
    if (actor_mismatch)
        g_session.telemetry.mismatch_actor += 1;
    if (checksum_mismatch)
        g_session.telemetry.mismatch_world += 1;

    const bool rng_only =
        (!pos_mismatch && rand_mismatch && !checksum_mismatch && !actor_mismatch);
    const bool diagnostic_only =
        (!pos_mismatch && !rand_mismatch && (checksum_mismatch || actor_mismatch));
    const bool soft_resync_candidate = (pos_mismatch || rand_mismatch);
    const bool actor_resync_candidate =
        (!pos_mismatch && !rand_mismatch && actor_mismatch && !checksum_mismatch);
    const bool world_only_mismatch =
        (!pos_mismatch && !rand_mismatch && checksum_mismatch && !actor_mismatch);
    if (pos_mismatch || rand_mismatch || actor_mismatch || checksum_mismatch) {
        if (rng_only)
            g_session.telemetry.mismatch_rng_only += 1;
        if (diagnostic_only)
            g_session.telemetry.mismatch_diagnostic_only += 1;
        if (soft_resync_candidate)
            g_session.telemetry.mismatch_soft_resync_candidate += 1;

        const char *action =
            rng_only ? "rng-fix"
                     : soft_resync_candidate ? "soft-resync"
                                             : actor_resync_candidate ? "actor-resync"
                                             : diagnostic_only ? "diagnostic-only"
                                                               : "none";
        debug_log("mp desync: tick=%u rand local=%u remote=%u "
                  "flags(pos=%d rand=%d actor=%d world=%d) action=%s "
                  "p0(%.2f,%.2f)->(%.2f,%.2f) p1(%.2f,%.2f)->(%.2f,%.2f)",
                  sync.tick, static_cast<unsigned>(server::RandomState),
                  static_cast<unsigned>(sync.random_state),
                  pos_mismatch ? 1 : 0, rand_mismatch ? 1 : 0, actor_mismatch ? 1 : 0,
                  checksum_mismatch ? 1 : 0, action,
                  p0_x, p0_y, sync.p0_x, sync.p0_y,
                  p1_x, p1_y, sync.p1_x, sync.p1_y);
        if (actor_mismatch || checksum_mismatch)
            dump_actor_digest_once("sync_current_mismatch", sync.tick, &sync);
        if (rng_only) {
            server::RandomState = sync.random_state;
            g_session.telemetry.rng_resync_applied += 1;
            debug_log("mp rng resynced to %u", static_cast<unsigned>(sync.random_state));
            return;
        }
    }

    // Actor/world checksums can diverge due to harmless platform floating-point
    // drift in physics-heavy levels. Drive recovery from position/RNG mismatch,
    // and keep checksums for diagnostics only.
    if (checksum_mismatch || kind_mismatch || state_mismatch) {
        g_session.world_only_desync_streak += 1;
        if (!g_session.host && g_session.world_state_cooldown <= 0.0) {
            send_world_state_request();
            g_session.world_state_cooldown = kResyncCooldown;
        }
        if (g_session.world_only_desync_streak >= 3 && !g_session.desync_reported) {
            g_session.desync_reported = true;
            client::Msg_ShowText("World desync detected. Syncing from host...", true, 4.0);
        }
    } else {
        g_session.world_only_desync_streak = 0;
    }
    if (soft_resync_candidate) {
        g_session.actor_desync_streak = 0;
        g_session.desync_streak += 1;
        if (g_session.desync_streak < kDesyncStreakForResync)
            return;
        if (g_session.resync_attempts < kResyncMaxAttempts && !g_session.resync_inflight &&
            g_session.resync_cooldown <= 0.0) {
            send_resync_request();
            g_session.resync_inflight = true;
            g_session.resync_inflight_timer = 0.0;
            g_session.resync_attempts += 1;
            g_session.resync_cooldown = kResyncCooldown;
        }
        if (g_session.resync_attempts >= kResyncMaxAttempts) {
            // Resync did not quickly converge. Keep running without prompting a restart:
            // most of these cases are physics drift that can self-heal or improve over time.
            // Throttle further requests instead of permanently giving up.
            g_session.telemetry.resync_giveups += 1;
            g_session.resync_attempts = 0;
            g_session.resync_inflight = false;
            g_session.resync_inflight_timer = 0.0;
            g_session.resync_cooldown = std::max(g_session.resync_cooldown, 3.0);
        }
    } else if (actor_resync_candidate) {
        g_session.desync_streak = 0;
        g_session.actor_desync_streak += 1;
        if (g_session.actor_desync_streak < kActorDesyncStreakForResync)
            return;
        if (g_session.resync_attempts < kResyncMaxAttempts && !g_session.resync_inflight &&
            g_session.resync_cooldown <= 0.0) {
            send_resync_request();
            g_session.resync_inflight = true;
            g_session.resync_inflight_timer = 0.0;
            g_session.resync_attempts += 1;
            g_session.resync_cooldown = kResyncCooldown;
        }
        if (g_session.resync_attempts >= kResyncMaxAttempts) {
            g_session.telemetry.resync_giveups += 1;
            g_session.resync_attempts = 0;
            g_session.resync_inflight = false;
            g_session.resync_inflight_timer = 0.0;
            g_session.resync_cooldown = std::max(g_session.resync_cooldown, 3.0);
        }
    } else {
        g_session.resync_attempts = 0;
        g_session.resync_inflight = false;
        g_session.resync_inflight_timer = 0.0;
        g_session.desync_streak = 0;
        g_session.actor_desync_streak = 0;
        g_session.world_only_desync_streak = 0;
    }
}

void handle_sync_sample(const protocol::SyncPacket &sync,
                        const SessionState::ChecksumSample &sample) {
    if (g_session.abort_pending)
        return;
    if (sync.epoch != g_session.input_epoch) {
        debug_log("mp sync skip: epoch=%u local epoch=%u", sync.epoch, g_session.input_epoch);
        return;
    }
    g_session.telemetry.sync_sample_total += 1;
    bool checksum_mismatch = false;
    if (sample.world_valid && sync.world_checksum != 0)
        checksum_mismatch = sync.world_checksum != sample.world_checksum;
    bool kind_mismatch = false;
    bool state_mismatch = false;
    if (sample.world_valid) {
        if (sync.grid_kind_checksum != 0)
            kind_mismatch = sync.grid_kind_checksum != sample.grid_kind_checksum;
        if (sync.grid_state_checksum != 0)
            state_mismatch = sync.grid_state_checksum != sample.grid_state_checksum;
    }
    bool actor_mismatch = (sync.actor_checksum != 0 && sync.actor_checksum != sample.actor_checksum);
    bool rand_mismatch = sync.random_state != sample.random_state;
    auto diff = [](float a, float b) { return fabs(a - b); };
    bool pos_mismatch = diff(sync.p0_x, sample.p0_x) > kSyncPosEpsilon ||
                        diff(sync.p0_y, sample.p0_y) > kSyncPosEpsilon ||
                        diff(sync.p1_x, sample.p1_x) > kSyncPosEpsilon ||
                        diff(sync.p1_y, sample.p1_y) > kSyncPosEpsilon;
    if (pos_mismatch)
        g_session.telemetry.mismatch_pos += 1;
    if (rand_mismatch)
        g_session.telemetry.mismatch_rand += 1;
    if (actor_mismatch)
        g_session.telemetry.mismatch_actor += 1;
    if (checksum_mismatch)
        g_session.telemetry.mismatch_world += 1;

    const bool rng_only =
        (!pos_mismatch && rand_mismatch && !checksum_mismatch && !actor_mismatch);
    const bool diagnostic_only =
        (!pos_mismatch && !rand_mismatch && (checksum_mismatch || actor_mismatch));
    const bool soft_resync_candidate = (pos_mismatch || rand_mismatch);
    const bool actor_resync_candidate =
        (!pos_mismatch && !rand_mismatch && actor_mismatch && !checksum_mismatch);
    const bool world_only_mismatch =
        (!pos_mismatch && !rand_mismatch && checksum_mismatch && !actor_mismatch);

    if (checksum_mismatch) {
        debug_log("mp checksum mismatch: tick=%u local=%llu remote=%llu",
                  sync.tick,
                  static_cast<unsigned long long>(sample.world_checksum),
                  static_cast<unsigned long long>(sync.world_checksum));
    }
    if (kind_mismatch || state_mismatch) {
        debug_log("mp grid mismatch (late): tick=%u kind(local=%llu remote=%llu) state(local=%llu remote=%llu)",
                  sync.tick,
                  static_cast<unsigned long long>(sample.grid_kind_checksum),
                  static_cast<unsigned long long>(sync.grid_kind_checksum),
                  static_cast<unsigned long long>(sample.grid_state_checksum),
                  static_cast<unsigned long long>(sync.grid_state_checksum));
    }
    if (actor_mismatch) {
        debug_log("mp actor mismatch: tick=%u local=%llu remote=%llu",
                  sync.tick,
                  static_cast<unsigned long long>(sample.actor_checksum),
                  static_cast<unsigned long long>(sync.actor_checksum));
    }
    if (pos_mismatch || rand_mismatch || actor_mismatch || checksum_mismatch) {
        if (rng_only)
            g_session.telemetry.mismatch_rng_only += 1;
        if (diagnostic_only)
            g_session.telemetry.mismatch_diagnostic_only += 1;
        if (soft_resync_candidate)
            g_session.telemetry.mismatch_soft_resync_candidate += 1;

        const char *action =
            rng_only ? "rng-fix"
                     : soft_resync_candidate ? "soft-resync"
                                             : actor_resync_candidate ? "actor-resync"
                                             : diagnostic_only ? "diagnostic-only"
                                                               : "none";
        debug_log("mp desync (late): tick=%u rand local=%u remote=%u "
                  "flags(pos=%d rand=%d actor=%d world=%d) action=%s "
                  "p0(%.2f,%.2f)->(%.2f,%.2f) p1(%.2f,%.2f)->(%.2f,%.2f)",
                  sync.tick,
                  static_cast<unsigned>(sample.random_state),
                  static_cast<unsigned>(sync.random_state),
                  pos_mismatch ? 1 : 0, rand_mismatch ? 1 : 0, actor_mismatch ? 1 : 0,
                  checksum_mismatch ? 1 : 0, action,
                  sample.p0_x, sample.p0_y, sync.p0_x, sync.p0_y,
                  sample.p1_x, sample.p1_y, sync.p1_x, sync.p1_y);
        if (actor_mismatch || checksum_mismatch)
            dump_actor_digest_once("sync_sample_mismatch", sync.tick, &sync);
    }

    // Actor checksums are useful diagnostics but too sensitive to drive recovery
    // on their own, especially in physics-heavy scenes.
    if (checksum_mismatch || kind_mismatch || state_mismatch) {
        g_session.world_only_desync_streak += 1;
        if (!g_session.host && g_session.world_state_cooldown <= 0.0) {
            send_world_state_request();
            g_session.world_state_cooldown = kResyncCooldown;
        }
        if (g_session.world_only_desync_streak >= 3 && !g_session.desync_reported) {
            g_session.desync_reported = true;
            client::Msg_ShowText("World desync detected. Syncing from host...", true, 4.0);
        }
    } else {
        g_session.world_only_desync_streak = 0;
    }
    if (soft_resync_candidate) {
        g_session.actor_desync_streak = 0;
        g_session.desync_streak += 1;
        if (g_session.desync_streak < kDesyncStreakForResync)
            return;
        if (g_session.resync_attempts < kResyncMaxAttempts && !g_session.resync_inflight &&
            g_session.resync_cooldown <= 0.0) {
            send_resync_request();
            g_session.resync_inflight = true;
            g_session.resync_inflight_timer = 0.0;
            g_session.resync_attempts += 1;
            g_session.resync_cooldown = kResyncCooldown;
        }
        if (g_session.resync_attempts >= kResyncMaxAttempts) {
            g_session.telemetry.resync_giveups += 1;
            g_session.resync_attempts = 0;
            g_session.resync_inflight = false;
            g_session.resync_inflight_timer = 0.0;
            g_session.resync_cooldown = std::max(g_session.resync_cooldown, 3.0);
        }
    } else if (actor_resync_candidate) {
        g_session.desync_streak = 0;
        g_session.actor_desync_streak += 1;
        if (g_session.actor_desync_streak < kActorDesyncStreakForResync)
            return;
        if (g_session.resync_attempts < kResyncMaxAttempts && !g_session.resync_inflight &&
            g_session.resync_cooldown <= 0.0) {
            send_resync_request();
            g_session.resync_inflight = true;
            g_session.resync_inflight_timer = 0.0;
            g_session.resync_attempts += 1;
            g_session.resync_cooldown = kResyncCooldown;
        }
        if (g_session.resync_attempts >= kResyncMaxAttempts) {
            g_session.telemetry.resync_giveups += 1;
            g_session.resync_attempts = 0;
            g_session.resync_inflight = false;
            g_session.resync_inflight_timer = 0.0;
            g_session.resync_cooldown = std::max(g_session.resync_cooldown, 3.0);
        }
    } else {
        g_session.resync_attempts = 0;
        g_session.resync_inflight = false;
        g_session.resync_inflight_timer = 0.0;
        g_session.desync_streak = 0;
        g_session.actor_desync_streak = 0;
        g_session.world_only_desync_streak = 0;
    }
}

void record_checksum_sample() {
    uint32_t tick = input::CurrentTick();
    g_session.last_checksum_tick = tick;
    SessionState::ChecksumSample sample;
    sample.tick = tick;
    sample.actor_checksum = ActorChecksum();
    sample.world_checksum = 0;
    sample.grid_kind_checksum = 0;
    sample.grid_state_checksum = 0;
    sample.world_valid = false;
    sample.random_state = server::RandomState;
    Actor *p0 = sync_reference_actor(0, tick);
    Actor *p1 = sync_reference_actor(1, tick);
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

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
