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

#include "input.hh"
#include "options.hh"
#include "server.hh"
#include "world.hh"

#include "SDL.h"

#include <algorithm>
#include <cmath>
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

void SessionNotifyLoadLevel(const std::string &pack_name, const std::string &level_id) {
    if (!g_session.active || !g_session.host)
        return;
    g_session.load_id += 1;
    g_session.level_pack_name = pack_name;
    g_session.level_id = level_id;
    if (debug_enabled())
        debug_log("mp host: load level id=%u pack=%s level_id=%s",
                  static_cast<unsigned>(g_session.load_id),
                  pack_name.c_str(), level_id.c_str());
    send_load_level_to_peers(pack_name, level_id);
}

bool SessionIsPaused() {
    if (!g_session.active)
        return false;
    return g_session.paused;
}

void SessionSetInputClockFrozen(bool frozen) {
    if (!g_session.active)
        return;
    g_session.input_clock_frozen = frozen;
    // Rebase immediately so we don't carry forward a large lookahead after
    // recovering from a long stall.
    g_session.input_clock_tick = input::CurrentTick();
    g_session.input_clock_accu = 0.0;
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

    // Some levels have no valid placement tiles (or no meaningful base actor to place near),
    // so auto placement can fail and leave the host waiting forever. We intentionally do not
    // try to "solve" such esoteric levels: instead, fail open and start the session. The extra
    // actors remain at their deterministic initial spawn position, which is consistent across
    // peers as long as level loading is deterministic.
    if (g_session.host && g_session.expected_players > g_session.level_players &&
        g_session.level_players > 0) {
        for (unsigned player = g_session.level_players; player < g_session.expected_players;
             ++player) {
            if (player < g_session.placement_received.size() && !g_session.placement_received[player]) {
                if (debug_enabled())
                    debug_log("mp extra actors: placement failed for player=%u; starting anyway",
                              player);
                g_session.placement_received[player] = true;
            }
        }
    }
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
    rollback::Reset();
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
    g_session.world_state_cooldown = 0.0;
    g_session.resync_attempts = 0;
    g_session.desync_streak = 0;
    g_session.actor_desync_streak = 0;
    g_session.world_only_desync_streak = 0;
    g_session.ready_timer = 0.0;
    g_session.level_players = 0;
    g_session.placement_received.clear();
    g_session.needs_placement.clear();
    g_session.last_defer_start_log = -1;
    g_session.auto_detect_active = false;
    g_session.auto_detect_done = false;
    g_session.auto_detect_next_ping_id = 1;
    g_session.auto_detect_next_send_ms = 0;
    g_session.auto_detect_end_ms = 0;
    g_session.auto_detect_selected_preset = -1;
    g_session.auto_detect_inflight_ms.clear();
    g_session.auto_detect_rtts_ms.clear();
    g_session.auto_detect_sent.clear();
    g_session.auto_detect_recv.clear();
    if (g_session.host) {
        for (auto &entry : g_session.peer_ready)
            entry.second = false;
        for (auto &entry : g_session.relay_ready)
            entry.second = false;
        for (auto &entry : g_session.tcp_relay_ready)
            entry.second = false;
    }
}

namespace {

Uint32 percentile_ms(std::vector<Uint32> samples, double p) {
    if (samples.empty())
        return 0;
    if (p <= 0.0)
        p = 0.0;
    if (p >= 1.0)
        p = 1.0;
    std::sort(samples.begin(), samples.end());
    const size_t idx = static_cast<size_t>(std::floor(p * static_cast<double>(samples.size() - 1)));
    return samples[idx];
}

int classify_connectivity_preset(Uint32 worst_p90_ms) {
    // Thresholds are conservative: we prefer a slower but stable experience.
    if (worst_p90_ms <= 70)
        return 0;  // good
    if (worst_p90_ms <= 180)
        return 1;  // mediocre
    return 2;      // bad
}

void apply_connectivity_preset_to_options(int preset_id) {
    // Keep in sync with the UI presets (OptionsMenu::apply_mp_debug_preset()).
    bool zerofill = false;
    bool rollback = false;
    bool remote_local_ball = false;
    int tick_ms = 10;
    int input_delay_legacy_ticks = 4;
    int predict_mouse_ticks = 0;
    int host_resync_stride = 50;
    int host_world_stride = 50;

    if (preset_id == 1) {  // mediocre
        zerofill = true;
        rollback = true;
        remote_local_ball = false;
        tick_ms = 20;
        input_delay_legacy_ticks = 8;
        predict_mouse_ticks = 2;
        host_resync_stride = 25;
        host_world_stride = 25;
    } else if (preset_id == 2) {  // bad
        zerofill = true;
        rollback = true;
        remote_local_ball = true;
        tick_ms = 50;
        input_delay_legacy_ticks = 16;
        predict_mouse_ticks = 5;
        host_resync_stride = 10;
        host_world_stride = 10;
    }

    options::SetOption("MultiplayerDebugSmoothRender", true);
    options::SetOption("MultiplayerDebugZeroFillInputs", zerofill);
    options::SetOption("MultiplayerDebugRollbackEnabled", rollback);
    options::SetOption("MultiplayerDebugRemoteControlLocalBall", remote_local_ball);
    // Keep experimental features off in presets unless explicitly enabled.
    options::SetOption("MultiplayerDebugClientAuthBallPos", false);

    options::SetOption("MultiplayerDebugTickLengthMs", static_cast<double>(tick_ms));
    options::SetOption("MultiplayerDebugInputDelayTicks", static_cast<double>(input_delay_legacy_ticks));
    options::SetOption("MultiplayerDebugPredictMissingMouseTicks", static_cast<double>(predict_mouse_ticks));
    options::SetOption("MultiplayerDebugHostBroadcastResyncStrideTicks", static_cast<double>(host_resync_stride));
    options::SetOption("MultiplayerDebugHostBroadcastWorldStateStrideTicks", static_cast<double>(host_world_stride));
    options::SetOption("MultiplayerDebugRollbackKeepTicks", 200.0);

    // Ensure the host session uses the chosen tick immediately (before NET_START).
    g_session.tick_ms = static_cast<Uint16>(tick_ms);
    input::SetTickTimestep(static_cast<double>(g_session.tick_ms) / 1000.0);
}

void host_send_ping_to_remote(HostSource source, ENetPeer *peer, Uint32 relay_client_id, Uint32 ping_id) {
    protocol::PingPacket msg;
    msg.ping_id = ping_id;
    ecl::Buffer payload;
    protocol::encode_ping(payload, msg);
    if (source == HostSource::DIRECT) {
        g_transport.HostSendDirectUnreliable(peer, payload);
        return;
    }
    if (source == HostSource::UDP_RELAY) {
        g_transport.HostSendUdpRelay(relay_client_id, payload);
        return;
    }
    if (source == HostSource::TCP_RELAY) {
        g_transport.HostSendTcpRelay(relay_client_id, payload);
        return;
    }
}

void start_host_auto_detect_connectivity() {
    g_session.auto_detect_active = true;
    g_session.auto_detect_done = false;
    g_session.auto_detect_selected_preset = -1;
    g_session.auto_detect_next_ping_id = 1;
    const Uint32 now_ms = SDL_GetTicks();
    g_session.auto_detect_next_send_ms = now_ms;
    g_session.auto_detect_end_ms = now_ms + 1200;  // ~12 pings @ 100ms

    const size_t n = g_session.expected_players;
    g_session.auto_detect_inflight_ms.assign(n, std::unordered_map<Uint32, Uint32>());
    g_session.auto_detect_rtts_ms.assign(n, std::vector<Uint32>());
    g_session.auto_detect_sent.assign(n, 0);
    g_session.auto_detect_recv.assign(n, 0);
}

bool tick_host_auto_detect_connectivity() {
    if (!g_session.auto_detect_active)
        return true;
    const Uint32 now_ms = SDL_GetTicks();

    if (now_ms >= g_session.auto_detect_next_send_ms && now_ms < g_session.auto_detect_end_ms) {
        // Send one ping per remote player.
        for (const auto &entry : g_session.peer_players) {
            ENetPeer *peer = entry.first;
            const unsigned player_id = entry.second;
            if (player_id >= g_session.expected_players)
                continue;
            const Uint32 ping_id = g_session.auto_detect_next_ping_id++;
            g_session.auto_detect_inflight_ms[player_id][ping_id] = now_ms;
            g_session.auto_detect_sent[player_id] += 1;
            host_send_ping_to_remote(HostSource::DIRECT, peer, 0, ping_id);
        }
        for (const auto &entry : g_session.relay_players) {
            const Uint32 client_id = entry.first;
            const unsigned player_id = entry.second;
            if (player_id >= g_session.expected_players)
                continue;
            const Uint32 ping_id = g_session.auto_detect_next_ping_id++;
            g_session.auto_detect_inflight_ms[player_id][ping_id] = now_ms;
            g_session.auto_detect_sent[player_id] += 1;
            host_send_ping_to_remote(HostSource::UDP_RELAY, nullptr, client_id, ping_id);
        }
        for (const auto &entry : g_session.tcp_relay_players) {
            const Uint32 client_id = entry.first;
            const unsigned player_id = entry.second;
            if (player_id >= g_session.expected_players)
                continue;
            const Uint32 ping_id = g_session.auto_detect_next_ping_id++;
            g_session.auto_detect_inflight_ms[player_id][ping_id] = now_ms;
            g_session.auto_detect_sent[player_id] += 1;
            host_send_ping_to_remote(HostSource::TCP_RELAY, nullptr, client_id, ping_id);
        }
        g_transport.Flush();
        g_session.auto_detect_next_send_ms = now_ms + 100;
    }

    if (now_ms < g_session.auto_detect_end_ms)
        return false;

    // Finish: classify based on worst p90 RTT across remotes.
    Uint32 worst_p90 = 0;
    for (unsigned player = 1; player < g_session.expected_players; ++player) {
        Uint32 p90 = percentile_ms(g_session.auto_detect_rtts_ms[player], 0.90);
        // If we got no responses, treat as extremely bad.
        if (g_session.auto_detect_rtts_ms[player].empty())
            p90 = 10000;
        if (p90 > worst_p90)
            worst_p90 = p90;
    }
    const int preset = classify_connectivity_preset(worst_p90);
    g_session.auto_detect_selected_preset = preset;
    g_session.auto_detect_active = false;
    g_session.auto_detect_done = true;
    debug_log("mp auto-detect: worst_p90_rtt_ms=%u preset=%d", static_cast<unsigned>(worst_p90), preset);
    return true;
}

void tick_advance_input_clock(double dtime) {
    g_session.input_clock_accu += dtime;
    if (g_session.input_clock_accu > 1.0)
        g_session.input_clock_accu = 1.0;
    const double step = input::TickTimestep();
    while (g_session.input_clock_accu >= step) {
        g_session.input_clock_accu -= step;
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

void tick_update_world_state_cooldown(double dtime) {
    if (g_session.world_state_cooldown <= 0.0)
        return;
    g_session.world_state_cooldown -= dtime;
    if (g_session.world_state_cooldown < 0.0)
        g_session.world_state_cooldown = 0.0;
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
        if (!host_ready_to_start()) {
            // Robustness: keep re-announcing the current level load while waiting for READY.
            // This covers packet loss and ensures clients can satisfy the "must have seen
            // NET_LOAD_LEVEL before READY" invariant.
            if (has_remote_peers() && g_session.load_id != 0 && !g_session.level_id.empty()) {
                g_session.load_announce_timer += input::TickTimestep();
                if (g_session.load_announce_timer >= 1.0) {
                    send_load_level_to_peers(g_session.level_pack_name, g_session.level_id);
                    g_session.load_announce_timer = 0.0;
                }
            }
            if (debug_enabled()) {
                static Uint32 last_sid = 0;
                static Uint32 last_epoch = 0;
                static Uint32 last_load = 0;
                static double wait_accu = 0.0;
                if (g_session.session_id != last_sid || g_session.input_epoch != last_epoch ||
                    g_session.load_id != last_load) {
                    last_sid = g_session.session_id;
                    last_epoch = g_session.input_epoch;
                    last_load = g_session.load_id;
                    wait_accu = 0.0;
                }
                wait_accu += input::TickTimestep();
                if (wait_accu >= 2.0) {
                    wait_accu = 0.0;
                    unsigned direct = static_cast<unsigned>(g_session.peer_players.size());
                    unsigned udp = static_cast<unsigned>(g_session.relay_players.size());
                    unsigned tcp = static_cast<unsigned>(g_session.tcp_relay_players.size());
                    debug_log("mp host: waiting for ready (session=%u epoch=%u load=%u expected=%u have=%u direct=%u udp=%u tcp=%u)",
                              static_cast<unsigned>(g_session.session_id),
                              static_cast<unsigned>(g_session.input_epoch),
                              static_cast<unsigned>(g_session.load_id),
                              g_session.expected_players,
                              direct + udp + tcp + 1,
                              direct, udp, tcp);
                    if (g_session.expected_players > 1) {
                        for (const auto &entry : g_session.peer_players) {
                            auto it = g_session.peer_ready.find(entry.first);
                            if (it == g_session.peer_ready.end() || !it->second)
                                debug_log("mp host: missing ready (direct peer=%p player=%u)",
                                          static_cast<void *>(entry.first), entry.second);
                        }
                        for (const auto &entry : g_session.relay_players) {
                            auto it = g_session.relay_ready.find(entry.first);
                            if (it == g_session.relay_ready.end() || !it->second)
                                debug_log("mp host: missing ready (udp relay client=%u player=%u)",
                                          static_cast<unsigned>(entry.first), entry.second);
                        }
                        for (const auto &entry : g_session.tcp_relay_players) {
                            auto it = g_session.tcp_relay_ready.find(entry.first);
                            if (it == g_session.tcp_relay_ready.end() || !it->second)
                                debug_log("mp host: missing ready (tcp relay client=%u player=%u)",
                                          static_cast<unsigned>(entry.first), entry.second);
                        }
                    }
                    if (g_session.expected_players > g_session.level_players && g_session.level_players > 0) {
                        if (g_session.placement_received.size() < g_session.expected_players) {
                            debug_log("mp host: missing placements (vector size=%u expected=%u)",
                                      static_cast<unsigned>(g_session.placement_received.size()),
                                      g_session.expected_players);
                        } else {
                            for (unsigned player = g_session.level_players; player < g_session.expected_players; ++player) {
                                if (!g_session.placement_received[player])
                                    debug_log("mp host: missing placement player=%u", player);
                            }
                        }
                    }
                }
            }
            return;
        }
        g_session.load_announce_timer = 0.0;
        if (debug_enabled())
            debug_log("mp host: start allowed");

        // Ensure all peers use the same host-selected debug/session settings before NET_START.
        // If auto-detect is enabled, override the host's manual settings with the chosen preset.
        if (options::GetBool("MultiplayerAutoDetectConnectivity") && g_session.expected_players > 1) {
            if (!g_session.auto_detect_done && !g_session.auto_detect_active)
                start_host_auto_detect_connectivity();
            if (!tick_host_auto_detect_connectivity())
                return;
            if (g_session.auto_detect_selected_preset >= 0)
                apply_connectivity_preset_to_options(g_session.auto_detect_selected_preset);
        }
        SessionBroadcastDebugOptions();

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

void tick_host_broadcast_resync() {
    if (!g_session.host || !has_remote_peers())
        return;
    if (g_session.phase != SessionState::Phase::RUNNING)
        return;
    int legacy_stride = options::GetInt("MultiplayerDebugHostBroadcastResyncStrideTicks");
    int stride = legacy_stride;
    if (options::GetBool("MultiplayerDebugRemoteControlLocalBall")) {
        // Remote-control mode relies on frequent authoritative actor snapshots.
        // Force a per-tick resync broadcast so the local ball stays responsive.
        stride = 1;
    }
    if (legacy_stride > 0) {
        // Keep debug UX stable across tick sizes: interpret stride as 10ms ticks.
        const double tick_s = input::TickTimestep();
        const double legacy_s = 0.01;
        if (tick_s > 0.0) {
            const double desired_s = static_cast<double>(legacy_stride) * legacy_s;
            stride = static_cast<int>(std::lround(desired_s / tick_s));
            if (stride < 1)
                stride = 1;
        }
    }
    if (stride <= 0)
        return;
    uint32_t tick = input::CurrentTick();
    if (tick == g_session.last_host_resync_broadcast_tick)
        return;
    if (stride < 1)
        stride = 1;
    if ((tick % static_cast<uint32_t>(stride)) != 0)
        return;
    g_session.last_host_resync_broadcast_tick = tick;
    broadcast_resync_state_unreliable();
}

void tick_host_broadcast_world_state() {
    if (!g_session.host || !has_remote_peers())
        return;
    if (g_session.phase != SessionState::Phase::RUNNING)
        return;
    int legacy_stride = options::GetInt("MultiplayerDebugHostBroadcastWorldStateStrideTicks");
    int stride = legacy_stride;
    if (legacy_stride > 0) {
        // Keep debug UX stable across tick sizes: interpret stride as 10ms ticks.
        const double tick_s = input::TickTimestep();
        const double legacy_s = 0.01;
        if (tick_s > 0.0) {
            const double desired_s = static_cast<double>(legacy_stride) * legacy_s;
            stride = static_cast<int>(std::lround(desired_s / tick_s));
            if (stride < 1)
                stride = 1;
        }
    }
    if (stride <= 0)
        return;
    uint32_t tick = input::CurrentTick();
    if (tick == g_session.last_host_world_state_broadcast_tick)
        return;
    if (stride < 1)
        stride = 1;
    if ((tick % static_cast<uint32_t>(stride)) != 0)
        return;
    g_session.last_host_world_state_broadcast_tick = tick;
    broadcast_world_state_unreliable();
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
    if (!g_session.input_clock_frozen)
        tick_advance_input_clock(dtime);
    process_network_events();
    if (!g_session.active)
        return;
    if (g_session.paused)
        return;
    if (g_session.abort_pending)
        return;
    tick_apply_pending_sync();
    record_checksum_sample();
    tick_update_resync_cooldown(dtime);
    tick_update_world_state_cooldown(dtime);
    tick_update_resync_inflight_timeout(dtime);
    tick_send_periodic_ready(dtime);
    tick_debug_report_missing_input();
    tick_update_start_phase();
    send_local_inputs();
    tick_host_periodic_sync(dtime);
    tick_host_broadcast_resync();
    tick_host_broadcast_world_state();
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
