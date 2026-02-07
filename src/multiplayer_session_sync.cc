#include "multiplayer_session_impl.hh"

#include "multiplayer_transport.hh"

#include "client.hh"
#include "enigma.hh"
#include "input.hh"
#include "player.hh"
#include "server.hh"
#include "world.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

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
    g_transport.HostBroadcast(buf);
}

namespace {

void send_resync_request() {
    protocol::ResyncRequest req;
    req.epoch = g_session.input_epoch;
    req.tick = input::CurrentTick();
    ecl::Buffer buf;
    protocol::encode_resync_request(buf, req);
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
        entry.x = static_cast<float>(actor->get_pos()[0]);
        entry.y = static_cast<float>(actor->get_pos()[1]);
        entry.vx = static_cast<float>(actor->get_vel()[0]);
        entry.vy = static_cast<float>(actor->get_vel()[1]);
        state.actors.push_back(entry);
    }
    return state;
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

}  // namespace

void send_resync_state(ENetPeer *peer) {
    if (!peer)
        return;
    protocol::ResyncState state = build_resync_state_snapshot();
    ecl::Buffer buf;
    protocol::encode_resync_state(buf, state);
    g_transport.HostSendDirect(peer, buf);
}

void send_resync_state_to_relay(Uint32 client_id) {
    if (!g_session.relay_peer)
        return;
    protocol::ResyncState state = build_resync_state_snapshot();
    ecl::Buffer buf;
    protocol::encode_resync_state(buf, state);
    g_transport.HostSendUdpRelay(client_id, buf);
}

void send_resync_state_to_tcp_relay(Uint32 client_id) {
    if (!tcp_socket_valid(g_session.tcp_relay_socket))
        return;
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
    server::RandomState = state.random_state;

    std::unordered_map<Uint32, Actor *> by_id;
    std::vector<Actor *> actors;
    GetActors(actors);
    by_id.reserve(actors.size());
    for (Actor *actor : actors)
        by_id[static_cast<Uint32>(actor->getId())] = actor;

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

    unsigned applied = 0;
    float max_pos_delta = 0.0f;
    for (const auto &entry : state.actors) {
        Actor *actor = nullptr;
        auto it = by_id.find(entry.object_id);
        if (it != by_id.end()) {
            actor = it->second;
        } else {
            int desired_owner = (entry.owner == 0xFFFF) ? -1 : static_cast<int>(entry.owner);
            for (Actor *candidate : actors) {
                if (get_id(candidate) == entry.actor_id) {
                    Value owner = candidate->getAttr("owner");
                    int candidate_owner = (owner.getType() != Value::NIL) ? static_cast<int>(owner)
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
        ActorInfo *ai = actor->get_actorinfo();
        float dx = static_cast<float>(ai->pos[0]) - entry.x;
        float dy = static_cast<float>(ai->pos[1]) - entry.y;
        float d = std::sqrt(dx * dx + dy * dy);
        if (d > max_pos_delta)
            max_pos_delta = d;
        resync_teleport(actor, entry.x, entry.y, entry.vx, entry.vy);
        applied += 1;
    }
    if (debug_enabled())
        debug_log("mp resync applied: actors=%u max_pos_delta=%.3f", applied,
                  static_cast<double>(max_pos_delta));

    g_session.resync_inflight = false;
    g_session.resync_cooldown = 0.0;
    g_session.resync_attempts = 0;
    g_session.desync_reported = false;
    record_checksum_sample();
}

void send_ready_to_host() {
    debug_log("mp send ready via=%s", transport_name(g_session.active_transport));
    ecl::Buffer buf;
    protocol::encode_ready(buf);
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
}

void send_start_to_peers() {
    ecl::Buffer buf;
    protocol::encode_start(buf, g_session.input_epoch);
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
    if (sync.epoch != g_session.input_epoch) {
        debug_log("mp sync skip: epoch=%u local epoch=%u", sync.epoch, g_session.input_epoch);
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
    bool checksum_mismatch =
        (sync.world_checksum != 0 && sync.world_checksum != local_checksum);
    if (checksum_mismatch) {
        debug_log("mp checksum mismatch: tick=%u local=%llu remote=%llu", sync.tick,
                  static_cast<unsigned long long>(local_checksum),
                  static_cast<unsigned long long>(sync.world_checksum));
    }
    uint64_t local_actor_checksum = ActorChecksum();
    bool actor_mismatch =
        (sync.actor_checksum != 0 && sync.actor_checksum != local_actor_checksum);
    if (actor_mismatch) {
        debug_log("mp actor mismatch: tick=%u local=%llu remote=%llu", sync.tick,
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
                  static_cast<unsigned>(sync.random_state), p0_x, p0_y, sync.p0_x, sync.p0_y,
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

void handle_sync_sample(const protocol::SyncPacket &sync,
                        const SessionState::ChecksumSample &sample) {
    if (sync.epoch != g_session.input_epoch) {
        debug_log("mp sync skip: epoch=%u local epoch=%u", sync.epoch, g_session.input_epoch);
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

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
