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
#include "world.hh"

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
    return true;
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
    if (input_msg.tick < current_tick) {
        if (!input::ZerofillMissingInputsEnabled())
            return true;
        if (!rollback::Enabled()) {
            // In zerofill mode without rollback/replay, clamp late input samples to the
            // current tick so they still affect gameplay instead of being dropped forever.
            input_msg.tick = current_tick;
        } else if (input_msg.tick < rollback::EarliestTick(current_tick)) {
            // Too old to roll back to safely; fall back to legacy clamping behavior.
            input_msg.tick = current_tick;
        }
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
            // If rollback can still replay this tick, keep it as-is.
            if (rollback::Enabled() && tick >= rollback::EarliestTick(current_tick)) {
                input::EnqueueInput(tick, player_id, pi);
                rollback::RecordInput(tick, player_id, pi);
                continue;
            }
            // Otherwise, best-effort apply at `current_tick` once.
            have_late = true;
            late_agg.rotate_steps += pi.rotate_steps;
            late_agg.activate_count += pi.activate_count;
            if (pi.mouse_force[0] != 0.0f || pi.mouse_force[1] != 0.0f) {
                late_mouse_set = true;
                late_agg.mouse_force = pi.mouse_force;
            }
            continue;
        }
        input::EnqueueInput(tick, player_id, pi);
        rollback::RecordInput(tick, player_id, pi);
    }
    if (have_late) {
        if (!late_mouse_set) {
            late_agg.mouse_force = ecl::V2(0.0f, 0.0f);
        }
        if (!late_agg.empty() && !input::HasInput(current_tick, player_id)) {
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
    // For now broadcast to all peers. This keeps everyone converging even if only
    // one client noticed the mismatch.
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

bool handle_host_packet(const char *data, size_t len, ENetPeer *peer, HostSource source,
                        Uint32 relay_client_id) {
    if (handle_host_input_bundle_packet(data, len, peer, source, relay_client_id))
        return true;
    if (handle_host_input_packet(data, len, peer, source, relay_client_id))
        return true;
    if (handle_host_resync_request_packet(data, len, source, relay_client_id, peer))
        return true;
    if (handle_host_world_state_request_packet(data, len))
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
    if (input_msg.tick < current_tick) {
        if (!input::ZerofillMissingInputsEnabled())
            return true;
        if (!rollback::Enabled())
            input_msg.tick = current_tick;
        else if (input_msg.tick < rollback::EarliestTick(current_tick))
            input_msg.tick = current_tick;
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
            if (rollback::Enabled() && tick >= rollback::EarliestTick(current_tick)) {
                input::EnqueueInput(tick, bundle.player, pi);
                rollback::RecordInput(tick, bundle.player, pi);
                continue;
            }
            have_late = true;
            late_agg.rotate_steps += pi.rotate_steps;
            late_agg.activate_count += pi.activate_count;
            if (pi.mouse_force[0] != 0.0f || pi.mouse_force[1] != 0.0f) {
                late_mouse_set = true;
                late_agg.mouse_force = pi.mouse_force;
            }
            continue;
        }
        input::EnqueueInput(tick, bundle.player, pi);
        rollback::RecordInput(tick, bundle.player, pi);
    }
    if (have_late) {
        if (!late_mouse_set)
            late_agg.mouse_force = ecl::V2(0.0f, 0.0f);
        if (!late_agg.empty() && !input::HasInput(current_tick, bundle.player)) {
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
    if (!protocol::decode_welcome(buf, player_id, expected_players, seed))
        return false;
    g_session.local_player = player_id;
    g_session.local_player_known = true;
    g_session.expected_players = expected_players;
    g_session.seed = seed;
    input::SetExpectedPlayers(expected_players);
    debug_log("mp client: welcome player=%u expected=%u seed=%u", player_id, expected_players, seed);
    if (debug_enabled())
        debug_log("mp client: transport=%s", transport_name(g_session.active_transport));
    // Do not send READY here.
    //
    // READY must mean "level pack switched + level loaded, waiting for NET_START".
    // If we send READY immediately after WELCOME, the host can start running while
    // the client is still switching packs (or even failing to load the level).
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
    apply_resync_state(state);
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
    const int w = Width();
    const int h = Height();
    if (w <= 0 || h <= 0)
        return true;
    if (static_cast<Uint16>(w) != pkt.width || static_cast<Uint16>(h) != pkt.height)
        return true;
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (pkt.floor_state.size() != count || pkt.stone_state.size() != count || pkt.item_state.size() != count)
        return true;

    // First, reconcile positions of movable stones (puzzle stones, doors, etc).
    // Apply this before per-tile state so "stone_state" lands on the correct object.
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
        std::unordered_set<uint32_t> src_keys;
        std::unordered_set<uint32_t> dst_keys;
        src_to_dst.reserve(pkt.movable_stones.size());
        src_by_dst.reserve(pkt.movable_stones.size());
        src_keys.reserve(pkt.movable_stones.size());
        dst_keys.reserve(pkt.movable_stones.size());
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
            src_keys.insert(src);
            dst_keys.insert(dst);
        }

        uint32_t buffer_key = 0;
        bool have_buffer = false;
        for (int y = 0; y < h && !have_buffer; ++y) {
            for (int x = 0; x < w && !have_buffer; ++x) {
                uint32_t k = key(x, y);
                if (src_keys.count(k) || dst_keys.count(k))
                    continue;
                GridPos p(x, y);
                if (GetStone(p) != nullptr)
                    continue;
                buffer_key = k;
                have_buffer = true;
            }
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

        // Remaining moves are cycles. Break each cycle using the empty buffer cell.
        while (!src_to_dst.empty() && have_buffer) {
            const uint32_t start_src = src_to_dst.begin()->first;
            const uint32_t start_dst = src_to_dst.begin()->second;
            MoveStone(key_to_pos(start_src), key_to_pos(buffer_key));

            uint32_t empty = start_src;
            while (empty != start_dst) {
                auto it_prev = src_by_dst.find(empty);
                if (it_prev == src_by_dst.end()) {
                    // Unexpected shape; stop trying to reorder this snapshot.
                    break;
                }
                const uint32_t prev_src = it_prev->second;
                MoveStone(key_to_pos(prev_src), key_to_pos(empty));
                auto it_dst = src_to_dst.find(prev_src);
                if (it_dst != src_to_dst.end()) {
                    src_to_dst.erase(it_dst);
                }
                src_by_dst.erase(it_prev);
                empty = prev_src;
            }
            MoveStone(key_to_pos(buffer_key), key_to_pos(start_dst));
            src_by_dst.erase(start_dst);
            src_to_dst.erase(start_src);
        }
    }

    auto apply_state = [](Object *obj, Uint16 s) {
        if (!obj || s == 0xFFFF)
            return;
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

void handle_client_payload(const char *data, size_t len) {
    if (handle_client_input_bundle_packet(data, len))
        return;
    if (handle_client_input_packet(data, len))
        return;
    if (handle_client_welcome_packet(data, len))
        return;
    if (handle_client_load_level_packet(data, len))
        return;
    if (handle_client_sync_packet(data, len))
        return;
    if (handle_client_resync_state_packet(data, len))
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
                             static_cast<Uint8>(g_session.expected_players), g_session.seed);

    if (source == HostSource::UDP_RELAY) {
        g_session.relay_players[client_id] = player_id;
        g_session.relay_ready[client_id] = false;
        debug_log("mp host: relay client -> player %u", player_id);
        g_transport.HostSendUdpRelay(client_id, welcome);
        send_existing_placements_to_relay(client_id);
        return true;
    }

    if (source == HostSource::TCP_RELAY) {
        g_session.tcp_relay_players[client_id] = player_id;
        g_session.tcp_relay_ready[client_id] = false;
        debug_log("mp host: tcp relay client -> player %u", player_id);
        g_transport.HostSendTcpRelay(client_id, welcome);
        send_existing_placements_to_tcp_relay(client_id);
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
                             static_cast<Uint8>(g_session.expected_players), g_session.seed);
    g_transport.HostSendDirect(peer, buf);
    send_existing_placements_to_peer(peer);
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
    uint32_t current_tick = input::CurrentTick();
    uint32_t delay = g_session.input_delay ? g_session.input_delay : kInputDelay;
    uint32_t target_tick = current_tick + delay;
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
        rollback::RecordInput(pkt.tick, g_session.local_player, send);
        g_session.local_history[pkt.tick] = send;
        applied = true;
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

    uint32_t send_base = g_session.next_send_tick;
    if (send_base < current_tick)
        send_base = current_tick;
    uint32_t bundle_start = 0;
    if (send_base > kInputBundleBackTicks)
        bundle_start = send_base - kInputBundleBackTicks;
    if (bundle_start < current_tick)
        bundle_start = current_tick;
    uint32_t bundle_end = g_session.next_local_tick;
    if (bundle_end > bundle_start + kInputBundleMaxCount)
        bundle_end = bundle_start + kInputBundleMaxCount;

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

    if (sent_packets)
        g_transport.Flush();
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
