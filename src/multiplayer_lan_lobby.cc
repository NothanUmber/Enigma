#include "multiplayer.hh"

#include "multiplayer_internal.hh"

#include "multiplayer_config.hh"

#include "SDL.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace enigma {
namespace multiplayer {

namespace {

using namespace internal;

std::string encode_hex(const char *data, size_t len) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        out.push_back(hex[(c >> 4) & 0xF]);
        out.push_back(hex[c & 0xF]);
    }
    return out;
}

bool decode_hex(const std::string &text, ecl::Buffer &out) {
    if (text.empty() || (text.size() % 2) != 0)
        return false;
    out.clear();
    for (size_t i = 0; i < text.size(); i += 2) {
        char hi = text[i];
        char lo = text[i + 1];
        auto hexval = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'A' && c <= 'F')
                return 10 + (c - 'A');
            if (c >= 'a' && c <= 'f')
                return 10 + (c - 'a');
            return -1;
        };
        int h = hexval(hi);
        int l = hexval(lo);
        if (h < 0 || l < 0)
            return false;
        char byte = static_cast<char>((h << 4) | l);
        out.write(byte);
    }
    return true;
}

void send_lobby_announce() {
    if (!g_lobby.active || g_lobby.socket == ENET_SOCKET_NULL)
        return;
    protocol::LobbyAnnounce msg;
    msg.id = g_lobby.local_id;
    msg.name = g_lobby.local_name;
    msg.level_id = g_lobby.selected_level;
    msg.player_count = static_cast<Uint8>(g_lobby.peers.size() + 1);

    ecl::Buffer buf;
    protocol::encode_lobby_announce(buf, msg);

    ENetAddress addr;
    addr.host = ENET_HOST_BROADCAST;
    addr.port = kLobbyPort;
    ENetBuffer eb;
    eb.data = const_cast<char *>(buf.data());
    eb.dataLength = buf.size();
    enet_socket_send(g_lobby.socket, &addr, &eb, 1);
}

void poll_lobby_socket() {
    if (!g_lobby.active || g_lobby.socket == ENET_SOCKET_NULL)
        return;
    ENetAddress src;
    char data[512];
    ENetBuffer buffer;
    buffer.data = data;
    buffer.dataLength = sizeof(data);

    while (true) {
        int received = enet_socket_receive(g_lobby.socket, &src, &buffer, 1);
        if (received <= 0)
            break;

        std::string ip = address_to_ip_string(src);
        if (ip.empty())
            continue;

        ecl::Buffer buf;
        buf.assign(data, static_cast<ecl::Buffer::size_t>(received));
        protocol::LobbyAnnounce announce;
        if (protocol::decode_lobby_announce(buf, announce)) {
            if (announce.id == g_lobby.local_id)
                continue;
            LobbyPeerEntry &entry = g_lobby.peers[announce.id];
            entry.peer.id = announce.id;
            entry.peer.name = announce.name;
            entry.peer.level_id = announce.level_id;
            entry.peer.address = ip;
            entry.peer.is_self = false;
            entry.last_seen = g_lobby.time;
            continue;
        }

        buf.assign(data, static_cast<ecl::Buffer::size_t>(received));
        protocol::LobbyStart start;
        if (protocol::decode_lobby_start(buf, start)) {
            if (start.host_id == g_lobby.local_id)
                continue;
            if (g_session.active)
                continue;
            if (start.session_id == g_lobby.last_session_id)
                continue;
            g_lobby.pending_start = start;
            g_lobby.pending_host_ip = ip;
            g_lobby.has_pending_start = true;
            g_lobby.last_session_id = start.session_id;
        }
    }
}

void cleanup_lobby_peers() {
    for (auto it = g_lobby.peers.begin(); it != g_lobby.peers.end();) {
        if (g_lobby.time - it->second.last_seen > kPeerTimeout)
            it = g_lobby.peers.erase(it);
        else
            ++it;
    }
}

}  // namespace

void LobbyStart() {
    if (g_lobby.active)
        return;
    if (!LoadMultiplayerConfig().enable_direct) {
        // LAN mode has no relay. If direct connect is disabled, don't join the
        // broadcast lobby (prevents confusing half-working LAN behavior).
        debug_log("mp lobby: not starting (direct connect disabled)");
        return;
    }
    g_lobby.active = true;
    // Keep a stable lobby identity across LobbyStop/LobbyStart cycles.
    // Internet mode uses the same identity for room membership; if the id changes,
    // LEAVE requests may not remove the original member entry on the lobby server.
    ensure_lobby_identity();
    g_lobby.time = 0.0;
    g_lobby.announce_timer = 0.0;
    g_lobby.peers.clear();
    g_lobby.has_pending_start = false;
    g_lobby.pending_host_ip.clear();
    g_lobby.last_session_id = 0;

    g_lobby.socket = enet_socket_create_compat(ENET_SOCKET_TYPE_DATAGRAM);
    if (g_lobby.socket == ENET_SOCKET_NULL) {
        debug_log("mp lobby: socket create failed");
        g_lobby.active = false;
        return;
    }
    if (!bind_lobby_socket(g_lobby.socket, kLobbyPort)) {
        debug_log("mp lobby: bind failed port=%u", static_cast<unsigned>(kLobbyPort));
        enet_socket_destroy(g_lobby.socket);
        g_lobby.socket = ENET_SOCKET_NULL;
        g_lobby.active = false;
        return;
    }
    debug_log("mp lobby: started port=%u id=%s name=%s",
              static_cast<unsigned>(kLobbyPort),
              g_lobby.local_id.c_str(),
              g_lobby.local_name.c_str());
    send_lobby_announce();
}

void LobbyStop() {
    if (!g_lobby.active)
        return;
    if (g_lobby.socket != ENET_SOCKET_NULL) {
        enet_socket_destroy(g_lobby.socket);
        g_lobby.socket = ENET_SOCKET_NULL;
    }
    g_lobby.active = false;
    g_lobby.peers.clear();
}

void LobbyTick(double dtime) {
    if (!g_lobby.active)
        return;
    g_lobby.time += dtime;
    g_lobby.announce_timer += dtime;
    if (g_lobby.announce_timer >= kAnnounceInterval) {
        g_lobby.announce_timer = 0.0;
        send_lobby_announce();
    }
    poll_lobby_socket();
    cleanup_lobby_peers();
}

std::vector<LobbyPeer> LobbyPeers() {
    std::vector<LobbyPeer> result;
    if (!g_lobby.active)
        return result;
    LobbyPeer self;
    self.id = g_lobby.local_id;
    self.name = g_lobby.local_name;
    self.level_id = g_lobby.selected_level;
    self.address = "localhost";
    self.is_self = true;
    result.push_back(self);
    for (const auto &entry : g_lobby.peers) {
        result.push_back(entry.second.peer);
    }
    return result;
}

unsigned LobbySize() {
    if (!g_lobby.active)
        return 0;
    return static_cast<unsigned>(g_lobby.peers.size() + 1);
}

void LobbySetSelectedLevel(const std::string &level_id) {
    g_lobby.selected_level = level_id;
}

std::string LobbySelectedLevel() {
    return g_lobby.selected_level;
}

bool LobbyPollStart(protocol::LobbyStart &start, std::string &host_ip) {
    if (!g_lobby.has_pending_start)
        return false;
    start = g_lobby.pending_start;
    host_ip = g_lobby.pending_host_ip;
    g_lobby.has_pending_start = false;
    return true;
}

protocol::LobbyStart BuildStartMessage(const std::string &level_id, unsigned expected_players,
                                       unsigned filter_min_players) {
    protocol::LobbyStart start;
    std::random_device rd;
    Uint32 seed = static_cast<Uint32>(rd() ^ SDL_GetTicks());
    Uint32 session_id = static_cast<Uint32>((rd() << 16) ^ SDL_GetTicks());
    start.session_id = session_id;
    start.level_id = level_id;
    start.seed = seed;
    start.expected_players = static_cast<Uint8>(expected_players);
    start.host_port = kGamePort;
    start.host_id = g_lobby.local_id;
    if (filter_min_players < 1)
        filter_min_players = 1;
    if (filter_min_players > expected_players)
        filter_min_players = expected_players;
    start.filter_optimized = static_cast<Uint8>(filter_min_players);
    return start;
}

void LobbyBroadcastStart(const protocol::LobbyStart &start) {
    if (!g_lobby.active || g_lobby.socket == ENET_SOCKET_NULL)
        return;
    ecl::Buffer buf;
    protocol::encode_lobby_start(buf, start);
    ENetAddress addr;
    addr.host = ENET_HOST_BROADCAST;
    addr.port = kLobbyPort;
    ENetBuffer eb;
    eb.data = const_cast<char *>(buf.data());
    eb.dataLength = buf.size();
    enet_socket_send(g_lobby.socket, &addr, &eb, 1);
}

std::string EncodeStartToken(const protocol::LobbyStart &start) {
    ecl::Buffer buf;
    protocol::encode_lobby_start(buf, start);
    return encode_hex(buf.data(), buf.size());
}

bool DecodeStartToken(const std::string &token, protocol::LobbyStart &start) {
    ecl::Buffer buf;
    if (!decode_hex(token, buf))
        return false;
    return protocol::decode_lobby_start(buf, start);
}

}  // namespace multiplayer
}  // namespace enigma
