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

#include "multiplayer.hh"

#include "multiplayer_internal.hh"

#include "SDL.h"

#include <cstddef>
#include <cstdint>
#include <string>

/* -------------------- Multiplayer Internet lobby -------------------- */
/*
 * Internet lobby client.
 *
 * Talks to the lobby server to create/join/leave rooms and to discover the
 * host's connect address (direct and relay endpoints).
 */

namespace enigma {
namespace multiplayer {

namespace {

using namespace internal;

struct PendingPoll {
    bool active = false;
    std::string server;
    std::string room_code;
    ENetAddress addr;
    ENetSocket socket = ENET_SOCKET_NULL;
    Uint32 start_ticks = 0;
    Uint32 last_send_ticks = 0;
};

PendingPoll g_pending_poll;

void cancel_pending_poll() {
    if (g_pending_poll.socket != ENET_SOCKET_NULL) {
        enet_socket_destroy(g_pending_poll.socket);
        g_pending_poll.socket = ENET_SOCKET_NULL;
    }
    g_pending_poll.active = false;
    g_pending_poll.server.clear();
    g_pending_poll.room_code.clear();
    g_pending_poll.start_ticks = 0;
    g_pending_poll.last_send_ticks = 0;
}

bool internet_exchange(const std::string &server, const ecl::Buffer &request,
                       ecl::Buffer &response, std::string &error) {
    std::string host;
    Uint16 port = 0;
    if (!parse_host_port(server, host, port)) {
        error = "Invalid server address.";
        return false;
    }

    ENetAddress addr;
    if (enet_address_set_host(&addr, host.c_str()) != 0) {
        error = "Failed to resolve server.";
        return false;
    }
    addr.port = port;

    ENetSocket socket = enet_socket_create_compat(ENET_SOCKET_TYPE_DATAGRAM);
    if (socket == ENET_SOCKET_NULL) {
        error = "Failed to open socket.";
        return false;
    }
    ENetBuffer eb;
    eb.data = const_cast<char *>(request.data());
    eb.dataLength = request.size();
    if (enet_socket_send(socket, &addr, &eb, 1) <= 0) {
        enet_socket_destroy(socket);
        error = "Failed to send request.";
        return false;
    }

    char data[1024];
    ENetBuffer rb;
    rb.data = data;
    rb.dataLength = sizeof(data);
    Uint32 start = SDL_GetTicks();
    while (SDL_GetTicks() - start < 1500) {
        enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
        enet_socket_wait(socket, &condition, 50);
        if ((condition & ENET_SOCKET_WAIT_RECEIVE) == 0)
            continue;
        ENetAddress src;
        int received = enet_socket_receive(socket, &src, &rb, 1);
        if (received > 0) {
            response.assign(data, static_cast<ecl::Buffer::size_t>(received));
            enet_socket_destroy(socket);
            return true;
        }
    }

    enet_socket_destroy(socket);
    error = "No response from server.";
    return false;
}

bool internet_poll_start(const std::string &server, const std::string &room_code,
                         std::string &error) {
    cancel_pending_poll();

    std::string host;
    Uint16 port = 0;
    if (!parse_host_port(server, host, port)) {
        error = "Invalid server address.";
        return false;
    }

    ENetAddress addr;
    if (enet_address_set_host(&addr, host.c_str()) != 0) {
        error = "Failed to resolve server.";
        return false;
    }
    addr.port = port;

    ENetSocket socket = enet_socket_create_compat(ENET_SOCKET_TYPE_DATAGRAM);
    if (socket == ENET_SOCKET_NULL) {
        error = "Failed to open socket.";
        return false;
    }

    ecl::Buffer request;
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_POLL)
            << room_code;

    ENetBuffer eb;
    eb.data = const_cast<char *>(request.data());
    eb.dataLength = request.size();
    if (enet_socket_send(socket, &addr, &eb, 1) <= 0) {
        enet_socket_destroy(socket);
        error = "Failed to send request.";
        return false;
    }

    g_pending_poll.active = true;
    g_pending_poll.server = server;
    g_pending_poll.room_code = room_code;
    g_pending_poll.addr = addr;
    g_pending_poll.socket = socket;
    g_pending_poll.start_ticks = SDL_GetTicks();
    g_pending_poll.last_send_ticks = g_pending_poll.start_ticks;
    return true;
}

bool internet_poll_pump(ecl::Buffer &response, std::string &error) {
    if (!g_pending_poll.active || g_pending_poll.socket == ENET_SOCKET_NULL) {
        error = "waiting";
        return false;
    }

    Uint32 now = SDL_GetTicks();

    // Resend once in case the datagram was dropped.
    if (now - g_pending_poll.last_send_ticks > 500 && now - g_pending_poll.start_ticks < 1200) {
        ecl::Buffer request;
        request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_POLL)
                << g_pending_poll.room_code;
        ENetBuffer eb;
        eb.data = const_cast<char *>(request.data());
        eb.dataLength = request.size();
        enet_socket_send(g_pending_poll.socket, &g_pending_poll.addr, &eb, 1);
        g_pending_poll.last_send_ticks = now;
    }

    char data[1024];
    ENetBuffer rb;
    rb.data = data;
    rb.dataLength = sizeof(data);

    enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
    enet_socket_wait(g_pending_poll.socket, &condition, 0);
    if ((condition & ENET_SOCKET_WAIT_RECEIVE) != 0) {
        ENetAddress src;
        int received = enet_socket_receive(g_pending_poll.socket, &src, &rb, 1);
        if (received > 0) {
            response.assign(data, static_cast<ecl::Buffer::size_t>(received));
            cancel_pending_poll();
            return true;
        }
    }

    if (now - g_pending_poll.start_ticks > 1500) {
        cancel_pending_poll();
        error = "No response from server.";
        return false;
    }

    error = "waiting";
    return false;
}

}  // namespace

bool InternetCreateRoom(const std::string &server, const std::string &room_code,
                        const protocol::LobbyStart &start, std::string &error) {
    ensure_lobby_identity();
    ecl::Buffer request;
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_CREATE)
            << room_code << Uint32(start.session_id) << Uint32(start.seed)
            << Uint8(start.expected_players) << Uint16(start.host_port)
            << Uint8(start.filter_optimized) << start.level_id << start.host_id << start.pack_name;

    ecl::Buffer response;
    if (!internet_exchange(server, request, response, error))
        return false;

    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 type = 0;
    if (!(response >> magic >> version >> type))
        return false;
    if (magic != kInternetMagic || version != kInternetVersion)
        return false;
    if (type == INET_ERROR) {
        std::string msg;
        response >> msg;
        error = msg.empty() ? "Server error." : msg;
        return false;
    }
    if (type != INET_CREATE_OK)
        return false;
    std::string host_ip;
    std::string server_room;
    if (!(response >> server_room >> host_ip))
        return false;
    (void)host_ip;
    return true;
}

bool InternetJoinRoom(const std::string &server, const std::string &room_code,
                      protocol::LobbyStart &start, std::string &host_ip,
                      unsigned &player_count, std::string &error) {
    ensure_lobby_identity();
    player_count = 0;
    ecl::Buffer request;
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_JOIN)
            << room_code << g_lobby.local_id;

    ecl::Buffer response;
    if (!internet_exchange(server, request, response, error))
        return false;

    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 type = 0;
    if (!(response >> magic >> version >> type))
        return false;
    if (magic != kInternetMagic || version != kInternetVersion)
        return false;
    if (type == INET_ERROR) {
        std::string msg;
        response >> msg;
        error = msg.empty() ? "Server error." : msg;
        return false;
    }
    if (type != INET_JOIN_OK)
        return false;
    if (!(response >> start.session_id >> start.level_id >> start.seed
          >> start.expected_players >> start.host_port >> start.host_id
          >> start.filter_optimized >> host_ip))
        return false;
    // Optional fields for newer servers:
    // - player_count (u8)
    // - pack_name (string)
    if (response.get_rpos() < static_cast<std::ptrdiff_t>(response.size())) {
        Uint8 count = 0;
        if (response >> count)
            player_count = count;
    }
    start.pack_name.clear();
    if (response.get_rpos() < static_cast<std::ptrdiff_t>(response.size())) {
        if (!(response >> start.pack_name))
            return false;
    }
    return true;
}

bool InternetStartRoom(const std::string &server, const std::string &room_code,
                       const protocol::LobbyStart &start, std::string &error) {
    ensure_lobby_identity();
    ecl::Buffer request;
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_START)
            << room_code << Uint32(start.session_id) << Uint32(start.seed)
            << Uint8(start.expected_players) << Uint16(start.host_port)
            << Uint8(start.filter_optimized) << start.level_id << start.host_id << start.pack_name;

    ecl::Buffer response;
    if (!internet_exchange(server, request, response, error))
        return false;

    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 type = 0;
    if (!(response >> magic >> version >> type))
        return false;
    if (magic != kInternetMagic || version != kInternetVersion)
        return false;
    if (type == INET_ERROR) {
        std::string msg;
        response >> msg;
        error = msg.empty() ? "Server error." : msg;
        return false;
    }
    if (type != INET_START_OK)
        return false;
    return true;
}

bool InternetPollRoom(const std::string &server, const std::string &room_code,
                      protocol::LobbyStart &start, std::string &host_ip,
                      bool &started, unsigned &player_count, std::string &error) {
    ensure_lobby_identity();
    started = false;
    player_count = 0;
    ecl::Buffer response;
    if (!g_pending_poll.active || g_pending_poll.server != server
        || g_pending_poll.room_code != room_code) {
        if (!internet_poll_start(server, room_code, error))
            return false;
    }
    if (!internet_poll_pump(response, error))
        return false;

    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 type = 0;
    if (!(response >> magic >> version >> type))
        return false;
    if (magic != kInternetMagic || version != kInternetVersion)
        return false;
    if (type == INET_ERROR) {
        std::string msg;
        response >> msg;
        error = msg.empty() ? "Server error." : msg;
        return false;
    }
    if (type != INET_POLL_OK)
        return false;
    Uint8 started_flag = 0;
    Uint8 count = 0;
    if (!(response >> started_flag >> count))
        return false;
    started = (started_flag != 0);
    player_count = count;
    if (!started)
        return true;
    if (!(response >> start.session_id >> start.level_id >> start.seed
          >> start.expected_players >> start.host_port >> start.host_id
          >> start.filter_optimized >> host_ip))
        return false;
    start.pack_name.clear();
    if (response.get_rpos() < static_cast<std::ptrdiff_t>(response.size())) {
        // Pack name may be appended after host_ip.
        if (!(response >> start.pack_name))
            return false;
    }
    return true;
}

bool InternetLeaveRoom(const std::string &server, const std::string &room_code,
                       std::string &error) {
    ensure_lobby_identity();
    // If we are leaving, drop any pending poll to avoid keeping an extra socket
    // around and to prevent late responses from touching UI state.
    cancel_pending_poll();
    ecl::Buffer request;
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_LEAVE)
            << room_code << g_lobby.local_id;

    ecl::Buffer response;
    if (!internet_exchange(server, request, response, error))
        return false;

    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 type = 0;
    if (!(response >> magic >> version >> type))
        return false;
    if (magic != kInternetMagic || version != kInternetVersion)
        return false;
    if (type == INET_ERROR) {
        std::string msg;
        response >> msg;
        error = msg.empty() ? "Server error." : msg;
        return false;
    }
    return true;
}

void SetRelayServer(const std::string &server) {
    g_relay_server = server;
}

void SetTcpRelayServer(const std::string &server) {
    g_tcp_relay_server = server;
}

}  // namespace multiplayer
}  // namespace enigma
