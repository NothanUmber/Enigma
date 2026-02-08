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

#include <cstring>

#ifdef WIN32
#include <winsock2.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* -------------------- Multiplayer ENet socket -------------------- */
/*
 * ENet API compatibility wrappers.
 *
 * System ENet and vendored ENet differ in some low-level APIs across versions.
 * Multiplayer uses these wrappers to compile and behave consistently.
 */

namespace enigma {
namespace multiplayer {
namespace internal {

ENetSocket enet_socket_create_compat(ENetSocketType type) {
#ifdef ENET_VER_EQ_GT_13
    return enet_socket_create(type);
#else
    return enet_socket_create(type, nullptr);
#endif
}

void tune_enet_socket(ENetSocket socket, bool broadcast) {
    if (socket == ENET_SOCKET_NULL)
        return;

    int opt = 1;
#ifdef WIN32
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));
    if (broadcast)
        setsockopt(socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&opt), sizeof(opt));
    u_long mode = 1;
    ioctlsocket(socket, FIONBIO, &mode);
#else
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (broadcast)
        setsockopt(socket, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    // Force non-blocking behavior across ENet variants. We never want UI/game
    // loops to block on a recvfrom()/sendto().
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags != -1)
        (void)fcntl(socket, F_SETFL, flags | O_NONBLOCK);
#endif

    // Increase buffers a bit to reduce packet loss under load.
    int buf_sz = 1 << 20;  // 1 MiB
#ifdef WIN32
    setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&buf_sz), sizeof(buf_sz));
    setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&buf_sz), sizeof(buf_sz));
#else
    setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));
    setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));
#endif
}

bool bind_lobby_socket(ENetSocket socket, Uint16 port) {
    int opt = 1;
#ifdef WIN32
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(socket, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char *>(&opt), sizeof(opt));
#endif
    setsockopt(socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&opt), sizeof(opt));
    u_long mode = 1;
    ioctlsocket(socket, FIONBIO, &mode);
#else
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(socket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
    setsockopt(socket, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    // Make the lobby socket non-blocking. Some ENet builds do not set this by
    // default, and a blocking recvfrom() would stall the UI loop.
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags != -1) {
        (void)fcntl(socket, F_SETFL, flags | O_NONBLOCK);
    }
#endif

    sockaddr_in sin;
    ::memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = ENET_HOST_TO_NET_16(port);
    sin.sin_addr.s_addr = ENET_HOST_ANY;
    bool ok = bind(socket, reinterpret_cast<sockaddr *>(&sin), sizeof(sin)) == 0;
    if (ok) {
        // Make sure socket behaves consistently across ENet builds.
        tune_enet_socket(socket, true);
    }
    return ok;
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
