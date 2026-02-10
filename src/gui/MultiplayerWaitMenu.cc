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

#include "gui/MultiplayerWaitMenu.hh"

#include "ecl_font.hh"
#include "ecl_video.hh"
#include "nls.hh"
#include "resource_cache.hh"
#include "video.hh"

using namespace ecl;

namespace enigma {
namespace gui {

namespace {
std::string format_text(const std::string &message, int seconds_left) {
    std::string text = message;
    if (!text.empty())
        text += "\n";
    text += ecl::strf("Leaving lobby in %d seconds.", seconds_left);
    text += "\n";
    text += "Press Leave to cancel for everybody.";
    return text;
}
}  // namespace

MultiplayerWaitMenu::MultiplayerWaitMenu()
    : message_("Waiting for player...") {
    const VMInfo *vminfo = video_engine->GetInfo();
    const int vshrink = vminfo->width < 640 ? 1 : 0;
    leave_button_ = new gui::StaticTextButton(N_("Leave"), this);
    add(leave_button_,
        Rect(vminfo->width - (vshrink ? 85 : 170),
             vminfo->height - (vshrink ? 30 : 60),
             vshrink ? 75 : 150, vshrink ? 42 : 40));
    update_text_cache();
}

void MultiplayerWaitMenu::SetMessage(std::string message) {
    message_ = std::move(message);
    update_text_cache();
}

void MultiplayerWaitMenu::SetCountdownSeconds(int seconds) {
    if (seconds < 0)
        seconds = 0;
    if (countdown_seconds_ == seconds)
        return;
    countdown_seconds_ = seconds;
    update_text_cache();
}

bool MultiplayerWaitMenu::on_event(const SDL_Event &e) {
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
        // Swallow ESC: leaving this screen should be an explicit action.
        return true;
    }
    return false;
}

void MultiplayerWaitMenu::on_action(gui::Widget *w) {
    if (w == leave_button_) {
        leave_requested_ = true;
        Menu::quit();
    }
}

void MultiplayerWaitMenu::update_text_cache() {
    text_cache_ = format_text(message_, countdown_seconds_);
    invalidate_all();
}

void MultiplayerWaitMenu::draw_background(ecl::GC &gc) {
    blit(gc, 0, 0, enigma::GetImage("menu_bg", ".jpg"));
    Font *f = enigma::GetFont("menufont");

    std::vector<std::string> lines;
    ecl::split_copy(text_cache_, '\n', back_inserter(lines));
    int x = 60;
    int y = 60;
    int yskip = 25;
    const VMInfo *vminfo = video_engine->GetInfo();
    int width = vminfo->width - 120;
    for (unsigned i = 0; i < lines.size(); i++) {
        std::vector<std::string> subLines = ecl::breakToLines(f, lines[i], " ", width);
        for (auto it = subLines.begin(); it != subLines.end(); it++) {
            f->render(gc, x, y, *it);
            y += yskip;
        }
    }
}

}  // namespace gui
}  // namespace enigma
