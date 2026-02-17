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

#include "gui/MultiplayerMenu.hh"

#include "gui/MultiplayerMenu_internal.hh"
#include "game.hh"
#include "multiplayer.hh"
#include "nls.hh"

/* -------------------- Multiplayer menu tick -------------------- */
/*
 * Multiplayer menu per-frame tick logic.
 *
 * Handles lobby polling/updates and the "connecting / waiting" UI transitions.
 */

namespace enigma {
namespace gui {

std::string MultiplayerMenu::current_room_code() const {
    return room_field ? room_field->getText() : "";
}

void MultiplayerMenu::clear_internet_room_state() {
    multiplayer::CancelClientJoin("clear room state");
    internet_in_room = false;
    internet_start_valid = false;
    internet_room_code.clear();
    internet_room_peers.clear();
    internet_is_host = false;
    internet_connecting = false;
    internet_player_count = 1;
    internet_last_join_session_id = 0;
    internet_last_join_failed = false;
    internet_join_in_progress = false;
    internet_join_candidate_session_id = 0;
    internet_join_candidate_session_streak = 0;
    internet_join_retry_backoff = 0.0;
    internet_join_start = multiplayer::protocol::LobbyStart();
    internet_join_host_ip.clear();
}

void MultiplayerMenu::leave_current_internet_room() {
    // Leaving should cancel any in-progress join attempt (it can keep retrying for a
    // while via timeouts even though the UI is already back in the lobby).
    multiplayer::CancelClientJoin("leave room");
    internet_join_in_progress = false;
    lan_join_in_progress = false;

    std::string server = mp_menu::multiplayer_server_host_from_options();
    mp_menu::InternetServers servers = mp_menu::resolve_internet_servers(server);
    std::string error;
    std::string room = !internet_room_code.empty() ? internet_room_code : current_room_code();
    if (!multiplayer::InternetLeaveRoom(mp_menu::resolved_lobby_server(server, servers),
                                        room, error)) {
        // Best-effort: even if the leave packet did not get an ACK, we still leave locally.
        // The server will eventually expire stale rooms, and the user can re-join if needed.
        if (!error.empty())
            show_info(error);
    }
    clear_internet_room_state();
    update_internet_layout();
}

void MultiplayerMenu::show_transport_info(int transport_kind) {
    switch (static_cast<multiplayer::TransportKind>(transport_kind)) {
    case multiplayer::TransportKind::TCP_RELAY:
        show_info(_("Using TCP relay (higher latency)."));
        break;
    case multiplayer::TransportKind::UDP_RELAY:
        show_info(_("Using UDP relay."));
        break;
    case multiplayer::TransportKind::DIRECT:
        show_info(_("Using direct connection."));
        break;
    default:
        break;
    }
}

void MultiplayerMenu::enter_game_from_lobby() {
    multiplayer::LobbyStop();
    game::StartGame();
    multiplayer::LobbyStart();
    rebuild_index();
    refresh_selection();
    draw_all();
}

void MultiplayerMenu::apply_start_selection(const multiplayer::protocol::LobbyStart &start) {
    filter_min_players = start.filter_optimized;
    update_filter_button();
    selected_level_id = start.level_id;
    if (!start.pack_name.empty()) {
        selected_pack_name = start.pack_name;
        // Ensure the pack index is active/known before rebuilding the filtered lobby index.
        // This mirrors what the host did when selecting the pack via LevelPackMenu.
        if (!lev::Index::setCurrentIndex(selected_pack_name)) {
            // If pack selection fails (different installation, renamed pack), fall back to
            // best-effort lookup by level id. This keeps LAN play usable even when the
            // pack name differs across machines.
            select_pack_for_level(start.level_id);
            if (!selected_pack_name.empty())
                lev::Index::setCurrentIndex(selected_pack_name);
        }
    } else {
        // Backward compat with older peers/servers: best-effort lookup by level id.
        select_pack_for_level(start.level_id);
    }
    rebuild_index();
}

void MultiplayerMenu::set_internet_connecting(bool connecting) {
    internet_connecting = connecting;
    update_internet_layout();
}

bool MultiplayerMenu::start_host_and_enter_game(const multiplayer::protocol::LobbyStart &start,
                                                bool broadcast_start) {
    if (!multiplayer::StartHostSession(start)) {
        show_info(_("Failed to start multiplayer session."));
        return false;
    }
    // Important: publish the intended level as a NET_LOAD_LEVEL so clients can set
    // `last_load_id` and send READY. Without this, hosts waiting in the lobby can
    // deadlock forever ("other players connecting...") because clients refuse to
    // READY until they have observed a load-id.
    multiplayer::NotifyLoadLevel(start.pack_name, start.level_id);
    if (broadcast_start) {
        // Start listening before broadcasting the start message to avoid a race
        // where clients attempt to connect before the host socket is bound.
        multiplayer::LobbyBroadcastStart(start);
    }
    host_pending_start = start;
    host_pending_expected = std::max<unsigned>(1, start.expected_players);
    host_waiting_for_peers = (host_pending_expected > 1);
    if (!host_waiting_for_peers) {
        enter_game_from_lobby();
        return true;
    }
    show_info(_("Waiting for other players to connect..."));
    return true;
}

void MultiplayerMenu::tick_host_waiting_for_peers(double dtime) {
    if (!host_waiting_for_peers)
        return;

    // Pump the multiplayer transport while still in the lobby UI. This allows
    // clients to connect and reach READY (after switching packs / loading).
    multiplayer::Tick(dtime);

    // Wait until the session runtime reports that start should no longer be
    // deferred. For hosts this means: all expected peers connected + READY.
    if (multiplayer::ShouldDeferStart())
        return;

    host_waiting_for_peers = false;
    // Ensure the host loads exactly the level that was announced in the lobby start
    // message. The lobby UI can rebuild indices while waiting, which can change the
    // current selection; enforce the pending start selection right before StartGame().
    apply_start_selection(host_pending_start);
    set_level_by_id(host_pending_start.level_id);
    enter_game_from_lobby();
}

bool MultiplayerMenu::begin_client_join(const multiplayer::protocol::LobbyStart &start,
                                        const std::vector<std::string> &host_ips) {
    if (!set_level_by_id(start.level_id)) {
        show_info(_("Selected level not available."));
        return false;
    }
    if (!multiplayer::BeginClientJoin(start, host_ips)) {
        return false;
    }
    return true;
}

multiplayer::ClientJoinStatus MultiplayerMenu::poll_client_join_and_maybe_enter_game() {
    multiplayer::ClientJoinStatus status = multiplayer::PollClientJoin();
    if (status == multiplayer::ClientJoinStatus::JOINED) {
        show_transport_info(static_cast<int>(multiplayer::ActiveTransport()));
        enter_game_from_lobby();
    }
    return status;
}

void MultiplayerMenu::tick(double dtime) {
    update_filter_button();
    tick_host_waiting_for_peers(dtime);
    if (!internet_mode)
        tick_lan_mode(dtime);
    else
        tick_internet_mode(dtime);

    update_players();
    refresh_selection();

    if (info_ttl > 0.0) {
        info_ttl -= dtime;
        if (info_ttl <= 0.0)
            info_label->set_text("");
    }
}

void MultiplayerMenu::tick_lan_mode(double dtime) {
    multiplayer::LobbyTick(dtime);
    unsigned lobby_size = multiplayer::LobbySize();
    if (lobby_size != last_lobby_size) {
        last_lobby_size = lobby_size;
        rebuild_index();
        refresh_selection();
        invalidate_all();
    }

    if (lan_join_in_progress) {
        multiplayer::ClientJoinStatus st = poll_client_join_and_maybe_enter_game();
        if (st == multiplayer::ClientJoinStatus::FAILED) {
            show_info(_("Failed to join host. LAN play requires direct UDP connectivity."));
            lan_last_join_failed = true;
            lan_join_in_progress = false;
        } else if (st == multiplayer::ClientJoinStatus::JOINED) {
            lan_join_in_progress = false;
            // We just switched to in-game; don't keep processing lobby events in this tick.
            return;
        }
    }

    multiplayer::protocol::LobbyStart start;
    std::vector<std::string> host_ips;
    if (!multiplayer::LobbyPollStart(start, host_ips))
        return;

    if (start.session_id != lan_last_join_session_id) {
        // If a join attempt is already in progress, don't cancel it immediately.
        // Under jittery conditions (or if the host restarted quickly), session_id
        // can change while the client is still waiting for WELCOME, and canceling
        // would cause "Connecting..." thrash. Let the current attempt time out or
        // succeed; the next tick will join the new session if needed.
        if (!lan_join_in_progress) {
            lan_last_join_session_id = start.session_id;
            lan_last_join_failed = false;
            multiplayer::CancelClientJoin("lan: session changed");
        }
    }
    if (lan_last_join_failed)
        return;

    apply_start_selection(start);
    lan_join_start = start;
    lan_join_host_ip = host_ips.empty() ? std::string() : host_ips[0];
    if (!begin_client_join(start, host_ips)) {
        show_info(_("Failed to join host. LAN play requires direct UDP connectivity."));
        lan_last_join_failed = true;
        lan_join_in_progress = false;
    } else {
        lan_join_in_progress = true;
    }
    refresh_selection();
}

void MultiplayerMenu::tick_internet_mode(double dtime) {
    if (internet_join_retry_backoff > 0.0) {
        internet_join_retry_backoff -= dtime;
        if (internet_join_retry_backoff < 0.0)
            internet_join_retry_backoff = 0.0;
    }
    if (internet_join_in_progress) {
        multiplayer::ClientJoinStatus st = poll_client_join_and_maybe_enter_game();
        if (st == multiplayer::ClientJoinStatus::FAILED) {
            show_info(_("Failed to join host."));
            internet_last_join_failed = true;
            internet_join_in_progress = false;
            set_internet_connecting(false);
            // Backoff to avoid spamming join attempts under packet loss.
            internet_join_retry_backoff = 1.0;
        } else if (st == multiplayer::ClientJoinStatus::JOINED) {
            internet_join_in_progress = false;
            set_internet_connecting(false);
            // We just switched to in-game; don't keep processing lobby events in this tick.
            return;
        }
    }

    if (!internet_in_room) {
        multiplayer::CancelClientJoin("internet: not in room");
        internet_join_in_progress = false;
        return;
    }

    internet_poll_timer += dtime;
    if (internet_poll_timer < 0.2)
        return;
    internet_poll_timer = 0.0;

    std::string server = mp_menu::multiplayer_server_host_from_options();
    // Use the tracked room code for networking. The text field is a UI artifact and
    // can temporarily be empty/out-of-sync across menu transitions.
    std::string room = !internet_room_code.empty()
                           ? internet_room_code
                           : (room_field ? room_field->getText() : "");
    mp_menu::InternetServers servers = mp_menu::resolve_internet_servers(server);
    multiplayer::protocol::LobbyStart start;
    std::string host_ip;
    std::string error;
    bool started = false;
    unsigned player_count = 0;
    std::vector<multiplayer::LobbyPeer> peers;
    if (!multiplayer::InternetPollRoom(mp_menu::resolved_lobby_server(server, servers), room,
                                       start, host_ip, started, player_count, peers, error)) {
        if (!error.empty() && error != "waiting") {
            if (error == "Room not found.") {
                clear_internet_room_state();
                update_internet_layout();
                invalidate_all();
                show_info(_("Room closed by host."));
            } else {
                show_info(error);
            }
        }
        return;
    }

    bool peers_changed = false;
    if (peers.size() != internet_room_peers.size()) {
        peers_changed = true;
    } else {
        for (size_t i = 0; i < peers.size(); ++i) {
            if (peers[i].id != internet_room_peers[i].id ||
                peers[i].name != internet_room_peers[i].name ||
                peers[i].is_self != internet_room_peers[i].is_self) {
                peers_changed = true;
                break;
            }
        }
    }
    if (peers_changed)
        internet_room_peers = peers;
    unsigned effective_count = player_count;
    if (!internet_room_peers.empty())
        effective_count = static_cast<unsigned>(internet_room_peers.size());
    if (effective_count > 0 && effective_count != internet_player_count) {
        internet_player_count = effective_count;
        update_players();
        invalidate_all();
    } else if (peers_changed) {
        update_players();
        invalidate_all();
    }
    if (!started) {
        internet_last_join_session_id = 0;
        internet_last_join_failed = false;
        multiplayer::CancelClientJoin("internet: not started");
        internet_join_in_progress = false;
        set_internet_connecting(false);
        internet_join_candidate_session_id = 0;
        internet_join_candidate_session_streak = 0;
        internet_join_retry_backoff = 0.0;
        return;
    }
    if (internet_is_host)
        return;

    // If we're already trying to join but the lobby reports a different session id,
    // it's likely that the host restarted the session (abort/restart) or that we
    // observed an out-of-order poll response. Avoid waiting for a full welcome
    // timeout on a stale session; only switch after observing the new session id
    // consistently a couple times to prevent "Connecting..." thrash under jitter.
    if (internet_join_in_progress && start.session_id != internet_join_start.session_id) {
        if (start.session_id == internet_join_candidate_session_id) {
            internet_join_candidate_session_streak += 1;
        } else {
            internet_join_candidate_session_id = start.session_id;
            internet_join_candidate_session_streak = 1;
        }
        if (internet_join_candidate_session_streak >= 2) {
            multiplayer::CancelClientJoin("internet: start changed");
            internet_join_in_progress = false;
            set_internet_connecting(false);
            internet_last_join_failed = false;
            internet_join_retry_backoff = 0.0;
            internet_last_join_session_id = start.session_id;
            // Fall through: we'll start a fresh join attempt for the new session below.
        } else {
            // Keep the in-flight attempt for now.
            return;
        }
    } else {
        internet_join_candidate_session_id = 0;
        internet_join_candidate_session_streak = 0;
    }

    if (start.session_id != internet_last_join_session_id) {
        // Same rationale as LAN: avoid canceling a join attempt that already has
        // a live transport connection, otherwise we can loop "Connecting..." when
        // the lobby's session id flaps (host restarts, packet loss, etc.).
        if (!internet_join_in_progress) {
            internet_last_join_session_id = start.session_id;
            internet_last_join_failed = false;
            multiplayer::CancelClientJoin("internet: session changed");
            set_internet_connecting(false);
        }
    }
    if (internet_last_join_failed) {
        if (internet_join_retry_backoff > 0.0)
            return;
        internet_last_join_failed = false;
    }

    apply_start_selection(start);
    if (!internet_join_in_progress) {
        set_internet_connecting(true);
        internet_join_start = start;
        internet_join_host_ip = host_ip;
        std::vector<std::string> host_ips;
        if (!host_ip.empty())
            host_ips.push_back(host_ip);
        if (!begin_client_join(start, host_ips)) {
            show_info(_("Failed to join host."));
            internet_last_join_failed = true;
            internet_join_in_progress = false;
            set_internet_connecting(false);
            internet_join_retry_backoff = 1.0;
        } else {
            internet_join_in_progress = true;
        }
    }
    refresh_selection();
}

}  // namespace gui
}  // namespace enigma
