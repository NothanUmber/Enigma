#ifndef GUI_MULTIPLAYER_WAIT_MENU_HH_INCLUDED
#define GUI_MULTIPLAYER_WAIT_MENU_HH_INCLUDED

#include "gui/Menu.hh"
#include "gui/widgets.hh"

#include <string>

namespace enigma {
namespace gui {

class MultiplayerWaitMenu : public Menu {
public:
    MultiplayerWaitMenu();

    void SetMessage(std::string message);
    void SetCountdownSeconds(int seconds);
    bool LeaveRequested() const { return leave_requested_; }

protected:
    bool on_event(const SDL_Event &e) override;
    void on_action(gui::Widget *w) override;
    void draw_background(ecl::GC &gc) override;

private:
    void update_text_cache();

    std::string message_;
    int countdown_seconds_ = 0;
    std::string text_cache_;

    gui::StaticTextButton *leave_button_ = nullptr;
    bool leave_requested_ = false;
};

}  // namespace gui
}  // namespace enigma

#endif  // GUI_MULTIPLAYER_WAIT_MENU_HH_INCLUDED

