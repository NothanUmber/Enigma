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

#include "multiplayer_extra_players.hh"
#include "multiplayer_rollback.hh"
#include "multiplayer_session_impl.hh"
#include "multiplayer_transport.hh"
#include "multiplayer_wait_settings.hh"

#include "client.hh"
#include "errors.hh"
#include "input.hh"
#include "options.hh"
#include "player.hh"
#include "server.hh"
#include "lev/Index.hh"
#include "lev/Proxy.hh"
#include "stones/OxydStone.hh"
#include "stones/PuzzleStone.hh"
#include "world.hh"

#include "SDL.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/* -------------------- Multiplayer session transport -------------------- */
/*
 * Transport selection and session-level transport adapters.
 *
 * The session engine uses the `Transport` interface regardless of how peers
 * connect (direct, UDP relay, TCP relay).
 */

namespace enigma {
namespace multiplayer {
namespace internal {

namespace {
std::string stable_world_kind(Object *obj) {
    if (!obj)
        return std::string();
    // Must match the encoding side (multiplayer_session_sync.cc): use template-backed
    // kind strings for objects where Object::getKind() is schema-incomplete.
    if (PuzzleStone *ps = dynamic_cast<PuzzleStone *>(obj)) {
        const int color = static_cast<int>(ps->getAttr("color"));
        const std::string con = ps->getAttr("connections").to_string();
        int bits = 0;
        if (con.find('w') != std::string::npos)
            bits |= 1;
        if (con.find('s') != std::string::npos)
            bits |= 2;
        if (con.find('e') != std::string::npos)
            bits |= 4;
        if (con.find('n') != std::string::npos)
            bits |= 8;
        static const char *kSuffix[16] = {"",    "w",   "s",   "sw",  "e",   "ew",  "es",  "esw",
                                          "n",   "nw",  "ns",  "nsw", "ne",  "new", "nes", "nesw"};
        const char *base = "st_puzzle";
        if (color == BLUE)
            base = "st_puzzle_blue";
        else if (color == YELLOW)
            base = "st_puzzle_yellow";
        const bool hollow = ps->getAttr("hollow").to_bool();
        if (hollow && color == YELLOW && bits == 15)
            return "st_puzzle_yellow_nesw_hollow";
        const char *suffix = kSuffix[bits & 15];
        if (!suffix || !*suffix)
            return base;
        return std::string(base) + "_" + suffix;
    }
    return obj->getKind();
}
}  // namespace

bool abort_session_with_message(const char *message) {
    if (!client::AbortGameP())
        client::Msg_ShowText(message, true, 3.0);
    client::Msg_Command("abort");
    SessionShutdown();
    return false;
}

bool local_can_send_ready() {
    if (!g_session.active || g_session.host)
        return false;
    if (!g_session.local_player_known)
        return false;
    // READY must mean: we've observed at least one NET_LOAD_LEVEL and are now
    // waiting for NET_START for that level. Without this, a client can send READY
    // while still on an arbitrary previously loaded level (WorldInitialized=1),
    // allowing the host to start with mismatched worlds.
    if (g_session.last_load_id == 0)
        return false;
    return true;
}

unsigned world_state_streak_threshold() {
    int streak_threshold = static_cast<int>(kWorldDesyncStreakForWorldStateRequest);
    int threshold_override =
        options::GetInt("MultiplayerDebugWorldDesyncStreakForWorldStateRequest");
    if (threshold_override > 0)
        streak_threshold = threshold_override;
    if (streak_threshold < 1)
        streak_threshold = 1;
    return static_cast<unsigned>(streak_threshold);
}

bool has_remote_peers() {
    return !g_session.peer_players.empty() || !g_session.relay_players.empty() ||
           !g_session.tcp_relay_players.empty();
}

bool SessionHasRemotePeers() {
    if (!g_session.active)
        return false;
    return has_remote_peers();
}

unsigned SessionConnectedRemotePlayers() {
    if (!g_session.active || !g_session.host)
        return 0;
    return static_cast<unsigned>(g_session.peer_players.size() + g_session.relay_players.size() +
                                 g_session.tcp_relay_players.size());
}

namespace {
void host_broadcast_debug_options();
}  // namespace

void SessionBroadcastDebugOptions() {
    host_broadcast_debug_options();
}

bool can_accept_more_remote_players() {
    if (!g_session.host)
        return false;
    if (g_session.expected_players <= 1)
        return false;
    if (g_session.player_in_use.size() != g_session.expected_players)
        return true;  // allocation will normalize
    for (unsigned id = 1; id < g_session.expected_players; ++id) {
        if (!g_session.player_in_use[id])
            return true;
    }
    return false;
}

namespace {

// Bit layout for protocol::DebugOptionsPacket::bool_mask.
enum DebugOptBits : Uint32 {
    DBG_LOGGING = 1u << 0,
    DBG_DUMP_STATE = 1u << 1,
    DBG_TRACE_INIT = 1u << 2,
    DBG_SMOOTH_RENDER = 1u << 3,
    DBG_SKIP_LOCAL_RESYNC = 1u << 4,
    DBG_FORCE_RELAY = 1u << 5,
    DBG_BIND_LOCAL = 1u << 6,
    DBG_ZEROFILL = 1u << 7,
    DBG_ROLLBACK = 1u << 8,
    DBG_REMOTE_LOCAL_BALL = 1u << 9,
    DBG_NETSIM = 1u << 10,
    DBG_NETSIM_ALL = 1u << 11,
    // Experimental: client-controlled ball sends authoritative position updates.
    DBG_CLIENT_AUTH_BALL_POS = 1u << 12
};

protocol::DebugOptionsPacket build_debug_options_from_prefs() {
    protocol::DebugOptionsPacket msg;
    msg.version = 1;
    msg.tick_ms = g_session.tick_ms;

    Uint32 mask = 0;
    if (options::GetBool("MultiplayerDebugLogging"))
        mask |= DBG_LOGGING;
    if (options::GetBool("MultiplayerDebugDumpState"))
        mask |= DBG_DUMP_STATE;
    if (options::GetBool("MultiplayerDebugTraceWorldInit"))
        mask |= DBG_TRACE_INIT;
    if (options::GetBool("MultiplayerDebugSmoothRender"))
        mask |= DBG_SMOOTH_RENDER;
    if (options::GetBool("MultiplayerDebugSkipLocalResync"))
        mask |= DBG_SKIP_LOCAL_RESYNC;
    if (options::GetBool("MultiplayerDebugForceRelay"))
        mask |= DBG_FORCE_RELAY;
    if (options::GetBool("MultiplayerDebugBindLocal"))
        mask |= DBG_BIND_LOCAL;
    if (options::GetBool("MultiplayerDebugZeroFillInputs"))
        mask |= DBG_ZEROFILL;
    if (options::GetBool("MultiplayerDebugRollbackEnabled"))
        mask |= DBG_ROLLBACK;
    if (options::GetBool("MultiplayerDebugRemoteControlLocalBall"))
        mask |= DBG_REMOTE_LOCAL_BALL;
    if (options::GetBool("MultiplayerDebugNetSimEnabled"))
        mask |= DBG_NETSIM;
    if (options::GetBool("MultiplayerDebugNetSimAll"))
        mask |= DBG_NETSIM_ALL;
    if (options::GetBool("MultiplayerDebugClientAuthBallPos"))
        mask |= DBG_CLIENT_AUTH_BALL_POS;

    msg.bool_mask = mask;
    msg.predict_missing_mouse_ticks =
        static_cast<Uint16>(std::max(0, options::GetInt("MultiplayerDebugPredictMissingMouseTicks")));
    msg.input_delay_legacy_ticks =
        static_cast<Uint16>(std::max(0, options::GetInt("MultiplayerDebugInputDelayTicks")));
    msg.host_resync_stride_legacy_ticks = static_cast<Uint16>(
        std::max(0, options::GetInt("MultiplayerDebugHostBroadcastResyncStrideTicks")));
    msg.host_world_stride_legacy_ticks = static_cast<Uint16>(
        std::max(0, options::GetInt("MultiplayerDebugHostBroadcastWorldStateStrideTicks")));
    msg.rollback_keep_ticks =
        static_cast<Uint16>(std::max(0, options::GetInt("MultiplayerDebugRollbackKeepTicks")));
    msg.netsim_delay_ms =
        static_cast<Uint16>(std::max(0, options::GetInt("MultiplayerDebugNetSimDelayMs")));
    msg.netsim_jitter_ms =
        static_cast<Uint16>(std::max(0, options::GetInt("MultiplayerDebugNetSimJitterMs")));
    msg.netsim_drop_pct = static_cast<Uint8>(
        std::max(0, std::min(100, options::GetInt("MultiplayerDebugNetSimDropPct"))));
    msg.netsim_dup_pct = static_cast<Uint8>(
        std::max(0, std::min(100, options::GetInt("MultiplayerDebugNetSimDupPct"))));
    return msg;
}

void apply_debug_options_to_options(const protocol::DebugOptionsPacket &msg, bool allow_simulation_mutation) {
    const Uint32 m = msg.bool_mask;

    options::SetOption("MultiplayerDebugLogging", (m & DBG_LOGGING) != 0);
    options::SetOption("MultiplayerDebugDumpState", (m & DBG_DUMP_STATE) != 0);
    options::SetOption("MultiplayerDebugTraceWorldInit", (m & DBG_TRACE_INIT) != 0);
    options::SetOption("MultiplayerDebugSmoothRender", (m & DBG_SMOOTH_RENDER) != 0);
    // Transport-only experiments: allow toggling while RUNNING so host changes can
    // propagate without needing a full session restart.
    options::SetOption("MultiplayerDebugClientAuthBallPos", (m & DBG_CLIENT_AUTH_BALL_POS) != 0);

    if (!allow_simulation_mutation)
        return;

    options::SetOption("MultiplayerDebugSkipLocalResync", (m & DBG_SKIP_LOCAL_RESYNC) != 0);
    options::SetOption("MultiplayerDebugForceRelay", (m & DBG_FORCE_RELAY) != 0);
    options::SetOption("MultiplayerDebugBindLocal", (m & DBG_BIND_LOCAL) != 0);
    options::SetOption("MultiplayerDebugZeroFillInputs", (m & DBG_ZEROFILL) != 0);
    options::SetOption("MultiplayerDebugRollbackEnabled", (m & DBG_ROLLBACK) != 0);
    options::SetOption("MultiplayerDebugRemoteControlLocalBall", (m & DBG_REMOTE_LOCAL_BALL) != 0);
    options::SetOption("MultiplayerDebugNetSimEnabled", (m & DBG_NETSIM) != 0);
    options::SetOption("MultiplayerDebugNetSimAll", (m & DBG_NETSIM_ALL) != 0);

    options::SetOption("MultiplayerDebugPredictMissingMouseTicks",
                       static_cast<double>(msg.predict_missing_mouse_ticks));
    options::SetOption("MultiplayerDebugInputDelayTicks", static_cast<double>(msg.input_delay_legacy_ticks));
    options::SetOption("MultiplayerDebugHostBroadcastResyncStrideTicks",
                       static_cast<double>(msg.host_resync_stride_legacy_ticks));
    options::SetOption("MultiplayerDebugHostBroadcastWorldStateStrideTicks",
                       static_cast<double>(msg.host_world_stride_legacy_ticks));
    options::SetOption("MultiplayerDebugRollbackKeepTicks", static_cast<double>(msg.rollback_keep_ticks));
    options::SetOption("MultiplayerDebugNetSimDelayMs", static_cast<double>(msg.netsim_delay_ms));
    options::SetOption("MultiplayerDebugNetSimJitterMs", static_cast<double>(msg.netsim_jitter_ms));
    options::SetOption("MultiplayerDebugNetSimDropPct", static_cast<double>(msg.netsim_drop_pct));
    options::SetOption("MultiplayerDebugNetSimDupPct", static_cast<double>(msg.netsim_dup_pct));
    // Keep the Debug UI consistent with the negotiated tick length.
    options::SetOption("MultiplayerDebugTickLengthMs", static_cast<double>(msg.tick_ms));
}

namespace {
uint32_t stable_name_hash(Actor *actor) {
    if (!actor)
        return 0;
    Value name = actor->getAttr("name");
    if (name.getType() != Value::STRING)
        return 0;
    const std::string &s = name.get_string();
    if (s.empty())
        return 0;
    // FNV-1a 32-bit (must match multiplayer_session_sync.cc).
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= static_cast<uint32_t>(c);
        h *= 16777619u;
    }
    return h ? h : 1u;
}

void teleport_actor_physics_only(Actor *actor, float x, float y, float vx, float vy) {
    if (!actor)
        return;
    // Same semantics as apply_resync_state(): update physics + spatial index only.
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
}

void nudge_actor_pos_physics(Actor *actor, float x, float y) {
    if (!actor)
        return;
    // Softer than teleport_actor_physics_only(): keep velocity/forces/contacts intact.
    // This avoids injecting large discontinuities into collision resolution (e.g. when
    // the actor is pushing movable stones).
    ActorInfo *ai = actor->get_actorinfo();
    ai->pos = ecl::V2(x, y);
    DidMoveActor(actor);
    ai->last_gridpos = ai->gridpos;
    ai->pos_force = ai->pos;
}

Actor *find_controlled_steerable_actor_by_owner_state(unsigned player_id,
                                                      const protocol::OwnerActorStatePacket &pkt) {
    std::vector<Actor *> actors;
    GetActors(actors);

    // First: exact object id match.
    for (Actor *a : actors) {
        if (!a)
            continue;
        if (static_cast<uint32_t>(a->getId()) == pkt.object_id)
            return a;
    }

    // Fallback: match by (controlled_by, steerable, kind, name_hash) and nearest position.
    Actor *best = nullptr;
    float best_dist2 = 0.0f;
    for (Actor *a : actors) {
        if (!a)
            continue;
        if (!a->isSteerable())
            continue;
        if (!a->controlled_by(static_cast<int>(player_id)))
            continue;
        const uint16_t kind = static_cast<uint16_t>(get_id(a));
        if (pkt.actor_id != 0 && kind != pkt.actor_id)
            continue;
        if (pkt.name_hash != 0) {
            const uint32_t nh = stable_name_hash(a);
            if (nh != pkt.name_hash)
                continue;
        }
        const ecl::V2 &pos = a->get_pos();
        const float dx = static_cast<float>(pos[0]) - pkt.x;
        const float dy = static_cast<float>(pos[1]) - pkt.y;
        const float d2 = dx * dx + dy * dy;
        if (!best || d2 < best_dist2) {
            best = a;
            best_dist2 = d2;
        }
    }
    return best;
}
}  // namespace

void host_broadcast_debug_options() {
    if (!g_session.active || !g_session.host || !has_remote_peers())
        return;
    protocol::DebugOptionsPacket msg = build_debug_options_from_prefs();
    ecl::Buffer payload;
    protocol::encode_debug_options(payload, msg);
    g_transport.HostBroadcast(payload);
    g_transport.Flush();
}

void host_send_current_load_to_remote(HostSource source, ENetPeer *peer, Uint32 relay_client_id) {
    if (!g_session.active || !g_session.host)
        return;
    if (g_session.load_id == 0 || g_session.level_id.empty()) {
        if (debug_enabled())
            debug_log("mp host: skip load send (no load) via=%s load=%u level_id=%s",
                      host_source_name(source),
                      static_cast<unsigned>(g_session.load_id),
                      g_session.level_id.empty() ? "(empty)" : g_session.level_id.c_str());
        return;
    }

    protocol::LoadLevelPacket msg;
    msg.load_id = g_session.load_id;
    msg.pack_name = g_session.level_pack_name;
    msg.level_id = g_session.level_id;
    ecl::Buffer buf;
    protocol::encode_load_level(buf, msg);

    switch (source) {
    case HostSource::DIRECT:
        if (debug_enabled())
            debug_log("mp host: send load to peer via=%s load=%u level_id=%s peer=%p",
                      host_source_name(source),
                      static_cast<unsigned>(msg.load_id),
                      msg.level_id.c_str(),
                      static_cast<void *>(peer));
        g_transport.HostSendDirect(peer, buf);
        break;
    case HostSource::UDP_RELAY:
        if (debug_enabled())
            debug_log("mp host: send load to client via=%s load=%u level_id=%s client=%u",
                      host_source_name(source),
                      static_cast<unsigned>(msg.load_id),
                      msg.level_id.c_str(),
                      static_cast<unsigned>(relay_client_id));
        g_transport.HostSendUdpRelay(relay_client_id, buf);
        break;
    case HostSource::TCP_RELAY:
        if (debug_enabled())
            debug_log("mp host: send load to client via=%s load=%u level_id=%s client=%u",
                      host_source_name(source),
                      static_cast<unsigned>(msg.load_id),
                      msg.level_id.c_str(),
                      static_cast<unsigned>(relay_client_id));
        g_transport.HostSendTcpRelay(relay_client_id, buf);
        break;
    default:
        break;
    }
    g_transport.Flush();
}

bool should_apply_late_mouse_sample(unsigned player_id, uint32_t current_tick, uint32_t src_tick) {
    if (player_id >= input::kMaxPlayers)
        return false;
    // Never overwrite an on-time sample for the current tick with a late one.
    if (!g_session.late_mouse_valid[player_id] || g_session.late_mouse_applied_tick[player_id] != current_tick) {
        if (input::HasInput(current_tick, player_id))
            return false;
        g_session.late_mouse_valid[player_id] = true;
        g_session.late_mouse_applied_tick[player_id] = current_tick;
        g_session.late_mouse_src_tick[player_id] = 0;
    }
    if (src_tick <= g_session.late_mouse_src_tick[player_id])
        return false;
    g_session.late_mouse_src_tick[player_id] = src_tick;
    return true;
}

lev::Proxy *find_level_proxy_in_current_index(const std::string &level_id) {
    lev::Index *ind = lev::Index::getCurrentIndex();
    if (!ind)
        return nullptr;
    for (int i = 0; i < ind->size(); ++i) {
        lev::Proxy *proxy = ind->getProxy(i);
        if (proxy && proxy->getNormLevelPath() == level_id)
            return proxy;
    }
    return nullptr;
}

lev::Proxy *find_level_proxy_anywhere(const std::string &level_id) {
    // Best-effort fallback for cases where pack names differ across installations.
    // This can be expensive, but it only triggers on network load-level control messages.
    for (lev::Proxy *proxy : lev::Proxy::getProxies()) {
        if (proxy && proxy->getNormLevelPath() == level_id)
            return proxy;
    }
    return nullptr;
}

bool allocate_remote_player_id(unsigned &out_player_id) {
    out_player_id = 0;
    if (!g_session.host || g_session.expected_players <= 1)
        return false;
    if (g_session.player_in_use.size() != g_session.expected_players)
        g_session.player_in_use.assign(g_session.expected_players, false);
    if (g_session.player_in_use.empty())
        return false;
    g_session.player_in_use[0] = true;
    for (unsigned id = 1; id < g_session.expected_players; ++id) {
        if (!g_session.player_in_use[id]) {
            g_session.player_in_use[id] = true;
            out_player_id = id;
            return true;
        }
    }
    return false;
}

void release_remote_player_id(unsigned player_id) {
    if (!g_session.host)
        return;
    if (player_id == 0)
        return;
    if (g_session.player_in_use.size() != g_session.expected_players)
        return;
    if (player_id >= g_session.player_in_use.size())
        return;
    g_session.player_in_use[player_id] = false;
}

bool lookup_remote_player(HostSource source, ENetPeer *peer, Uint32 relay_client_id,
                          unsigned &player_id) {
    if (source == HostSource::UDP_RELAY) {
        auto it = g_session.relay_players.find(relay_client_id);
        if (it != g_session.relay_players.end()) {
            player_id = it->second;
            return true;
        }
        return false;
    }
    if (source == HostSource::TCP_RELAY) {
        auto it = g_session.tcp_relay_players.find(relay_client_id);
        if (it != g_session.tcp_relay_players.end()) {
            player_id = it->second;
            return true;
        }
        return false;
    }
    auto it = g_session.peer_players.find(peer);
    if (it != g_session.peer_players.end()) {
        player_id = it->second;
        return true;
    }
    return false;
}

void mark_remote_ready(HostSource source, ENetPeer *peer, Uint32 relay_client_id) {
    if (source == HostSource::UDP_RELAY)
        g_session.relay_ready[relay_client_id] = true;
    else if (source == HostSource::TCP_RELAY)
        g_session.tcp_relay_ready[relay_client_id] = true;
    else
        g_session.peer_ready[peer] = true;
}

void send_input_to_peer(ENetPeer *peer, const protocol::InputPacket &pkt) {
    ecl::Buffer buf;
    protocol::encode_input(buf, pkt);
    g_transport.HostSendDirect(peer, buf);
}

void send_input_bundle_to_peer(ENetPeer *peer, const protocol::InputBundlePacket &pkt) {
    ecl::Buffer buf;
    protocol::encode_input_bundle(buf, pkt);
    g_transport.HostSendDirectUnreliable(peer, buf);
}

void broadcast_input(const protocol::InputPacket &pkt, ENetPeer *exclude, Uint32 exclude_udp_relay,
                     Uint32 exclude_tcp_relay) {
    for (const auto &entry : g_session.peer_players) {
        if (entry.first == exclude)
            continue;
        send_input_to_peer(entry.first, pkt);
    }
    ecl::Buffer buf;
    protocol::encode_input(buf, pkt);
    if (!g_session.relay_players.empty())
        g_transport.HostBroadcastUdpRelay(buf, exclude_udp_relay);
    if (!g_session.tcp_relay_players.empty())
        g_transport.HostBroadcastTcpRelay(buf, exclude_tcp_relay);
}

void broadcast_input_bundle(const protocol::InputBundlePacket &pkt, ENetPeer *exclude,
                            Uint32 exclude_udp_relay, Uint32 exclude_tcp_relay) {
    for (const auto &entry : g_session.peer_players) {
        if (entry.first == exclude)
            continue;
        send_input_bundle_to_peer(entry.first, pkt);
    }
    ecl::Buffer buf;
    protocol::encode_input_bundle(buf, pkt);
    // Relay forwarding is currently reliable on the host->relay hop; payload redundancy still helps.
    if (!g_session.relay_players.empty())
        g_transport.HostBroadcastUdpRelay(buf, exclude_udp_relay);
    if (!g_session.tcp_relay_players.empty())
        g_transport.HostBroadcastTcpRelay(buf, exclude_tcp_relay);
}

bool handle_host_input_packet(const char *data, size_t len, ENetPeer *peer, HostSource source,
                              Uint32 relay_client_id) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::InputPacket input_msg;
    if (!protocol::decode_input(buf, input_msg))
        return false;
    if (input_msg.epoch != g_session.input_epoch) {
        debug_log("mp drop input: tick=%u epoch=%u local epoch=%u", input_msg.tick, input_msg.epoch,
                  g_session.input_epoch);
        return true;
    }
    if (input_msg.tick < 20) {
        debug_log("mp recv input: tick=%u player=%u mouse=(%.2f,%.2f) rot=%d act=%u",
                  input_msg.tick, input_msg.player, input_msg.mouse_x, input_msg.mouse_y,
                  static_cast<int>(input_msg.rotate_steps),
                  static_cast<unsigned>(input_msg.activate_count));
    }
    const uint32_t current_tick = input::CurrentTick();
    const uint32_t src_tick = input_msg.tick;
    const bool late = (input_msg.tick < current_tick);
    if (late) {
        if (!input::ZerofillMissingInputsEnabled())
            return true;
        // Monotonic processing: late inputs are applied at the current tick (best-effort).
        // This avoids requiring rollback/replay and prevents unbounded growth of past-tick
        // inputs that can never be consumed again.
        input_msg.tick = current_tick;
    }
    unsigned player_id = 0;
    if (!lookup_remote_player(source, peer, relay_client_id, player_id)) {
        // Fallback: if transport-side peer->player mapping is temporarily missing
        // (e.g. relay race/replace edge-cases), accept the claimed player id as a
        // best-effort input source. This keeps gameplay responsive and is limited
        // to cases where we already failed to attribute the packet to a known peer.
        const unsigned claimed = static_cast<unsigned>(input_msg.player);
        if (claimed < g_session.expected_players && claimed != g_session.local_player) {
            player_id = claimed;
            if (debug_enabled()) {
                debug_log("mp host: input fallback claim=%u (source=%d relay_id=%u peer=%p)",
                          claimed, static_cast<int>(source),
                          static_cast<unsigned>(relay_client_id),
                          static_cast<void *>(peer));
            }
        } else {
            return true;
        }
    }
    if (late) {
        if (!should_apply_late_mouse_sample(player_id, current_tick, src_tick)) {
            input_msg.mouse_x = 0.0f;
            input_msg.mouse_y = 0.0f;
        }
    }
    input::PlayerInput pi;
    pi.mouse_force = ecl::V2(input_msg.mouse_x, input_msg.mouse_y);
    pi.rotate_steps = input_msg.rotate_steps;
    pi.activate_count = input_msg.activate_count;
    input::EnqueueInput(input_msg.tick, player_id, pi);
    rollback::RecordInput(input_msg.tick, player_id, pi);
    protocol::InputPacket forward = input_msg;
    forward.player = static_cast<Uint8>(player_id);
    broadcast_input(forward, source == HostSource::DIRECT ? peer : nullptr,
                    source == HostSource::UDP_RELAY ? relay_client_id : 0,
                    source == HostSource::TCP_RELAY ? relay_client_id : 0);
    return true;
}

bool handle_host_input_bundle_packet(const char *data, size_t len, ENetPeer *peer, HostSource source,
                                     Uint32 relay_client_id) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::InputBundlePacket bundle;
    if (!protocol::decode_input_bundle(buf, bundle))
        return false;
    if (bundle.epoch != g_session.input_epoch) {
        debug_log("mp drop input bundle: first tick=%u epoch=%u local epoch=%u",
                  bundle.first_tick, bundle.epoch, g_session.input_epoch);
        return true;
    }
    if (bundle.first_tick < 20) {
        debug_log("mp recv input bundle: first tick=%u player=%u count=%u", bundle.first_tick,
                  bundle.player, static_cast<unsigned>(bundle.entries.size()));
    }
    unsigned player_id = 0;
    if (!lookup_remote_player(source, peer, relay_client_id, player_id)) {
        // Same rationale as handle_host_input_packet(): preserve playability when
        // peer->player attribution is missing.
        const unsigned claimed = static_cast<unsigned>(bundle.player);
        if (claimed < g_session.expected_players && claimed != g_session.local_player) {
            player_id = claimed;
            if (debug_enabled()) {
                debug_log("mp host: input bundle fallback claim=%u (source=%d relay_id=%u peer=%p)",
                          claimed, static_cast<int>(source),
                          static_cast<unsigned>(relay_client_id),
                          static_cast<void *>(peer));
            }
        } else {
            return true;
        }
    }
    const uint32_t current_tick = input::CurrentTick();
    // If we receive a whole bundle "too late" to apply at its intended ticks (common under
    // high latency when zerofill keeps the sim running), clamping every entry to
    // `current_tick` will overwrite earlier non-zero samples with later zero samples.
    // Instead, aggregate the late entries and apply at most one sample at `current_tick`.
    input::PlayerInput late_agg;
    bool late_mouse_set = false;
    uint32_t late_mouse_src_tick = 0;
    bool have_late = false;
    for (size_t i = 0; i < bundle.entries.size(); ++i) {
        uint32_t tick = bundle.first_tick + static_cast<uint32_t>(i);
        const auto &e = bundle.entries[i];
        input::PlayerInput pi;
        pi.mouse_force = ecl::V2(e.mouse_x, e.mouse_y);
        pi.rotate_steps = e.rotate_steps;
        pi.activate_count = e.activate_count;
        if (tick < current_tick) {
            if (!input::ZerofillMissingInputsEnabled())
                continue;
            // Otherwise, best-effort apply at `current_tick` once.
            have_late = true;
            late_agg.rotate_steps += pi.rotate_steps;
            late_agg.activate_count += pi.activate_count;
            // Mouse input is an impulse. If packets arrive out of order, applying an
            // older impulse after a newer one causes visible "kicks". Keep only the
            // newest non-zero impulse and ignore stale ones.
            if (pi.mouse_force[0] != 0.0f || pi.mouse_force[1] != 0.0f) {
                late_mouse_set = true;
                if (!late_mouse_src_tick || tick > late_mouse_src_tick) {
                    late_mouse_src_tick = tick;
                    late_agg.mouse_force = pi.mouse_force;
                }
            }
            continue;
        }
        input::EnqueueInput(tick, player_id, pi);
        rollback::RecordInput(tick, player_id, pi);
    }
    if (have_late) {
        if (late_mouse_set) {
            if (!should_apply_late_mouse_sample(player_id, current_tick, late_mouse_src_tick))
                late_agg.mouse_force = ecl::V2(0.0f, 0.0f);
        } else {
            late_agg.mouse_force = ecl::V2(0.0f, 0.0f);
        }
        const bool already_late =
            (player_id < input::kMaxPlayers && g_session.late_mouse_valid[player_id] &&
             g_session.late_mouse_applied_tick[player_id] == current_tick);
        if (!late_agg.empty() && (!input::HasInput(current_tick, player_id) || already_late)) {
            input::EnqueueInput(current_tick, player_id, late_agg);
            rollback::RecordInput(current_tick, player_id, late_agg);
        }
    }
    protocol::InputBundlePacket forward = bundle;
    forward.player = static_cast<Uint8>(player_id);
    broadcast_input_bundle(forward, source == HostSource::DIRECT ? peer : nullptr,
                           source == HostSource::UDP_RELAY ? relay_client_id : 0,
                           source == HostSource::TCP_RELAY ? relay_client_id : 0);
    return true;
}

bool handle_host_resync_request_packet(const char *data, size_t len, HostSource source,
                                       Uint32 relay_client_id, ENetPeer *peer) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::ResyncRequest req;
    if (!protocol::decode_resync_request(buf, req))
        return false;
    if (req.epoch == g_session.input_epoch) {
        if (source == HostSource::UDP_RELAY)
            send_resync_state_to_relay(relay_client_id);
        else if (source == HostSource::TCP_RELAY)
            send_resync_state_to_tcp_relay(relay_client_id);
        else
            send_resync_state(peer);
    }
    return true;
}

bool handle_host_world_state_request_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::WorldStateRequest req;
    if (!protocol::decode_world_state_request(buf, req))
        return false;
    if (req.epoch != g_session.input_epoch)
        return true;
    if (g_session.phase != SessionState::Phase::RUNNING || !server::WorldInitialized) {
        if (debug_enabled())
            debug_log("mp host: drop world-state request (not running / world not initialized)");
        return true;
    }
    // For now broadcast to all peers. This keeps everyone converging even if only
    // one client noticed the mismatch.
    if (debug_enabled())
        debug_log("mp host: world-state request recv: tick=%u", req.tick);
    broadcast_world_state_reliable();
    return true;
}

bool handle_host_ready_packet(const char *data, size_t len, ENetPeer *peer, HostSource source,
                              Uint32 relay_client_id) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 session_id = 0;
    Uint32 epoch = 0;
    Uint32 load_id = 0;
    if (!protocol::decode_ready(buf, session_id, epoch, load_id))
        return false;
    if (session_id != g_session.session_id) {
        if (debug_enabled())
            debug_log("mp host: drop ready (session mismatch remote=%u local=%u)",
                      static_cast<unsigned>(session_id),
                      static_cast<unsigned>(g_session.session_id));
        return true;
    }
    if (epoch != g_session.input_epoch) {
        if (debug_enabled())
            debug_log("mp host: drop ready (epoch mismatch remote=%u local=%u)",
                      static_cast<unsigned>(epoch),
                      static_cast<unsigned>(g_session.input_epoch));
        return true;
    }
    if (load_id != g_session.load_id) {
        if (debug_enabled())
            debug_log("mp host: drop ready (load mismatch remote=%u local=%u)",
                      static_cast<unsigned>(load_id),
                      static_cast<unsigned>(g_session.load_id));
        // Help clients recover if they sent READY before observing NET_LOAD_LEVEL,
        // or if a load packet was lost. Re-announce the current load to that peer.
        host_send_current_load_to_remote(source, peer, relay_client_id);
        return true;
    }
    unsigned player_id = 0;
    if (lookup_remote_player(source, peer, relay_client_id, player_id)) {
        bool already_ready = false;
        if (source == HostSource::UDP_RELAY) {
            auto it = g_session.relay_ready.find(relay_client_id);
            already_ready = (it != g_session.relay_ready.end() && it->second);
        } else if (source == HostSource::TCP_RELAY) {
            auto it = g_session.tcp_relay_ready.find(relay_client_id);
            already_ready = (it != g_session.tcp_relay_ready.end() && it->second);
        } else {
            auto it = g_session.peer_ready.find(peer);
            already_ready = (it != g_session.peer_ready.end() && it->second);
        }
        if (!already_ready) {
            debug_log("mp host: ready player=%u source=%s", player_id, host_source_name(source));
            mark_remote_ready(source, peer, relay_client_id);
            debug_log("mp host: peer ready");
        }
    }
    return true;
}

bool handle_host_pause_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 epoch = 0;
    bool paused = false;
    if (!protocol::decode_pause(buf, epoch, paused))
        return false;
    if (epoch != g_session.input_epoch)
        return true;
    SessionRequestPause(paused);
    return true;
}

bool handle_host_menu_packet(const char *data, size_t len, ENetPeer *peer, HostSource source,
                             Uint32 relay_client_id) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 epoch = 0;
    Uint8 ignored_player = 0;
    bool open = false;
    if (!protocol::decode_menu(buf, epoch, ignored_player, open))
        return false;
    if (epoch != g_session.input_epoch)
        return true;
    unsigned player_id = 0;
    if (!lookup_remote_player(source, peer, relay_client_id, player_id))
        return true;
    if (player_id >= g_session.menu_open.size())
        return true;
    if (g_session.menu_open[player_id] == open)
        return true;
    g_session.menu_open[player_id] = open;

    bool want_pause = false;
    for (bool any : g_session.menu_open) {
        if (any) {
            want_pause = true;
            break;
        }
    }
    SessionRequestPause(want_pause);
    return true;
}

bool handle_host_abort_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 epoch = 0;
    if (!protocol::decode_abort(buf, epoch))
        return false;
    if (epoch != g_session.input_epoch)
        return true;
    // Abort is global: one player aborts => end the session for all.
    send_abort_to_peers();
    begin_abort_after_grace("Game aborted. Ending session.");
    return true;
}

bool handle_host_placement_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::PlacementPacket place;
    if (!protocol::decode_place(buf, place))
        return false;
    if (place.restart_id == g_session.restart_id && place.player < g_session.expected_players) {
        GridPos pos(static_cast<int>(place.x), static_cast<int>(place.y));
        if (placement_required_for_player(place.player) && is_valid_placement(pos, place.player)) {
            apply_placement(place.player, pos);
            if (g_session.placement_received.size() > place.player)
                g_session.placement_received[place.player] = true;
            broadcast_placement(place);
        }
    }
    return true;
}

bool handle_host_pong_packet(const char *data, size_t len, ENetPeer *peer, HostSource source,
                             Uint32 relay_client_id) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::PongPacket pong;
    if (!protocol::decode_pong(buf, pong))
        return false;
    if (!g_session.host || !g_session.auto_detect_active)
        return true;
    unsigned player_id = 0;
    if (!lookup_remote_player(source, peer, relay_client_id, player_id))
        return true;
    if (player_id >= g_session.auto_detect_inflight_ms.size())
        return true;
    auto &inflight = g_session.auto_detect_inflight_ms[player_id];
    auto it = inflight.find(pong.ping_id);
    if (it == inflight.end())
        return true;
    const Uint32 sent_ms = it->second;
    inflight.erase(it);
    const Uint32 now_ms = SDL_GetTicks();
    const Uint32 rtt_ms = (now_ms >= sent_ms) ? (now_ms - sent_ms) : 0;
    if (player_id < g_session.auto_detect_recv.size())
        g_session.auto_detect_recv[player_id] += 1;
    if (player_id < g_session.auto_detect_rtts_ms.size())
        g_session.auto_detect_rtts_ms[player_id].push_back(rtt_ms);
    return true;
}

bool handle_host_owner_actor_state_packet(const char *data, size_t len, ENetPeer *peer, HostSource source,
                                         Uint32 relay_client_id);

bool handle_host_packet(const char *data, size_t len, ENetPeer *peer, HostSource source,
                        Uint32 relay_client_id) {
    if (handle_host_pong_packet(data, len, peer, source, relay_client_id))
        return true;
    if (handle_host_input_bundle_packet(data, len, peer, source, relay_client_id))
        return true;
    if (handle_host_input_packet(data, len, peer, source, relay_client_id))
        return true;
    if (handle_host_resync_request_packet(data, len, source, relay_client_id, peer))
        return true;
    if (handle_host_world_state_request_packet(data, len))
        return true;
    if (handle_host_owner_actor_state_packet(data, len, peer, source, relay_client_id))
        return true;
    if (handle_host_ready_packet(data, len, peer, source, relay_client_id))
        return true;
    if (handle_host_menu_packet(data, len, peer, source, relay_client_id))
        return true;
    if (handle_host_pause_packet(data, len))
        return true;
    if (handle_host_abort_packet(data, len))
        return true;
    if (handle_host_placement_packet(data, len))
        return true;
    return false;
}

void broadcast_owner_actor_state(const protocol::OwnerActorStatePacket &pkt,
                                 ENetPeer *exclude,
                                 Uint32 exclude_udp_relay,
                                 Uint32 exclude_tcp_relay) {
    for (const auto &entry : g_session.peer_players) {
        if (entry.first == exclude)
            continue;
        ecl::Buffer buf;
        protocol::encode_owner_actor_state(buf, pkt);
        g_transport.HostSendDirectUnreliable(entry.first, buf);
    }
    ecl::Buffer out;
    protocol::encode_owner_actor_state(out, pkt);
    if (!g_session.relay_players.empty())
        g_transport.HostBroadcastUdpRelay(out, exclude_udp_relay);
    if (!g_session.tcp_relay_players.empty())
        g_transport.HostBroadcastTcpRelay(out, exclude_tcp_relay);
}

bool handle_host_owner_actor_state_packet(const char *data, size_t len, ENetPeer *peer, HostSource source,
                                         Uint32 relay_client_id) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::OwnerActorStatePacket pkt;
    if (!protocol::decode_owner_actor_state(buf, pkt))
        return false;
    if (!g_session.host)
        return true;
    if (!g_session.active)
        return true;
    if (!options::GetBool("MultiplayerDebugClientAuthBallPos")) {
        if (debug_enabled() && pkt.tick < 20) {
            debug_log("mp host: drop owner-state (disabled) tick=%u claimed_player=%u", pkt.tick,
                      static_cast<unsigned>(pkt.player));
        }
        return true;
    }
    if (pkt.epoch != g_session.input_epoch)
        return true;

    unsigned player_id = 0;
    if (!lookup_remote_player(source, peer, relay_client_id, player_id)) {
        const unsigned claimed = static_cast<unsigned>(pkt.player);
        if (claimed < g_session.expected_players && claimed != g_session.local_player) {
            player_id = claimed;
            if (debug_enabled()) {
                debug_log("mp host: owner-state fallback claim=%u (source=%d relay_id=%u peer=%p)",
                          claimed, static_cast<int>(source),
                          static_cast<unsigned>(relay_client_id),
                          static_cast<void *>(peer));
            }
        } else {
            return true;
        }
    }
    if (player_id >= g_session.expected_players)
        return true;
    if (player_id == g_session.local_player)
        return true;

    if (player_id < g_session.last_accepted_owner_state_tick.size()) {
        const uint32_t last = g_session.last_accepted_owner_state_tick[player_id];
        if (last != 0 && pkt.tick <= last)
            return true;
        g_session.last_accepted_owner_state_tick[player_id] = pkt.tick;
    }

    pkt.player = static_cast<Uint8>(player_id);
    if (Actor *a = find_controlled_steerable_actor_by_owner_state(player_id, pkt)) {
        // Owner-state packets are unreliable and can arrive late. Teleporting to a past
        // tick would "rewind" the actor and can cause stone push jitter. Instead, project
        // the received state forward towards the current tick (best-effort).
        const uint32_t current_tick = input::CurrentTick();
        // NOTE: Linear velocity projection is only safe for small deltas. With longer
        // delays/jitter and high-acceleration physics (collisions, rubberbands), dead
        // reckoning can put the host copy into positions the client was never in.
        // Keep the projection window small and bias towards lagging behind rather than
        // overshooting.
        uint32_t max_project_ticks = g_session.input_delay ? g_session.input_delay : 4u;
        if (max_project_ticks < 2u)
            max_project_ticks = 2u;
        if (max_project_ticks > 8u)
            max_project_ticks = 8u;
        uint32_t dt_ticks = 0;
        if (current_tick > pkt.tick)
            dt_ticks = current_tick - pkt.tick;
        if (dt_ticks > max_project_ticks) {
            if (debug_enabled())
                debug_log("mp host: owner-state lag clamp tick=%u cur=%u dt=%u max=%u",
                          pkt.tick, current_tick, dt_ticks, max_project_ticks);
            dt_ticks = max_project_ticks;
        }
        const float dt = static_cast<float>(input::TickTimestep() * static_cast<double>(dt_ticks));
        const float x = pkt.x + pkt.vx * dt;
        const float y = pkt.y + pkt.vy * dt;
        // Apply a bounded correction instead of an unconditional teleport. Even with
        // a small projection window, late packets can be far from the current host
        // state, and hard teleports can cause movable stones to "snap" backwards.
        const ecl::V2 cur = a->get_pos();
        const float dx = x - static_cast<float>(cur[0]);
        const float dy = y - static_cast<float>(cur[1]);
        float d2 = dx * dx + dy * dy;
        // Allow a limited correction per packet; bias towards stability.
        const float max_step = 0.15f;
        if (d2 > max_step * max_step) {
            const float d = std::sqrt(d2);
            const float s = (d > 0.0f) ? (max_step / d) : 0.0f;
            nudge_actor_pos_physics(a,
                                    static_cast<float>(cur[0]) + dx * s,
                                    static_cast<float>(cur[1]) + dy * s);
        } else {
            nudge_actor_pos_physics(a, x, y);
        }
    }
    if (debug_enabled() && pkt.tick < 20) {
        debug_log("mp host: recv owner-state tick=%u player=%u obj=%u pos=(%.2f,%.2f) vel=(%.2f,%.2f)",
                  pkt.tick, static_cast<unsigned>(pkt.player), static_cast<unsigned>(pkt.object_id),
                  static_cast<double>(pkt.x), static_cast<double>(pkt.y),
                  static_cast<double>(pkt.vx), static_cast<double>(pkt.vy));
    }

    Uint32 ex_udp = 0;
    Uint32 ex_tcp = 0;
    ENetPeer *ex_peer = nullptr;
    if (source == HostSource::DIRECT)
        ex_peer = peer;
    else if (source == HostSource::UDP_RELAY)
        ex_udp = relay_client_id;
    else if (source == HostSource::TCP_RELAY)
        ex_tcp = relay_client_id;
    broadcast_owner_actor_state(pkt, ex_peer, ex_udp, ex_tcp);
    return true;
}

bool handle_client_input_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::InputPacket input_msg;
    if (!protocol::decode_input(buf, input_msg))
        return false;
    if (input_msg.epoch != g_session.input_epoch) {
        debug_log("mp drop input: tick=%u epoch=%u local epoch=%u", input_msg.tick, input_msg.epoch,
                  g_session.input_epoch);
        return true;
    }
    if (input_msg.tick < 20) {
        debug_log("mp recv input: tick=%u player=%u mouse=(%.2f,%.2f) rot=%d act=%u",
                  input_msg.tick, input_msg.player, input_msg.mouse_x, input_msg.mouse_y,
                  static_cast<int>(input_msg.rotate_steps),
                  static_cast<unsigned>(input_msg.activate_count));
    }
    const uint32_t current_tick = input::CurrentTick();
    const uint32_t src_tick = input_msg.tick;
    if (input_msg.tick < current_tick) {
        if (!input::ZerofillMissingInputsEnabled())
            return true;
        // Monotonic processing: late inputs are applied at the current tick.
        input_msg.tick = current_tick;
        if (!should_apply_late_mouse_sample(static_cast<unsigned>(input_msg.player), current_tick, src_tick)) {
            input_msg.mouse_x = 0.0f;
            input_msg.mouse_y = 0.0f;
        }
    }
    input::PlayerInput pi;
    pi.mouse_force = ecl::V2(input_msg.mouse_x, input_msg.mouse_y);
    pi.rotate_steps = input_msg.rotate_steps;
    pi.activate_count = input_msg.activate_count;
    input::EnqueueInput(input_msg.tick, input_msg.player, pi);
    rollback::RecordInput(input_msg.tick, input_msg.player, pi);
    return true;
}

bool handle_client_input_bundle_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::InputBundlePacket bundle;
    if (!protocol::decode_input_bundle(buf, bundle))
        return false;
    if (bundle.epoch != g_session.input_epoch) {
        debug_log("mp drop input bundle: first tick=%u epoch=%u local epoch=%u",
                  bundle.first_tick, bundle.epoch, g_session.input_epoch);
        return true;
    }
    if (bundle.first_tick < 20) {
        debug_log("mp recv input bundle: first tick=%u player=%u count=%u", bundle.first_tick,
                  bundle.player, static_cast<unsigned>(bundle.entries.size()));
    }
    const uint32_t current_tick = input::CurrentTick();
    input::PlayerInput late_agg;
    bool late_mouse_set = false;
    uint32_t late_mouse_src_tick = 0;
    bool have_late = false;
    for (size_t i = 0; i < bundle.entries.size(); ++i) {
        uint32_t tick = bundle.first_tick + static_cast<uint32_t>(i);
        const auto &e = bundle.entries[i];
        input::PlayerInput pi;
        pi.mouse_force = ecl::V2(e.mouse_x, e.mouse_y);
        pi.rotate_steps = e.rotate_steps;
        pi.activate_count = e.activate_count;
        if (tick < current_tick) {
            if (!input::ZerofillMissingInputsEnabled())
                continue;
            have_late = true;
            late_agg.rotate_steps += pi.rotate_steps;
            late_agg.activate_count += pi.activate_count;
            if (pi.mouse_force[0] != 0.0f || pi.mouse_force[1] != 0.0f) {
                late_mouse_set = true;
                if (!late_mouse_src_tick || tick > late_mouse_src_tick) {
                    late_mouse_src_tick = tick;
                    late_agg.mouse_force = pi.mouse_force;
                }
            }
            continue;
        }
        input::EnqueueInput(tick, bundle.player, pi);
        rollback::RecordInput(tick, bundle.player, pi);
    }
    if (have_late) {
        const unsigned player_id = static_cast<unsigned>(bundle.player);
        if (late_mouse_set) {
            if (!should_apply_late_mouse_sample(player_id, current_tick, late_mouse_src_tick))
                late_agg.mouse_force = ecl::V2(0.0f, 0.0f);
        } else {
            late_agg.mouse_force = ecl::V2(0.0f, 0.0f);
        }
        const bool already_late =
            (player_id < input::kMaxPlayers && g_session.late_mouse_valid[player_id] &&
             g_session.late_mouse_applied_tick[player_id] == current_tick);
        if (!late_agg.empty() && (!input::HasInput(current_tick, player_id) || already_late)) {
            input::EnqueueInput(current_tick, bundle.player, late_agg);
            rollback::RecordInput(current_tick, bundle.player, late_agg);
        }
    }
    return true;
}

bool handle_client_welcome_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint8 player_id = 0;
    Uint8 expected_players = 0;
    Uint32 seed = 0;
    Uint16 tick_ms = 0;
    if (!protocol::decode_welcome(buf, player_id, expected_players, seed, &tick_ms))
        return false;
    g_session.local_player = player_id;
    g_session.local_player_known = true;
    g_session.expected_players = expected_players;
    g_session.seed = seed;
    if (tick_ms != 0)
        g_session.tick_ms = tick_ms;
    input::SetTickTimestep(static_cast<double>(g_session.tick_ms) / 1000.0);
    input::SetExpectedPlayers(expected_players);
    debug_log("mp client: welcome player=%u expected=%u seed=%u tick_ms=%u",
              player_id, expected_players, seed, static_cast<unsigned>(g_session.tick_ms));
    if (debug_enabled())
        debug_log("mp client: transport=%s", transport_name(g_session.active_transport));
    // Do not send READY here.
    //
    // READY must mean "level pack switched + level loaded, waiting for NET_START".
    // If we send READY immediately after WELCOME, the host can start running while
    // the client is still switching packs (or even failing to load the level).
    return true;
}

bool handle_client_debug_options_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::DebugOptionsPacket msg;
    if (!protocol::decode_debug_options(buf, msg))
        return false;
    // Apply host-provided session settings before NET_START. Avoid mutating
    // simulation-affecting settings while the world is already running.
    const bool allow_sim_mutation = (g_session.phase != SessionState::Phase::RUNNING);
    apply_debug_options_to_options(msg, allow_sim_mutation);
    if (allow_sim_mutation && msg.tick_ms != 0) {
        g_session.tick_ms = msg.tick_ms;
        input::SetTickTimestep(static_cast<double>(g_session.tick_ms) / 1000.0);
    }
    if (debug_enabled()) {
        debug_log("mp client: debug options applied tick_ms=%u sim_mut=%d",
                  static_cast<unsigned>(msg.tick_ms),
                  allow_sim_mutation ? 1 : 0);
    }
    return true;
}

bool handle_client_ping_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::PingPacket ping;
    if (!protocol::decode_ping(buf, ping))
        return false;
    if (g_session.host)
        return true;
    protocol::PongPacket pong;
    pong.ping_id = ping.ping_id;
    ecl::Buffer out;
    protocol::encode_pong(out, pong);
    g_transport.ClientSendUnreliable(out);
    return true;
}

bool handle_client_sync_packet(const char *data, size_t len) {
    if (g_session.abort_pending)
        return true;
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::SyncPacket sync;
    if (!protocol::decode_sync(buf, sync))
        return false;
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
            debug_log("mp sync skip: sync tick=%u local tick=%u (no history)", sync.tick,
                      local_tick);
        }
    }
    return true;
}

bool handle_client_resync_state_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::ResyncState state;
    if (!protocol::decode_resync_state(buf, state))
        return false;
    if (g_session.active && state.epoch == g_session.input_epoch) {
        // NET_RESYNC_STATE can be sent via unreliable broadcast (debug/remote-control mode).
        // That path can reorder packets. Applying an older snapshot after a newer one
        // causes visible "backwards" zickzack motion.
        if (!g_session.host && state.tick <= g_session.last_accepted_resync_tick) {
            if (debug_enabled())
                debug_log("mp drop resync state: stale tick=%u last=%u", state.tick,
                          g_session.last_accepted_resync_tick);
            return true;
        }
    }
    // If we treat the local ball as remote-controlled, apply authoritative snapshots
    // immediately; rollback/replay adds latency and is not useful when we suppress
    // local simulation inputs.
    if (!g_session.host && options::GetBool("MultiplayerDebugRemoteControlLocalBall")) {
        apply_resync_state(state);
        if (g_session.active && state.epoch == g_session.input_epoch)
            g_session.last_accepted_resync_tick = state.tick;
        return true;
    }
    // If rollback is enabled, prefer reconciling via rollback/replay instead of
    // teleporting immediately. This reduces visible zig-zagging under packet loss.
    if (rollback::TryQueueReconcileResyncState(state)) {
        if (g_session.active && state.epoch == g_session.input_epoch)
            g_session.last_accepted_resync_tick = state.tick;
        return true;
    }
    apply_resync_state(state);
    if (g_session.active && state.epoch == g_session.input_epoch)
        g_session.last_accepted_resync_tick = state.tick;
    return true;
}

bool handle_client_world_state_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::WorldStatePacket pkt;
    if (!protocol::decode_world_state(buf, pkt))
        return false;
    if (!g_session.active)
        return true;
    if (pkt.epoch != g_session.input_epoch)
        return true;
    if (!g_session.host) {
        // Mitigation for "movable stones snap back" under latency/tick divergence:
        // ignore short-lived world mismatches so we don't apply a host snapshot taken
        // before the host processes the client's push, followed by another snapshot
        // that moves the stone forward again.
        const unsigned threshold = world_state_streak_threshold();
        if (g_session.world_only_desync_streak > 0 &&
            g_session.world_only_desync_streak < threshold) {
            if (debug_enabled())
                debug_log("mp drop world-state: pre-threshold tick=%u streak=%u threshold=%u",
                          pkt.tick, g_session.world_only_desync_streak, threshold);
            return true;
        }
    }
    // NET_WORLD_STATE may be broadcast via unreliable packets (debug stride) and can
    // arrive reordered under jitter/duplication. Applying an older snapshot after a
    // newer one causes visible "forward/backward" jumps for movable stones.
    if (!g_session.host &&
        g_session.last_accepted_world_state_tick != UINT32_MAX &&
        pkt.tick <= g_session.last_accepted_world_state_tick) {
        if (debug_enabled())
            debug_log("mp drop world-state: stale tick=%u last=%u",
                      pkt.tick, g_session.last_accepted_world_state_tick);
        return true;
    }
    const int w = Width();
    const int h = Height();
    if (w <= 0 || h <= 0)
        return true;
    if (static_cast<Uint16>(w) != pkt.width || static_cast<Uint16>(h) != pkt.height)
        return true;
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (pkt.floor_state.size() != count || pkt.stone_state.size() != count || pkt.item_state.size() != count)
        return true;

	    const bool have_kinds =
	        (pkt.floor_kind.size() == count && pkt.stone_kind.size() == count && pkt.item_kind.size() == count &&
	         !pkt.kind_dict.empty());
	    if (debug_enabled()) {
	        debug_log("mp world-state recv: tick=%u kinds=%d movable=%u", pkt.tick, have_kinds ? 1 : 0,
	                  static_cast<unsigned>(pkt.movable_stones.size()));
	    }

	    auto kind_lookup = [&pkt](Uint16 id) -> const std::string * {
        if (id == 0)
            return nullptr;
        const size_t idx = static_cast<size_t>(id - 1);
        if (idx >= pkt.kind_dict.size())
            return nullptr;
	        return &pkt.kind_dict[idx];
	    };

	    // Reconcile positions of movable stones (puzzle stones, doors, etc) before applying
	    // per-tile kind/state. This avoids destructive kill/recreate moves for movable stones.
	    if (!pkt.movable_stones.empty()) {
	        auto key = [](int x, int y) -> uint32_t {
	            return (static_cast<uint32_t>(x) << 16) | static_cast<uint32_t>(y);
	        };
	        auto key_to_pos = [](uint32_t k) -> GridPos {
	            int x = static_cast<int>((k >> 16) & 0xFFFFu);
	            int y = static_cast<int>(k & 0xFFFFu);
	            return GridPos(x, y);
	        };

	        std::unordered_map<uint32_t, uint32_t> current_pos_by_id;
	        current_pos_by_id.reserve(pkt.movable_stones.size() * 2);
	        for (int y = 0; y < h; ++y) {
	            for (int x = 0; x < w; ++x) {
	                GridPos p(x, y);
	                Stone *st = GetStone(p);
	                if (!st || !st->is_movable())
	                    continue;
	                current_pos_by_id[static_cast<uint32_t>(st->getId())] = key(x, y);
	            }
	        }

	        std::unordered_map<uint32_t, uint32_t> src_to_dst;
	        std::unordered_map<uint32_t, uint32_t> src_by_dst;
	        src_to_dst.reserve(pkt.movable_stones.size());
	        src_by_dst.reserve(pkt.movable_stones.size());
	        for (const auto &e : pkt.movable_stones) {
	            auto it = current_pos_by_id.find(static_cast<uint32_t>(e.object_id));
	            if (it == current_pos_by_id.end())
	                continue;
	            const uint32_t src = it->second;
	            const uint32_t dst = key(static_cast<int>(e.x), static_cast<int>(e.y));
	            if (src == dst)
	                continue;
	            src_to_dst[src] = dst;
	            src_by_dst[dst] = src;
	        }

	        // Resolve simple chains first: whenever a destination is empty, move into it.
	        bool progressed = true;
	        while (progressed) {
	            progressed = false;
	            for (auto it = src_to_dst.begin(); it != src_to_dst.end(); ++it) {
	                const uint32_t src = it->first;
	                const uint32_t dst = it->second;
	                GridPos dst_pos = key_to_pos(dst);
	                if (GetStone(dst_pos) != nullptr)
	                    continue;
	                MoveStone(key_to_pos(src), dst_pos);
	                src_by_dst.erase(dst);
	                src_to_dst.erase(it);
	                progressed = true;
	                break;
	            }
	        }

	        // Remaining moves are cycles. Break cycles by temporarily yielding one stone into memory.
	        while (!src_to_dst.empty()) {
	            const uint32_t start_src = src_to_dst.begin()->first;
	            const uint32_t start_dst = src_to_dst.begin()->second;
	            GridPos start_src_pos = key_to_pos(start_src);
	            Stone *held = YieldStone(start_src_pos);
	            if (!held) {
	                src_by_dst.erase(start_dst);
	                src_to_dst.erase(start_src);
	                continue;
	            }

	            uint32_t empty = start_src;
	            while (empty != start_dst) {
	                auto it_prev = src_by_dst.find(empty);
	                if (it_prev == src_by_dst.end()) {
	                    SetStone(start_src_pos, held);
	                    held = nullptr;
	                    src_to_dst.clear();
	                    src_by_dst.clear();
	                    break;
	                }
	                const uint32_t prev_src = it_prev->second;
	                MoveStone(key_to_pos(prev_src), key_to_pos(empty));
	                auto it_dst = src_to_dst.find(prev_src);
	                if (it_dst != src_to_dst.end())
	                    src_to_dst.erase(it_dst);
	                src_by_dst.erase(it_prev);
	                empty = prev_src;
	            }
	            if (held) {
	                SetStone(key_to_pos(start_dst), held);
	                src_by_dst.erase(start_dst);
	                src_to_dst.erase(start_src);
	            }
	        }
	    }

	    // If we have authoritative kinds, rebuild the grid from them first. This forces
	    // convergence for objects whose kind depends on non-"state" attributes.
	    if (have_kinds) {
	        int changed = 0;
	        int logged = 0;
	        const int kMaxLogged = 32;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
                GridPos p(x, y);

	                const std::string floor_kind = stable_world_kind(GetFloor(p));
	                const std::string stone_kind = stable_world_kind(GetStone(p));
	                const std::string item_kind = stable_world_kind(GetItem(p));

                const std::string *want_floor = kind_lookup(pkt.floor_kind[idx]);
                const std::string *want_stone = kind_lookup(pkt.stone_kind[idx]);
                const std::string *want_item = kind_lookup(pkt.item_kind[idx]);

                const std::string want_floor_str = (want_floor ? *want_floor : std::string());
                const std::string want_stone_str = (want_stone ? *want_stone : std::string());
                const std::string want_item_str = (want_item ? *want_item : std::string());

                if (want_floor_str != floor_kind) {
                    if (debug_enabled() && logged < kMaxLogged) {
                        debug_log("mp world-state kind fix: pos=(%d,%d) layer=floor cur=%s want=%s", x, y,
                                  floor_kind.empty() ? "(none)" : floor_kind.c_str(),
                                  want_floor_str.empty() ? "(none)" : want_floor_str.c_str());
                        logged += 1;
                    }
                    if (!want_floor)
                        KillFloor(p);
                    else
                        SetFloor(p, MakeFloor(want_floor->c_str()));
                    changed += 1;
                }
                if (want_stone_str != stone_kind) {
                    if (debug_enabled() && logged < kMaxLogged) {
                        debug_log("mp world-state kind fix: pos=(%d,%d) layer=stone cur=%s want=%s", x, y,
                                  stone_kind.empty() ? "(none)" : stone_kind.c_str(),
                                  want_stone_str.empty() ? "(none)" : want_stone_str.c_str());
                        logged += 1;
                    }
                    if (!want_stone)
                        KillStone(p);
                    else
                        SetStone(p, MakeStone(want_stone->c_str()));
                    changed += 1;
                }
                if (want_item_str != item_kind) {
                    if (debug_enabled() && logged < kMaxLogged) {
                        debug_log("mp world-state kind fix: pos=(%d,%d) layer=item cur=%s want=%s", x, y,
                                  item_kind.empty() ? "(none)" : item_kind.c_str(),
                                  want_item_str.empty() ? "(none)" : want_item_str.c_str());
                        logged += 1;
                    }
                    if (!want_item)
                        KillItem(p);
                    else
                        SetItem(p, MakeItem(want_item->c_str()));
                    changed += 1;
                }
            }
        }
	        if (debug_enabled())
	            debug_log("mp world-state apply kinds: changed=%d", changed);
	    }

	    // Apply authoritative oxyd colors before per-tile state so subsequent MpForceExternalState
	    // uses the right model variant.
	    if (!pkt.oxyd_colors.empty()) {
        int changed = 0;
        for (const auto &e : pkt.oxyd_colors) {
            const int x = static_cast<int>(e.x);
            const int y = static_cast<int>(e.y);
            if (x < 0 || y < 0 || x >= w || y >= h)
                continue;
            GridPos p(x, y);
            if (OxydStone *ox = dynamic_cast<OxydStone *>(GetStone(p))) {
                const int color = static_cast<int>(static_cast<int16_t>(e.color_raw));
                if (static_cast<int>(ox->getAttr("oxydcolor")) != color) {
                    ox->MpForceOxydColor(color);
                    changed += 1;
                }
            }
        }
        if (debug_enabled() && changed > 0)
            debug_log("mp world-state apply oxydcolor: changed=%d", changed);
    }

    auto apply_state = [](Object *obj, Uint16 s) {
        if (!obj || s == 0xFFFF)
            return;
        // Some StateObject subclasses (notably OxydStone) implement "state" as gameplay
        // operations (tryOpen/close) that are not idempotent and can refuse to close
        // (OPEN_PAIR). World-state reconciliation must be able to override them.
        if (Stone *st = dynamic_cast<Stone *>(obj)) {
            if (OxydStone *ox = dynamic_cast<OxydStone *>(st)) {
                ox->MpForceExternalState(static_cast<int>(s));
                return;
            }
        }
        obj->setAttr("state", Value(static_cast<int>(s)));
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            GridPos p(x, y);
            apply_state(GetFloor(p), pkt.floor_state[idx]);
            apply_state(GetStone(p), pkt.stone_state[idx]);
            apply_state(GetItem(p), pkt.item_state[idx]);
        }
    }
    if (debug_enabled()) {
        debug_log("mp world-state applied: world=%llu kind=%llu state=%llu movable=%llu",
                  static_cast<unsigned long long>(WorldGridChecksum()),
                  static_cast<unsigned long long>(WorldGridKindChecksum()),
                  static_cast<unsigned long long>(WorldGridStateChecksum()),
                  static_cast<unsigned long long>(WorldGridMovableStoneChecksum()));
    }
    if (!g_session.host)
        g_session.last_accepted_world_state_tick = pkt.tick;
    return true;
}

bool handle_client_start_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 epoch = 0;
    Uint32 load_id = 0;
    if (!protocol::decode_start(buf, epoch, load_id))
        return false;
    if (load_id != g_session.last_load_id) {
        if (debug_enabled())
            debug_log("mp client: drop start (load mismatch remote=%u local=%u)",
                      static_cast<unsigned>(load_id),
                      static_cast<unsigned>(g_session.last_load_id));
        return true;
    }
    g_session.input_epoch = epoch;
    g_session.debug_state_dumped = false;
    configure_input_session(g_session.expected_players);
    g_session.phase = SessionState::Phase::READY_TO_START;
    debug_log("mp client: start allowed");
    return true;
}

bool handle_client_pause_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 epoch = 0;
    bool paused = false;
    if (!protocol::decode_pause(buf, epoch, paused))
        return false;
    if (epoch != g_session.input_epoch)
        return true;
    if (g_session.paused == paused)
        return true;
    g_session.paused = paused;
    server::Msg_Pause(paused);
    return true;
}

bool handle_client_abort_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 epoch = 0;
    if (!protocol::decode_abort(buf, epoch))
        return false;
    if (epoch != g_session.input_epoch)
        return true;
    return abort_session_with_message("Game aborted. Ending session.");
}

bool handle_client_restart_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::RestartPacket restart;
    if (!protocol::decode_restart(buf, restart))
        return false;
    if (restart.restart_id > g_session.last_restart_id) {
        g_session.last_restart_id = restart.restart_id;
        g_session.restart_id = restart.restart_id;
        if (restart.level_restart)
            server::RestartLevelFromNetwork();
        else
            server::Msg_RestartGameFromNetwork();
    }
    return true;
}

bool handle_client_load_level_packet(const char *data, size_t len) {
    if (g_session.abort_pending)
        return true;
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::LoadLevelPacket msg;
    if (!protocol::decode_load_level(buf, msg))
        return false;
    if (msg.load_id <= g_session.last_load_id)
        return true;
    g_session.last_load_id = msg.load_id;
    debug_log("mp client: load level id=%u pack=%s level_id=%s",
              static_cast<unsigned>(msg.load_id),
              msg.pack_name.c_str(),
              msg.level_id.c_str());

    if (!msg.pack_name.empty())
        server::Msg_SetLevelPack(msg.pack_name);

    lev::Proxy *proxy = find_level_proxy_in_current_index(msg.level_id);
    if (!proxy)
        proxy = find_level_proxy_anywhere(msg.level_id);
    if (!proxy)
        return abort_session_with_message("Selected level not available. Ending session.");

    server::Msg_LoadLevel(proxy, false);
    return true;
}

bool handle_client_placement_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::PlacementPacket place;
    if (!protocol::decode_place(buf, place))
        return false;
    if (place.restart_id == g_session.restart_id && place.player < g_session.expected_players) {
        GridPos pos(static_cast<int>(place.x), static_cast<int>(place.y));
        apply_placement(place.player, pos);
    }
    return true;
}

bool handle_client_owner_actor_state_packet(const char *data, size_t len);

void handle_client_payload(const char *data, size_t len) {
    if (handle_client_input_bundle_packet(data, len))
        return;
    if (handle_client_input_packet(data, len))
        return;
    if (handle_client_debug_options_packet(data, len))
        return;
    if (handle_client_ping_packet(data, len))
        return;
    if (handle_client_welcome_packet(data, len))
        return;
    if (handle_client_load_level_packet(data, len))
        return;
    if (handle_client_sync_packet(data, len))
        return;
    if (handle_client_resync_state_packet(data, len))
        return;
    if (handle_client_owner_actor_state_packet(data, len))
        return;
    if (handle_client_world_state_packet(data, len))
        return;
    if (handle_client_start_packet(data, len))
        return;
    if (handle_client_pause_packet(data, len))
        return;
    if (handle_client_abort_packet(data, len))
        return;
    if (handle_client_restart_packet(data, len))
        return;
    handle_client_placement_packet(data, len);
}

bool handle_client_owner_actor_state_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::OwnerActorStatePacket pkt;
    if (!protocol::decode_owner_actor_state(buf, pkt))
        return false;
    if (g_session.host)
        return true;
    if (!g_session.active)
        return true;
    if (!options::GetBool("MultiplayerDebugClientAuthBallPos"))
        return true;
    if (pkt.epoch != g_session.input_epoch)
        return true;
    const unsigned player_id = static_cast<unsigned>(pkt.player);
    if (player_id >= g_session.expected_players)
        return true;
    if (g_session.local_player_known && player_id == g_session.local_player)
        return true;  // local is authoritative in this mode

    if (player_id < g_session.last_accepted_owner_state_tick.size()) {
        const uint32_t last = g_session.last_accepted_owner_state_tick[player_id];
        if (last != 0 && pkt.tick <= last)
            return true;
        g_session.last_accepted_owner_state_tick[player_id] = pkt.tick;
    }
    if (Actor *a = find_controlled_steerable_actor_by_owner_state(player_id, pkt)) {
        const uint32_t current_tick = input::CurrentTick();
        uint32_t max_project_ticks = g_session.input_delay ? g_session.input_delay : 4u;
        if (max_project_ticks < 2u)
            max_project_ticks = 2u;
        if (max_project_ticks > 8u)
            max_project_ticks = 8u;
        uint32_t dt_ticks = 0;
        if (current_tick > pkt.tick)
            dt_ticks = current_tick - pkt.tick;
        if (dt_ticks > max_project_ticks) {
            if (debug_enabled())
                debug_log("mp client: owner-state lag clamp tick=%u cur=%u dt=%u max=%u",
                          pkt.tick, current_tick, dt_ticks, max_project_ticks);
            dt_ticks = max_project_ticks;
        }
        const float dt = static_cast<float>(input::TickTimestep() * static_cast<double>(dt_ticks));
        const float x = pkt.x + pkt.vx * dt;
        const float y = pkt.y + pkt.vy * dt;
        teleport_actor_physics_only(a, x, y, pkt.vx, pkt.vy);
    }
    if (debug_enabled() && pkt.tick < 20) {
        debug_log("mp client: recv owner-state tick=%u player=%u obj=%u pos=(%.2f,%.2f) vel=(%.2f,%.2f)",
                  pkt.tick, static_cast<unsigned>(pkt.player), static_cast<unsigned>(pkt.object_id),
                  static_cast<double>(pkt.x), static_cast<double>(pkt.y),
                  static_cast<double>(pkt.vx), static_cast<double>(pkt.vy));
    }
    return true;
}

bool host_register_relay_client(Uint32 client_id, HostSource source);
bool host_unregister_relay_client(Uint32 client_id, HostSource source);

bool host_register_udp_relay_client(Uint32 client_id) {
    return host_register_relay_client(client_id, HostSource::UDP_RELAY);
}

bool host_register_tcp_relay_client(Uint32 client_id) {
    return host_register_relay_client(client_id, HostSource::TCP_RELAY);
}

bool host_unregister_udp_relay_client(Uint32 client_id) {
    return host_unregister_relay_client(client_id, HostSource::UDP_RELAY);
}

bool host_unregister_tcp_relay_client(Uint32 client_id) {
    return host_unregister_relay_client(client_id, HostSource::TCP_RELAY);
}

bool host_register_relay_client(Uint32 client_id, HostSource source) {
    if (!can_accept_more_remote_players())
        return false;

    unsigned player_id = 0;
    if (!allocate_remote_player_id(player_id))
        return false;
    ecl::Buffer welcome;
    protocol::encode_welcome(welcome, static_cast<Uint8>(player_id),
                             static_cast<Uint8>(g_session.expected_players), g_session.seed,
                             static_cast<Uint16>(g_session.tick_ms));

    if (source == HostSource::UDP_RELAY) {
        g_session.relay_players[client_id] = player_id;
        g_session.relay_ready[client_id] = false;
        debug_log("mp host: relay client -> player %u", player_id);
        g_transport.HostSendUdpRelay(client_id, welcome);
        host_broadcast_debug_options();
        send_existing_placements_to_relay(client_id);
        host_send_current_load_to_remote(HostSource::UDP_RELAY, nullptr, client_id);
        return true;
    }

    if (source == HostSource::TCP_RELAY) {
        g_session.tcp_relay_players[client_id] = player_id;
        g_session.tcp_relay_ready[client_id] = false;
        debug_log("mp host: tcp relay client -> player %u", player_id);
        g_transport.HostSendTcpRelay(client_id, welcome);
        host_broadcast_debug_options();
        send_existing_placements_to_tcp_relay(client_id);
        host_send_current_load_to_remote(HostSource::TCP_RELAY, nullptr, client_id);
        return true;
    }

    return false;
}

bool host_unregister_relay_client(Uint32 client_id, HostSource source) {
    if (source == HostSource::UDP_RELAY) {
        auto it = g_session.relay_players.find(client_id);
        if (it != g_session.relay_players.end())
            release_remote_player_id(it->second);
        g_session.relay_players.erase(client_id);
        g_session.relay_ready.erase(client_id);
    } else if (source == HostSource::TCP_RELAY) {
        auto it = g_session.tcp_relay_players.find(client_id);
        if (it != g_session.tcp_relay_players.end())
            release_remote_player_id(it->second);
        g_session.tcp_relay_players.erase(client_id);
        g_session.tcp_relay_ready.erase(client_id);
    } else {
        return true;
    }
    if (g_session.active)
        return abort_session_with_message("Player disconnected. Ending session.");
    return true;
}

bool handle_direct_connect_event(ENetPeer *peer) {
    if (!g_session.host)
        return true;
    if (!can_accept_more_remote_players()) {
        // During pre-start, allow retries: if an existing direct peer has not
        // reached READY yet, drop it and accept the new connection attempt.
        // This prevents LAN sessions getting stuck forever on a half-connected
        // peer (common with VM/NAT/multi-homing quirks).
        if (g_session.phase != SessionState::Phase::RUNNING) {
            ENetPeer *replace = nullptr;
            for (const auto &entry : g_session.peer_ready) {
                if (!entry.second) {
                    replace = entry.first;
                    break;
                }
            }
            if (replace) {
                unsigned old_id = 0;
                auto it = g_session.peer_players.find(replace);
                if (it != g_session.peer_players.end())
                    old_id = it->second;
                if (debug_enabled()) {
                    char rip[64];
                    rip[0] = '\0';
                    std::string replace_ip = address_to_ip_string(replace->address);
                    if (replace_ip.empty())
                        std::snprintf(rip, sizeof(rip), "<unknown>");
                    else
                        std::snprintf(rip, sizeof(rip), "%s", replace_ip.c_str());
                    debug_log("mp host: dropping unready peer player=%u ip=%s:%u (retry connect)",
                              old_id, rip, static_cast<unsigned>(replace->address.port));
                }
                g_session.peer_players.erase(replace);
                g_session.peer_ready.erase(replace);
                release_remote_player_id(old_id);
                enet_peer_reset(replace);
            }
        }
        if (!can_accept_more_remote_players()) {
            enet_peer_disconnect(peer, 0);
            return true;
        }
    }

    char host_ip[64];
    host_ip[0] = '\0';
    std::string peer_ip = address_to_ip_string(peer->address);
    if (peer_ip.empty())
        std::snprintf(host_ip, sizeof(host_ip), "<unknown>");
    else
        std::snprintf(host_ip, sizeof(host_ip), "%s", peer_ip.c_str());

    unsigned player_id = 0;
    if (!allocate_remote_player_id(player_id)) {
        enet_peer_disconnect(peer, 0);
        return true;
    }
    g_session.peer_players[peer] = player_id;
    g_session.peer_ready[peer] = false;
    peer->data = reinterpret_cast<void *>(static_cast<uintptr_t>(player_id));
#ifdef ENET_VER_EQ_GT_13
    enet_peer_timeout(peer, ENET_PEER_TIMEOUT_LIMIT,
                      static_cast<enet_uint32>(multiplayer::wait::kEnetPeerTimeoutMs),
                      static_cast<enet_uint32>(multiplayer::wait::kEnetPeerTimeoutMs));
#endif
    debug_log("mp host: peer connected -> player %u ip=%s:%u", player_id, host_ip,
              static_cast<unsigned>(peer->address.port));

    ecl::Buffer buf;
    protocol::encode_welcome(buf, static_cast<Uint8>(player_id),
                             static_cast<Uint8>(g_session.expected_players), g_session.seed,
                             static_cast<Uint16>(g_session.tick_ms));
    g_transport.HostSendDirect(peer, buf);
    host_broadcast_debug_options();
    send_existing_placements_to_peer(peer);
    host_send_current_load_to_remote(HostSource::DIRECT, peer, 0);
    g_transport.Flush();
    return true;
}

bool handle_relay_payload_as_host(const char *data, size_t len, HostSource source,
                                  bool close_on_decode_error, bool (*on_connect)(Uint32),
                                  bool (*on_disconnect)(Uint32)) {
    RelayMessageType type = RELAY_ERROR;
    Uint32 session_id = 0;
    Uint32 client_id = 0;
    const char *payload = nullptr;
    size_t payload_len = 0;
    if (!decode_relay_header(data, len, type, session_id, client_id, payload, payload_len)) {
        if (close_on_decode_error)
            tcp_close(g_session.tcp_relay_socket);
        return true;
    }
    if (session_id != g_session.session_id)
        return true;
    if (type == RELAY_CLIENT_CONNECT) {
        return on_connect(client_id);
    }
    if (type == RELAY_CLIENT_DISCONNECT)
        return on_disconnect(client_id);
    if (type == RELAY_DATA) {
        // Propagate abort/disconnect requests back to the poll loop.
        if (!handle_host_packet(payload, payload_len, nullptr, source, client_id))
            return false;
    }
    return true;
}

bool handle_udp_relay_payload(const char *data, size_t len) {
    return handle_relay_payload_as_host(data, len, HostSource::UDP_RELAY, false,
                                        host_register_udp_relay_client,
                                        host_unregister_udp_relay_client);
}

bool handle_tcp_relay_payload(const char *data, size_t len) {
    if (!g_session.host) {
        if (g_session.active_transport == TransportKind::TCP_RELAY)
            handle_client_payload(data, len);
        // The client can receive a session-ending message (e.g. NET_ABORT) via relay.
        // If that happens, stop polling immediately because SessionShutdown may have
        // destroyed sockets/ENet hosts still being serviced by the transport loop.
        if (!g_session.active)
            return false;
        return true;
    }
    return handle_relay_payload_as_host(data, len, HostSource::TCP_RELAY, true,
                                        host_register_tcp_relay_client,
                                        host_unregister_tcp_relay_client);
}

}  // namespace

void process_network_events() {
    if (!g_session.active)
        return;
    // Poll all active transports via one facade.
    class Sink : public ITransportSink {
    public:
        bool OnConnect(HostSource source, ENetPeer *peer) override {
            if (source != HostSource::DIRECT)
                return true;
            return handle_direct_connect_event(peer);
        }
        bool OnDisconnect(HostSource source, ENetPeer *peer) override {
            if (source == HostSource::UDP_RELAY) {
                if (g_session.relay_peer == peer) {
                    g_session.relay_peer = nullptr;
                    if (!g_session.relay_players.empty())
                        return abort_session_with_message("Relay disconnected. Ending session.");
                }
                return true;
            }
            if (source == HostSource::TCP_RELAY) {
                if (g_session.host) {
                    if (!g_session.tcp_relay_players.empty())
                        return abort_session_with_message("TCP relay disconnected. Ending session.");
                    return true;
                }
                if (g_session.active_transport == TransportKind::TCP_RELAY)
                    return abort_session_with_message("Disconnected from TCP relay.");
                return true;
            }
            return handle_direct_disconnect_event(peer);
        }
        bool OnPayload(HostSource source, ENetPeer *peer, const char *data, size_t len) override {
            // Any packet counts as progress (avoid false "host disconnected" aborts).
            g_session.no_payload_timer = 0.0;
            if (source == HostSource::UDP_RELAY) {
                return handle_udp_relay_payload(data, len);
            }
            if (source == HostSource::TCP_RELAY) {
                return handle_tcp_relay_payload(data, len);
            }
            return handle_direct_payload(peer, data, len);
        }
    private:
        bool handle_direct_payload(ENetPeer *peer, const char *data, size_t len) {
            if (g_session.host)
                return handle_host_packet(data, len, peer, HostSource::DIRECT, 0);
            if (g_session.active_transport != TransportKind::TCP_RELAY)
                handle_client_payload(data, len);
            if (!g_session.active)
                return false;
            return true;
        }
        bool handle_direct_disconnect_event(ENetPeer *peer) {
            return handle_direct_disconnect_event_impl(peer);
        }
        static bool handle_direct_disconnect_event_impl(ENetPeer *peer) {
            if (g_session.host) {
                auto it = g_session.peer_players.find(peer);
                if (it == g_session.peer_players.end()) {
                    // Ignore disconnects for peers we already replaced/dropped.
                    return true;
                }
                release_remote_player_id(it->second);
                g_session.peer_players.erase(it);
                g_session.peer_ready.erase(peer);
                if (g_session.active)
                    return abort_session_with_message("Player disconnected. Ending session.");
                return true;
            }
            if (g_session.server_peer == peer)
                g_session.server_peer = nullptr;
            return abort_session_with_message("Disconnected from host.");
        }
    };

    Sink sink;
    g_transport.Poll(sink);
}

void send_local_inputs() {
    if (!g_session.active || !g_session.local_player_known)
        return;
    const bool remote_control_local_ball =
        (!g_session.host && options::GetBool("MultiplayerDebugRemoteControlLocalBall"));
    uint32_t current_tick = input::CurrentTick();
    uint32_t delay = g_session.input_delay ? g_session.input_delay : kInputDelay;
    uint32_t target_tick = current_tick + delay;
	    if (g_session.input_clock_tick > current_tick) {
	        uint32_t extra = g_session.input_clock_tick - current_tick;
	        if (extra > kMaxInputLead)
	            extra = kMaxInputLead;
	        target_tick += extra;
	    }
	    const uint32_t ticks_to_fill =
	        (g_session.next_local_tick <= target_tick) ? (target_tick - g_session.next_local_tick + 1) : 0;
	    // IMPORTANT: `send_local_inputs()` runs every frame, but ticks advance at
	    // the simulation tick rate. When we already filled inputs up to `target_tick`,
	    // we must not drain (and thus drop) local pending input. Otherwise large
	    // tick lengths (e.g. 50ms) feel unresponsive because most mouse deltas get
	    // discarded between ticks.
	    input::PlayerInput pending;
	    if (ticks_to_fill)
	        pending = input::DrainLocalPending(g_session.local_player);
	    // Local pending inputs are accumulated across frames. If we need to fill
	    // multiple ticks at once (e.g. after a stall), distribute the accumulated
	    // mouse-force impulse across those ticks so physics stays consistent.
	    const float inv_ticks = ticks_to_fill ? (1.0f / static_cast<float>(ticks_to_fill)) : 0.0f;
	    const ecl::V2 per_tick_force = pending.mouse_force * inv_ticks;
    bool applied_actions = false;
    bool sent_packets = false;

    while (g_session.next_local_tick <= target_tick) {
        input::PlayerInput send;
        send.mouse_force = per_tick_force;
        if (!applied_actions) {
            send.rotate_steps = pending.rotate_steps;
            send.activate_count = pending.activate_count;
        } else {
            send.rotate_steps = 0;
            send.activate_count = 0;
        }
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

        // Experiment: let the host be authoritative for the locally controlled ball.
        // Suppress continuous mouse force locally, but keep discrete actions (rotate/activate)
        // so inventory/world interactions remain responsive and deterministic.
        input::PlayerInput local_sim = send;
        if (remote_control_local_ball)
            local_sim.mouse_force = ecl::V2(0.0f, 0.0f);
        input::EnqueueInput(pkt.tick, g_session.local_player, local_sim);
        rollback::RecordInput(pkt.tick, g_session.local_player, local_sim);
        g_session.local_history[pkt.tick] = send;
        applied_actions = true;
        ++g_session.next_local_tick;
    }

    // Keep a small history so we can resend a few ticks for redundancy.
    if (current_tick > kInputHistoryKeepTicks) {
        const uint32_t prune_before = current_tick - kInputHistoryKeepTicks;
        for (auto it = g_session.local_history.begin(); it != g_session.local_history.end(); ) {
            if (it->first < prune_before)
                it = g_session.local_history.erase(it);
            else
                ++it;
        }
    }

    if (g_session.host) {
        if (!has_remote_peers())
            return;
    } else if (!g_session.server_peer) {
        if (!(g_session.active_transport == TransportKind::TCP_RELAY &&
              tcp_socket_valid(g_session.tcp_relay_socket))) {
            return;
        }
    }

    auto bundle_back_ticks = []() -> uint32_t {
        switch (g_session.active_transport) {
        case TransportKind::UDP_RELAY:
            return kInputBundleBackTicksUdpRelay;
        case TransportKind::TCP_RELAY:
            return kInputBundleBackTicksTcpRelay;
        case TransportKind::DIRECT:
        default:
            return kInputBundleBackTicksDirect;
        }
    };
    auto bundle_max_count = []() -> uint32_t {
        switch (g_session.active_transport) {
        case TransportKind::UDP_RELAY:
            return kInputBundleMaxCountUdpRelay;
        case TransportKind::TCP_RELAY:
            return kInputBundleMaxCountTcpRelay;
        case TransportKind::DIRECT:
        default:
            return kInputBundleMaxCountDirect;
        }
    };
    const uint32_t back_ticks = bundle_back_ticks();
    const uint32_t max_count = bundle_max_count();

    uint32_t send_base = g_session.next_send_tick;
    uint32_t min_send_tick = current_tick;
    if (send_base < min_send_tick)
        send_base = min_send_tick;
    uint32_t bundle_start = 0;
    if (send_base > back_ticks)
        bundle_start = send_base - back_ticks;
    if (bundle_start < min_send_tick)
        bundle_start = min_send_tick;
    uint32_t bundle_end = g_session.next_local_tick;
    if (bundle_end > bundle_start + max_count)
        bundle_end = bundle_start + max_count;

    if (bundle_start < bundle_end) {
        protocol::InputBundlePacket bundle;
        bundle.epoch = g_session.input_epoch;
        bundle.first_tick = bundle_start;
        bundle.player = static_cast<Uint8>(g_session.local_player);
        const uint32_t count = bundle_end - bundle_start;
        bundle.entries.clear();
        bundle.entries.reserve(count);
        for (uint32_t tick = bundle_start; tick < bundle_end; ++tick) {
            protocol::InputBundleEntry e;
            auto it = g_session.local_history.find(tick);
            if (it != g_session.local_history.end()) {
                const input::PlayerInput &queued = it->second;
                e.mouse_x = static_cast<float>(queued.mouse_force[0]);
                e.mouse_y = static_cast<float>(queued.mouse_force[1]);
                e.rotate_steps = static_cast<int16_t>(queued.rotate_steps);
                e.activate_count = static_cast<Uint8>(queued.activate_count);
            } else {
                e.mouse_x = 0.0f;
                e.mouse_y = 0.0f;
                e.rotate_steps = 0;
                e.activate_count = 0;
            }
            bundle.entries.push_back(e);
        }
        if (bundle.first_tick < 20) {
            debug_log("mp send input bundle: first tick=%u player=%u count=%u",
                      bundle.first_tick, bundle.player,
                      static_cast<unsigned>(bundle.entries.size()));
        }
        if (g_session.host)
            broadcast_input_bundle(bundle, nullptr, 0, 0);
        else {
            ecl::Buffer payload;
            protocol::encode_input_bundle(payload, bundle);
            g_transport.ClientSendUnreliable(payload);
        }
        sent_packets = true;
        g_session.next_send_tick = bundle_end;
    }

    const bool client_auth_ball_pos =
        (!g_session.host && options::GetBool("MultiplayerDebugClientAuthBallPos"));
    if (client_auth_ball_pos && !remote_control_local_ball) {
        const uint32_t tick = input::CurrentTick();
        if (g_session.last_sent_owner_state_tick != tick) {
            g_session.last_sent_owner_state_tick = tick;
            std::vector<Actor *> actors;
            GetActors(actors);
            for (Actor *a : actors) {
                if (!a)
                    continue;
                if (!a->isSteerable())
                    continue;
                if (!a->controlled_by(static_cast<int>(g_session.local_player)))
                    continue;
                protocol::OwnerActorStatePacket st;
                st.epoch = g_session.input_epoch;
                st.tick = tick;
                st.player = static_cast<Uint8>(g_session.local_player);
                st.object_id = static_cast<Uint32>(a->getId());
                st.actor_id = static_cast<Uint16>(get_id(a));
                st.name_hash = static_cast<Uint32>(stable_name_hash(a));
                const ecl::V2 &pos = a->get_pos();
                const ecl::V2 &vel = a->get_vel();
                st.x = static_cast<float>(pos[0]);
                st.y = static_cast<float>(pos[1]);
                st.vx = static_cast<float>(vel[0]);
                st.vy = static_cast<float>(vel[1]);
                ecl::Buffer payload;
                protocol::encode_owner_actor_state(payload, st);
                g_transport.ClientSendUnreliable(payload);
                sent_packets = true;
                if (debug_enabled() && tick < 20) {
                    debug_log("mp client: send owner-state tick=%u player=%u obj=%u pos=(%.2f,%.2f) vel=(%.2f,%.2f)",
                              tick,
                              static_cast<unsigned>(st.player),
                              static_cast<unsigned>(st.object_id),
                              static_cast<double>(st.x),
                              static_cast<double>(st.y),
                              static_cast<double>(st.vx),
                              static_cast<double>(st.vy));
                }
            }
        }
    }

    if (sent_packets)
        g_transport.Flush();
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
