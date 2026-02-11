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

#include "multiplayer_transport.hh"

#include "multiplayer_session_impl.hh"

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

bool poll_direct_enet(ITransportSink &sink) {
    // Direct ENet sessions (host or client). TCP-relay-only clients don't have a host_handle.
    if (g_session.host_handle == nullptr)
        return true;

    ENetEvent event;
    while (enet_host_service(g_session.host_handle, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            if (!sink.OnConnect(HostSource::DIRECT, event.peer))
                return false;
            break;
        case ENET_EVENT_TYPE_RECEIVE: {
            const char *data = reinterpret_cast<const char *>(event.packet->data);
            bool ok = sink.OnPayload(HostSource::DIRECT, event.peer, data,
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
    return true;
}

bool poll_udp_relay(ITransportSink &sink) {
    // Only the host polls a dedicated ENet host for relay connection. Clients use host_handle.
    if (!g_session.host || g_session.relay_handle == nullptr)
        return true;

    ENetEvent event;
    while (enet_host_service(g_session.relay_handle, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE: {
            const char *data = reinterpret_cast<const char *>(event.packet->data);
            bool ok = sink.OnPayload(HostSource::UDP_RELAY, event.peer, data,
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
    return true;
}

bool poll_tcp_relay(ITransportSink &sink) {
    if (!tcp_socket_valid(g_session.tcp_relay_socket))
        return true;

    if (!tcp_pump_recv(g_session.tcp_relay_socket, g_session.tcp_relay_rx)) {
        tcp_close(g_session.tcp_relay_socket);
        return sink.OnDisconnect(HostSource::TCP_RELAY, nullptr);
    }

    std::vector<uint8_t> frame;
    while (tcp_socket_valid(g_session.tcp_relay_socket) &&
           tcp_try_extract_frame(g_session.tcp_relay_rx, g_session.tcp_relay_frame_len, frame)) {
        const char *data = reinterpret_cast<const char *>(frame.data());
        if (!sink.OnPayload(HostSource::TCP_RELAY, nullptr, data, frame.size()))
            return false;
    }
    return true;
}

}  // namespace

bool Transport::Poll(ITransportSink &sink) {
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
        ENetPacket *packet = enet_packet_create(payload.data(), payload.size(),
                                                ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(g_session.server_peer, 0, packet);
        Flush();
        return true;
    }
    if (g_session.active_transport == TransportKind::TCP_RELAY &&
        tcp_socket_valid(g_session.tcp_relay_socket)) {
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
        ENetPacket *packet = enet_packet_create(payload.data(), payload.size(), 0);
        enet_peer_send(g_session.server_peer, 0, packet);
        Flush();
        return true;
    }
    if (g_session.active_transport == TransportKind::TCP_RELAY &&
        tcp_socket_valid(g_session.tcp_relay_socket)) {
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
    ENetPacket *packet = enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, packet);
}

void Transport::HostSendDirectUnreliable(ENetPeer *peer, const ecl::Buffer &payload) {
    if (!peer)
        return;
    ENetPacket *packet = enet_packet_create(payload.data(), payload.size(), 0);
    enet_peer_send(peer, 0, packet);
}

void Transport::HostSendUdpRelay(Uint32 client_id, const ecl::Buffer &payload) {
    if (!g_session.relay_peer)
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
    if (g_session.host_handle)
        enet_host_flush(g_session.host_handle);
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
