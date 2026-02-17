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

#include "multiplayer_transport.hh"

#include "multiplayer_session_impl.hh"
#include "multiplayer_protocol.hh"
#include "options.hh"

#include "SDL.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <vector>

/* -------------------- Multiplayer transport -------------------- */
/*
 * Transport implementation.
 *
 * Contains low-level polling and send paths for direct ENet, UDP relay, and TCP
 * relay links.
 */

namespace enigma {
namespace multiplayer {
namespace internal {

Transport g_transport;

namespace {

struct NetSimConfig {
    bool enabled = false;
    bool all_packets = false;
    int delay_ms = 0;
    int jitter_ms = 0;
    int drop_pct = 0;
    int dup_pct = 0;
};

bool env_bool(const char *name) {
    const char *v = std::getenv(name);
    return v && *v;
}

int env_int(const char *name, int def, int minv, int maxv) {
    const char *v = std::getenv(name);
    if (!v || !*v)
        return def;
    int x = std::atoi(v);
    if (x < minv)
        x = minv;
    if (x > maxv)
        x = maxv;
    return x;
}

const NetSimConfig &netsim_config() {
    static NetSimConfig cfg;
    cfg.enabled = env_bool("ENIGMA_MP_NETSIM") || options::GetBool("MultiplayerDebugNetSimEnabled");
    cfg.all_packets = env_bool("ENIGMA_MP_NETSIM_ALL") || options::GetBool("MultiplayerDebugNetSimAll");
    cfg.delay_ms = env_int("ENIGMA_MP_NETSIM_DELAY_MS", options::GetInt("MultiplayerDebugNetSimDelayMs"), 0, 60000);
    cfg.jitter_ms = env_int("ENIGMA_MP_NETSIM_JITTER_MS", options::GetInt("MultiplayerDebugNetSimJitterMs"), 0, 60000);
    cfg.drop_pct = env_int("ENIGMA_MP_NETSIM_DROP_PCT", options::GetInt("MultiplayerDebugNetSimDropPct"), 0, 100);
    cfg.dup_pct = env_int("ENIGMA_MP_NETSIM_DUP_PCT", options::GetInt("MultiplayerDebugNetSimDupPct"), 0, 100);
    return cfg;
}

bool netsim_drop_eligible_type(Uint8 type) {
    // Only drop packets that are intended to be lossy at the application layer.
    // Dropping reliable control-plane packets here is unrealistic: ENet/TCP would
    // normally retransmit them, but dropping after encoding bypasses that layer.
    switch (type) {
    case protocol::NET_INPUT_BUNDLE:
    case protocol::NET_INPUT:
        return true;
    default:
        return false;
    }
}

bool netsim_should_apply(const ecl::Buffer &payload) {
    const NetSimConfig &cfg = netsim_config();
    if (!cfg.enabled)
        return false;
    if (cfg.all_packets)
        return true;
    if (payload.size() == 0)
        return false;
    const Uint8 type = static_cast<Uint8>(payload.data()[0]);
    switch (type) {
    case protocol::NET_INPUT:
    case protocol::NET_INPUT_BUNDLE:
    case protocol::NET_SYNC:
    case protocol::NET_RESYNC_REQUEST:
    case protocol::NET_RESYNC_STATE:
        return true;
    default:
        return false;
    }
}

int netsim_pick_delay_ms(std::mt19937 &rng) {
    const NetSimConfig &cfg = netsim_config();
    int delay = cfg.delay_ms;
    if (cfg.jitter_ms > 0) {
        std::uniform_int_distribution<int> dist(-cfg.jitter_ms, cfg.jitter_ms);
        delay += dist(rng);
    }
    if (delay < 0)
        delay = 0;
    return delay;
}

bool netsim_roll_pct(std::mt19937 &rng, int pct) {
    if (pct <= 0)
        return false;
    if (pct >= 100)
        return true;
    std::uniform_int_distribution<int> dist(1, 100);
    return dist(rng) <= pct;
}

enum class PendingKind {
    CLIENT_ENET = 0,
    CLIENT_TCP = 1,
    HOST_DIRECT = 2,
    HOST_UDP_RELAY = 3,
    HOST_TCP_RELAY = 4,
    HOST_BROADCAST = 5
};

struct PendingSend {
    Uint32 due_ms = 0;
    PendingKind kind = PendingKind::CLIENT_ENET;
    ENetPeer *peer = nullptr;
    Uint32 client_id = 0;
    bool reliable = true;
    std::vector<uint8_t> payload;
};

std::vector<PendingSend> g_pending_sends;

struct PendingRecv {
    Uint32 due_ms = 0;
    HostSource source = HostSource::DIRECT;
    ENetPeer *peer = nullptr;
    std::vector<uint8_t> payload;
};

std::vector<PendingRecv> g_pending_recvs;

std::mt19937 &netsim_rng() {
    static std::mt19937 rng(static_cast<unsigned>(SDL_GetTicks()));
    return rng;
}

void enet_send_peer(ENetPeer *peer, const std::vector<uint8_t> &payload, bool reliable) {
    if (!peer || payload.empty())
        return;
    ENetPacket *packet = enet_packet_create(payload.data(), payload.size(),
                                            reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_peer_send(peer, 0, packet);
}

void enet_broadcast_host(ENetHost *host, const std::vector<uint8_t> &payload, bool reliable) {
    if (!host || payload.empty())
        return;
    ENetPacket *packet = enet_packet_create(payload.data(), payload.size(),
                                            reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_host_broadcast(host, 0, packet);
}

void deliver_pending_send(const PendingSend &p, bool &need_flush_host, bool &need_flush_relay) {
    switch (p.kind) {
    case PendingKind::CLIENT_ENET:
        if (g_session.server_peer && g_session.server_peer == p.peer) {
            enet_send_peer(g_session.server_peer, p.payload, p.reliable);
            need_flush_host = true;
        }
        break;
    case PendingKind::CLIENT_TCP:
        if (tcp_socket_valid(g_session.tcp_relay_socket) && !p.payload.empty())
            tcp_send_frame(g_session.tcp_relay_socket, p.payload.data(), p.payload.size());
        break;
    case PendingKind::HOST_DIRECT:
        if (p.peer && g_session.peer_players.find(p.peer) != g_session.peer_players.end()) {
            enet_send_peer(p.peer, p.payload, p.reliable);
            need_flush_host = true;
        }
        break;
    case PendingKind::HOST_UDP_RELAY:
        if (g_session.relay_peer && !p.payload.empty()) {
            ecl::Buffer buf;
            encode_relay_header(buf, RELAY_SEND, g_session.session_id, p.client_id);
            buf.write(p.payload.data(), p.payload.size());
            std::vector<uint8_t> frame(buf.data(), buf.data() + buf.size());
            enet_send_peer(g_session.relay_peer, frame, true);
            need_flush_relay = true;
        }
        break;
    case PendingKind::HOST_TCP_RELAY:
        if (tcp_socket_valid(g_session.tcp_relay_socket) && !p.payload.empty()) {
            ecl::Buffer buf;
            encode_relay_header(buf, RELAY_SEND, g_session.session_id, p.client_id);
            buf.write(p.payload.data(), p.payload.size());
            tcp_send_frame(g_session.tcp_relay_socket, buf.data(), buf.size());
        }
        break;
    case PendingKind::HOST_BROADCAST:
        if (g_session.host_handle) {
            enet_broadcast_host(g_session.host_handle, p.payload, p.reliable);
            need_flush_host = true;
        }
        break;
    }
}

void netsim_pump_due_sends() {
    const NetSimConfig &cfg = netsim_config();
    if (!cfg.enabled || g_pending_sends.empty())
        return;
    const Uint32 now = SDL_GetTicks();
    bool need_flush_host = false;
    bool need_flush_relay = false;
    for (size_t i = 0; i < g_pending_sends.size(); ) {
        if (g_pending_sends[i].due_ms > now) {
            i += 1;
            continue;
        }
        deliver_pending_send(g_pending_sends[i], need_flush_host, need_flush_relay);
        g_pending_sends[i] = g_pending_sends.back();
        g_pending_sends.pop_back();
    }
    if (need_flush_host && g_session.host_handle)
        enet_host_flush(g_session.host_handle);
    if (need_flush_relay && g_session.relay_handle)
        enet_host_flush(g_session.relay_handle);
}

bool netsim_send_or_queue(PendingKind kind, ENetPeer *peer, Uint32 client_id,
                          const ecl::Buffer &payload, bool reliable) {
    if (!netsim_should_apply(payload))
        return false;
    const NetSimConfig &cfg = netsim_config();
    std::mt19937 &rng = netsim_rng();
    // Only drop packets that are meant to be lossy. For reliable packets, we
    // still apply delay/jitter/dup, but avoid drop to preserve ENet's reliability.
    bool allow_drop = false;
    if (!reliable && payload.size() > 0) {
        const Uint8 type = static_cast<Uint8>(payload.data()[0]);
        allow_drop = netsim_drop_eligible_type(type);
    }
    if (allow_drop && netsim_roll_pct(rng, cfg.drop_pct))
        return true;  // dropped

    int delay_ms = netsim_pick_delay_ms(rng);
    auto queue_one = [&](int extra_delay_ms) {
        PendingSend p;
        p.due_ms = SDL_GetTicks() + static_cast<Uint32>(extra_delay_ms);
        p.kind = kind;
        p.peer = peer;
        p.client_id = client_id;
        p.reliable = reliable;
        p.payload.assign(payload.data(), payload.data() + payload.size());
        g_pending_sends.push_back(std::move(p));
    };

    if (delay_ms <= 0) {
        // Deliver immediately (but still allow drop/dup simulation).
        bool need_flush_host = false;
        bool need_flush_relay = false;
        PendingSend p;
        p.kind = kind;
        p.peer = peer;
        p.client_id = client_id;
        p.reliable = reliable;
        p.payload.assign(payload.data(), payload.data() + payload.size());
        deliver_pending_send(p, need_flush_host, need_flush_relay);
        if (need_flush_host && g_session.host_handle)
            enet_host_flush(g_session.host_handle);
        if (need_flush_relay && g_session.relay_handle)
            enet_host_flush(g_session.relay_handle);
    } else {
        queue_one(delay_ms);
    }

    if (netsim_roll_pct(rng, cfg.dup_pct)) {
        int dup_delay = delay_ms;
        if (cfg.jitter_ms > 0)
            dup_delay = netsim_pick_delay_ms(rng);
        if (dup_delay <= 0)
            dup_delay = 1;
        queue_one(dup_delay);
    }
    return true;
}

bool netsim_should_apply_bytes(const uint8_t *data, size_t len) {
    const NetSimConfig &cfg = netsim_config();
    if (!cfg.enabled)
        return false;
    if (cfg.all_packets)
        return true;
    if (!data || len == 0)
        return false;
    const Uint8 type = static_cast<Uint8>(data[0]);
    switch (type) {
    case protocol::NET_INPUT:
    case protocol::NET_INPUT_BUNDLE:
    case protocol::NET_SYNC:
    case protocol::NET_RESYNC_REQUEST:
    case protocol::NET_RESYNC_STATE:
        return true;
    default:
        return false;
    }
}

bool netsim_deliver_due_recvs(ITransportSink &sink, HostSource source_filter) {
    const NetSimConfig &cfg = netsim_config();
    if (!cfg.enabled || g_pending_recvs.empty())
        return true;
    const Uint32 now = SDL_GetTicks();
    for (size_t i = 0; i < g_pending_recvs.size(); ) {
        const PendingRecv &r = g_pending_recvs[i];
        if (r.source != source_filter || r.due_ms > now) {
            i += 1;
            continue;
        }
        const char *data = r.payload.empty() ? nullptr : reinterpret_cast<const char *>(r.payload.data());
        const size_t len = r.payload.size();
        bool ok = sink.OnPayload(r.source, r.peer, data, len);
        g_pending_recvs[i] = g_pending_recvs.back();
        g_pending_recvs.pop_back();
        if (!ok)
            return false;
    }
    return true;
}

bool netsim_maybe_delay_recv(ITransportSink &sink, HostSource source, ENetPeer *peer,
                             const uint8_t *data, size_t len) {
    if (!netsim_should_apply_bytes(data, len))
        return sink.OnPayload(source, peer, reinterpret_cast<const char *>(data), len);
    const NetSimConfig &cfg = netsim_config();
    std::mt19937 &rng = netsim_rng();
    // Same rationale as send side: only drop payload types that are expected to
    // be lossy at the application layer.
    bool allow_drop = false;
    if (data && len > 0)
        allow_drop = netsim_drop_eligible_type(static_cast<Uint8>(data[0]));
    if (allow_drop && netsim_roll_pct(rng, cfg.drop_pct))
        return true;
    int delay_ms = netsim_pick_delay_ms(rng);
    if (delay_ms <= 0)
        return sink.OnPayload(source, peer, reinterpret_cast<const char *>(data), len);
    PendingRecv r;
    r.due_ms = SDL_GetTicks() + static_cast<Uint32>(delay_ms);
    r.source = source;
    r.peer = peer;
    r.payload.assign(data, data + len);
    g_pending_recvs.push_back(std::move(r));
    if (netsim_roll_pct(rng, cfg.dup_pct)) {
        PendingRecv r2 = g_pending_recvs.back();
        int dup_delay = delay_ms;
        if (cfg.jitter_ms > 0)
            dup_delay = netsim_pick_delay_ms(rng);
        if (dup_delay <= 0)
            dup_delay = 1;
        r2.due_ms = SDL_GetTicks() + static_cast<Uint32>(dup_delay);
        g_pending_recvs.push_back(std::move(r2));
    }
    return true;
}

bool poll_direct_enet(ITransportSink &sink) {
    // Direct ENet sessions (host or client). TCP-relay-only clients don't have a host_handle.
    if (g_session.host_handle == nullptr)
        return true;

    if (!netsim_deliver_due_recvs(sink, HostSource::DIRECT))
        return false;

    ENetEvent event;
    while (enet_host_service(g_session.host_handle, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            if (!sink.OnConnect(HostSource::DIRECT, event.peer))
                return false;
            break;
        case ENET_EVENT_TYPE_RECEIVE: {
            const uint8_t *data = reinterpret_cast<const uint8_t *>(event.packet->data);
            bool ok = netsim_maybe_delay_recv(sink, HostSource::DIRECT, event.peer, data,
                                              event.packet->dataLength);
            enet_packet_destroy(event.packet);
            if (!ok)
                return false;
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
            if (!sink.OnDisconnect(HostSource::DIRECT, event.peer))
                return false;
            break;
        default:
            break;
        }
    }
    return netsim_deliver_due_recvs(sink, HostSource::DIRECT);
}

bool poll_udp_relay(ITransportSink &sink) {
    // Only the host polls a dedicated ENet host for relay connection. Clients use host_handle.
    if (!g_session.host || g_session.relay_handle == nullptr)
        return true;

    if (!netsim_deliver_due_recvs(sink, HostSource::UDP_RELAY))
        return false;

    ENetEvent event;
    while (enet_host_service(g_session.relay_handle, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE: {
            const uint8_t *data = reinterpret_cast<const uint8_t *>(event.packet->data);
            bool ok = netsim_maybe_delay_recv(sink, HostSource::UDP_RELAY, event.peer, data,
                                              event.packet->dataLength);
            enet_packet_destroy(event.packet);
            if (!ok)
                return false;
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
            if (!sink.OnDisconnect(HostSource::UDP_RELAY, event.peer))
                return false;
            break;
        default:
            break;
        }
    }
    return netsim_deliver_due_recvs(sink, HostSource::UDP_RELAY);
}

bool poll_tcp_relay(ITransportSink &sink) {
    if (!tcp_socket_valid(g_session.tcp_relay_socket))
        return true;

    if (!netsim_deliver_due_recvs(sink, HostSource::TCP_RELAY))
        return false;

    if (!tcp_pump_recv(g_session.tcp_relay_socket, g_session.tcp_relay_rx)) {
        tcp_close(g_session.tcp_relay_socket);
        return sink.OnDisconnect(HostSource::TCP_RELAY, nullptr);
    }

    std::vector<uint8_t> frame;
    while (tcp_socket_valid(g_session.tcp_relay_socket) &&
           tcp_try_extract_frame(g_session.tcp_relay_rx, g_session.tcp_relay_frame_len, frame)) {
        const uint8_t *data = frame.empty() ? nullptr : frame.data();
        if (!netsim_maybe_delay_recv(sink, HostSource::TCP_RELAY, nullptr, data, frame.size()))
            return false;
    }
    return netsim_deliver_due_recvs(sink, HostSource::TCP_RELAY);
}

}  // namespace

bool Transport::Poll(ITransportSink &sink) {
    netsim_pump_due_sends();
    if (!poll_direct_enet(sink))
        return false;
    if (!poll_udp_relay(sink))
        return false;
    return poll_tcp_relay(sink);
}

bool Transport::ClientSend(const ecl::Buffer &payload) {
    if (g_session.host)
        return false;
    if (g_session.server_peer) {
        if (netsim_send_or_queue(PendingKind::CLIENT_ENET, g_session.server_peer, 0, payload, true))
            return true;
        ENetPacket *packet = enet_packet_create(payload.data(), payload.size(),
                                                ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(g_session.server_peer, 0, packet);
        Flush();
        return true;
    }
    if (g_session.active_transport == TransportKind::TCP_RELAY &&
        tcp_socket_valid(g_session.tcp_relay_socket)) {
        if (netsim_send_or_queue(PendingKind::CLIENT_TCP, nullptr, 0, payload, true))
            return true;
        bool ok = tcp_send_frame(g_session.tcp_relay_socket, payload.data(), payload.size());
        if (!ok && debug_enabled())
            debug_log("mp client: tcp relay send failed (len=%u)", (unsigned)payload.size());
        return ok;
    }
    return false;
}

bool Transport::ClientSendUnreliable(const ecl::Buffer &payload) {
    if (g_session.host)
        return false;
    if (g_session.server_peer) {
        if (netsim_send_or_queue(PendingKind::CLIENT_ENET, g_session.server_peer, 0, payload, false))
            return true;
        ENetPacket *packet = enet_packet_create(payload.data(), payload.size(), 0);
        enet_peer_send(g_session.server_peer, 0, packet);
        Flush();
        return true;
    }
    if (g_session.active_transport == TransportKind::TCP_RELAY &&
        tcp_socket_valid(g_session.tcp_relay_socket)) {
        if (netsim_send_or_queue(PendingKind::CLIENT_TCP, nullptr, 0, payload, true))
            return true;
        // TCP relay is inherently reliable; still accept the call for API symmetry.
        bool ok = tcp_send_frame(g_session.tcp_relay_socket, payload.data(), payload.size());
        if (!ok && debug_enabled())
            debug_log("mp client: tcp relay send failed (len=%u)", (unsigned)payload.size());
        return ok;
    }
    return false;
}

void Transport::HostSendDirect(ENetPeer *peer, const ecl::Buffer &payload) {
    if (!peer)
        return;
    if (netsim_send_or_queue(PendingKind::HOST_DIRECT, peer, 0, payload, true))
        return;
    ENetPacket *packet = enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, packet);
}

void Transport::HostSendDirectUnreliable(ENetPeer *peer, const ecl::Buffer &payload) {
    if (!peer)
        return;
    if (netsim_send_or_queue(PendingKind::HOST_DIRECT, peer, 0, payload, false))
        return;
    ENetPacket *packet = enet_packet_create(payload.data(), payload.size(), 0);
    enet_peer_send(peer, 0, packet);
}

void Transport::HostSendUdpRelay(Uint32 client_id, const ecl::Buffer &payload) {
    if (!g_session.relay_peer)
        return;
    if (netsim_send_or_queue(PendingKind::HOST_UDP_RELAY, nullptr, client_id, payload, true))
        return;
    ecl::Buffer buf;
    encode_relay_header(buf, RELAY_SEND, g_session.session_id, client_id);
    buf.write(payload.data(), payload.size());
    ENetPacket *packet = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(g_session.relay_peer, 0, packet);
    if (g_session.relay_handle)
        enet_host_flush(g_session.relay_handle);
}

void Transport::HostSendTcpRelay(Uint32 client_id, const ecl::Buffer &payload) {
    if (!tcp_socket_valid(g_session.tcp_relay_socket))
        return;
    if (netsim_send_or_queue(PendingKind::HOST_TCP_RELAY, nullptr, client_id, payload, true))
        return;
    ecl::Buffer buf;
    encode_relay_header(buf, RELAY_SEND, g_session.session_id, client_id);
    buf.write(payload.data(), payload.size());
    tcp_send_frame(g_session.tcp_relay_socket, buf.data(), buf.size());
}

void Transport::HostBroadcastUdpRelay(const ecl::Buffer &payload, Uint32 exclude_client_id) {
    for (const auto &entry : g_session.relay_players) {
        if (entry.first == exclude_client_id)
            continue;
        HostSendUdpRelay(entry.first, payload);
    }
}

void Transport::HostBroadcastTcpRelay(const ecl::Buffer &payload, Uint32 exclude_client_id) {
    for (const auto &entry : g_session.tcp_relay_players) {
        if (entry.first == exclude_client_id)
            continue;
        HostSendTcpRelay(entry.first, payload);
    }
}

void Transport::HostBroadcast(const ecl::Buffer &payload) {
    if (!g_session.host)
        return;
    if (g_session.peer_players.empty() && g_session.relay_players.empty() &&
        g_session.tcp_relay_players.empty()) {
        return;
    }
    if (!g_session.peer_players.empty()) {
        if (netsim_send_or_queue(PendingKind::HOST_BROADCAST, nullptr, 0, payload, true))
            return;
        ENetPacket *packet = enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE);
        enet_host_broadcast(g_session.host_handle, 0, packet);
    }
    if (!g_session.relay_players.empty())
        HostBroadcastUdpRelay(payload, 0);
    if (!g_session.tcp_relay_players.empty())
        HostBroadcastTcpRelay(payload, 0);
}

void Transport::HostBroadcastUnreliable(const ecl::Buffer &payload) {
    if (!g_session.host)
        return;
    if (g_session.peer_players.empty() && g_session.relay_players.empty() &&
        g_session.tcp_relay_players.empty()) {
        return;
    }
    if (!g_session.peer_players.empty()) {
        if (netsim_send_or_queue(PendingKind::HOST_BROADCAST, nullptr, 0, payload, false))
            return;
        ENetPacket *packet = enet_packet_create(payload.data(), payload.size(), 0);
        enet_host_broadcast(g_session.host_handle, 0, packet);
    }
    // Relay forwarding currently uses ENet/TCP on the host side. For simplicity,
    // keep relayed broadcasts reliable (the redundancy in the payload still helps).
    if (!g_session.relay_players.empty())
        HostBroadcastUdpRelay(payload, 0);
    if (!g_session.tcp_relay_players.empty())
        HostBroadcastTcpRelay(payload, 0);
}

void Transport::Flush() {
    netsim_pump_due_sends();
    if (g_session.host_handle)
        enet_host_flush(g_session.host_handle);
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
