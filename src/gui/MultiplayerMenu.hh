#ifndef GUI_MULTIPLAYERMENU_HH_INCLUDED
#define GUI_MULTIPLAYERMENU_HH_INCLUDED

#include "gui/Menu.hh"
#include "gui/LevelWidget.hh"
#include "gui/widgets.hh"
#include "gui/TextField.hh"
#include "multiplayer_protocol.hh"
#include "multiplayer.hh"

#include "lev/VolatileIndex.hh"

#include <string>
#include <vector>

namespace enigma { namespace gui {

class MultiplayerMenu : public gui::Menu {
public:
    MultiplayerMenu();
    ~MultiplayerMenu();

private:
    void restore_session_state();
    void store_session_state() const;
    void on_action(gui::Widget *w) override;
    void draw_background(ecl::GC &gc) override;
    void tick(double dtime) override;

    void rebuild_index();
    void refresh_selection();
    void update_players();
    void update_filter_button();
    void update_mode_button();
    void set_internet_mode(bool enabled);
    void update_internet_layout();
    unsigned desired_players() const;
    bool parse_players_field(unsigned &players) const;
    bool can_start() const;
    unsigned required_players() const;
    bool set_level_by_id(const std::string &level_id);
    bool select_pack_for_level(const std::string &level_id);
    void show_info(const std::string &text);
    void show_transport_info(int transport_kind);
    std::string current_room_code() const;
    void clear_internet_room_state();
    void leave_current_internet_room();
    void enter_game_from_lobby();
    void apply_start_selection(const multiplayer::protocol::LobbyStart &start);
    void set_internet_connecting(bool connecting);
    bool start_host_and_enter_game(const multiplayer::protocol::LobbyStart &start,
                                   bool broadcast_start);
    void tick_host_waiting_for_peers(double dtime);
    bool begin_client_join(const multiplayer::protocol::LobbyStart &start,
                           const std::vector<std::string> &host_ips);
    multiplayer::ClientJoinStatus poll_client_join_and_maybe_enter_game();
    void handle_level_activated();
    void handle_level_pack();
    void handle_filter();
    void handle_mode_toggle();
    void handle_create_room();
    void handle_join_room();
    void handle_leave_room();
    void tick_lan_mode(double dtime);
    void tick_internet_mode(double dtime);

    gui::LevelWidget *levelwidget;
    gui::StaticTextButton *levelpack_button;
    gui::StaticTextButton *filter_button;
    gui::StaticTextButton *mode_button;
    gui::StaticTextButton *create_room_button;
    gui::StaticTextButton *join_room_button;
    gui::StaticTextButton *leave_room_button;
    gui::StaticTextButton *back_button;
    gui::Label *info_label;
    gui::Label *waiting_label;
    gui::HList *internet_buttons;
    gui::HList *internet_leave_buttons;
    gui::Label *room_label;
    gui::Label *players_label;
    gui::TextField *room_field;
    gui::TextField *players_field;

    std::vector<gui::UntranslatedLabel *> player_labels;
    std::string selected_level_id;
    std::string selected_pack_name;
    std::string previous_index_name;
    int previous_index_pos;
    double info_ttl;
    unsigned last_lobby_size;
    unsigned filter_min_players;
    bool internet_mode;
    std::string internet_room_code;
    std::vector<multiplayer::LobbyPeer> internet_room_peers;
    enigma::multiplayer::protocol::LobbyStart internet_start;
    bool internet_start_valid;
    bool internet_in_room;
    bool internet_is_host;
    bool internet_connecting;
    double internet_poll_timer;
    unsigned internet_player_count;
    Uint32 lan_last_join_session_id;
    bool lan_last_join_failed;
    Uint32 internet_last_join_session_id;
    bool internet_last_join_failed;
    bool lan_join_in_progress;
    multiplayer::protocol::LobbyStart lan_join_start;
    std::string lan_join_host_ip;
    bool internet_join_in_progress;
    multiplayer::protocol::LobbyStart internet_join_start;
    std::string internet_join_host_ip;
    bool host_waiting_for_peers;
    multiplayer::protocol::LobbyStart host_pending_start;
    unsigned host_pending_expected;
    int internet_form_x;
    int internet_form_y;
    int internet_buttons_x;
    int internet_buttons_y;
    ecl::Rect info_area_default;
    ecl::Rect level_area;

    lev::VolatileIndex *lobby_index;
};

void ShowMultiplayerMenu();

}} // namespace enigma::gui

#endif
