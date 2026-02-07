#ifndef MULTIPLAYER_TRANSPORT_HH_INCLUDED
#define MULTIPLAYER_TRANSPORT_HH_INCLUDED

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
// - TCP relay framing for both host and clients
//
// Session logic provides an ITransportSink and stays agnostic to the underlying transport.
class Transport {
public:
    bool Poll(ITransportSink &sink);

    // Client side: sends raw game payload to the currently active host link.
    bool ClientSend(const ecl::Buffer &payload);

    // Host side: sends raw game payload to a specific remote.
    void HostSendDirect(ENetPeer *peer, const ecl::Buffer &payload);
    void HostSendUdpRelay(Uint32 client_id, const ecl::Buffer &payload);
    void HostSendTcpRelay(Uint32 client_id, const ecl::Buffer &payload);

    // Host side: broadcast raw game payload to all remotes.
    void HostBroadcast(const ecl::Buffer &payload);
    void HostBroadcastUdpRelay(const ecl::Buffer &payload, Uint32 exclude_client_id);
    void HostBroadcastTcpRelay(const ecl::Buffer &payload, Uint32 exclude_client_id);

    // Flush ENet outbound queues (safe to call even if not connected).
    void Flush();
};

extern Transport g_transport;

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma

#endif

