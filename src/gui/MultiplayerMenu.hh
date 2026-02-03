#ifndef GUI_MULTIPLAYERMENU_HH_INCLUDED
#define GUI_MULTIPLAYERMENU_HH_INCLUDED

#include "gui/Menu.hh"
#include "gui/LevelWidget.hh"
#include "gui/widgets.hh"
#include "gui/TextField.hh"
#include "multiplayer_protocol.hh"

#include "lev/VolatileIndex.hh"

#include <string>
#include <vector>

namespace enigma { namespace gui {

class MultiplayerMenu : public gui::Menu {
public:
    MultiplayerMenu();
    ~MultiplayerMenu();

private:
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
    gui::Label *server_label;
    gui::Label *room_label;
    gui::Label *players_label;
    gui::TextField *server_field;
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
    enigma::multiplayer::protocol::LobbyStart internet_start;
    bool internet_start_valid;
    bool internet_in_room;
    bool internet_is_host;
    bool internet_connecting;
    double internet_poll_timer;
    unsigned internet_player_count;
    int internet_form_x;
    int internet_form_y;
    int internet_buttons_x;
    int internet_buttons_y;
    ecl::Rect level_area;

    lev::VolatileIndex *lobby_index;
};

void ShowMultiplayerMenu();

}} // namespace enigma::gui

#endif
