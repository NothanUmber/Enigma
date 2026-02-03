/*
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "gui/MultiplayerMenu.hh"

#include "game.hh"
#include "gui/LevelPackMenu.hh"
#include "input.hh"
#include "main.hh"
#include "multiplayer.hh"
#include "nls.hh"
#include "errors.hh"
#include "resource_cache.hh"
#include "lev/Proxy.hh"
#include "lev/Index.hh"
#include "lev/RatingManager.hh"
#include "ecl_util.hh"

#include <algorithm>
#include <cstdlib>
#include <utility>

using namespace ecl;
using namespace std;

namespace enigma { namespace gui {

namespace {
    static lev::VolatileIndex *s_lobby_index = nullptr;

    lev::VolatileIndex *ensure_lobby_index() {
        if (s_lobby_index != nullptr)
            return s_lobby_index;
        std::vector<std::string> empty;
        s_lobby_index = new lev::VolatileIndex("Multiplayer Lobby",
                "Network Levels", "Multiplayer", empty, INDEX_DEFAULT_PACK_LOCATION);
        lev::Index::registerIndex(s_lobby_index);
        return s_lobby_index;
    }

    lev::Index *find_pack_for_level_id(const std::string &level_id, lev::Index *lobby_index) {
        std::vector<std::string> groups = lev::Index::getGroupNames();
        for (const auto &group : groups) {
            std::vector<lev::Index *> *indices = lev::Index::getGroup(group);
            if (!indices)
                continue;
            for (auto *index : *indices) {
                if (!index || index == lobby_index)
                    continue;
                if (index->hasNormLevelPath(level_id))
                    return index;
            }
        }
        return nullptr;
    }

    std::string relay_address_from_lobby(const std::string &server) {
        std::string host = server;
        Uint16 port = 12347;
        std::string::size_type pos = server.rfind(':');
        if (pos != std::string::npos) {
            host = server.substr(0, pos);
            std::string port_str = server.substr(pos + 1);
            if (!port_str.empty()) {
                char *end = nullptr;
                long parsed = std::strtol(port_str.c_str(), &end, 10);
                if (end && *end == '\0' && parsed > 0 && parsed <= 65535)
                    port = static_cast<Uint16>(parsed);
            }
        }
        if (host.empty())
            host = "localhost";
        Uint16 relay_port = static_cast<Uint16>(port + 1);
        return host + ":" + std::to_string(relay_port);
    }

    bool proxy_player_info(lev::Proxy *proxy, unsigned &players, bool &optimized) {
        if (!proxy)
            return false;
        try {
            proxy->loadMetadata(true);
        } catch (XLevelLoading &) {
            return false;
        }
        bool network = proxy->hasNetworkMode();
        bool single = proxy->hasSingleMode();
        if (!network && !single)
            return false;
        optimized = network;
        players = network ? proxy->getNetworkPlayers() : 1;
        if (players < 1)
            players = 1;
        return true;
    }
}

MultiplayerMenu::MultiplayerMenu()
    : levelwidget(nullptr),
      levelpack_button(nullptr),
      filter_button(nullptr),
      mode_button(nullptr),
      create_room_button(nullptr),
      join_room_button(nullptr),
      leave_room_button(nullptr),
      back_button(nullptr),
      info_label(nullptr),
      waiting_label(nullptr),
      internet_buttons(nullptr),
      internet_leave_buttons(nullptr),
      server_label(nullptr),
      room_label(nullptr),
      players_label(nullptr),
      server_field(nullptr),
      room_field(nullptr),
      players_field(nullptr),
      previous_index_pos(0),
      info_ttl(0.0),
      last_lobby_size(0),
      filter_min_players(1),
      internet_mode(false),
      internet_start_valid(false),
      internet_in_room(false),
      internet_is_host(false),
      internet_connecting(false),
      internet_poll_timer(0.0),
      internet_player_count(1),
      internet_form_x(0),
      internet_form_y(0),
      internet_buttons_x(0),
      internet_buttons_y(0),
      lobby_index(nullptr) {
    multiplayer::LobbyStart();

    lev::Index *prev = lev::Index::getCurrentIndex();
    if (prev) {
        previous_index_name = prev->getName();
        previous_index_pos = prev->getCurrentPosition();
    }
    selected_pack_name = previous_index_name;

    lobby_index = ensure_lobby_index();
    lev::Index::setCurrentIndex(lobby_index->getName());

    const VMInfo &vminfo = *video_engine->GetInfo();
    int margin = (vminfo.width < 640) ? 10 : 20;
    int bottom_h = (vminfo.width < 640) ? 50 : 60;
    int left_w = vminfo.width / 3;
    int right_x = margin + left_w + margin;
    int top_y = margin + 40;
    int left_h = vminfo.height - top_y - bottom_h;
    int line_h = (vminfo.width < 640) ? 18 : 22;
    int field_h = (vminfo.width < 640) ? 18 : 24;
    int button_h = (vminfo.width < 640) ? 25 : 35;
    int players_h = left_h;

    level_area = Rect(right_x, top_y, vminfo.width - right_x - margin,
                      vminfo.height - top_y - bottom_h);
    levelwidget = new LevelWidget();
    levelwidget->set_listener(this);
    levelwidget->realize(level_area);
    levelwidget->set_area(level_area);
    add(levelwidget);
    waiting_label = new UntranslatedLabel(_("Waiting for host..."), HALIGN_CENTER);
    add(waiting_label, Rect(level_area.x, level_area.y + level_area.h / 2 - 10,
                            level_area.w, 20));

    Label *players_title = new Label(N_("Players in Lobby"), HALIGN_LEFT);

    VList *players_list = new VList();
    players_list->set_spacing(5);
    players_list->set_alignment(HALIGN_LEFT, VALIGN_TOP);
    for (unsigned i = 0; i < input::kMaxPlayers; ++i) {
        UntranslatedLabel *label = new UntranslatedLabel("", HALIGN_LEFT);
        player_labels.push_back(label);
        players_list->add_back(label);
    }

    info_label = new UntranslatedLabel("", HALIGN_LEFT);

    levelpack_button = new StaticTextButton(N_("Level Pack"), this);
    filter_button = new StaticTextButton("", this);
    update_filter_button();
    mode_button = new StaticTextButton(N_("Mode: LAN"), this);
    create_room_button = new StaticTextButton(N_("Create Room"), this);
    join_room_button = new StaticTextButton(N_("Join Room"), this);
    leave_room_button = new StaticTextButton(N_("Leave Room"), this);
    back_button = new StaticTextButton(N_("Back"), this);

    HList *buttons = new HList();
    buttons->set_spacing(10);
    buttons->set_alignment(HALIGN_CENTER, VALIGN_TOP);
    buttons->set_default_size((vminfo.width < 640) ? 100 : 140,
                              (vminfo.width < 640) ? 25 : 35);
    buttons->add_back(levelpack_button);
    buttons->add_back(filter_button);
    buttons->add_back(mode_button);
    buttons->add_back(back_button);
    add(buttons, Rect(0, vminfo.height - bottom_h + 5,
                      vminfo.width, bottom_h - 10));

    server_label = new Label(N_("Lobby server:"), HALIGN_LEFT);
    server_field = new TextField("localhost:12347");
    room_label = new Label(N_("Room code:"), HALIGN_LEFT);
    room_field = new TextField("");
    players_label = new Label(N_("Players:"), HALIGN_LEFT);
    players_field = new TextField("2");

    int field_w = left_w;
    int internet_buttons_h = button_h + 8;
    int form_h = 2 * (line_h + field_h + 4) + internet_buttons_h + 6;
    int title_h = line_h;
    int players_list_h = players_h - form_h - title_h - 10;
    if (players_list_h < 0)
        players_list_h = 0;
    int players_title_y = top_y;
    int players_list_y = players_title_y + title_h + 5;
    int form_x = margin;
    int info_h = line_h;
    int form_y = players_list_y + players_list_h + 4;
    int info_y = form_y - info_h - 6;
    internet_form_x = form_x;
    internet_form_y = form_y;
    add(players_title, Rect(margin, players_title_y, left_w, title_h));
    add(players_list, Rect(margin, players_list_y, left_w, players_list_h));
    add(info_label, Rect(margin, info_y, left_w, info_h));

    int y = form_y;
    add(server_label, Rect(form_x, y, left_w, line_h));
    y += line_h;
    add(server_field, Rect(form_x, y, field_w, field_h));
    y += field_h + 6;
    add(room_label, Rect(form_x, y, left_w, line_h));
    y += line_h;
    add(room_field, Rect(form_x, y, field_w, field_h));
    y += field_h + 4;
    players_label->set_area(Rect(-10000, -10000, left_w, line_h));
    players_field->set_area(Rect(-10000, -10000, field_w, field_h));
    internet_buttons = new HList();
    internet_buttons->set_spacing(10);
    internet_buttons->set_alignment(HALIGN_CENTER, VALIGN_TOP);
    internet_buttons->set_default_size((vminfo.width < 640) ? 90 : 120,
                                       (vminfo.width < 640) ? 25 : 35);
    internet_buttons->add_back(create_room_button);
    internet_buttons->add_back(join_room_button);
    add(internet_buttons, Rect(form_x, y, field_w, internet_buttons_h));

    internet_leave_buttons = new HList();
    internet_leave_buttons->set_spacing(10);
    internet_leave_buttons->set_alignment(HALIGN_CENTER, VALIGN_TOP);
    internet_leave_buttons->set_default_size((vminfo.width < 640) ? 90 : 120,
                                             button_h);
    internet_leave_buttons->add_back(leave_room_button);
    add(internet_leave_buttons, Rect(form_x, y, field_w, internet_buttons_h));
    internet_buttons_x = form_x;
    internet_buttons_y = y;

    set_internet_mode(false);

    rebuild_index();
    refresh_selection();
}

MultiplayerMenu::~MultiplayerMenu() {
    if (internet_in_room) {
        std::string server = server_field ? server_field->getText() : "";
        std::string room = room_field ? room_field->getText() : "";
        std::string error;
        multiplayer::InternetLeaveRoom(server, room, error);
    }
    multiplayer::LobbyStop();
    if (!previous_index_name.empty()) {
        if (lev::Index::setCurrentIndex(previous_index_name)) {
            lev::Index *ind = lev::Index::getCurrentIndex();
            if (ind)
                ind->setCurrentPosition(previous_index_pos);
        }
    }
}

void MultiplayerMenu::on_action(gui::Widget *w) {
    if (w == levelwidget) {
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
            multiplayer::protocol::LobbyStart start = multiplayer::BuildStartMessage(
                selected_level_id, desired_players(), filter_min_players);
            std::string error;
            std::string server = server_field ? server_field->getText() : "";
            std::string room = room_field ? room_field->getText() : "";
            if (!multiplayer::InternetStartRoom(server, room, start, error)) {
                show_info(error.empty() ? _("Failed to start room.") : error);
                return;
            }
            if (!multiplayer::StartHostSession(start)) {
                show_info(_("Failed to start multiplayer session."));
                return;
            }
            multiplayer::LobbyStop();
            game::StartGame();
            multiplayer::LobbyStart();
            rebuild_index();
            refresh_selection();
            draw_all();
            return;
        }
        if (!can_start()) {
            show_info(_("Need matching players to start."));
            return;
        }
        multiplayer::protocol::LobbyStart start = multiplayer::BuildStartMessage(
            selected_level_id, desired_players(), filter_min_players);
        multiplayer::LobbyBroadcastStart(start);
        if (!multiplayer::StartHostSession(start)) {
            show_info(_("Failed to start multiplayer session."));
            return;
        }
        multiplayer::LobbyStop();
        game::StartGame();
        multiplayer::LobbyStart();
        rebuild_index();
        refresh_selection();
        draw_all();
        return;
    }
    if (w == levelpack_button) {
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
        return;
    }
    if (w == filter_button) {
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
        return;
    }
    if (w == mode_button) {
        set_internet_mode(!internet_mode);
        rebuild_index();
        refresh_selection();
        invalidate_all();
        return;
    }
    if (w == create_room_button) {
        if (!room_field || room_field->getText().empty()) {
            show_info(_("please choose room code"));
            return;
        }
        unsigned players = desired_players();
        std::string server = server_field ? server_field->getText() : "";
        std::string room_code = room_field ? room_field->getText() : "";
        multiplayer::SetRelayServer(relay_address_from_lobby(server));
        multiplayer::protocol::LobbyStart start = multiplayer::BuildStartMessage(
            selected_level_id, players, filter_min_players);
        std::string error;
        if (!multiplayer::InternetCreateRoom(server, room_code, start, error)) {
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
        return;
    }
    if (w == join_room_button) {
        std::string server = server_field ? server_field->getText() : "";
        std::string room = room_field ? room_field->getText() : "";
        if (room.empty()) {
            show_info(_("please choose room code"));
            return;
        }
        multiplayer::SetRelayServer(relay_address_from_lobby(server));
        multiplayer::protocol::LobbyStart start;
        std::string host_ip;
        std::string error;
        unsigned player_count = 0;
        if (!multiplayer::InternetJoinRoom(server, room, start, host_ip, player_count, error)) {
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
        return;
    }
    if (w == leave_room_button) {
        std::string server = server_field ? server_field->getText() : "";
        std::string room = room_field ? room_field->getText() : "";
        std::string error;
        multiplayer::InternetLeaveRoom(server, room, error);
        internet_in_room = false;
        internet_start_valid = false;
        internet_room_code.clear();
        internet_is_host = false;
        internet_connecting = false;
        internet_player_count = 1;
        update_internet_layout();
        show_info(_("Left room."));
        return;
    }
    if (w == back_button) {
        Menu::quit();
        return;
    }
}

void MultiplayerMenu::draw_background(ecl::GC &gc) {
    set_caption(_("Enigma - Multiplayer Lobby"));
    blit(gc, 0, 0, enigma::GetImage("menu_bg", ".jpg"));
}

void MultiplayerMenu::tick(double dtime) {
    update_filter_button();
    if (!internet_mode) {
        multiplayer::LobbyTick(dtime);
        unsigned lobby_size = multiplayer::LobbySize();
        if (lobby_size != last_lobby_size) {
            last_lobby_size = lobby_size;
            rebuild_index();
            refresh_selection();
            invalidate_all();
        }

        multiplayer::protocol::LobbyStart start;
        std::string host_ip;
        if (multiplayer::LobbyPollStart(start, host_ip)) {
        filter_min_players = start.filter_optimized;
        update_filter_button();
            selected_level_id = start.level_id;
            select_pack_for_level(start.level_id);
            rebuild_index();
            if (!set_level_by_id(start.level_id)) {
                show_info(_("Selected level not available."));
            } else if (!multiplayer::StartClientSession(start, host_ip)) {
                show_info(_("Failed to join host."));
            } else {
                multiplayer::LobbyStop();
                game::StartGame();
                multiplayer::LobbyStart();
                rebuild_index();
                draw_all();
            }
            refresh_selection();
        }
    } else {
        if (internet_in_room) {
            internet_poll_timer += dtime;
            if (internet_poll_timer >= 0.2) {
                internet_poll_timer = 0.0;
                std::string server = server_field ? server_field->getText() : "";
                std::string room = room_field ? room_field->getText() : "";
                multiplayer::protocol::LobbyStart start;
                std::string host_ip;
                std::string error;
                bool started = false;
                unsigned player_count = 0;
                if (multiplayer::InternetPollRoom(server, room, start, host_ip,
                                                  started, player_count, error)) {
                    if (player_count > 0 && player_count != internet_player_count) {
                        internet_player_count = player_count;
                        update_players();
                        invalidate_all();
                    }
                    if (started && !internet_is_host) {
                        internet_connecting = true;
                        update_internet_layout();
                        filter_min_players = start.filter_optimized;
                        update_filter_button();
                        selected_level_id = start.level_id;
                        select_pack_for_level(start.level_id);
                        rebuild_index();
                        if (!set_level_by_id(start.level_id)) {
                            show_info(_("Selected level not available."));
                            internet_connecting = false;
                            update_internet_layout();
                        } else if (!multiplayer::StartClientSession(start, host_ip)) {
                            show_info(_("Failed to join host."));
                            internet_connecting = false;
                            update_internet_layout();
                        } else {
                            internet_connecting = false;
                            multiplayer::LobbyStop();
                            game::StartGame();
                            multiplayer::LobbyStart();
                            rebuild_index();
                            draw_all();
                        }
                        refresh_selection();
                    }
                } else if (!error.empty() && error != "waiting") {
                    show_info(error);
                }
            }
        }
    }

    update_players();
    refresh_selection();

    if (info_ttl > 0.0) {
        info_ttl -= dtime;
        if (info_ttl <= 0.0)
            info_label->set_text("");
    }
}

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
        if (min_players == desired_players_count) {
            info_label->set_text(ecl::strf(_("No %u player levels are in this level pack"),
                                           desired_players_count));
        } else {
            info_label->set_text(ecl::strf(_("No %u-%u player levels are in this level pack"),
                                           min_players, desired_players_count));
        }
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
        if (!proxy_player_info(proxy, players, optimized))
            continue;
        if (players < min_players || players > desired_players_count)
            continue;
        filtered.push_back(proxy);
    }

    std::sort(filtered.begin(), filtered.end(),
              [ratings](lev::Proxy *a, lev::Proxy *b) {
                  short da = ratings->getDifficulty(a);
                  short db = ratings->getDifficulty(b);
                  if (da != db)
                      return da < db;
                  const std::string &ta = a->getTitle();
                  const std::string &tb = b->getTitle();
                  if (ta != tb)
                      return ta < tb;
                  return a->getNormLevelPath() < b->getNormLevelPath();
              });

    for (auto *proxy : filtered)
        lobby_index->appendProxy(proxy);

    if (lobby_index->size() == 0)
        selected_level_id.clear();

    if (filtered.empty()) {
        if (min_players == desired_players_count) {
            info_label->set_text(ecl::strf(_("No %u player levels are in this level pack"),
                                           desired_players_count));
        } else {
            info_label->set_text(ecl::strf(_("No %u-%u player levels are in this level pack"),
                                           min_players, desired_players_count));
        }
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
    if (!enabled && internet_in_room) {
        std::string server = server_field ? server_field->getText() : "";
        std::string room = room_field ? room_field->getText() : "";
        std::string error;
        multiplayer::InternetLeaveRoom(server, room, error);
        internet_in_room = false;
        internet_start_valid = false;
        internet_room_code.clear();
        internet_is_host = false;
        internet_connecting = false;
        internet_player_count = 1;
    }
    internet_mode = enabled;
    update_mode_button();
    update_internet_layout();
    if (internet_mode)
        multiplayer::LobbyStop();
    else
        multiplayer::LobbyStart();
}

void MultiplayerMenu::update_internet_layout() {
    int offscreen = -10000;
    int x = internet_mode ? internet_form_x : offscreen;
    int y = internet_mode ? internet_form_y : offscreen;
    bool show_levels = internet_mode && internet_in_room && internet_is_host;
    bool show_wait = internet_mode && !show_levels;
    if (!internet_mode) {
        server_label->move(offscreen, offscreen);
        server_field->move(offscreen, offscreen);
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
        invalidate_all();
        return;
    }
    if (show_levels) {
        levelwidget->set_area(level_area);
        waiting_label->move(offscreen, offscreen);
    } else {
        levelwidget->set_area(Rect(offscreen, offscreen, level_area.w, level_area.h));
        if (!internet_in_room)
            waiting_label->set_text(_("Create or join a room."));
        else if (internet_connecting)
            waiting_label->set_text(_("Connecting..."));
        else
            waiting_label->set_text(_("Waiting for host..."));
        waiting_label->move(level_area.x, level_area.y + level_area.h / 2 - 10);
    }
    server_label->move(x, y);
    server_field->move(x, y + server_label->get_h());
    room_label->move(x, y + server_label->get_h() + server_field->get_h() + 4);
    room_field->move(x, room_label->get_y() + room_label->get_h());
    players_label->move(offscreen, offscreen);
    players_field->move(offscreen, offscreen);
    int buttons_w = internet_buttons->get_w();
    int buttons_h = internet_buttons->get_h();
    int leave_w = internet_leave_buttons->get_w();
    int leave_h = internet_leave_buttons->get_h();
    internet_buttons->set_area(Rect(internet_in_room ? offscreen : internet_buttons_x,
                                    internet_in_room ? offscreen : internet_buttons_y,
                                    buttons_w, buttons_h));
    internet_leave_buttons->set_area(Rect(internet_in_room ? internet_buttons_x : offscreen,
                                          internet_in_room ? internet_buttons_y : offscreen,
                                          leave_w, leave_h));
    bool lock_fields = internet_in_room;
    server_field->set_locked(lock_fields);
    room_field->set_locked(lock_fields);
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
        unsigned count = internet_player_count > 0 ? internet_player_count : 1;
        multiplayer::LobbyPeer self;
        self.name = "Player";
        self.is_self = true;
        peers.push_back(self);
        for (unsigned i = 1; i < count; ++i) {
            multiplayer::LobbyPeer peer;
            peer.name = "Player";
            peer.is_self = false;
            peers.push_back(peer);
        }
    } else {
        peers = multiplayer::LobbyPeers();
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
    if (required_players() > input::kMaxPlayers)
        return false;
    if (internet_mode)
        return desired_players() >= required_players();
    return multiplayer::LobbySize() >= required_players();
}

unsigned MultiplayerMenu::required_players() const {
    lev::Index *ind = lev::Index::getCurrentIndex();
    if (!ind || ind->size() == 0)
        return 1;
    lev::Proxy *proxy = ind->getCurrent();
    unsigned players = 1;
    bool optimized = false;
    if (!proxy_player_info(proxy, players, optimized))
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
    lev::Index *pack = find_pack_for_level_id(level_id, lobby_index);
    if (!pack)
        return false;
    selected_pack_name = pack->getName();
    return true;
}

void MultiplayerMenu::show_info(const std::string &text) {
    info_label->set_text(text);
    info_ttl = 3.0;
}

void ShowMultiplayerMenu() {
    MultiplayerMenu m;
    m.manage();
}

}}  // namespace enigma::gui
