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
#include "multiplayer_session_impl.hh"

#include "input.hh"
#include "server.hh"
#include "world.hh"

#include <algorithm>
#include <cstdint>

/* -------------------- Multiplayer session runtime -------------------- */
/*
 * Session runtime glue (tick integration, per-level lifecycle helpers).
 */

namespace enigma {
namespace multiplayer {
namespace internal {

bool SessionIsActive() {
    return g_session.active;
}

bool SessionIsHost() {
    return g_session.host;
}

unsigned SessionLocalPlayer() {
    return g_session.local_player;
}

unsigned SessionExpectedPlayers() {
    return g_session.expected_players;
}

TransportKind SessionActiveTransport() {
    if (!g_session.active || g_session.host)
        return TransportKind::NONE;
    return g_session.active_transport;
}

bool SessionShouldDeferStart() {
    if (!g_session.active)
        return false;
    // For hosts, "ready" means that all expected peers have connected and reported READY
    // (and that any extra-actor placement is complete). This must gate simulation start,
    // otherwise the host can run ahead while a client is still switching packs / loading.
    bool can_start = false;
    if (g_session.host) {
        if (g_session.expected_players <= 1)
            can_start = true;
        else
            can_start = host_ready_to_start();
    } else {
        can_start = (g_session.phase == SessionState::Phase::READY_TO_START ||
                     g_session.phase == SessionState::Phase::RUNNING);
    }
    if (debug_enabled()) {
        int v = can_start ? 0 : 1;
        if (g_session.last_defer_start_log != v) {
            debug_log("mp should defer start: %d", v);
            g_session.last_defer_start_log = v;
        }
    }
    return !can_start;
}

void SessionNotifyStartRequested() {
    if (!g_session.active)
        return;
    if (debug_enabled())
        debug_log("mp notify start requested (host=%d local=%u expected=%u)", g_session.host ? 1 : 0,
                  g_session.local_player, g_session.expected_players);
    if (g_session.phase != SessionState::Phase::RUNNING)
        g_session.phase = SessionState::Phase::WAITING_FOR_READY;
    // READY must mean "the level is fully initialized and the client is waiting
    // for NET_START". During level loading, server::PrepareLevel() sets
    // sv_waiting_for_clients early, so Msg_StartGame() can be invoked before the
    // world is initialized; guard against sending READY too early.
    if (!g_session.host && server::WorldInitialized && local_can_send_ready() &&
        !g_session.local_ready_sent) {
        send_ready_to_host();
        g_session.local_ready_sent = true;
        g_session.ready_timer = 0.0;
    }
}

void SessionNotifyRestart(bool level_restart) {
    if (!g_session.active || !g_session.host)
        return;
    // If someone had the multiplayer ESC menu open, force-unpause before restarting.
    if (g_session.paused) {
        g_session.paused = false;
        if (!g_session.menu_open.empty())
            std::fill(g_session.menu_open.begin(), g_session.menu_open.end(), false);
        server::Msg_Pause(false);
        send_pause_to_peers(false);
    }
    g_session.restart_id += 1;
    g_session.last_restart_id = g_session.restart_id;
    send_restart_to_peers(level_restart);
}

bool SessionIsPaused() {
    if (!g_session.active)
        return false;
    return g_session.paused;
}

void SessionRequestPause(bool paused) {
    if (!g_session.active)
        return;
    if (g_session.host) {
        if (g_session.paused == paused)
            return;
        g_session.paused = paused;
        server::Msg_Pause(paused);
        send_pause_to_peers(paused);
        return;
    }
    send_pause_to_host(paused);
}

namespace {

void debug_dump_steerable_actors(const char *tag) {
    if (!debug_enabled())
        return;
    std::vector<Actor *> actors;
    GetActors(actors);
    unsigned dumped = 0;
    for (auto *a : actors) {
        if (!a || !a->isSteerable())
            continue;
        if (dumped >= 32) {
            debug_log("mp actors(%s): ... (truncated)", tag);
            return;
        }
        Value owner = a->getAttr("owner");
        Value color = a->getAttr("color");
        debug_log("mp actor(%s): kind=%s pos=(%.2f,%.2f) ctrl=%d owner=%s color=%s", tag,
                  a->getKind().c_str(), a->get_pos()[0], a->get_pos()[1], a->get_controllers(),
                  owner ? owner.to_string().c_str() : "nil",
                  color ? color.to_string().c_str() : "nil");
        dumped += 1;
    }
    if (dumped == 0)
        debug_log("mp actors(%s): none steerable", tag);
}

bool any_menu_open() {
    for (bool open : g_session.menu_open) {
        if (open)
            return true;
    }
    return false;
}

void host_update_pause_from_menu_state() {
    bool want_pause = any_menu_open();
    if (g_session.paused == want_pause)
        return;
    g_session.paused = want_pause;
    server::Msg_Pause(want_pause);
    send_pause_to_peers(want_pause);
}

void tick_abort_grace(double dtime) {
    if (!g_session.abort_pending)
        return;
    g_session.abort_timer += dtime;
    if (g_session.abort_timer < kAbortDeliveryGrace)
        return;
    const std::string message = g_session.abort_message;
    g_session.abort_pending = false;
    g_session.abort_timer = 0.0;
    g_session.abort_message.clear();
    abort_session_with_message(message.c_str());
}

}  // namespace

void begin_abort_after_grace(const char *message) {
    if (!g_session.active)
        return;
    if (g_session.abort_pending)
        return;
    g_session.abort_pending = true;
    g_session.abort_timer = 0.0;
    g_session.abort_message = message ? message : "Game aborted. Ending session.";
}

void SessionSetMenuOpen(bool open) {
    if (!g_session.active)
        return;
    if (g_session.host) {
        unsigned player = g_session.local_player;
        if (player < g_session.menu_open.size() && g_session.menu_open[player] != open) {
            g_session.menu_open[player] = open;
            host_update_pause_from_menu_state();
        }
        return;
    }
    if (!g_session.local_player_known)
        return;
    send_menu_to_host(open);
}

void SessionRequestAbort() {
    if (!g_session.active)
        return;
    if (g_session.host) {
        send_abort_to_peers();
        begin_abort_after_grace("Game aborted. Ending session.");
        return;
    }
    send_abort_to_host();
    begin_abort_after_grace("Game aborted. Ending session.");
}

void SessionPrepareExtraActors() {
    if (!g_session.active)
        return;
    g_session.level_players = compute_level_players();
    if (g_session.level_players < 1)
        g_session.level_players = 1;
    add_extra_actors(g_session.level_players, g_session.expected_players);
}

void SessionSetupExtraPlayerStartPositions() {
    if (!g_session.active)
        return;
    if (g_session.level_players == 0)
        g_session.level_players = compute_level_players();
    if (g_session.level_players < 1)
        g_session.level_players = 1;

    // Some levels run Lua/setup logic during WorldInitLevel that can overwrite
    // controllers/owner for steerable actors (notably meditation pearls). Run a
    // post-init rebalance to ensure authored multi-ball levels are distributed
    // across session players as intended.
    rebalance_authored_multi_ball_levels(g_session.level_players, g_session.expected_players);

    g_session.placement_received.clear();
    if (g_session.expected_players > 0) {
        g_session.placement_received.resize(g_session.expected_players, false);
        for (unsigned player = 0; player < g_session.expected_players; ++player) {
            if (!placement_required_for_player(player))
                g_session.placement_received[player] = true;
        }
    }
    auto_place_extra_players();
    debug_dump_steerable_actors("post-place");
}

void SessionPrimeInputQueueForNewLevel() {
    if (!g_session.active)
        return;
    // A new level/restart starts unpaused; any open menus from the prior run are discarded.
    bool was_paused = g_session.paused;
    g_session.paused = false;
    if (!g_session.menu_open.empty())
        std::fill(g_session.menu_open.begin(), g_session.menu_open.end(), false);
    server::Msg_Pause(false);
    if (was_paused && g_session.host)
        send_pause_to_peers(false);

    configure_input_session(g_session.expected_players);
    g_session.desync_reported = false;
    g_session.phase = SessionState::Phase::WAITING_FOR_START;
    g_session.local_ready_sent = false;
    g_session.ready_timer = 0.0;
    g_session.has_pending_sync = false;
    g_session.last_world_checksum = 0;
    g_session.checksum_history.clear();
    g_session.last_checksum_tick = UINT32_MAX;
    g_session.resync_inflight = false;
    g_session.resync_inflight_timer = 0.0;
    g_session.resync_cooldown = 0.0;
    g_session.resync_attempts = 0;
    g_session.desync_streak = 0;
    g_session.ready_timer = 0.0;
    g_session.level_players = 0;
    g_session.placement_received.clear();
    g_session.needs_placement.clear();
    g_session.last_defer_start_log = -1;
    if (g_session.host) {
        for (auto &entry : g_session.peer_ready)
            entry.second = false;
        for (auto &entry : g_session.relay_ready)
            entry.second = false;
    }
}

namespace {

void tick_advance_input_clock(double dtime) {
    g_session.input_clock_accu += dtime;
    if (g_session.input_clock_accu > 1.0)
        g_session.input_clock_accu = 1.0;
    while (g_session.input_clock_accu >= kInputTimestep) {
        g_session.input_clock_accu -= kInputTimestep;
        g_session.input_clock_tick += 1;
    }
}

void tick_apply_pending_sync() {
    if (!g_session.has_pending_sync)
        return;
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

void tick_update_resync_cooldown(double dtime) {
    if (g_session.resync_cooldown <= 0.0)
        return;
    g_session.resync_cooldown -= dtime;
    if (g_session.resync_cooldown < 0.0)
        g_session.resync_cooldown = 0.0;
}

void tick_update_resync_inflight_timeout(double dtime) {
    if (!g_session.resync_inflight)
        return;
    g_session.resync_inflight_timer += dtime;
    if (g_session.resync_inflight_timer < kResyncInflightTimeout)
        return;
    debug_log("mp resync timeout: local tick=%u attempts=%u via=%s", input::CurrentTick(),
              static_cast<unsigned>(g_session.resync_attempts),
              transport_name(g_session.active_transport));
    g_session.telemetry.resync_inflight_timeouts += 1;
    g_session.resync_inflight = false;
    g_session.resync_inflight_timer = 0.0;
    // Allow a retry on the next mismatch observation.
    g_session.resync_cooldown = 0.0;
}

void tick_send_periodic_ready(double dtime) {
    if (g_session.host || g_session.phase != SessionState::Phase::WAITING_FOR_READY)
        return;
    if (!server::WorldInitialized)
        return;
    g_session.ready_timer += dtime;
    if (g_session.ready_timer < 0.5)
        return;
    if (local_can_send_ready()) {
        send_ready_to_host();
        g_session.local_ready_sent = true;
    }
    g_session.ready_timer = 0.0;
}

void tick_debug_report_missing_input() {
    if (!debug_enabled() || !g_session.local_player_known || !input::IsNetworked())
        return;
    static uint32_t last_missing_tick = UINT32_MAX;
    uint32_t tick = input::CurrentTick();
    if (tick == last_missing_tick)
        return;
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

void tick_update_start_phase() {
    if (g_session.phase == SessionState::Phase::WAITING_FOR_READY && g_session.host) {
        if (!host_ready_to_start())
            return;
        if (debug_enabled())
            debug_log("mp host: start allowed");
        g_session.input_epoch += 1;
        g_session.debug_state_dumped = false;
        configure_input_session(g_session.expected_players);
        send_start_to_peers();
        g_session.phase = SessionState::Phase::READY_TO_START;
    }

    if (g_session.phase != SessionState::Phase::READY_TO_START)
        return;
    server::Msg_StartGame();
    // Msg_StartGame can still be deferred (e.g. host has not observed connected peers yet).
    // Only transition to RUNNING once the session logic says we can actually start.
    if (SessionShouldDeferStart())
        return;
    g_session.phase = SessionState::Phase::RUNNING;
}

void tick_host_periodic_sync(double dtime) {
    if (!g_session.host)
        return;
    g_session.sync_timer += dtime;
    if (g_session.sync_timer < kSyncInterval)
        return;
    g_session.sync_timer = 0.0;
    send_sync_to_peers();
}

void shutdown_enet_host_state() {
    if (g_session.relay_peer) {
        enet_peer_disconnect(g_session.relay_peer, 0);
        g_session.relay_peer = nullptr;
    }
    if (g_session.relay_handle) {
        enet_host_destroy(g_session.relay_handle);
        g_session.relay_handle = nullptr;
    }

    if (!g_session.host_handle)
        return;
    if (g_session.host) {
        for (const auto &entry : g_session.peer_players)
            enet_peer_disconnect(entry.first, 0);
    } else if (g_session.server_peer) {
        enet_peer_disconnect(g_session.server_peer, 0);
    }
    enet_host_destroy(g_session.host_handle);
    g_session.host_handle = nullptr;
    g_session.server_peer = nullptr;
}

}  // namespace

void SessionTick(double dtime) {
    if (!g_session.active)
        return;
    if (g_session.abort_pending) {
        process_network_events();
        tick_abort_grace(dtime);
        return;
    }
    // While paused we still need to pump the network (for unpause / disconnect),
    // but we must not advance the input clock or emit inputs.
    if (g_session.paused) {
        process_network_events();
        return;
    }
    g_session.no_payload_timer += dtime;
    tick_advance_input_clock(dtime);
    process_network_events();
    if (!g_session.active)
        return;
    if (g_session.paused)
        return;
    if (g_session.abort_pending)
        return;
    if (!g_session.host && g_session.phase == SessionState::Phase::RUNNING &&
        g_session.no_payload_timer >= kClientNoPayloadDisconnectTimeout) {
        abort_session_with_message("Lost connection to host. Ending session.");
        return;
    }
    tick_apply_pending_sync();
    record_checksum_sample();
    tick_update_resync_cooldown(dtime);
    tick_update_resync_inflight_timeout(dtime);
    tick_send_periodic_ready(dtime);
    tick_debug_report_missing_input();
    tick_update_start_phase();
    send_local_inputs();
    tick_host_periodic_sync(dtime);
}

void SessionShutdown() {
    if (!g_session.active)
        return;
    if (debug_enabled()) {
        const auto &t = g_session.telemetry;
        debug_log("mp telemetry: sync_current=%llu sync_sample=%llu "
                  "mismatch(pos=%llu rand=%llu actor=%llu world=%llu) "
                  "classified(diag_only=%llu rng_only=%llu soft_candidate=%llu) "
                  "rng_fixed=%llu resync(req=%llu resp=%llu applied=%llu timeout=%llu giveup=%llu)",
                  static_cast<unsigned long long>(t.sync_current_total),
                  static_cast<unsigned long long>(t.sync_sample_total),
                  static_cast<unsigned long long>(t.mismatch_pos),
                  static_cast<unsigned long long>(t.mismatch_rand),
                  static_cast<unsigned long long>(t.mismatch_actor),
                  static_cast<unsigned long long>(t.mismatch_world),
                  static_cast<unsigned long long>(t.mismatch_diagnostic_only),
                  static_cast<unsigned long long>(t.mismatch_rng_only),
                  static_cast<unsigned long long>(t.mismatch_soft_resync_candidate),
                  static_cast<unsigned long long>(t.rng_resync_applied),
                  static_cast<unsigned long long>(t.resync_requests_sent),
                  static_cast<unsigned long long>(t.resync_responses_recv),
                  static_cast<unsigned long long>(t.resync_applied),
                  static_cast<unsigned long long>(t.resync_inflight_timeouts),
                  static_cast<unsigned long long>(t.resync_giveups));
    }
    shutdown_enet_host_state();
    tcp_close(g_session.tcp_relay_socket);
    g_session = SessionState();
    input::Reset();
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
