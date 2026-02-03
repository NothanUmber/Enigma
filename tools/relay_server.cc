#include <enet/enet.h>

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
constexpr uint32_t kRelayMagic = 0x4C524E45;  // "ENRL"
constexpr uint8_t kRelayVersion = 1;
constexpr size_t kRelayHeaderSize = 4 + 1 + 1 + 4 + 4;

enum RelayType : uint8_t {
    RELAY_HELLO_HOST = 1,
    RELAY_HELLO_CLIENT = 2,
    RELAY_CLIENT_CONNECT = 3,
    RELAY_CLIENT_DISCONNECT = 4,
    RELAY_SEND = 5,
    RELAY_DATA = 6,
    RELAY_ERROR = 7
};

struct RelayHeader {
    uint8_t type = 0;
    uint32_t session_id = 0;
    uint32_t client_id = 0;
};

struct PeerInfo {
    enum Role { UNKNOWN, HOST, CLIENT } role = UNKNOWN;
    uint32_t session_id = 0;
    uint32_t client_id = 0;
};

struct RelaySession {
    ENetPeer *host = nullptr;
    uint32_t next_client_id = 1;
    std::unordered_map<uint32_t, ENetPeer *> clients;
};

bool read_u8(const uint8_t *data, size_t size, size_t &offset, uint8_t &out) {
    if (offset + 1 > size)
        return false;
    out = data[offset++];
    return true;
}

bool read_u32(const uint8_t *data, size_t size, size_t &offset, uint32_t &out) {
    if (offset + 4 > size)
        return false;
    out = static_cast<uint32_t>(data[offset]) |
          (static_cast<uint32_t>(data[offset + 1]) << 8) |
          (static_cast<uint32_t>(data[offset + 2]) << 16) |
          (static_cast<uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return true;
}

void write_u32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

bool parse_relay_header(const ENetPacket *packet, RelayHeader &out,
                        const uint8_t **payload, size_t *payload_len) {
    const auto *data = reinterpret_cast<const uint8_t *>(packet->data);
    size_t size = packet->dataLength;
    size_t offset = 0;
    uint32_t magic = 0;
    uint8_t version = 0;
    if (!read_u32(data, size, offset, magic))
        return false;
    if (!read_u8(data, size, offset, version))
        return false;
    if (!read_u8(data, size, offset, out.type))
        return false;
    if (!read_u32(data, size, offset, out.session_id))
        return false;
    if (!read_u32(data, size, offset, out.client_id))
        return false;
    if (magic != kRelayMagic || version != kRelayVersion)
        return false;
    if (payload) {
        *payload = data + offset;
    }
    if (payload_len) {
        *payload_len = (offset <= size) ? (size - offset) : 0;
    }
    return true;
}

ENetPacket *make_relay_packet(uint8_t type, uint32_t session_id, uint32_t client_id,
                              const uint8_t *payload, size_t payload_len,
                              enet_uint32 flags) {
    std::vector<uint8_t> data;
    data.reserve(kRelayHeaderSize + payload_len);
    write_u32(data, kRelayMagic);
    data.push_back(kRelayVersion);
    data.push_back(type);
    write_u32(data, session_id);
    write_u32(data, client_id);
    if (payload && payload_len > 0) {
        data.insert(data.end(), payload, payload + payload_len);
    }
    return enet_packet_create(data.data(), data.size(), flags);
}

}  // namespace

int main(int argc, char **argv) {
    if (enet_initialize() != 0) {
        std::fprintf(stderr, "Failed to initialize ENet\n");
        return 1;
    }
    atexit(enet_deinitialize);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    const char *host_str = "0.0.0.0";
    uint16_t port = 12348;
    if (argc > 1)
        host_str = argv[1];
    if (argc > 2)
        port = static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 10));

    ENetAddress address;
    enet_address_set_host(&address, host_str);
    address.port = port;
    ENetHost *server = enet_host_create(&address, 64, 0, 0);
    if (!server) {
        std::fprintf(stderr, "Failed to create relay server on %s:%u\n", host_str, port);
        return 1;
    }

    std::unordered_map<ENetPeer *, PeerInfo> peers;
    std::unordered_map<uint32_t, RelaySession> sessions;

    std::printf("Relay server listening on %s:%u\n", host_str, port);

    while (true) {
        ENetEvent event;
        while (enet_host_service(server, &event, 1) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                peers[event.peer] = PeerInfo();
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                auto info_it = peers.find(event.peer);
                if (info_it == peers.end()) {
                    enet_packet_destroy(event.packet);
                    break;
                }
                PeerInfo &info = info_it->second;
                if (info.role == PeerInfo::UNKNOWN) {
                    RelayHeader header;
                    if (!parse_relay_header(event.packet, header, nullptr, nullptr)) {
                        enet_peer_disconnect(event.peer, 0);
                        enet_packet_destroy(event.packet);
                        break;
                    }
                    if (header.type == RELAY_HELLO_HOST) {
                        info.role = PeerInfo::HOST;
                        info.session_id = header.session_id;
                        auto &session = sessions[header.session_id];
                        if (session.host && session.host != event.peer)
                            enet_peer_disconnect(session.host, 0);
                        session.host = event.peer;
                        for (const auto &entry : session.clients) {
                            ENetPacket *packet = make_relay_packet(RELAY_CLIENT_CONNECT,
                                                                   header.session_id,
                                                                   entry.first, nullptr, 0,
                                                                   ENET_PACKET_FLAG_RELIABLE);
                            enet_peer_send(session.host, 0, packet);
                        }
                    } else if (header.type == RELAY_HELLO_CLIENT) {
                        info.role = PeerInfo::CLIENT;
                        info.session_id = header.session_id;
                        auto &session = sessions[header.session_id];
                        uint32_t client_id = session.next_client_id++;
                        info.client_id = client_id;
                        session.clients[client_id] = event.peer;
                        if (session.host) {
                            ENetPacket *packet = make_relay_packet(RELAY_CLIENT_CONNECT,
                                                                   header.session_id,
                                                                   client_id, nullptr, 0,
                                                                   ENET_PACKET_FLAG_RELIABLE);
                            enet_peer_send(session.host, 0, packet);
                        }
                    } else {
                        enet_peer_disconnect(event.peer, 0);
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
                if (info.role == PeerInfo::CLIENT) {
                    auto session_it = sessions.find(info.session_id);
                    if (session_it != sessions.end() && session_it->second.host) {
                        ENetPacket *packet = make_relay_packet(RELAY_DATA,
                                                               info.session_id,
                                                               info.client_id,
                                                               static_cast<const uint8_t *>(event.packet->data),
                                                               event.packet->dataLength,
                                                               event.packet->flags);
                        enet_peer_send(session_it->second.host, 0, packet);
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
                if (info.role == PeerInfo::HOST) {
                    RelayHeader header;
                    const uint8_t *payload = nullptr;
                    size_t payload_len = 0;
                    if (!parse_relay_header(event.packet, header, &payload, &payload_len)) {
                        enet_packet_destroy(event.packet);
                        break;
                    }
                    if (header.type == RELAY_SEND) {
                        auto session_it = sessions.find(info.session_id);
                        if (session_it != sessions.end()) {
                            auto client_it = session_it->second.clients.find(header.client_id);
                            if (client_it != session_it->second.clients.end()) {
                                ENetPacket *packet = enet_packet_create(payload, payload_len,
                                                                        event.packet->flags);
                                enet_peer_send(client_it->second, 0, packet);
                            }
                        }
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
                enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                auto info_it = peers.find(event.peer);
                if (info_it == peers.end())
                    break;
                PeerInfo info = info_it->second;
                peers.erase(info_it);
                if (info.role == PeerInfo::HOST) {
                    auto session_it = sessions.find(info.session_id);
                    if (session_it != sessions.end() && session_it->second.host == event.peer) {
                        for (const auto &entry : session_it->second.clients)
                            enet_peer_disconnect(entry.second, 0);
                        sessions.erase(session_it);
                    }
                } else if (info.role == PeerInfo::CLIENT) {
                    auto session_it = sessions.find(info.session_id);
                    if (session_it != sessions.end()) {
                        session_it->second.clients.erase(info.client_id);
                        if (session_it->second.host) {
                            ENetPacket *packet = make_relay_packet(RELAY_CLIENT_DISCONNECT,
                                                                   info.session_id,
                                                                   info.client_id, nullptr, 0,
                                                                   ENET_PACKET_FLAG_RELIABLE);
                            enet_peer_send(session_it->second.host, 0, packet);
                        }
                        if (!session_it->second.host && session_it->second.clients.empty())
                            sessions.erase(session_it);
                    }
                }
                break;
            }
            default:
                break;
            }
        }
    }

    enet_host_destroy(server);
    return 0;
}
