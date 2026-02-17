/*
 * Copyright (C) 2026
 * 2026 LLM generated contribution - concept, review and revision by Ferdinand Strixner
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "gui/MultiplayerMenu.hh"

#include "gui/MultiplayerMenu_internal.hh"
#include "input.hh"
#include "main.hh"
#include "multiplayer.hh"
#include "nls.hh"

#include "lev/Index.hh"

#include "ecl_util.hh"

#include <algorithm>

using namespace ecl;
using namespace std;

namespace enigma {
namespace gui {

namespace {
struct MultiplayerMenuSessionState {
    bool has_state = false;
    bool internet_mode = false;
    bool internet_in_room = false;
    bool internet_is_host = false;
    std::string room_code;
    unsigned player_count = 1;
    std::vector<multiplayer::LobbyPeer> peers;
};

MultiplayerMenuSessionState g_menu_session;
}  // namespace

void MultiplayerMenu::restore_session_state() {
    if (!g_menu_session.has_state)
        return;
    internet_mode = g_menu_session.internet_mode;
    internet_in_room = g_menu_session.internet_in_room;
    internet_is_host = g_menu_session.internet_is_host;
    internet_room_code = g_menu_session.room_code;
    internet_player_count = std::max<unsigned>(1, g_menu_session.player_count);
    internet_room_peers = g_menu_session.peers;
    if (room_field)
        room_field->set_text(internet_room_code);
}

void MultiplayerMenu::store_session_state() const {
    g_menu_session.has_state = true;
    g_menu_session.internet_mode = internet_mode;
    g_menu_session.internet_in_room = internet_in_room;
    g_menu_session.internet_is_host = internet_is_host;
    // Prefer the tracked room code when we're in-room; it is the authoritative value
    // used for network calls and should survive UI transitions.
    if (internet_in_room && !internet_room_code.empty()) {
        g_menu_session.room_code = internet_room_code;
    } else if (room_field) {
        g_menu_session.room_code = room_field->getText();
    } else {
        g_menu_session.room_code = internet_room_code;
    }
    g_menu_session.player_count = std::max<unsigned>(1, internet_player_count);
    g_menu_session.peers = internet_room_peers;
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
      room_label(nullptr),
      players_label(nullptr),
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
      lan_last_join_session_id(0),
      lan_last_join_failed(false),
      internet_last_join_session_id(0),
      internet_last_join_failed(false),
      lan_join_in_progress(false),
      internet_join_in_progress(false),
      internet_join_candidate_session_id(0),
      internet_join_candidate_session_streak(0),
      internet_join_retry_backoff(0.0),
      host_waiting_for_peers(false),
      host_pending_expected(0),
      internet_form_x(0),
      internet_form_y(0),
      internet_buttons_x(0),
      internet_buttons_y(0),
      lobby_index(nullptr) {
    lev::Index *prev = lev::Index::getCurrentIndex();
    if (prev) {
        previous_index_name = prev->getName();
        previous_index_pos = prev->getCurrentPosition();
    }
    selected_pack_name = previous_index_name;

    lobby_index = mp_menu::ensure_lobby_index();
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
    info_area_default = Rect(margin, info_y, left_w, info_h);

    int y = form_y;
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

    restore_session_state();
    set_internet_mode(internet_mode);
    if (internet_mode && internet_in_room) {
        // Force an immediate poll on the first tick after reopening so the room state
        // and player list refresh right away.
        internet_poll_timer = 0.2;
    }

    rebuild_index();
    refresh_selection();
}

MultiplayerMenu::~MultiplayerMenu() {
    multiplayer::CancelClientJoin("menu closed");
    store_session_state();
    // If the host started a session but never entered the game (for example
    // while waiting for peers to connect), ensure we don't keep listening in
    // the background after leaving the menu.
    multiplayer::Shutdown();
    multiplayer::LobbyStop();
    if (!previous_index_name.empty()) {
        if (lev::Index::setCurrentIndex(previous_index_name)) {
            lev::Index *ind = lev::Index::getCurrentIndex();
            if (ind)
                ind->setCurrentPosition(previous_index_pos);
        }
    }
}

void ShowMultiplayerMenu() {
    MultiplayerMenu m;
    m.manage();
}

}  // namespace gui
}  // namespace enigma
