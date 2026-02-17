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

#include "multiplayer_internal.hh"

#include "main.hh"

#include <random>
#include <sstream>

/* -------------------- Multiplayer identity -------------------- */
/*
 * Local identity helpers for the LAN lobby.
 *
 * Generates a stable per-install ID and a user-visible default name.
 */

namespace enigma {
namespace multiplayer {
namespace internal {

std::string make_id() {
    std::random_device rd;
    uint64_t value = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    std::ostringstream out;
    out << std::hex << value;
    return out.str();
}

std::string resolve_local_name() {
    std::string name = app.state->getString("UserName");
    if (name.empty())
        name = "Player";
    return name;
}

void ensure_lobby_identity() {
    if (g_lobby.local_id.empty())
        g_lobby.local_id = make_id();
    // UserName can change while the process is running (Options menu). Keep the
    // lobby display name fresh so announces and UI reflect the current setting.
    g_lobby.local_name = resolve_local_name();
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
