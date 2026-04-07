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

#ifndef MULTIPLAYER_TRANSPORT_HH_INCLUDED
#define MULTIPLAYER_TRANSPORT_HH_INCLUDED

/* -------------------- Multiplayer transport -------------------- */
/*
 * Transport abstraction for multiplayer sessions.
 *
 * The session engine interacts with one send/recv/poll surface regardless of
 * whether peers connect directly or via relays.
 */

#include "multiplayer_internal.hh"

namespace enigma {
namespace multiplayer {
namespace internal {

class ITransportSink {
public:
    virtual ~ITransportSink() = default;

    virtual bool OnConnect(HostSource source, ENetPeer *peer) = 0;
    virtual bool OnDisconnect(HostSource source, ENetPeer *peer) = 0;
    virtual bool OnPayload(HostSource source, ENetPeer *peer, const char *data, size_t len) = 0;
};

// Session transport facade.
//
// This centralizes polling/sending across:
// - direct ENet client<->host (LAN)
// - UDP ENet relay host<->relay and client<->relay
// - stream-relay framing for both host and clients (raw TCP or WebSocket)
//
// Session logic provides an ITransportSink and stays agnostic to the underlying transport.
class Transport {
public:
    bool Poll(ITransportSink &sink);

    // Client side: sends raw game payload to the currently active host link.
    bool ClientSend(const ecl::Buffer &payload);
    bool ClientSendUnreliable(const ecl::Buffer &payload);

    // Host side: sends raw game payload to a specific remote.
    void HostSendDirect(ENetPeer *peer, const ecl::Buffer &payload);
    void HostSendDirectUnreliable(ENetPeer *peer, const ecl::Buffer &payload);
    void HostSendUdpRelay(Uint32 client_id, const ecl::Buffer &payload);
    void HostSendStreamRelay(Uint32 client_id, const ecl::Buffer &payload);

    // Host side: broadcast raw game payload to all remotes.
    void HostBroadcast(const ecl::Buffer &payload);
    void HostBroadcastUnreliable(const ecl::Buffer &payload);
    void HostBroadcastUdpRelay(const ecl::Buffer &payload, Uint32 exclude_client_id);
    void HostBroadcastStreamRelay(const ecl::Buffer &payload, Uint32 exclude_client_id);

    // Flush ENet outbound queues (safe to call even if not connected).
    void Flush();
};

extern Transport g_transport;

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma

#endif
