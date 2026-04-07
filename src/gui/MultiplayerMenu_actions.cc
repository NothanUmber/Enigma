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
    if (host_waiting_for_peers) {
        show_info(_("Waiting for other players to connect..."));
        return;
    }
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

        // Ensure the start message fully qualifies the level across packs.
        if (selected_pack_name.empty())
            select_pack_for_level(selected_level_id);
        multiplayer::protocol::LobbyStart start = multiplayer::BuildStartMessage(
            selected_level_id, desired_players(), selected_pack_name, filter_min_players);
        std::string error;
        std::string server = mp_menu::multiplayer_server_host_from_options();
        // Use the tracked room code for network calls; the text field may be stale.
        std::string room = !internet_room_code.empty() ? internet_room_code : current_room_code();
        mp_menu::InternetEndpoints servers = mp_menu::resolve_internet_servers(server);
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
    if (selected_pack_name.empty())
        select_pack_for_level(selected_level_id);
    multiplayer::protocol::LobbyStart start = multiplayer::BuildStartMessage(
        selected_level_id, desired_players(), selected_pack_name, filter_min_players);
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
    const std::string server_warning =
        _("Set lobby and relay server IP in Options/Multiplayer.");
    if (current_room_code().empty()) {
        show_info(_("please choose room code"));
        return;
    }
    unsigned players = desired_players();
    std::string server = mp_menu::multiplayer_server_host_from_options();
    std::string room_code = current_room_code();
    mp_menu::InternetEndpoints servers = mp_menu::resolve_internet_servers(server);
    if (!servers.lobby.is_valid() && !servers.lobby_control.is_valid()) {
        show_info(server_warning);
        return;
    }
    multiplayer::SetRelayServer(servers.udp_relay.server);
    multiplayer::SetWebSocketRelayUrl(servers.websocket_relay.url);
    multiplayer::SetTcpRelayServer(servers.tcp_relay.server);
    if (selected_pack_name.empty())
        select_pack_for_level(selected_level_id);
    multiplayer::protocol::LobbyStart start = multiplayer::BuildStartMessage(
        selected_level_id, players, selected_pack_name, filter_min_players);
    std::string error;
    if (!multiplayer::InternetCreateRoom(servers.lobby.server, room_code, start, error)) {
        show_info(error.empty() ? _("Failed to create room.") : error);
        return;
    }
    internet_room_code = room_code;
    if (room_field)
        room_field->set_text(internet_room_code);
    internet_room_peers.clear();
    multiplayer::LobbyPeer self;
    self.id = start.host_id;
    self.name = multiplayer::LobbyLocalName();
    self.is_self = true;
    internet_room_peers.push_back(self);
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
    const std::string server_warning =
        _("Set lobby and relay server IP in Options/Multiplayer.");
    std::string server = mp_menu::multiplayer_server_host_from_options();
    std::string room = current_room_code();
    if (room.empty()) {
        show_info(_("please choose room code"));
        return;
    }
    mp_menu::InternetEndpoints servers = mp_menu::resolve_internet_servers(server);
    if (!servers.lobby.is_valid() && !servers.lobby_control.is_valid()) {
        show_info(server_warning);
        return;
    }
    multiplayer::SetRelayServer(servers.udp_relay.server);
    multiplayer::SetWebSocketRelayUrl(servers.websocket_relay.url);
    multiplayer::SetTcpRelayServer(servers.tcp_relay.server);
    multiplayer::protocol::LobbyStart start;
    std::string host_ip;
    std::string error;
    unsigned player_count = 0;
    std::vector<multiplayer::LobbyPeer> peers;
    if (!multiplayer::InternetJoinRoom(servers.lobby.server, room, start, host_ip, player_count,
                                       peers, error)) {
        show_info(error.empty() ? _("Failed to join room.") : error);
        return;
    }
    internet_room_code = room;
    if (room_field)
        room_field->set_text(internet_room_code);
    internet_room_peers = peers;
    internet_in_room = true;
    internet_is_host = false;
    internet_connecting = false;
    if (!internet_room_peers.empty())
        internet_player_count = static_cast<unsigned>(internet_room_peers.size());
    else
        internet_player_count = player_count > 0 ? player_count : 1;
    update_internet_layout();
    internet_poll_timer = 0.0;
    show_info(_("Waiting for host."));
}

void MultiplayerMenu::handle_leave_room() {
    leave_current_internet_room();
    if (multiplayer::IsActive())
        multiplayer::Shutdown();
    host_waiting_for_peers = false;
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
    if (w == back_button) {
        if (multiplayer::IsActive())
            multiplayer::Shutdown();
        host_waiting_for_peers = false;
        Menu::quit();
    }
}

}  // namespace gui
}  // namespace enigma
