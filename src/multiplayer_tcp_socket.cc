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

#include "SDL.h"

#include <cstring>
#include <string>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* -------------------- Multiplayer TCP socket -------------------- */
/*
 * Small TCP socket helpers used by the TCP relay transport.
 *
 * Isolated from session code to avoid platform socket ifdef noise elsewhere.
 */

namespace enigma {
namespace multiplayer {
namespace internal {

bool tcp_socket_valid(TcpSocket s) {
#ifdef WIN32
    return s != INVALID_SOCKET;
#else
    return s >= 0;
#endif
}

void tcp_close(TcpSocket &s) {
    if (!tcp_socket_valid(s))
        return;
#ifdef WIN32
    closesocket(s);
    s = INVALID_SOCKET;
#else
    close(s);
    s = -1;
#endif
}

bool tcp_set_nonblocking(TcpSocket s) {
#ifdef WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0)
        return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static bool tcp_send_all(TcpSocket s, const void *data, size_t len) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(data);
    size_t sent = 0;
    while (sent < len) {
#ifdef WIN32
        int n = ::send(s, reinterpret_cast<const char *>(p + sent),
                       static_cast<int>(len - sent), 0);
#else
        ssize_t n = ::send(s, p + sent, len - sent, 0);
#endif
        if (n <= 0) {
#ifdef WIN32
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                if (debug_enabled())
                    debug_log("mp tcp send failed err=%d", err);
                return false;
            }
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                if (debug_enabled())
                    debug_log("mp tcp send failed errno=%d (%s)", errno, strerror(errno));
                return false;
            }
#endif
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(s, &wfds);
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200 * 1000;
#ifdef WIN32
            int sel = ::select(0, nullptr, &wfds, nullptr, &tv);
#else
            int sel = ::select(s + 1, nullptr, &wfds, nullptr, &tv);
#endif
            if (sel <= 0) {
                if (debug_enabled())
                    debug_log("mp tcp send wait timeout/err sel=%d", sel);
                return false;
            }
            continue;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool tcp_send_frame(TcpSocket s, const void *data, size_t len) {
    if (!tcp_socket_valid(s))
        return false;
    if (len > (1u << 20))
        return false;
    uint8_t prefix[4];
    uint32_t v = static_cast<uint32_t>(len);
    prefix[0] = static_cast<uint8_t>(v & 0xFF);
    prefix[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    prefix[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    prefix[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    return tcp_send_all(s, prefix, sizeof(prefix)) && tcp_send_all(s, data, len);
}

bool tcp_pump_recv(TcpSocket s, std::vector<uint8_t> &rx) {
    if (!tcp_socket_valid(s))
        return false;
    uint8_t buf[4096];
    while (true) {
#ifdef WIN32
        int n = ::recv(s, reinterpret_cast<char *>(buf), static_cast<int>(sizeof(buf)), 0);
        if (n == 0)
            return false;
        if (n < 0) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                break;
            return false;
        }
#else
        ssize_t n = ::recv(s, buf, sizeof(buf), 0);
        if (n == 0)
            return false;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        }
#endif
        rx.insert(rx.end(), buf, buf + n);
        if (static_cast<size_t>(n) < sizeof(buf))
            break;
    }
    return true;
}

bool tcp_try_extract_frame(std::vector<uint8_t> &rx, uint32_t &pending_len,
                           std::vector<uint8_t> &out) {
    out.clear();
    if (pending_len == 0) {
        if (rx.size() < 4)
            return false;
        uint32_t v = static_cast<uint32_t>(rx[0]) |
                     (static_cast<uint32_t>(rx[1]) << 8) |
                     (static_cast<uint32_t>(rx[2]) << 16) |
                     (static_cast<uint32_t>(rx[3]) << 24);
        rx.erase(rx.begin(), rx.begin() + 4);
        if (v == 0 || v > (1u << 20)) {
            pending_len = 0;
            rx.clear();
            return false;
        }
        pending_len = v;
    }
    if (rx.size() < pending_len)
        return false;
    out.assign(rx.begin(), rx.begin() + pending_len);
    rx.erase(rx.begin(), rx.begin() + pending_len);
    pending_len = 0;
    return true;
}

bool tcp_connect_timeout(const std::string &host, Uint16 port, Uint32 timeout_ms,
                         TcpSocket &out) {
    out = kInvalidTcpSocket;
    if (host.empty() || port == 0)
        return false;
    addrinfo hints;
    ::memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    addrinfo *res = nullptr;
    std::string port_str = std::to_string(static_cast<unsigned>(port));
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0)
        return false;

    Uint32 start = SDL_GetTicks();
    for (addrinfo *ai = res; ai != nullptr; ai = ai->ai_next) {
        if (SDL_GetTicks() - start > timeout_ms)
            break;
        TcpSocket s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (!tcp_socket_valid(s))
            continue;
        if (!tcp_set_nonblocking(s)) {
            tcp_close(s);
            continue;
        }
        int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
#ifdef WIN32
        if (rc == 0) {
            out = s;
            freeaddrinfo(res);
            return true;
        }
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            tcp_close(s);
            continue;
        }
#else
        if (rc == 0) {
            out = s;
            freeaddrinfo(res);
            return true;
        }
        if (errno != EINPROGRESS) {
            tcp_close(s);
            continue;
        }
#endif
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        timeval tv;
        tv.tv_sec = static_cast<long>(timeout_ms / 1000);
        tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
#ifdef WIN32
        int sel = ::select(0, nullptr, &wfds, nullptr, &tv);
#else
        int sel = ::select(s + 1, nullptr, &wfds, nullptr, &tv);
#endif
        if (sel > 0) {
            out = s;
            freeaddrinfo(res);
            return true;
        }
        tcp_close(s);
    }

    freeaddrinfo(res);
    return false;
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
