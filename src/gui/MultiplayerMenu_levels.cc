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
#include "ecl_video.hh"
#include "input.hh"
#include "multiplayer.hh"
#include "multiplayer_config.hh"
#include "nls.hh"
#include "resource_cache.hh"

#include "lev/Index.hh"
#include "lev/RatingManager.hh"

#include <algorithm>
#include <cstdlib>
#include <vector>

using namespace ecl;
using namespace std;

/* -------------------- Multiplayer menu levels -------------------- */
/*
 * Level pack discovery, filtering, and thumbnails for the multiplayer lobby.
 *
 * This file owns the level list, selection state, and filtering UI behavior.
 */

namespace enigma {
namespace gui {

void MultiplayerMenu::rebuild_index() {
    if (!lobby_index)
        return;
    std::string keep = selected_level_id;
    lobby_index->clear();
    unsigned desired_players_count = desired_players();
    unsigned min_players = filter_min_players;
    if (min_players < 1)
        min_players = 1;
    if (min_players > desired_players_count)
        min_players = desired_players_count;

    lev::Index *pack_index = nullptr;
    if (!selected_pack_name.empty())
        pack_index = lev::Index::findIndex(selected_pack_name);
    if (!pack_index)
        pack_index = lev::Index::getCurrentIndex();
    if (!pack_index) {
        info_label->set_text(mp_menu::no_level_message(min_players, desired_players_count));
        info_ttl = -1.0;
        lev::Index::setCurrentIndex(lobby_index->getName());
        levelwidget->syncFromIndexMgr();
        return;
    }

    std::vector<lev::Proxy *> proxies;
    if (pack_index) {
        proxies.reserve(static_cast<size_t>(pack_index->size()));
        for (int i = 0; i < pack_index->size(); ++i) {
            if (lev::Proxy *proxy = pack_index->getProxy(i))
                proxies.push_back(proxy);
        }
    }

    std::vector<lev::Proxy *> filtered;
    filtered.reserve(proxies.size());
    lev::RatingManager *ratings = lev::RatingManager::instance();
    for (auto *proxy : proxies) {
        unsigned players = 0;
        bool optimized = false;
        if (!mp_menu::proxy_player_info(proxy, players, optimized))
            continue;
        if (players > desired_players_count)
            continue;
        if (players < min_players)
            continue;
        filtered.push_back(proxy);
    }

    std::sort(filtered.begin(), filtered.end(),
              [ratings](lev::Proxy *a, lev::Proxy *b) {
                  short da = ratings ? ratings->getDifficulty(a) : 0;
                  short db = ratings ? ratings->getDifficulty(b) : 0;
                  if (da != db)
                      return da < db;
                  std::string ta = a->getTitle();
                  std::string tb = b->getTitle();
                  if (ta != tb)
                      return ta < tb;
                  return a->getNormLevelPath() < b->getNormLevelPath();
              });

    for (auto *proxy : filtered)
        lobby_index->appendProxy(proxy);

    if (lobby_index->size() == 0)
        selected_level_id.clear();

    if (filtered.empty()) {
        info_label->set_text(mp_menu::no_level_message(min_players, desired_players_count));
        info_ttl = -1.0;
    } else if (info_ttl < 0.0) {
        info_label->set_text("");
        info_ttl = 0.0;
    }

    lev::Index::setCurrentIndex(lobby_index->getName());
    if (!keep.empty())
        set_level_by_id(keep);
    levelwidget->syncFromIndexMgr();
}

void MultiplayerMenu::refresh_selection() {
    lev::Index *ind = lev::Index::getCurrentIndex();
    if (!ind || ind->size() == 0)
        return;
    lev::Proxy *proxy = ind->getCurrent();
    if (!proxy)
        return;
    std::string level_id = proxy->getNormLevelPath();
    if (level_id != selected_level_id) {
        selected_level_id = level_id;
        multiplayer::LobbySetSelectedLevel(selected_level_id);
    }
}

void MultiplayerMenu::update_filter_button() {
    if (!filter_button)
        return;
    unsigned players = desired_players();
    if (players < 1)
        players = 1;
    if (filter_min_players < 1)
        filter_min_players = 1;
    if (filter_min_players > players)
        filter_min_players = players;
    if (filter_min_players == players) {
        filter_button->set_text(ecl::strf(_("Maps for %u"), players));
    } else {
        filter_button->set_text(ecl::strf(_("Maps for %u-%u"),
                                          filter_min_players, players));
    }
}

void MultiplayerMenu::update_mode_button() {
    if (!mode_button)
        return;
    if (internet_mode)
        mode_button->set_text(N_("Mode: Internet"));
    else
        mode_button->set_text(N_("Mode: LAN"));
}

void MultiplayerMenu::set_internet_mode(bool enabled) {
    // Switching modes should never leave a join attempt running in the background,
    // otherwise the lobby tick can keep polling a stale connect attempt.
    multiplayer::CancelClientJoin("mode switch");
    lan_join_in_progress = false;
    internet_join_in_progress = false;
    if (!enabled && internet_in_room)
        leave_current_internet_room();
    internet_mode = enabled;
    update_mode_button();
    update_internet_layout();
    if (internet_mode) {
        multiplayer::LobbyStop();
        return;
    }

    multiplayer::MultiplayerConfig cfg = multiplayer::LoadMultiplayerConfig();
    if (!cfg.enable_direct) {
        info_label->set_text(_("Enable Direct Connect for LAN mode."));
        info_ttl = -1.0;
        multiplayer::LobbyStop();
        return;
    }

    if (info_ttl < 0.0) {
        info_label->set_text("");
        info_ttl = 0.0;
    }
    multiplayer::LobbyStart();
}

void MultiplayerMenu::update_internet_layout() {
    const std::string server_warning =
        _("Set lobby and relay server IP in Options/Multiplayer.");
    const std::string waiting_create = _("Create or join a room.");
    const std::string waiting_connecting = _("Connecting...");
    const std::string waiting_host = _("Waiting for host...");
    int offscreen = -10000;
    int x = internet_mode ? internet_form_x : offscreen;
    int y = internet_mode ? internet_form_y : offscreen;
    bool show_levels = internet_mode && internet_in_room && internet_is_host;
    std::string waiting_text;
    if (!internet_mode) {
        room_label->move(offscreen, offscreen);
        room_field->move(offscreen, offscreen);
        players_label->move(offscreen, offscreen);
        players_field->move(offscreen, offscreen);
        internet_buttons->set_area(Rect(offscreen, offscreen,
                                        internet_buttons->get_w(),
                                        internet_buttons->get_h()));
        internet_leave_buttons->set_area(Rect(offscreen, offscreen,
                                              internet_leave_buttons->get_w(),
                                              internet_leave_buttons->get_h()));
        levelwidget->set_area(level_area);
        waiting_label->move(offscreen, offscreen);
        info_label->set_area(info_area_default);
        invalidate_all();
        return;
    }
    if (show_levels) {
        levelwidget->set_area(level_area);
        waiting_label->move(offscreen, offscreen);
        info_label->set_area(info_area_default);
    } else {
        levelwidget->set_area(Rect(offscreen, offscreen, level_area.w, level_area.h));
        if (!internet_in_room)
            waiting_text = waiting_create;
        else if (internet_connecting)
            waiting_text = waiting_connecting;
        else
            waiting_text = waiting_host;
        waiting_label->set_text(waiting_text);
        waiting_label->move(offscreen, offscreen);
        int info_w = (level_area.x + level_area.w) - info_area_default.x;
        info_label->set_area(Rect(info_area_default.x, info_area_default.y,
                                  info_w, info_area_default.h));
    }
    room_label->move(x, y);
    room_field->move(x, y + room_label->get_h());
    players_label->move(offscreen, offscreen);
    players_field->move(offscreen, offscreen);
    int compact_buttons_w = info_area_default.w;
    int expanded_buttons_w = (level_area.x + level_area.w) - info_area_default.x;
    int buttons_w = show_levels ? compact_buttons_w : expanded_buttons_w;
    int buttons_h = internet_buttons->get_h();
    int leave_w = show_levels ? compact_buttons_w : expanded_buttons_w;
    int leave_h = internet_leave_buttons->get_h();
    internet_buttons->set_area(Rect(internet_in_room ? offscreen : internet_buttons_x,
                                    internet_in_room ? offscreen : internet_buttons_y,
                                    buttons_w, buttons_h));
    internet_leave_buttons->set_area(Rect(internet_in_room ? internet_buttons_x : offscreen,
                                          internet_in_room ? internet_buttons_y : offscreen,
                                          leave_w, leave_h));
    bool lock_fields = internet_in_room;
    room_field->set_locked(lock_fields);
    std::string current_info = info_label->getText();
    bool has_status_message = (current_info == waiting_create ||
                               current_info == waiting_connecting ||
                               current_info == waiting_host);
    if (!internet_in_room) {
        std::string server = mp_menu::multiplayer_server_host_from_options();
        mp_menu::InternetServers servers = mp_menu::resolve_internet_servers(server);
        if (servers.lobby.empty()) {
            info_label->set_text(server_warning);
            info_ttl = -1.0;
        } else if (current_info == server_warning) {
            info_label->set_text("");
            info_ttl = 0.0;
        }
    }
    if (!show_levels && !waiting_text.empty()) {
        current_info = info_label->getText();
        bool has_persistent_info = (info_ttl < 0.0 && current_info != server_warning);
        if (current_info != server_warning &&
            !has_persistent_info && (info_ttl <= 0.0 || has_status_message ||
                                     current_info.empty())) {
            info_label->set_text(waiting_text);
            if (info_ttl > 0.0)
                info_ttl = 0.0;
        }
    } else if (show_levels) {
        current_info = info_label->getText();
        if (current_info == waiting_create ||
            current_info == waiting_connecting ||
            current_info == waiting_host) {
            info_label->set_text("");
            if (info_ttl <= 0.0)
                info_ttl = 0.0;
        }
    }
    create_room_button->set_text(N_("Create Room"));
    invalidate_all();
}

unsigned MultiplayerMenu::desired_players() const {
    if (!internet_mode)
        return multiplayer::LobbySize() == 0 ? 1 : multiplayer::LobbySize();
    if (internet_in_room && internet_player_count > 0)
        return internet_player_count;
    unsigned players = 0;
    if (parse_players_field(players))
        return players;
    return 2;
}

bool MultiplayerMenu::parse_players_field(unsigned &players) const {
    if (!players_field)
        return false;
    std::string text = players_field->getText();
    if (text.empty())
        return false;
    char *end = nullptr;
    long value = std::strtol(text.c_str(), &end, 10);
    if (!end || *end != '\0')
        return false;
    if (value < 1)
        value = 1;
    if (value > static_cast<long>(input::kMaxPlayers))
        value = input::kMaxPlayers;
    players = static_cast<unsigned>(value);
    return true;
}

void MultiplayerMenu::update_players() {
    std::vector<multiplayer::LobbyPeer> peers;
    if (internet_mode) {
        peers = internet_room_peers;
        if (peers.empty()) {
            unsigned count = internet_player_count > 0 ? internet_player_count : 1;
            multiplayer::LobbyPeer self;
            self.name = multiplayer::LobbyLocalName();
            self.is_self = true;
            peers.push_back(self);
            for (unsigned i = 1; i < count; ++i) {
                multiplayer::LobbyPeer peer;
                peer.name = ecl::strf(_("Player %u"), static_cast<unsigned>(i + 1));
                peer.is_self = false;
                peers.push_back(peer);
            }
        } else {
            for (size_t i = 0; i < peers.size(); ++i) {
                if (peers[i].name.empty()) {
                    if (peers[i].is_self)
                        peers[i].name = multiplayer::LobbyLocalName();
                    else
                        peers[i].name = ecl::strf(_("Player %u"), static_cast<unsigned>(i + 1));
                }
            }
        }
    } else {
        peers = multiplayer::LobbyPeers();
        if (peers.empty()) {
            multiplayer::LobbyPeer self;
            self.name = multiplayer::LobbyLocalName();
            self.is_self = true;
            peers.push_back(self);
        }
        std::sort(peers.begin(), peers.end(),
                  [](const multiplayer::LobbyPeer &a, const multiplayer::LobbyPeer &b) {
                      if (a.is_self != b.is_self)
                          return a.is_self;
                      return a.name < b.name;
                  });
    }
    for (size_t i = 0; i < player_labels.size(); ++i) {
        if (i < peers.size()) {
            std::string label = peers[i].name;
            if (peers[i].is_self) {
                label += " (you)";
            }
            player_labels[i]->set_text(label);
        } else {
            player_labels[i]->set_text("");
        }
    }
}

bool MultiplayerMenu::can_start() const {
    if (selected_level_id.empty())
        return false;
    if (desired_players() < 2)
        return false;
    if (required_players() > input::kMaxPlayers)
        return false;
    if (internet_mode)
        return desired_players() >= required_players();
    if (!multiplayer::LoadMultiplayerConfig().enable_direct)
        return false;
    return multiplayer::LobbySize() >= required_players();
}

unsigned MultiplayerMenu::required_players() const {
    lev::Index *ind = lev::Index::getCurrentIndex();
    if (!ind || ind->size() == 0)
        return 1;
    lev::Proxy *proxy = ind->getCurrent();
    unsigned players = 1;
    bool optimized = false;
    if (!mp_menu::proxy_player_info(proxy, players, optimized))
        return 1;
    return players;
}

bool MultiplayerMenu::set_level_by_id(const std::string &level_id) {
    if (!lobby_index)
        return false;
    for (int i = 0; i < lobby_index->size(); ++i) {
        lev::Proxy *proxy = lobby_index->getProxy(i);
        if (proxy && proxy->getNormLevelPath() == level_id) {
            lobby_index->setCurrentPosition(i);
            levelwidget->syncFromIndexMgr();
            return true;
        }
    }
    return false;
}

bool MultiplayerMenu::select_pack_for_level(const std::string &level_id) {
    if (!lobby_index)
        return false;
    lev::Index *pack = mp_menu::find_pack_for_level_id(level_id, lobby_index);
    if (!pack)
        return false;
    selected_pack_name = pack->getName();
    return true;
}

void MultiplayerMenu::show_info(const std::string &text) {
    info_label->set_text(text);
    info_ttl = 3.0;
}

void MultiplayerMenu::draw_background(ecl::GC &gc) {
    set_caption(_("Enigma - Multiplayer Lobby"));
    blit(gc, 0, 0, enigma::GetImage("menu_bg", ".jpg"));
}

}  // namespace gui
}  // namespace enigma
