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
    multiplayer::CancelClientJoin();
    internet_in_room = false;
    internet_start_valid = false;
    internet_room_code.clear();
    internet_is_host = false;
    internet_connecting = false;
    internet_player_count = 1;
    internet_last_join_session_id = 0;
    internet_last_join_failed = false;
    internet_join_in_progress = false;
    internet_join_start = multiplayer::protocol::LobbyStart();
    internet_join_host_ip.clear();
}

void MultiplayerMenu::leave_current_internet_room() {
    // Leaving should cancel any in-progress join attempt (it can keep retrying for a
    // while via timeouts even though the UI is already back in the lobby).
    multiplayer::CancelClientJoin();
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
    if (broadcast_start) {
        // Start listening before broadcasting the start message to avoid a race
        // where clients attempt to connect before the host socket is bound.
        multiplayer::LobbyBroadcastStart(start);
    }
    enter_game_from_lobby();
    return true;
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
        }
    }

    multiplayer::protocol::LobbyStart start;
    std::vector<std::string> host_ips;
    if (!multiplayer::LobbyPollStart(start, host_ips))
        return;

    if (start.session_id != lan_last_join_session_id) {
        lan_last_join_session_id = start.session_id;
        lan_last_join_failed = false;
        multiplayer::CancelClientJoin();
        lan_join_in_progress = false;
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
    if (internet_join_in_progress) {
        multiplayer::ClientJoinStatus st = poll_client_join_and_maybe_enter_game();
        if (st == multiplayer::ClientJoinStatus::FAILED) {
            show_info(_("Failed to join host."));
            internet_last_join_failed = true;
            internet_join_in_progress = false;
            set_internet_connecting(false);
        } else if (st == multiplayer::ClientJoinStatus::JOINED) {
            internet_join_in_progress = false;
            set_internet_connecting(false);
        }
    }

    if (!internet_in_room) {
        multiplayer::CancelClientJoin();
        internet_join_in_progress = false;
        return;
    }

    internet_poll_timer += dtime;
    if (internet_poll_timer < 0.2)
        return;
    internet_poll_timer = 0.0;

    std::string server = mp_menu::multiplayer_server_host_from_options();
    std::string room = room_field ? room_field->getText() : "";
    mp_menu::InternetServers servers = mp_menu::resolve_internet_servers(server);
    multiplayer::protocol::LobbyStart start;
    std::string host_ip;
    std::string error;
    bool started = false;
    unsigned player_count = 0;
    if (!multiplayer::InternetPollRoom(mp_menu::resolved_lobby_server(server, servers), room,
                                       start, host_ip, started, player_count, error)) {
        if (!error.empty() && error != "waiting")
            show_info(error);
        return;
    }

    if (player_count > 0 && player_count != internet_player_count) {
        internet_player_count = player_count;
        update_players();
        invalidate_all();
    }
    if (!started) {
        internet_last_join_session_id = 0;
        internet_last_join_failed = false;
        multiplayer::CancelClientJoin();
        internet_join_in_progress = false;
        set_internet_connecting(false);
        return;
    }
    if (internet_is_host)
        return;

    if (start.session_id != internet_last_join_session_id) {
        internet_last_join_session_id = start.session_id;
        internet_last_join_failed = false;
        multiplayer::CancelClientJoin();
        internet_join_in_progress = false;
        set_internet_connecting(false);
    }
    if (internet_last_join_failed)
        return;

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
        } else {
            internet_join_in_progress = true;
        }
    }
    refresh_selection();
}

}  // namespace gui
}  // namespace enigma
