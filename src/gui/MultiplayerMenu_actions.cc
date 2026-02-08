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
#include "gui/LevelPackMenu.hh"
#include "multiplayer.hh"
#include "nls.hh"

#include "lev/Index.hh"

/* -------------------- Multiplayer menu actions -------------------- */
/*
 * Multiplayer menu event handlers (create/join/leave/start/restart).
 *
 * This file contains the "button action" side of the menu logic.
 */

namespace enigma {
namespace gui {

void MultiplayerMenu::handle_level_activated() {
    refresh_selection();
    if (internet_mode) {
        if (!internet_in_room) {
            show_info(_("Create or join a room to start."));
            return;
        }
        if (!internet_is_host) {
            show_info(_("Waiting for host."));
            return;
        }
        if (desired_players() < 2) {
            show_info(_("Need at least two players to start."));
            return;
        }

        multiplayer::protocol::LobbyStart start = multiplayer::BuildStartMessage(
            selected_level_id, desired_players(), filter_min_players);
        std::string error;
        std::string server = mp_menu::multiplayer_server_host_from_options();
        std::string room = current_room_code();
        mp_menu::InternetServers servers = mp_menu::resolve_internet_servers(server);
        if (!multiplayer::InternetStartRoom(mp_menu::resolved_lobby_server(server, servers),
                                            room, start, error)) {
            show_info(error.empty() ? _("Failed to start room.") : error);
            return;
        }
        start_host_and_enter_game(start, false);
        return;
    }

    if (!multiplayer::LoadMultiplayerConfig().enable_direct) {
        show_info(_("Enable Direct Connect for LAN mode."));
        return;
    }
    if (desired_players() < 2) {
        show_info(_("Need at least two players to start."));
        return;
    }
    if (!can_start()) {
        show_info(_("Need matching players to start."));
        return;
    }
    multiplayer::protocol::LobbyStart start = multiplayer::BuildStartMessage(
        selected_level_id, desired_players(), filter_min_players);
    start_host_and_enter_game(start, true);
}

void MultiplayerMenu::handle_level_pack() {
    if (internet_mode && internet_in_room && !internet_is_host) {
        show_info(_("Waiting for host."));
        return;
    }
    if (!selected_pack_name.empty())
        lev::Index::setCurrentIndex(selected_pack_name);
    LevelPackMenu menu(true);
    menu.manage();
    lev::Index *current = lev::Index::getCurrentIndex();
    if (current) {
        selected_pack_name = current->getName();
        previous_index_name = selected_pack_name;
        previous_index_pos = current->getCurrentPosition();
    }
    if (!lev::Index::setCurrentIndex(lobby_index->getName()))
        return;
    rebuild_index();
    refresh_selection();
    invalidate_all();
}

void MultiplayerMenu::handle_filter() {
    if (internet_mode && internet_in_room && !internet_is_host) {
        show_info(_("Waiting for host."));
        return;
    }
    unsigned players = desired_players();
    if (filter_min_players >= players)
        filter_min_players = 1;
    else
        filter_min_players += 1;
    update_filter_button();
    rebuild_index();
    refresh_selection();
    invalidate_all();
}

void MultiplayerMenu::handle_mode_toggle() {
    set_internet_mode(!internet_mode);
    rebuild_index();
    refresh_selection();
    invalidate_all();
}

void MultiplayerMenu::handle_create_room() {
    if (current_room_code().empty()) {
        show_info(_("please choose room code"));
        return;
    }
    unsigned players = desired_players();
    std::string server = mp_menu::multiplayer_server_host_from_options();
    std::string room_code = current_room_code();
    mp_menu::InternetServers servers = mp_menu::resolve_internet_servers(server);
    if (servers.lobby.empty()) {
        show_info(_("Invalid server address."));
        return;
    }
    multiplayer::SetRelayServer(servers.udp_relay);
    multiplayer::SetTcpRelayServer(servers.tcp_relay);
    multiplayer::protocol::LobbyStart start = multiplayer::BuildStartMessage(
        selected_level_id, players, filter_min_players);
    std::string error;
    if (!multiplayer::InternetCreateRoom(servers.lobby, room_code, start, error)) {
        show_info(error.empty() ? _("Failed to create room.") : error);
        return;
    }
    internet_room_code = room_code;
    internet_start = start;
    internet_start_valid = true;
    internet_in_room = true;
    internet_is_host = true;
    internet_connecting = false;
    internet_player_count = 1;
    update_internet_layout();
    show_info(_("Room created. Click a level to start."));
}

void MultiplayerMenu::handle_join_room() {
    std::string server = mp_menu::multiplayer_server_host_from_options();
    std::string room = current_room_code();
    if (room.empty()) {
        show_info(_("please choose room code"));
        return;
    }
    mp_menu::InternetServers servers = mp_menu::resolve_internet_servers(server);
    if (servers.lobby.empty()) {
        show_info(_("Invalid server address."));
        return;
    }
    multiplayer::SetRelayServer(servers.udp_relay);
    multiplayer::SetTcpRelayServer(servers.tcp_relay);
    multiplayer::protocol::LobbyStart start;
    std::string host_ip;
    std::string error;
    unsigned player_count = 0;
    if (!multiplayer::InternetJoinRoom(servers.lobby, room, start, host_ip, player_count, error)) {
        show_info(error.empty() ? _("Failed to join room.") : error);
        return;
    }
    internet_room_code = room;
    internet_in_room = true;
    internet_is_host = false;
    internet_connecting = false;
    internet_player_count = player_count > 0 ? player_count : 1;
    update_internet_layout();
    internet_poll_timer = 0.0;
    show_info(_("Waiting for host."));
}

void MultiplayerMenu::handle_leave_room() {
    leave_current_internet_room();
    show_info(_("Left room."));
}

void MultiplayerMenu::on_action(gui::Widget *w) {
    if (w == levelwidget) {
        handle_level_activated();
        return;
    }
    if (w == levelpack_button) {
        handle_level_pack();
        return;
    }
    if (w == filter_button) {
        handle_filter();
        return;
    }
    if (w == mode_button) {
        handle_mode_toggle();
        return;
    }
    if (w == create_room_button) {
        handle_create_room();
        return;
    }
    if (w == join_room_button) {
        handle_join_room();
        return;
    }
    if (w == leave_room_button) {
        handle_leave_room();
        return;
    }
    if (w == back_button)
        Menu::quit();
}

}  // namespace gui
}  // namespace enigma
