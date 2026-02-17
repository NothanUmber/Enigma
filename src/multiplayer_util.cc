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

#include <cstdlib>

/* -------------------- Multiplayer utilities -------------------- */
/*
 * Small shared parsing helpers.
 */

namespace enigma {
namespace multiplayer {
namespace internal {

bool parse_host_port(const std::string &value, std::string &host, Uint16 &port,
                     Uint16 default_port) {
    host.clear();
    port = default_port;
    if (value.empty())
        return false;
    std::string::size_type pos = value.rfind(':');
    if (pos == std::string::npos) {
        host = value;
    } else {
        host = value.substr(0, pos);
        std::string port_str = value.substr(pos + 1);
        if (!port_str.empty()) {
            char *end = nullptr;
            long parsed = std::strtol(port_str.c_str(), &end, 10);
            if (end && *end == '\0' && parsed > 0 && parsed <= 65535)
                port = static_cast<Uint16>(parsed);
        }
    }

    if (host == "localhost" || host == "::1")
        host = "127.0.0.1";
    return !host.empty();
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
