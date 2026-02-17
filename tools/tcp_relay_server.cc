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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

/* -------------------- TCP relay server -------------------- */
/*
 * Simple TCP relay server for Enigma multiplayer.
 *
 * Provides a fallback transport for networks that block UDP or relayed UDP.
 */

namespace {
constexpr uint32_t kRelayMagic = 0x4C524E45;  // "ENRL"
constexpr uint8_t kRelayVersion = 1;
constexpr uint32_t kMaxFrame = 1u << 20;

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

struct Conn {
    int fd = -1;
    enum Role { UNKNOWN, HOST, CLIENT } role = UNKNOWN;
    uint32_t session_id = 0;
    uint32_t client_id = 0;
    std::vector<uint8_t> rx;
    uint32_t pending_len = 0;
};

struct Session {
    int host_fd = -1;
    uint32_t next_client_id = 1;
    std::unordered_map<uint32_t, int> clients;
};

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool send_all(int fd, const uint8_t *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int rc = ::poll(&pfd, 1, 250);
            if (rc <= 0)
                return false;
            continue;
        }
        return false;
    }
    return true;
}

bool send_frame(int fd, const uint8_t *data, size_t len) {
    if (len == 0 || len > kMaxFrame)
        return false;
    uint8_t prefix[4];
    uint32_t v = static_cast<uint32_t>(len);
    prefix[0] = static_cast<uint8_t>(v & 0xFF);
    prefix[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    prefix[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    prefix[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    return send_all(fd, prefix, sizeof(prefix)) && send_all(fd, data, len);
}

void write_u32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

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

bool parse_relay_header(const uint8_t *data, size_t size, RelayHeader &out,
                        const uint8_t **payload, size_t *payload_len) {
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
    if (payload)
        *payload = data + offset;
    if (payload_len)
        *payload_len = (offset <= size) ? (size - offset) : 0;
    return true;
}

std::vector<uint8_t> make_relay_packet(uint8_t type, uint32_t session_id, uint32_t client_id,
                                       const uint8_t *payload, size_t payload_len) {
    std::vector<uint8_t> out;
    out.reserve(4 + 1 + 1 + 4 + 4 + payload_len);
    write_u32(out, kRelayMagic);
    out.push_back(kRelayVersion);
    out.push_back(type);
    write_u32(out, session_id);
    write_u32(out, client_id);
    if (payload && payload_len)
        out.insert(out.end(), payload, payload + payload_len);
    return out;
}

bool pump_recv(Conn &c) {
    uint8_t buf[4096];
    while (true) {
        ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
        if (n == 0)
            return false;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        }
        c.rx.insert(c.rx.end(), buf, buf + n);
        if (static_cast<size_t>(n) < sizeof(buf))
            break;
    }
    return true;
}

bool try_extract_frame(Conn &c, std::vector<uint8_t> &out) {
    out.clear();
    if (c.pending_len == 0) {
        if (c.rx.size() < 4)
            return false;
        uint32_t v = static_cast<uint32_t>(c.rx[0]) |
                     (static_cast<uint32_t>(c.rx[1]) << 8) |
                     (static_cast<uint32_t>(c.rx[2]) << 16) |
                     (static_cast<uint32_t>(c.rx[3]) << 24);
        c.rx.erase(c.rx.begin(), c.rx.begin() + 4);
        if (v == 0 || v > kMaxFrame) {
            c.rx.clear();
            c.pending_len = 0;
            return false;
        }
        c.pending_len = v;
    }
    if (c.rx.size() < c.pending_len)
        return false;
    out.assign(c.rx.begin(), c.rx.begin() + c.pending_len);
    c.rx.erase(c.rx.begin(), c.rx.begin() + c.pending_len);
    c.pending_len = 0;
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    const char *host = "0.0.0.0";
    uint16_t port = 12349;
    if (argc > 1)
        host = argv[1];
    if (argc > 2)
        port = static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 10));

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo *res = nullptr;
    std::string port_str = std::to_string(static_cast<unsigned>(port));
    if (getaddrinfo(host, port_str.c_str(), &hints, &res) != 0) {
        std::fprintf(stderr, "Failed to resolve bind address\n");
        return 1;
    }

    int listen_fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (listen_fd < 0) {
        std::fprintf(stderr, "Failed to create socket\n");
        freeaddrinfo(res);
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (::bind(listen_fd, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen)) != 0) {
        std::fprintf(stderr, "Failed to bind %s:%u\n", host, port);
        freeaddrinfo(res);
        close(listen_fd);
        return 1;
    }
    freeaddrinfo(res);

    if (::listen(listen_fd, 64) != 0) {
        std::fprintf(stderr, "Failed to listen\n");
        close(listen_fd);
        return 1;
    }

    set_nonblocking(listen_fd);

    std::printf("TCP relay server listening on %s:%u\n", host, port);

    std::unordered_map<int, Conn> conns;
    std::unordered_map<uint32_t, Session> sessions;

    while (true) {
        std::vector<pollfd> fds;
        fds.reserve(conns.size() + 1);
        fds.push_back({listen_fd, POLLIN, 0});
        for (const auto &kv : conns)
            fds.push_back({kv.first, POLLIN, 0});

        int rc = ::poll(fds.data(), fds.size(), 10);
        if (rc < 0)
            continue;

        if (fds[0].revents & POLLIN) {
            while (true) {
                sockaddr_in addr;
                socklen_t alen = sizeof(addr);
                int fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&addr), &alen);
                if (fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    break;
                }
                set_nonblocking(fd);
                conns[fd] = Conn{fd};
            }
        }

        std::vector<int> to_close;
        std::vector<uint8_t> frame;

        for (size_t i = 1; i < fds.size(); ++i) {
            int fd = fds[i].fd;
            if ((fds[i].revents & (POLLIN | POLLERR | POLLHUP)) == 0)
                continue;
            auto it = conns.find(fd);
            if (it == conns.end())
                continue;
            Conn &c = it->second;

            if (!pump_recv(c)) {
                to_close.push_back(fd);
                continue;
            }

            while (try_extract_frame(c, frame)) {
                if (c.role == Conn::UNKNOWN) {
                    RelayHeader hdr;
                    const uint8_t *payload = nullptr;
                    size_t payload_len = 0;
                    if (!parse_relay_header(frame.data(), frame.size(), hdr, &payload, &payload_len)) {
                        to_close.push_back(fd);
                        break;
                    }
                    if (hdr.type == RELAY_HELLO_HOST) {
                        c.role = Conn::HOST;
                        c.session_id = hdr.session_id;
                        Session &s = sessions[c.session_id];
                        s.host_fd = fd;
                        // Notify host about already connected clients.
                        for (const auto &entry : s.clients) {
                            auto pkt = make_relay_packet(RELAY_CLIENT_CONNECT, c.session_id,
                                                         entry.first, nullptr, 0);
                            send_frame(fd, pkt.data(), pkt.size());
                        }
                    } else if (hdr.type == RELAY_HELLO_CLIENT) {
                        c.role = Conn::CLIENT;
                        c.session_id = hdr.session_id;
                        Session &s = sessions[c.session_id];
                        c.client_id = s.next_client_id++;
                        s.clients[c.client_id] = fd;
                        if (s.host_fd >= 0) {
                            auto pkt = make_relay_packet(RELAY_CLIENT_CONNECT, c.session_id,
                                                         c.client_id, nullptr, 0);
                            send_frame(s.host_fd, pkt.data(), pkt.size());
                        }
                    } else {
                        to_close.push_back(fd);
                        break;
                    }
                    continue;
                }

                if (c.role == Conn::CLIENT) {
                    auto sit = sessions.find(c.session_id);
                    if (sit != sessions.end() && sit->second.host_fd >= 0) {
                        auto pkt = make_relay_packet(RELAY_DATA, c.session_id, c.client_id,
                                                     frame.data(), frame.size());
                        send_frame(sit->second.host_fd, pkt.data(), pkt.size());
                    }
                    continue;
                }

                if (c.role == Conn::HOST) {
                    RelayHeader hdr;
                    const uint8_t *payload = nullptr;
                    size_t payload_len = 0;
                    if (!parse_relay_header(frame.data(), frame.size(), hdr, &payload, &payload_len))
                        continue;
                    if (hdr.type != RELAY_SEND)
                        continue;
                    auto sit = sessions.find(c.session_id);
                    if (sit == sessions.end())
                        continue;
                    auto cit = sit->second.clients.find(hdr.client_id);
                    if (cit == sit->second.clients.end())
                        continue;
                    send_frame(cit->second, payload, payload_len);
                    continue;
                }
            }
        }

        // Apply queued disconnects (and send control notifications).
        for (int fd : to_close) {
            auto it = conns.find(fd);
            if (it == conns.end())
                continue;
            Conn c = it->second;
            conns.erase(it);
            close(fd);

            if (c.role == Conn::CLIENT) {
                auto sit = sessions.find(c.session_id);
                if (sit != sessions.end()) {
                    sit->second.clients.erase(c.client_id);
                    if (sit->second.host_fd >= 0) {
                        auto pkt = make_relay_packet(RELAY_CLIENT_DISCONNECT, c.session_id,
                                                     c.client_id, nullptr, 0);
                        send_frame(sit->second.host_fd, pkt.data(), pkt.size());
                    }
                    if (sit->second.host_fd < 0 && sit->second.clients.empty())
                        sessions.erase(sit);
                }
            } else if (c.role == Conn::HOST) {
                auto sit = sessions.find(c.session_id);
                if (sit != sessions.end() && sit->second.host_fd == fd) {
                    // Close all clients when the host goes away.
                    for (const auto &entry : sit->second.clients) {
                        int cfd = entry.second;
                        auto cit = conns.find(cfd);
                        if (cit != conns.end()) {
                            conns.erase(cit);
                            close(cfd);
                        }
                    }
                    sessions.erase(sit);
                }
            }
        }
    }
}
