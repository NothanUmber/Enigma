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

#include "multiplayer.hh"

#include "multiplayer_config.hh"
#include "multiplayer_internal.hh"

#include "SDL.h"

#include <curl/curl.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

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

struct LoggedPollSnapshot {
    bool has = false;
    std::string server;
    std::string room_code;
    bool started = false;
    Uint32 session_id = 0;
    std::string level_id;
    std::string host_ip;
    Uint8 expected_players = 0;
    unsigned player_count = 0;
};

LoggedPollSnapshot g_last_logged_poll;

struct TrackedInternetRoom {
    bool active = false;
    std::string server;
    std::string room_code;
    bool prefer_http = false;
};

TrackedInternetRoom g_tracked_room;

void clear_tracked_room() {
    g_tracked_room.active = false;
    g_tracked_room.server.clear();
    g_tracked_room.room_code.clear();
    g_tracked_room.prefer_http = false;
}

void track_room(const std::string &server, const std::string &room_code,
                bool prefer_http = false) {
    if (server.empty() || room_code.empty()) {
        clear_tracked_room();
        return;
    }
    g_tracked_room.active = true;
    g_tracked_room.server = server;
    g_tracked_room.room_code = room_code;
    g_tracked_room.prefer_http = prefer_http;
}

struct LobbyTargets {
    std::string udp_server;
    std::string http_url;
    bool force_http = false;
};

bool is_http_url(const std::string &server) {
    return server.compare(0, 7, "http://") == 0 ||
           server.compare(0, 8, "https://") == 0;
}

LobbyTargets resolve_lobby_targets(const std::string &server) {
    LobbyTargets targets;
    if (is_http_url(server)) {
        ResolvedInternetUrl resolved = ResolveHttpUrl(server);
        if (resolved.is_valid()) {
            targets.http_url = resolved.url;
            targets.force_http = true;
        }
        return targets;
    }

    targets.udp_server = server;
    MultiplayerConfig cfg = LoadMultiplayerConfig();
    ResolvedInternetServers servers = ResolveInternetEndpoints(cfg);
    if (targets.udp_server.empty() && servers.lobby.is_valid())
        targets.udp_server = servers.lobby.server;
    if (servers.lobby_control.is_valid())
        targets.http_url = servers.lobby_control.url;
    return targets;
}

bool should_fallback_to_http(const std::string &error) {
    return error == "Invalid server address." ||
           error == "Failed to resolve server." ||
           error == "Failed to open socket." ||
           error == "Failed to send request." ||
           error == "No response from server.";
}

bool room_prefers_http(const std::string &room_code) {
    return g_tracked_room.active && g_tracked_room.room_code == room_code &&
           g_tracked_room.prefer_http;
}

void build_create_request(ecl::Buffer &request, const std::string &room_code,
                          const protocol::LobbyStart &start) {
    request.clear();
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_CREATE)
            << room_code << Uint32(start.session_id) << Uint32(start.seed)
            << Uint8(start.expected_players) << Uint16(start.host_port)
            << Uint8(start.filter_optimized) << start.level_id << start.host_id
            << start.pack_name << g_lobby.local_name;
}

void build_join_request(ecl::Buffer &request, const std::string &room_code) {
    request.clear();
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_JOIN)
            << room_code << g_lobby.local_id << g_lobby.local_name;
}

void build_start_request(ecl::Buffer &request, const std::string &room_code,
                         const protocol::LobbyStart &start) {
    request.clear();
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_START)
            << room_code << Uint32(start.session_id) << Uint32(start.seed)
            << Uint8(start.expected_players) << Uint16(start.host_port)
            << Uint8(start.filter_optimized) << start.level_id << start.host_id
            << start.pack_name << g_lobby.local_name;
}

void build_poll_request(ecl::Buffer &request, const std::string &room_code) {
    request.clear();
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_POLL)
            << room_code;
}

void build_leave_request(ecl::Buffer &request, const std::string &room_code) {
    request.clear();
    request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_LEAVE)
            << room_code << g_lobby.local_id;
}

bool decode_response_type(ecl::Buffer &response, Uint8 expected_type, std::string &error) {
    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 type = 0;
    if (!(response >> magic >> version >> type)) {
        error = "Bad response from server.";
        return false;
    }
    if (magic != kInternetMagic || version != kInternetVersion) {
        error = "Bad response from server.";
        return false;
    }
    if (type == INET_ERROR) {
        std::string msg;
        response >> msg;
        error = msg.empty() ? "Server error." : msg;
        return false;
    }
    if (type != expected_type) {
        error = "Bad response from server.";
        return false;
    }
    return true;
}

void log_poll_snapshot(const std::string &server, const std::string &room_code,
                       const protocol::LobbyStart &start, const std::string &host_ip,
                       bool started, unsigned player_count) {
    bool log_changed = false;
    if (!g_last_logged_poll.has ||
        g_last_logged_poll.server != server ||
        g_last_logged_poll.room_code != room_code ||
        g_last_logged_poll.started != started ||
        g_last_logged_poll.player_count != player_count) {
        log_changed = true;
    } else if (started) {
        if (g_last_logged_poll.session_id != start.session_id ||
            g_last_logged_poll.level_id != start.level_id ||
            g_last_logged_poll.host_ip != host_ip ||
            g_last_logged_poll.expected_players != start.expected_players) {
            log_changed = true;
        }
    }
    if (!log_changed)
        return;

    if (!started) {
        debug_log("mp internet: poll room=%s started=0 count=%u",
                  room_code.c_str(),
                  static_cast<unsigned>(player_count));
    } else {
        debug_log("mp internet: poll room=%s started=1 session=%u level=%s expected=%u host_ip=%s count=%u",
                  room_code.c_str(),
                  static_cast<unsigned>(start.session_id),
                  start.level_id.c_str(),
                  static_cast<unsigned>(start.expected_players),
                  host_ip.c_str(),
                  static_cast<unsigned>(player_count));
    }
    g_last_logged_poll.has = true;
    g_last_logged_poll.server = server;
    g_last_logged_poll.room_code = room_code;
    g_last_logged_poll.started = started;
    g_last_logged_poll.player_count = player_count;
    g_last_logged_poll.session_id = started ? start.session_id : 0;
    g_last_logged_poll.level_id = started ? start.level_id : std::string();
    g_last_logged_poll.host_ip = started ? host_ip : std::string();
    g_last_logged_poll.expected_players = started ? start.expected_players : 0;
}

size_t http_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    std::vector<char> *out = reinterpret_cast<std::vector<char> *>(userdata);
    const size_t count = size * nmemb;
    out->insert(out->end(), ptr, ptr + count);
    return count;
}

std::string curl_error_message(CURLcode code) {
    switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
        return "Failed to resolve server.";
    case CURLE_COULDNT_CONNECT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
        return "No response from server.";
    case CURLE_OPERATION_TIMEDOUT:
        return "No response from server.";
    default:
        return curl_easy_strerror(code);
    }
}

bool http_exchange(const std::string &url, const ecl::Buffer &request,
                   ecl::Buffer &response, std::string &error,
                   long timeout_ms = 2000) {
    ResolvedInternetUrl resolved = ResolveHttpUrl(url);
    if (!resolved.is_valid()) {
        error = "Invalid server address.";
        return false;
    }

    CURL *easy = curl_easy_init();
    if (!easy) {
        error = "Failed to open socket.";
        return false;
    }

    std::vector<char> response_data;
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    headers = curl_slist_append(headers, "Accept: application/octet-stream");

    curl_easy_setopt(easy, CURLOPT_URL, resolved.url.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request.data());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.size()));
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, timeout_ms);

    CURLcode rc = curl_easy_perform(easy);
    long http_status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);

    if (rc != CURLE_OK) {
        error = curl_error_message(rc);
        return false;
    }
    if (http_status != 200) {
        error = "HTTP " + std::to_string(http_status) + " from server.";
        return false;
    }

    response.assign(response_data.data(),
                    static_cast<ecl::Buffer::size_t>(response_data.size()));
    return true;
}

bool parse_member_peers(ecl::Buffer &response, std::vector<LobbyPeer> &peers,
                        const std::string &self_id) {
    peers.clear();
    if (response.get_rpos() >= static_cast<std::ptrdiff_t>(response.size()))
        return true;
    Uint8 count = 0;
    if (!(response >> count))
        return false;
    peers.reserve(count);
    for (Uint8 i = 0; i < count; ++i) {
        std::string id;
        std::string name;
        if (!(response >> id >> name))
            return false;
        LobbyPeer peer;
        peer.id = id;
        peer.name = name.empty() ? "Player" : name;
        peer.level_id.clear();
        peer.address.clear();
        peer.is_self = (id == self_id);
        peers.push_back(peer);
    }
    return true;
}

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

class InternetLobbyBackend {
public:
    virtual ~InternetLobbyBackend() = default;

    virtual bool CreateRoom(const std::string &server, const std::string &room_code,
                            const protocol::LobbyStart &start, std::string &error) = 0;
    virtual bool JoinRoom(const std::string &server, const std::string &room_code,
                          protocol::LobbyStart &start, std::string &host_ip,
                          unsigned &player_count, std::vector<LobbyPeer> &peers,
                          std::string &error) = 0;
    virtual bool StartRoom(const std::string &server, const std::string &room_code,
                           const protocol::LobbyStart &start, std::string &error) = 0;
    virtual bool PollRoom(const std::string &server, const std::string &room_code,
                          protocol::LobbyStart &start, std::string &host_ip,
                          bool &started, unsigned &player_count,
                          std::vector<LobbyPeer> &peers, std::string &error) = 0;
    virtual bool LeaveRoom(const std::string &server, const std::string &room_code,
                           std::string &error) = 0;
    virtual void LeaveTrackedRoomOnShutdown() = 0;
};

class UdpInternetLobbyBackend final : public InternetLobbyBackend {
public:
    bool CreateRoom(const std::string &server, const std::string &room_code,
                    const protocol::LobbyStart &start, std::string &error) override {
        ensure_lobby_identity();
        ecl::Buffer request;
        request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_CREATE)
                << room_code << Uint32(start.session_id) << Uint32(start.seed)
                << Uint8(start.expected_players) << Uint16(start.host_port)
                << Uint8(start.filter_optimized) << start.level_id << start.host_id
                << start.pack_name
                // Optional extension: host display name for room member lists.
                << g_lobby.local_name;

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
        track_room(server, server_room.empty() ? room_code : server_room);
        return true;
    }

    bool JoinRoom(const std::string &server, const std::string &room_code,
                  protocol::LobbyStart &start, std::string &host_ip,
                  unsigned &player_count, std::vector<LobbyPeer> &peers,
                  std::string &error) override {
        ensure_lobby_identity();
        player_count = 0;
        peers.clear();
        ecl::Buffer request;
        request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_JOIN)
                << room_code << g_lobby.local_id
                // Optional extension: joining member display name.
                << g_lobby.local_name;

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
        if (!parse_member_peers(response, peers, g_lobby.local_id))
            return false;
        if (!peers.empty())
            player_count = static_cast<unsigned>(peers.size());
        track_room(server, room_code);
        return true;
    }

    bool StartRoom(const std::string &server, const std::string &room_code,
                   const protocol::LobbyStart &start, std::string &error) override {
        ensure_lobby_identity();
        debug_log("mp internet: start room=%s session=%u level=%s expected=%u",
                  room_code.c_str(),
                  static_cast<unsigned>(start.session_id),
                  start.level_id.c_str(),
                  static_cast<unsigned>(start.expected_players));
        ecl::Buffer request;
        request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_START)
                << room_code << Uint32(start.session_id) << Uint32(start.seed)
                << Uint8(start.expected_players) << Uint16(start.host_port)
                << Uint8(start.filter_optimized) << start.level_id << start.host_id
                << start.pack_name
                // Optional extension: host display name for room member lists.
                << g_lobby.local_name;

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

    bool PollRoom(const std::string &server, const std::string &room_code,
                  protocol::LobbyStart &start, std::string &host_ip,
                  bool &started, unsigned &player_count,
                  std::vector<LobbyPeer> &peers, std::string &error) override {
        ensure_lobby_identity();
        started = false;
        player_count = 0;
        peers.clear();
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
        if (started) {
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
        }

        // Log only on changes to avoid spamming (poll runs frequently).
        bool log_changed = false;
        if (!g_last_logged_poll.has ||
            g_last_logged_poll.server != server ||
            g_last_logged_poll.room_code != room_code ||
            g_last_logged_poll.started != started ||
            g_last_logged_poll.player_count != player_count) {
            log_changed = true;
        } else if (started) {
            if (g_last_logged_poll.session_id != start.session_id ||
                g_last_logged_poll.level_id != start.level_id ||
                g_last_logged_poll.host_ip != host_ip ||
                g_last_logged_poll.expected_players != start.expected_players) {
                log_changed = true;
            }
        }
        if (log_changed) {
            if (!started) {
                debug_log("mp internet: poll room=%s started=0 count=%u",
                          room_code.c_str(),
                          static_cast<unsigned>(player_count));
            } else {
                debug_log("mp internet: poll room=%s started=1 session=%u level=%s expected=%u host_ip=%s count=%u",
                          room_code.c_str(),
                          static_cast<unsigned>(start.session_id),
                          start.level_id.c_str(),
                          static_cast<unsigned>(start.expected_players),
                          host_ip.c_str(),
                          static_cast<unsigned>(player_count));
            }
            g_last_logged_poll.has = true;
            g_last_logged_poll.server = server;
            g_last_logged_poll.room_code = room_code;
            g_last_logged_poll.started = started;
            g_last_logged_poll.player_count = player_count;
            g_last_logged_poll.session_id = started ? start.session_id : 0;
            g_last_logged_poll.level_id = started ? start.level_id : std::string();
            g_last_logged_poll.host_ip = started ? host_ip : std::string();
            g_last_logged_poll.expected_players = started ? start.expected_players : 0;
        }

        if (!parse_member_peers(response, peers, g_lobby.local_id))
            return false;
        if (!peers.empty())
            player_count = static_cast<unsigned>(peers.size());
        return true;
    }

    bool LeaveRoom(const std::string &server, const std::string &room_code,
                   std::string &error) override {
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
        if (g_tracked_room.active && g_tracked_room.server == server
            && g_tracked_room.room_code == room_code) {
            clear_tracked_room();
        }
        return true;
    }

    void LeaveTrackedRoomOnShutdown() override {
        if (!g_tracked_room.active || g_tracked_room.server.empty()
            || g_tracked_room.room_code.empty()) {
            return;
        }
        ensure_lobby_identity();
        cancel_pending_poll();

        std::string host;
        Uint16 port = 0;
        if (!parse_host_port(g_tracked_room.server, host, port)) {
            clear_tracked_room();
            return;
        }

        ENetAddress addr;
        if (enet_address_set_host(&addr, host.c_str()) != 0) {
            clear_tracked_room();
            return;
        }
        addr.port = port;

        ENetSocket socket = enet_socket_create_compat(ENET_SOCKET_TYPE_DATAGRAM);
        if (socket == ENET_SOCKET_NULL) {
            clear_tracked_room();
            return;
        }

        // Best-effort shutdown cleanup: send LEAVE without waiting for a reply so
        // quitting does not block on network timeouts.
        ecl::Buffer request;
        request << Uint32(kInternetMagic) << Uint8(kInternetVersion) << Uint8(INET_LEAVE)
                << g_tracked_room.room_code << g_lobby.local_id;
        ENetBuffer eb;
        eb.data = const_cast<char *>(request.data());
        eb.dataLength = request.size();
        enet_socket_send(socket, &addr, &eb, 1);
        enet_socket_destroy(socket);

        clear_tracked_room();
    }
};

class HttpInternetLobbyBackend final : public InternetLobbyBackend {
public:
    bool CreateRoom(const std::string &server, const std::string &room_code,
                    const protocol::LobbyStart &start, std::string &error) override {
        ensure_lobby_identity();
        cancel_pending_poll();

        ecl::Buffer request;
        build_create_request(request, room_code, start);
        ecl::Buffer response;
        if (!http_exchange(server, request, response, error))
            return false;
        if (!decode_response_type(response, INET_CREATE_OK, error))
            return false;

        std::string host_ip;
        std::string server_room;
        if (!(response >> server_room >> host_ip)) {
            error = "Bad response from server.";
            return false;
        }
        (void)host_ip;
        track_room(server, server_room.empty() ? room_code : server_room, true);
        return true;
    }

    bool JoinRoom(const std::string &server, const std::string &room_code,
                  protocol::LobbyStart &start, std::string &host_ip,
                  unsigned &player_count, std::vector<LobbyPeer> &peers,
                  std::string &error) override {
        ensure_lobby_identity();
        cancel_pending_poll();
        player_count = 0;
        peers.clear();

        ecl::Buffer request;
        build_join_request(request, room_code);
        ecl::Buffer response;
        if (!http_exchange(server, request, response, error))
            return false;
        if (!decode_response_type(response, INET_JOIN_OK, error))
            return false;

        if (!(response >> start.session_id >> start.level_id >> start.seed
              >> start.expected_players >> start.host_port >> start.host_id
              >> start.filter_optimized >> host_ip)) {
            error = "Bad response from server.";
            return false;
        }
        if (response.get_rpos() < static_cast<std::ptrdiff_t>(response.size())) {
            Uint8 count = 0;
            if (response >> count)
                player_count = count;
        }
        start.pack_name.clear();
        if (response.get_rpos() < static_cast<std::ptrdiff_t>(response.size())) {
            if (!(response >> start.pack_name)) {
                error = "Bad response from server.";
                return false;
            }
        }
        if (!parse_member_peers(response, peers, g_lobby.local_id)) {
            error = "Bad response from server.";
            return false;
        }
        if (!peers.empty())
            player_count = static_cast<unsigned>(peers.size());
        track_room(server, room_code, true);
        return true;
    }

    bool StartRoom(const std::string &server, const std::string &room_code,
                   const protocol::LobbyStart &start, std::string &error) override {
        ensure_lobby_identity();
        cancel_pending_poll();
        debug_log("mp internet: start room=%s session=%u level=%s expected=%u",
                  room_code.c_str(),
                  static_cast<unsigned>(start.session_id),
                  start.level_id.c_str(),
                  static_cast<unsigned>(start.expected_players));

        ecl::Buffer request;
        build_start_request(request, room_code, start);
        ecl::Buffer response;
        if (!http_exchange(server, request, response, error))
            return false;
        if (!decode_response_type(response, INET_START_OK, error))
            return false;
        track_room(server, room_code, true);
        return true;
    }

    bool PollRoom(const std::string &server, const std::string &room_code,
                  protocol::LobbyStart &start, std::string &host_ip,
                  bool &started, unsigned &player_count,
                  std::vector<LobbyPeer> &peers, std::string &error) override {
        ensure_lobby_identity();
        started = false;
        player_count = 0;
        peers.clear();

        ecl::Buffer request;
        build_poll_request(request, room_code);
        ecl::Buffer response;
        if (!http_exchange(server, request, response, error, 500))
            return false;
        if (!decode_response_type(response, INET_POLL_OK, error))
            return false;

        Uint8 started_flag = 0;
        Uint8 count = 0;
        if (!(response >> started_flag >> count)) {
            error = "Bad response from server.";
            return false;
        }
        started = (started_flag != 0);
        player_count = count;
        if (started) {
            if (!(response >> start.session_id >> start.level_id >> start.seed
                  >> start.expected_players >> start.host_port >> start.host_id
                  >> start.filter_optimized >> host_ip)) {
                error = "Bad response from server.";
                return false;
            }
            start.pack_name.clear();
            if (response.get_rpos() < static_cast<std::ptrdiff_t>(response.size())) {
                if (!(response >> start.pack_name)) {
                    error = "Bad response from server.";
                    return false;
                }
            }
        }

        log_poll_snapshot(server, room_code, start, host_ip, started, player_count);

        if (!parse_member_peers(response, peers, g_lobby.local_id)) {
            error = "Bad response from server.";
            return false;
        }
        if (!peers.empty())
            player_count = static_cast<unsigned>(peers.size());
        track_room(server, room_code, true);
        return true;
    }

    bool LeaveRoom(const std::string &server, const std::string &room_code,
                   std::string &error) override {
        ensure_lobby_identity();
        cancel_pending_poll();

        ecl::Buffer request;
        build_leave_request(request, room_code);
        ecl::Buffer response;
        if (!http_exchange(server, request, response, error))
            return false;
        if (!decode_response_type(response, INET_CREATE_OK, error))
            return false;
        if (g_tracked_room.active && g_tracked_room.room_code == room_code)
            clear_tracked_room();
        return true;
    }

    void LeaveTrackedRoomOnShutdown() override {
        if (!g_tracked_room.active || g_tracked_room.room_code.empty()) {
            clear_tracked_room();
            return;
        }
        ensure_lobby_identity();
        cancel_pending_poll();

        std::string error;
        ecl::Buffer request;
        ecl::Buffer response;
        build_leave_request(request, g_tracked_room.room_code);
        http_exchange(g_tracked_room.server, request, response, error, 250);
        clear_tracked_room();
    }
};

class CombinedInternetLobbyBackend final : public InternetLobbyBackend {
public:
    bool CreateRoom(const std::string &server, const std::string &room_code,
                    const protocol::LobbyStart &start, std::string &error) override {
        LobbyTargets targets = resolve_lobby_targets(server);
        if (targets.force_http ||
            (!ResolveInternetEndpoint(targets.udp_server).is_valid() && !targets.http_url.empty())) {
            return http_backend_.CreateRoom(targets.http_url, room_code, start, error);
        }

        if (!targets.udp_server.empty() &&
            udp_backend_.CreateRoom(targets.udp_server, room_code, start, error)) {
            return true;
        }
        if (!targets.http_url.empty() && should_fallback_to_http(error))
            return http_backend_.CreateRoom(targets.http_url, room_code, start, error);
        return false;
    }

    bool JoinRoom(const std::string &server, const std::string &room_code,
                  protocol::LobbyStart &start, std::string &host_ip,
                  unsigned &player_count, std::vector<LobbyPeer> &peers,
                  std::string &error) override {
        LobbyTargets targets = resolve_lobby_targets(server);
        if (targets.force_http ||
            (!ResolveInternetEndpoint(targets.udp_server).is_valid() && !targets.http_url.empty())) {
            return http_backend_.JoinRoom(targets.http_url, room_code, start, host_ip,
                                          player_count, peers, error);
        }

        if (!targets.udp_server.empty() &&
            udp_backend_.JoinRoom(targets.udp_server, room_code, start, host_ip,
                                  player_count, peers, error)) {
            return true;
        }
        if (!targets.http_url.empty() && should_fallback_to_http(error))
            return http_backend_.JoinRoom(targets.http_url, room_code, start, host_ip,
                                          player_count, peers, error);
        return false;
    }

    bool StartRoom(const std::string &server, const std::string &room_code,
                   const protocol::LobbyStart &start, std::string &error) override {
        LobbyTargets targets = resolve_lobby_targets(server);
        if (targets.force_http ||
            (!targets.http_url.empty() && room_prefers_http(room_code)) ||
            (!ResolveInternetEndpoint(targets.udp_server).is_valid() && !targets.http_url.empty())) {
            return http_backend_.StartRoom(targets.http_url, room_code, start, error);
        }

        if (!targets.udp_server.empty() &&
            udp_backend_.StartRoom(targets.udp_server, room_code, start, error)) {
            return true;
        }
        if (!targets.http_url.empty() && should_fallback_to_http(error))
            return http_backend_.StartRoom(targets.http_url, room_code, start, error);
        return false;
    }

    bool PollRoom(const std::string &server, const std::string &room_code,
                  protocol::LobbyStart &start, std::string &host_ip,
                  bool &started, unsigned &player_count,
                  std::vector<LobbyPeer> &peers, std::string &error) override {
        LobbyTargets targets = resolve_lobby_targets(server);
        if (targets.force_http ||
            (!targets.http_url.empty() && room_prefers_http(room_code)) ||
            (!ResolveInternetEndpoint(targets.udp_server).is_valid() && !targets.http_url.empty())) {
            return http_backend_.PollRoom(targets.http_url, room_code, start, host_ip,
                                          started, player_count, peers, error);
        }

        if (!targets.udp_server.empty() &&
            udp_backend_.PollRoom(targets.udp_server, room_code, start, host_ip,
                                  started, player_count, peers, error)) {
            return true;
        }
        if (!targets.http_url.empty() && should_fallback_to_http(error))
            return http_backend_.PollRoom(targets.http_url, room_code, start, host_ip,
                                          started, player_count, peers, error);
        return false;
    }

    bool LeaveRoom(const std::string &server, const std::string &room_code,
                   std::string &error) override {
        LobbyTargets targets = resolve_lobby_targets(server);
        if (targets.force_http ||
            (!targets.http_url.empty() && room_prefers_http(room_code)) ||
            (!ResolveInternetEndpoint(targets.udp_server).is_valid() && !targets.http_url.empty())) {
            return http_backend_.LeaveRoom(targets.http_url, room_code, error);
        }

        if (!targets.udp_server.empty() &&
            udp_backend_.LeaveRoom(targets.udp_server, room_code, error)) {
            return true;
        }
        if (!targets.http_url.empty() && should_fallback_to_http(error))
            return http_backend_.LeaveRoom(targets.http_url, room_code, error);
        return false;
    }

    void LeaveTrackedRoomOnShutdown() override {
        if (g_tracked_room.prefer_http)
            http_backend_.LeaveTrackedRoomOnShutdown();
        else
            udp_backend_.LeaveTrackedRoomOnShutdown();
    }

private:
    UdpInternetLobbyBackend udp_backend_;
    HttpInternetLobbyBackend http_backend_;
};

InternetLobbyBackend &internet_lobby_backend() {
    static CombinedInternetLobbyBackend backend;
    return backend;
}

}  // namespace

bool InternetCreateRoom(const std::string &server, const std::string &room_code,
                        const protocol::LobbyStart &start, std::string &error) {
    return internet_lobby_backend().CreateRoom(server, room_code, start, error);
}

bool InternetJoinRoom(const std::string &server, const std::string &room_code,
                      protocol::LobbyStart &start, std::string &host_ip,
                      unsigned &player_count, std::vector<LobbyPeer> &peers,
                      std::string &error) {
    return internet_lobby_backend().JoinRoom(server, room_code, start, host_ip,
                                             player_count, peers, error);
}

bool InternetStartRoom(const std::string &server, const std::string &room_code,
                       const protocol::LobbyStart &start, std::string &error) {
    return internet_lobby_backend().StartRoom(server, room_code, start, error);
}

bool InternetPollRoom(const std::string &server, const std::string &room_code,
                      protocol::LobbyStart &start, std::string &host_ip,
                      bool &started, unsigned &player_count,
                      std::vector<LobbyPeer> &peers, std::string &error) {
    return internet_lobby_backend().PollRoom(server, room_code, start, host_ip,
                                             started, player_count, peers, error);
}

bool InternetLeaveRoom(const std::string &server, const std::string &room_code,
                       std::string &error) {
    return internet_lobby_backend().LeaveRoom(server, room_code, error);
}

void InternetLeaveTrackedRoomOnShutdown() {
    internet_lobby_backend().LeaveTrackedRoomOnShutdown();
}

void SetRelayServer(const std::string &server) {
    g_relay_server = server;
}

void SetTcpRelayServer(const std::string &server) {
    g_tcp_relay_server = server;
}

void SetWebSocketRelayUrl(const std::string &url) {
    g_websocket_relay_url = url;
}

}  // namespace multiplayer
}  // namespace enigma
