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

#include "multiplayer_internal.hh"

/* -------------------- Multiplayer globals -------------------- */
/*
 * Global multiplayer state instances.
 *
 * The multiplayer subsystem is integrated into the existing game loop and
 * reuses several globals (server/client/world). Keeping MP state in one TU
 * avoids header-only globals and makes lifetime transitions explicit.
 */

namespace enigma {
namespace multiplayer {
namespace internal {

LobbyState g_lobby;
SessionState g_session;
std::string g_relay_server;
std::string g_tcp_relay_server;

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
