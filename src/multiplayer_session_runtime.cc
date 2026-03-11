/*
 * 2026 LLM generated contribution - concept, review and revision by Ferdinand Strixner
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

#include "multiplayer_connectivity_presets.hh"
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
#include <sstream>

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

namespace {

const char *on_off(bool value) {
    return value ? "On" : "Off";
}

std::string connectivity_profile_name_or_custom() {
    return connectivity::profile_name_or_custom(options::GetBool, options::GetInt);
}

void append_bool_option(std::vector<std::string> &lines, const char *label, const char *option) {
    std::ostringstream os;
    os << label << ": " << on_off(options::GetBool(option));
    lines.push_back(os.str());
}

void append_int_option(std::vector<std::string> &lines, const char *label, const char *option) {
    std::ostringstream os;
    os << label << ": " << options::GetInt(option);
    lines.push_back(os.str());
}

void ensure_runtime_latency_storage() {
    size_t players = std::max<unsigned>(1u, g_session.expected_players);
    if (g_session.runtime_latency_inflight_ms.size() != players)
        g_session.runtime_latency_inflight_ms.assign(players, std::unordered_map<Uint32, Uint32>());
    if (g_session.runtime_latency_rtt_ms.size() != players)
        g_session.runtime_latency_rtt_ms.assign(players, 0);
    if (g_session.runtime_latency_valid.size() != players)
        g_session.runtime_latency_valid.assign(players, false);
    if (g_session.local_player < players) {
        g_session.runtime_latency_valid[g_session.local_player] = true;
        g_session.runtime_latency_rtt_ms[g_session.local_player] = 0;
    }
}

std::string latency_text_for_player(unsigned player) {
    if (!g_session.active)
        return "n/a";
    if (g_session.host) {
        if (player == g_session.local_player)
            return "0 ms";
        if (player < g_session.runtime_latency_valid.size() && g_session.runtime_latency_valid[player]) {
            std::ostringstream os;
            os << g_session.runtime_latency_rtt_ms[player] << " ms";
            return os.str();
        }
        return "n/a";
    }

    // Client-side: show RTT to host when available.
    const unsigned host_player = 0;
    if (player == g_session.local_player)
        return "0 ms";
    if (player != host_player)
        return "n/a";

    Uint32 rtt = 0;
    bool have_rtt = false;
    if (host_player < g_session.runtime_latency_valid.size() &&
        g_session.runtime_latency_valid[host_player]) {
        rtt = g_session.runtime_latency_rtt_ms[host_player];
        have_rtt = true;
    } else if ((g_session.active_transport == TransportKind::DIRECT ||
                g_session.active_transport == TransportKind::UDP_RELAY) &&
               g_session.server_peer) {
        // Fallback: ENet RTT to the connected peer (host for direct, relay for UDP relay).
        rtt = g_session.server_peer->roundTripTime;
        have_rtt = true;
    }
    if (!have_rtt)
        return "n/a";
    std::ostringstream os;
    os << rtt << " ms";
    return os.str();
}

unsigned host_transport_mask_for_player(unsigned player) {
    if (!g_session.active || !g_session.host)
        return 0;
    if (player == g_session.local_player)
        return 0;

    unsigned mask = 0;
    for (const auto &entry : g_session.peer_players) {
        if (entry.second == player) {
            mask |= 1u;  // direct
            break;
        }
    }
    for (const auto &entry : g_session.relay_players) {
        if (entry.second == player) {
            mask |= 2u;  // udp-relay
            break;
        }
    }
    for (const auto &entry : g_session.tcp_relay_players) {
        if (entry.second == player) {
            mask |= 4u;  // tcp-relay
            break;
        }
    }
    return mask;
}

std::string overlay_transport_text_for_player(unsigned player) {
    if (!g_session.active)
        return "";
    if (player == g_session.local_player)
        return "local";

    if (g_session.host) {
        unsigned mask = host_transport_mask_for_player(player);
        if (mask == 1u)
            return transport_name(TransportKind::DIRECT);
        if (mask == 2u)
            return transport_name(TransportKind::UDP_RELAY);
        if (mask == 4u)
            return transport_name(TransportKind::TCP_RELAY);
        if (mask == 0u)
            return "";
        return "mixed";
    }

    // Client-side: only the host transport is known/useful.
    const unsigned host_player = 0;
    if (player != host_player)
        return "";
    return transport_name(g_session.active_transport);
}

std::string host_transport_summary_line() {
    const unsigned direct = static_cast<unsigned>(g_session.peer_players.size());
    const unsigned udp = static_cast<unsigned>(g_session.relay_players.size());
    const unsigned tcp = static_cast<unsigned>(g_session.tcp_relay_players.size());
    const unsigned kinds = (direct ? 1u : 0u) + (udp ? 1u : 0u) + (tcp ? 1u : 0u);

    if (kinds == 0u)
        return "Transport: local";
    if (kinds == 1u) {
        if (direct)
            return std::string("Transport: ") + transport_name(TransportKind::DIRECT);
        if (udp)
            return std::string("Transport: ") + transport_name(TransportKind::UDP_RELAY);
        return std::string("Transport: ") + transport_name(TransportKind::TCP_RELAY);
    }

    std::string used;
    if (direct)
        used += transport_name(TransportKind::DIRECT);
    if (udp) {
        if (!used.empty())
            used += ", ";
        used += transport_name(TransportKind::UDP_RELAY);
    }
    if (tcp) {
        if (!used.empty())
            used += ", ";
        used += transport_name(TransportKind::TCP_RELAY);
    }
    return std::string("Transport: mixed (") + used + ")";
}

}  // namespace

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

void SessionBuildStatsOverlayLines(std::vector<std::string> &lines,
                                   ::enigma::multiplayer::StatsOverlayPage page) {
    lines.clear();
    if (!g_session.active)
        return;

    ensure_runtime_latency_storage();

    const char *section = "MP Debug";
    if (page == ::enigma::multiplayer::StatsOverlayPage::SYNC)
        section = "MP Sync";
    else if (page == ::enigma::multiplayer::StatsOverlayPage::NETSIM)
        section = "MP Netsim";
    lines.push_back("MP stats");
    {
        std::ostringstream os;
        os << "Role: " << (g_session.host ? "Host" : "Client");
        lines.push_back(os.str());
    }
    if (g_session.host) {
        lines.push_back(host_transport_summary_line());
    } else {
        std::ostringstream os;
        os << "Transport: " << transport_name(g_session.active_transport);
        lines.push_back(os.str());
    }
    lines.push_back(std::string("Connectivity profile: ") + connectivity_profile_name_or_custom());
    lines.push_back("Latency (RTT):");
    for (unsigned player = 0; player < g_session.expected_players; ++player) {
        std::ostringstream os;
        os << "  P" << player << ": " << latency_text_for_player(player);
        const std::string transport = overlay_transport_text_for_player(player);
        if (!transport.empty())
            os << " (" << transport << ")";
        lines.push_back(os.str());
    }

    lines.push_back("");
    lines.push_back(std::string(section) + ":");

    if (page == ::enigma::multiplayer::StatsOverlayPage::DEBUG) {
        append_bool_option(lines, "MP logs", "MultiplayerDebugLogging");
        append_bool_option(lines, "MP dump", "MultiplayerDebugDumpState");
        append_bool_option(lines, "MP trace init", "MultiplayerDebugTraceWorldInit");
        append_bool_option(lines, "MP smooth render", "MultiplayerDebugSmoothRender");
        append_bool_option(lines, "MP skip local resync", "MultiplayerDebugSkipLocalResync");
        append_bool_option(lines, "MP force relay", "MultiplayerDebugForceRelay");
        append_bool_option(lines, "MP bind local", "MultiplayerDebugBindLocal");
        append_int_option(lines, "Host resync stride", "MultiplayerDebugHostBroadcastResyncStrideTicks");
        append_int_option(lines, "Host world stride", "MultiplayerDebugHostBroadcastWorldStateStrideTicks");
        append_int_option(lines, "Rollback keep ticks", "MultiplayerDebugRollbackKeepTicks");
    } else if (page == ::enigma::multiplayer::StatsOverlayPage::SYNC) {
        append_bool_option(lines, "MP zerofill", "MultiplayerDebugZeroFillInputs");
        append_bool_option(lines, "MP rollback", "MultiplayerDebugRollbackEnabled");
        append_bool_option(lines, "MP remote local ball", "MultiplayerDebugRemoteControlLocalBall");
        append_bool_option(lines, "MP client auth pos", "MultiplayerDebugClientAuthBallPos");
        append_bool_option(lines, "MP host world only", "MultiplayerDebugHostOnlyWorldInteractions");
        append_int_option(lines, "Predict mouse ticks", "MultiplayerDebugPredictMissingMouseTicks");
        append_int_option(lines, "Input delay ticks", "MultiplayerDebugInputDelayTicks");
        append_int_option(lines, "World desync streak",
                          "MultiplayerDebugWorldDesyncStreakForWorldStateRequest");
        append_int_option(lines, "Tick length ms", "MultiplayerDebugTickLengthMs");
    } else {
        append_bool_option(lines, "MP netsim", "MultiplayerDebugNetSimEnabled");
        append_bool_option(lines, "MP netsim all", "MultiplayerDebugNetSimAll");
        append_int_option(lines, "Netsim delay ms", "MultiplayerDebugNetSimDelayMs");
        append_int_option(lines, "Netsim jitter ms", "MultiplayerDebugNetSimJitterMs");
        append_int_option(lines, "Netsim drop %", "MultiplayerDebugNetSimDropPct");
        append_int_option(lines, "Netsim dup %", "MultiplayerDebugNetSimDupPct");
    }
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
    g_session.runtime_latency_next_ping_id = 1;
    g_session.runtime_latency_next_send_ms = 0;
    g_session.runtime_latency_inflight_ms.clear();
    g_session.runtime_latency_rtt_ms.clear();
    g_session.runtime_latency_valid.clear();
    g_session.debug_options_broadcast_timer = 0.0;
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
    if (worst_p90_ms <= connectivity::kConnectivityGoodMaxP90Ms)
        return 0;  // good
    if (worst_p90_ms <= connectivity::kConnectivityNormalMaxP90Ms)
        return 1;  // mediocre
    return 2;      // bad
}

void apply_connectivity_preset_to_options(int preset_id) {
    const bool ok = connectivity::apply_preset_id(
        preset_id,
        [](const char *name, bool value) { options::SetOption(name, value ? 1.0 : 0.0); },
        [](const char *name, int value) { options::SetOption(name, static_cast<double>(value)); });
    if (!ok)
        return;

    // Ensure the host session uses the chosen tick immediately (before NET_START).
    int tick_ms = options::GetInt("MultiplayerDebugTickLengthMs");
    if (tick_ms <= 0)
        tick_ms = 10;
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

void tick_host_runtime_latency_probes() {
    if (!g_session.active || !g_session.host)
        return;
    if (g_session.phase != SessionState::Phase::RUNNING)
        return;
    if (!has_remote_peers())
        return;

    ensure_runtime_latency_storage();
    const Uint32 now_ms = SDL_GetTicks();
    if (now_ms < g_session.runtime_latency_next_send_ms)
        return;

    for (const auto &entry : g_session.peer_players) {
        ENetPeer *peer = entry.first;
        const unsigned player_id = entry.second;
        if (player_id >= g_session.expected_players)
            continue;
        auto &inflight = g_session.runtime_latency_inflight_ms[player_id];
        for (auto it = inflight.begin(); it != inflight.end(); ) {
            if (now_ms >= it->second && (now_ms - it->second) > 5000)
                it = inflight.erase(it);
            else
                ++it;
        }
        const Uint32 ping_id = g_session.runtime_latency_next_ping_id++;
        inflight[ping_id] = now_ms;
        host_send_ping_to_remote(HostSource::DIRECT, peer, 0, ping_id);
    }
    for (const auto &entry : g_session.relay_players) {
        const Uint32 client_id = entry.first;
        const unsigned player_id = entry.second;
        if (player_id >= g_session.expected_players)
            continue;
        auto &inflight = g_session.runtime_latency_inflight_ms[player_id];
        for (auto it = inflight.begin(); it != inflight.end(); ) {
            if (now_ms >= it->second && (now_ms - it->second) > 5000)
                it = inflight.erase(it);
            else
                ++it;
        }
        const Uint32 ping_id = g_session.runtime_latency_next_ping_id++;
        inflight[ping_id] = now_ms;
        host_send_ping_to_remote(HostSource::UDP_RELAY, nullptr, client_id, ping_id);
    }
    for (const auto &entry : g_session.tcp_relay_players) {
        const Uint32 client_id = entry.first;
        const unsigned player_id = entry.second;
        if (player_id >= g_session.expected_players)
            continue;
        auto &inflight = g_session.runtime_latency_inflight_ms[player_id];
        for (auto it = inflight.begin(); it != inflight.end(); ) {
            if (now_ms >= it->second && (now_ms - it->second) > 5000)
                it = inflight.erase(it);
            else
                ++it;
        }
        const Uint32 ping_id = g_session.runtime_latency_next_ping_id++;
        inflight[ping_id] = now_ms;
        host_send_ping_to_remote(HostSource::TCP_RELAY, nullptr, client_id, ping_id);
    }
    g_transport.Flush();
    g_session.runtime_latency_next_send_ms = now_ms + 1000;
}

void tick_client_runtime_latency_probe() {
    if (!g_session.active || g_session.host)
        return;
    if (g_session.phase != SessionState::Phase::RUNNING)
        return;

    // Ensure we have a path to the host (direct/UDP relay via ENet, or TCP relay socket).
    const bool have_transport =
        (g_session.server_peer != nullptr) ||
        (g_session.active_transport == TransportKind::TCP_RELAY && tcp_socket_valid(g_session.tcp_relay_socket));
    if (!have_transport)
        return;

    ensure_runtime_latency_storage();
    const Uint32 now_ms = SDL_GetTicks();
    if (now_ms < g_session.runtime_latency_next_send_ms)
        return;

    // Client measures RTT to host (player 0) by pinging and waiting for PONG.
    const unsigned host_player = 0;
    if (host_player >= g_session.runtime_latency_inflight_ms.size())
        return;
    auto &inflight = g_session.runtime_latency_inflight_ms[host_player];
    for (auto it = inflight.begin(); it != inflight.end(); ) {
        if (now_ms >= it->second && (now_ms - it->second) > 5000)
            it = inflight.erase(it);
        else
            ++it;
    }

    if (g_session.runtime_latency_next_ping_id == 0)
        g_session.runtime_latency_next_ping_id = 1;
    const Uint32 ping_id = g_session.runtime_latency_next_ping_id++;
    inflight[ping_id] = now_ms;

    protocol::PingPacket msg;
    msg.ping_id = ping_id;
    ecl::Buffer payload;
    protocol::encode_ping(payload, msg);
    g_transport.ClientSendUnreliable(payload);

    g_session.runtime_latency_next_send_ms = now_ms + 1000;
}

void tick_host_periodic_debug_options_broadcast(double dtime) {
    if (!g_session.active || !g_session.host)
        return;
    if (g_session.phase != SessionState::Phase::RUNNING)
        return;
    if (!has_remote_peers())
        return;
    g_session.debug_options_broadcast_timer += dtime;
    if (g_session.debug_options_broadcast_timer < 0.5)
        return;
    g_session.debug_options_broadcast_timer = 0.0;
    SessionBroadcastDebugOptions();
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

void tick_host_reannounce_load(double dtime) {
    if (!g_session.host || !g_session.active)
        return;
    if (g_session.expected_players <= 1)
        return;
    if (!has_remote_peers())
        return;
    if (g_session.load_id == 0 || g_session.level_id.empty())
        return;

    // Robustness: re-announce the current level load while the host is waiting
    // for peers to become READY. This must work both while still in the lobby UI
    // (before Msg_StartGame / WAITING_FOR_READY) and during the deferred-start
    // phase inside the loaded level. UDP relay forwarding can reorder/delay
    // packets; periodic announcements prevent a single missed NET_LOAD_LEVEL from
    // deadlocking the session forever.
    if (host_ready_to_start()) {
        g_session.load_announce_timer = 0.0;
        return;
    }

    g_session.load_announce_timer += dtime;
    if (g_session.load_announce_timer < 0.5)
        return;
    send_load_level_to_peers(g_session.level_pack_name, g_session.level_id);
    g_session.load_announce_timer = 0.0;
}

void tick_update_start_phase() {
    if (g_session.phase == SessionState::Phase::WAITING_FOR_READY && g_session.host) {
        if (!host_ready_to_start()) {
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
    if (g_session.phase != SessionState::Phase::RUNNING || !server::WorldInitialized)
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
    } else if (options::GetBool("MultiplayerDebugVisualPrediction")) {
        // Mixed-time visual prediction suppresses local resync while the local actor
        // is still moving. Once the actor settles, we want the latest authoritative
        // snapshot immediately so post-collision drift heals in one step instead of
        // waiting for a coarse periodic broadcast cadence.
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
    tick_host_reannounce_load(dtime);
    tick_update_start_phase();
    tick_host_runtime_latency_probes();
    tick_client_runtime_latency_probe();
    tick_host_periodic_debug_options_broadcast(dtime);
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
