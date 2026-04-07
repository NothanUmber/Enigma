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

#include "multiplayer_internal.hh"

#include "multiplayer_config.hh"

#include "SDL.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#endif

/* -------------------- Multiplayer stream relay -------------------- */
/*
 * Shared socket/framing helper for stream-based relays.
 *
 * Raw TCP relay still uses the existing 4-byte length prefix protocol. The
 * WebSocket relay variant keeps the relay payload format unchanged and only
 * swaps the outer transport to RFC6455 binary messages. TLS/proxy handling for
 * `wss://` is delegated to libcurl via CONNECT_ONLY using an `https://` URL,
 * then the HTTP Upgrade handshake and framing are handled here.
 */

namespace enigma {
namespace multiplayer {
namespace internal {

namespace {

constexpr size_t kWebSocketFrameChunk = 60 * 1024;
constexpr size_t kWebSocketMaxFrame = 1u << 20;
constexpr size_t kWebSocketKeyBytes = 16;
constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

enum class WebSocketParseStatus {
    NEED_MORE = 0,
    OK = 1,
    INVALID = 2
};

CURL *ws_easy_handle(const StreamRelayConnection &relay) {
    return reinterpret_cast<CURL *>(relay.ws_easy);
}

CURLM *ws_multi_handle(const StreamRelayConnection &relay) {
    return reinterpret_cast<CURLM *>(relay.ws_multi);
}

bool wait_for_socket(TcpSocket socket, Uint32 timeout_ms, bool want_write, bool &ready) {
    ready = false;
    if (!tcp_socket_valid(socket))
        return false;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(socket, &fds);
    timeval tv;
    tv.tv_sec = static_cast<long>(timeout_ms / 1000);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);

#ifdef WIN32
    int sel = want_write ? ::select(0, nullptr, &fds, nullptr, &tv)
                         : ::select(0, &fds, nullptr, nullptr, &tv);
#else
    int sel = want_write ? ::select(socket + 1, nullptr, &fds, nullptr, &tv)
                         : ::select(socket + 1, &fds, nullptr, nullptr, &tv);
#endif
    if (sel < 0)
        return false;
    ready = (sel > 0);
    return true;
}

bool send_hello(StreamRelayConnection &relay, RelayMessageType type, Uint32 session_id) {
    ecl::Buffer hello;
    encode_relay_header(hello, type, session_id, 0);
    return stream_relay_send_frame(relay, hello.data(), hello.size());
}

bool send_payload(StreamRelayConnection &relay, Uint32 session_id,
                  Uint32 client_id, const void *payload, size_t payload_len) {
    if (!stream_relay_connected(relay))
        return false;
    ecl::Buffer buf;
    encode_relay_header(buf, RELAY_SEND, session_id, client_id);
    buf.write(payload, payload_len);
    return stream_relay_send_frame(relay, buf.data(), buf.size());
}

uint32_t rotate_left32(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32 - bits));
}

std::array<uint8_t, 20> sha1_digest(const uint8_t *data, size_t len) {
    uint64_t bit_len = static_cast<uint64_t>(len) * 8u;
    std::array<uint8_t, 20> digest = {};
    std::array<uint8_t, 64> block = {};
    std::array<uint32_t, 5> h = {
        0x67452301u,
        0xEFCDAB89u,
        0x98BADCFEu,
        0x10325476u,
        0xC3D2E1F0u
    };

    auto process_block = [&h](const uint8_t *chunk) {
        uint32_t w[80];
        for (unsigned i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(chunk[i * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(chunk[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(chunk[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(chunk[i * 4 + 3]);
        }
        for (unsigned i = 16; i < 80; ++i)
            w[i] = rotate_left32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0];
        uint32_t b = h[1];
        uint32_t c = h[2];
        uint32_t d = h[3];
        uint32_t e = h[4];

        for (unsigned i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t temp = rotate_left32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotate_left32(b, 30);
            b = a;
            a = temp;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    };

    size_t offset = 0;
    while (offset + 64 <= len) {
        process_block(data + offset);
        offset += 64;
    }

    size_t tail = len - offset;
    std::fill(block.begin(), block.end(), 0);
    if (tail > 0)
        std::memcpy(block.data(), data + offset, tail);
    block[tail] = 0x80;

    if (tail >= 56) {
        process_block(block.data());
        std::fill(block.begin(), block.end(), 0);
    }

    for (unsigned i = 0; i < 8; ++i) {
        block[63 - i] = static_cast<uint8_t>(bit_len & 0xFFu);
        bit_len >>= 8;
    }
    process_block(block.data());

    for (unsigned i = 0; i < h.size(); ++i) {
        digest[i * 4 + 0] = static_cast<uint8_t>((h[i] >> 24) & 0xFFu);
        digest[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFFu);
        digest[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFFu);
        digest[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFFu);
    }
    return digest;
}

std::string base64_encode(const uint8_t *data, size_t len) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t value = static_cast<uint32_t>(data[i]) << 16;
        bool have_b1 = (i + 1 < len);
        bool have_b2 = (i + 2 < len);
        if (have_b1)
            value |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (have_b2)
            value |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTable[(value >> 18) & 0x3F]);
        out.push_back(kTable[(value >> 12) & 0x3F]);
        out.push_back(have_b1 ? kTable[(value >> 6) & 0x3F] : '=');
        out.push_back(have_b2 ? kTable[value & 0x3F] : '=');
    }
    return out;
}

void fill_random_bytes(uint8_t *data, size_t len) {
    std::random_device rd;
    for (size_t i = 0; i < len; ++i)
        data[i] = static_cast<uint8_t>(rd());
}

std::string to_lower_ascii(std::string value) {
    for (char &ch : value) {
        unsigned char uch = static_cast<unsigned char>(ch);
        ch = static_cast<char>(std::tolower(uch));
    }
    return value;
}

std::string trim_ascii(const std::string &value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0)
        ++begin;
    size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
        --end;
    return value.substr(begin, end - begin);
}

bool header_contains_token(const std::string &value, const std::string &token) {
    size_t pos = 0;
    while (pos < value.size()) {
        size_t next = value.find(',', pos);
        if (next == std::string::npos)
            next = value.size();
        if (to_lower_ascii(trim_ascii(value.substr(pos, next - pos))) == token)
            return true;
        pos = next + 1;
    }
    return false;
}

std::string websocket_accept_value(const std::string &key) {
    const std::string accept_source = key + kWebSocketGuid;
    const auto digest = sha1_digest(
        reinterpret_cast<const uint8_t *>(accept_source.data()), accept_source.size());
    return base64_encode(digest.data(), digest.size());
}

bool refresh_active_socket(StreamRelayConnection &relay) {
    if (relay.mode != StreamRelayMode::WEBSOCKET)
        return tcp_socket_valid(relay.socket);
    CURL *easy = ws_easy_handle(relay);
    if (!easy)
        return false;
    curl_socket_t active = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &active) != CURLE_OK ||
        active == CURL_SOCKET_BAD) {
        relay.socket = kInvalidTcpSocket;
        return false;
    }
    relay.socket = static_cast<TcpSocket>(active);
    return true;
}

void cleanup_websocket(StreamRelayConnection &relay) {
    CURLM *multi = ws_multi_handle(relay);
    CURL *easy = ws_easy_handle(relay);
    if (multi && easy)
        curl_multi_remove_handle(multi, easy);
    if (multi)
        curl_multi_cleanup(multi);
    if (easy)
        curl_easy_cleanup(easy);
    relay.ws_easy = nullptr;
    relay.ws_multi = nullptr;
    relay.ws_connecting = false;
    relay.ws_open = false;
    relay.ws_message_active = false;
    relay.ws_message.clear();
    relay.ws_handshake_request.clear();
    relay.ws_handshake_offset = 0;
    relay.ws_accept_value.clear();
    relay.frames.clear();
}

std::string websocket_connect_url(
    const ResolvedInternetUrl &relay_url) {
    return std::string(relay_url.secure() ? "https" : "http") +
           "://" + relay_url.host + ":" + std::to_string(relay_url.port) + relay_url.path;
}

bool websocket_prepare_handshake(
    const ResolvedInternetUrl &relay_url,
    StreamRelayConnection &relay) {
    if (!relay_url.is_valid())
        return false;

    uint8_t raw_key[kWebSocketKeyBytes];
    fill_random_bytes(raw_key, sizeof(raw_key));
    const std::string key = base64_encode(raw_key, sizeof(raw_key));
    relay.ws_accept_value = websocket_accept_value(key);

    const std::string host_header =
        relay_url.host + ":" + std::to_string(static_cast<unsigned>(relay_url.port));
    const std::string request =
        "GET " + relay_url.path + " HTTP/1.1\r\n"
        "Host: " + host_header + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    relay.ws_handshake_request.assign(request.begin(), request.end());
    relay.ws_handshake_offset = 0;
    return true;
}

bool websocket_send_bytes(StreamRelayConnection &relay, const uint8_t *data, size_t len,
                          Uint32 timeout_ms, size_t &offset) {
    CURL *easy = ws_easy_handle(relay);
    if (!easy || !refresh_active_socket(relay))
        return false;

    const Uint32 start = SDL_GetTicks();
    while (offset < len) {
        size_t sent = 0;
        CURLcode rc = curl_easy_send(
            easy, reinterpret_cast<const char *>(data + offset), len - offset, &sent);
        if (rc == CURLE_OK) {
            if (sent == 0)
                return false;
            offset += sent;
            continue;
        }
        if (rc != CURLE_AGAIN) {
            if (debug_enabled())
                debug_log("mp ws relay: send failed: %s", curl_easy_strerror(rc));
            return false;
        }
        if (timeout_ms == 0)
            return true;
        Uint32 elapsed = SDL_GetTicks() - start;
        if (elapsed >= timeout_ms)
            return true;
        bool writable = false;
        if (!wait_for_socket(relay.socket, timeout_ms - elapsed, true, writable))
            return false;
        if (!writable)
            return true;
    }
    return true;
}

bool websocket_recv_bytes(StreamRelayConnection &relay, Uint32 timeout_ms, bool &got_data) {
    CURL *easy = ws_easy_handle(relay);
    if (!easy || !refresh_active_socket(relay))
        return false;

    got_data = false;
    const Uint32 start = SDL_GetTicks();
    while (true) {
        uint8_t buf[4096];
        size_t n = 0;
        CURLcode rc =
            curl_easy_recv(easy, reinterpret_cast<char *>(buf), sizeof(buf), &n);
        if (rc == CURLE_OK) {
            if (n == 0)
                return false;
            relay.rx.insert(relay.rx.end(), buf, buf + n);
            got_data = true;
            if (n < sizeof(buf))
                return true;
            continue;
        }
        if (rc != CURLE_AGAIN) {
            if (debug_enabled())
                debug_log("mp ws relay: recv failed: %s", curl_easy_strerror(rc));
            return false;
        }
        if (got_data || timeout_ms == 0)
            return true;
        Uint32 elapsed = SDL_GetTicks() - start;
        if (elapsed >= timeout_ms)
            return true;
        bool readable = false;
        if (!wait_for_socket(relay.socket, timeout_ms - elapsed, false, readable))
            return false;
        if (!readable)
            return true;
    }
}

bool websocket_validate_handshake(const std::string &response,
                                  const std::string &accept_value) {
    const size_t status_end = response.find("\r\n");
    if (status_end == std::string::npos)
        return false;

    const std::string status_line = response.substr(0, status_end);
    size_t code_pos = status_line.find(' ');
    while (code_pos != std::string::npos && code_pos < status_line.size() &&
           status_line[code_pos] == ' ') {
        ++code_pos;
    }
    if (code_pos == std::string::npos || status_line.substr(code_pos, 3) != "101")
        return false;

    bool saw_upgrade = false;
    bool saw_connection = false;
    bool saw_accept = false;
    size_t pos = status_end + 2;
    while (pos < response.size()) {
        size_t next = response.find("\r\n", pos);
        if (next == std::string::npos || next == pos)
            break;
        const std::string line = response.substr(pos, next - pos);
        pos = next + 2;

        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string name = to_lower_ascii(trim_ascii(line.substr(0, colon)));
        const std::string value = trim_ascii(line.substr(colon + 1));
        if (name == "upgrade")
            saw_upgrade = (to_lower_ascii(value) == "websocket");
        else if (name == "connection")
            saw_connection = header_contains_token(value, "upgrade");
        else if (name == "sec-websocket-accept")
            saw_accept = (value == accept_value);
    }
    return saw_upgrade && saw_connection && saw_accept;
}

bool websocket_try_finish_handshake(StreamRelayConnection &relay, bool &completed,
                                    bool &success) {
    completed = false;
    success = false;
    static const uint8_t kHeaderEnd[] = {'\r', '\n', '\r', '\n'};
    auto it = std::search(relay.rx.begin(), relay.rx.end(),
                          std::begin(kHeaderEnd), std::end(kHeaderEnd));
    if (it == relay.rx.end())
        return true;

    size_t header_len = static_cast<size_t>(it - relay.rx.begin()) + 4;
    std::string response(reinterpret_cast<const char *>(relay.rx.data()), header_len);
    relay.rx.erase(relay.rx.begin(), relay.rx.begin() + header_len);

    completed = true;
    success = websocket_validate_handshake(response, relay.ws_accept_value);
    if (!success) {
        if (debug_enabled())
            debug_log("mp ws relay: invalid handshake response");
        return true;
    }

    relay.ws_connecting = false;
    relay.ws_open = true;
    relay.ws_handshake_request.clear();
    relay.ws_handshake_offset = 0;
    relay.ws_accept_value.clear();
    return true;
}

bool websocket_progress_handshake(StreamRelayConnection &relay, Uint32 timeout_ms,
                                  bool &completed, bool &success) {
    completed = false;
    success = false;

    const Uint32 start = SDL_GetTicks();
    while (true) {
        if (relay.ws_handshake_offset < relay.ws_handshake_request.size()) {
            Uint32 remaining = 0;
            if (timeout_ms > 0) {
                Uint32 elapsed = SDL_GetTicks() - start;
                if (elapsed >= timeout_ms)
                    return true;
                remaining = timeout_ms - elapsed;
            }
            if (!websocket_send_bytes(relay,
                                      relay.ws_handshake_request.data(),
                                      relay.ws_handshake_request.size(),
                                      remaining,
                                      relay.ws_handshake_offset)) {
                return false;
            }
            if (relay.ws_handshake_offset < relay.ws_handshake_request.size())
                return true;
        }

        Uint32 remaining = 0;
        if (timeout_ms > 0) {
            Uint32 elapsed = SDL_GetTicks() - start;
            if (elapsed >= timeout_ms)
                return true;
            remaining = timeout_ms - elapsed;
        }
        bool got_data = false;
        if (!websocket_recv_bytes(relay, remaining, got_data))
            return false;
        if (!websocket_try_finish_handshake(relay, completed, success))
            return false;
        if (completed)
            return true;
        if (!got_data)
            return true;
    }
}

bool websocket_drain_connect_messages(StreamRelayConnection &relay,
                                      bool &completed, bool &success) {
    completed = false;
    success = false;
    CURLM *multi = ws_multi_handle(relay);
    if (!multi)
        return false;

    int msgs_left = 0;
    while (true) {
        CURLMsg *msg = curl_multi_info_read(multi, &msgs_left);
        if (!msg)
            break;
        if (msg->msg != CURLMSG_DONE)
            continue;
        completed = true;
        success = (msg->data.result == CURLE_OK);
        if (!success && debug_enabled()) {
            debug_log("mp ws relay: connect failed: %s",
                      curl_easy_strerror(msg->data.result));
        }
        CURL *easy = ws_easy_handle(relay);
        if (easy)
            curl_multi_remove_handle(multi, easy);
        curl_multi_cleanup(multi);
        relay.ws_multi = nullptr;
        if (success)
            refresh_active_socket(relay);
        return true;
    }
    return true;
}

bool websocket_drive_connect(StreamRelayConnection &relay, Uint32 timeout_ms,
                             bool &completed, bool &success) {
    completed = false;
    success = false;
    CURLM *multi = ws_multi_handle(relay);
    if (!multi)
        return false;

    int still_running = 0;
    CURLMcode mc = curl_multi_perform(multi, &still_running);
    if (mc != CURLM_OK) {
        if (debug_enabled()) {
            debug_log("mp ws relay: curl_multi_perform failed: %s",
                      curl_multi_strerror(mc));
        }
        return false;
    }
    if (!websocket_drain_connect_messages(relay, completed, success))
        return false;
    if (completed || timeout_ms == 0)
        return true;

    mc = curl_multi_poll(multi, nullptr, 0, static_cast<int>(timeout_ms), nullptr);
    if (mc != CURLM_OK) {
        if (debug_enabled()) {
            debug_log("mp ws relay: curl_multi_poll failed: %s",
                      curl_multi_strerror(mc));
        }
        return false;
    }
    mc = curl_multi_perform(multi, &still_running);
    if (mc != CURLM_OK) {
        if (debug_enabled()) {
            debug_log("mp ws relay: curl_multi_perform failed: %s",
                      curl_multi_strerror(mc));
        }
        return false;
    }
    return websocket_drain_connect_messages(relay, completed, success);
}

bool websocket_progress_connect(StreamRelayConnection &relay, Uint32 timeout_ms,
                                bool &completed, bool &success) {
    completed = false;
    success = false;
    const Uint32 start = SDL_GetTicks();

    if (relay.ws_multi != nullptr) {
        Uint32 remaining = 0;
        if (timeout_ms > 0) {
            Uint32 elapsed = SDL_GetTicks() - start;
            if (elapsed >= timeout_ms)
                return true;
            remaining = timeout_ms - elapsed;
        }
        bool connect_done = false;
        bool connect_ok = false;
        if (!websocket_drive_connect(relay, remaining, connect_done, connect_ok))
            return false;
        if (connect_done && !connect_ok) {
            completed = true;
            success = false;
            return true;
        }
        if (!connect_done)
            return true;
    }

    Uint32 remaining = 0;
    if (timeout_ms > 0) {
        Uint32 elapsed = SDL_GetTicks() - start;
        if (elapsed >= timeout_ms)
            return true;
        remaining = timeout_ms - elapsed;
    }
    if (!websocket_progress_handshake(relay, remaining, completed, success))
        return false;
    return true;
}

bool websocket_send_raw_frame(StreamRelayConnection &relay, uint8_t opcode,
                              const uint8_t *payload, size_t payload_len, bool fin) {
    if (!relay.ws_open || !refresh_active_socket(relay))
        return false;

    std::vector<uint8_t> frame;
    frame.reserve(payload_len + 16);
    frame.push_back(static_cast<uint8_t>((fin ? 0x80u : 0x00u) | (opcode & 0x0Fu)));
    if (payload_len < 126) {
        frame.push_back(static_cast<uint8_t>(0x80u | payload_len));
    } else if (payload_len <= 0xFFFFu) {
        frame.push_back(0x80u | 126u);
        frame.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFFu));
        frame.push_back(static_cast<uint8_t>(payload_len & 0xFFu));
    } else {
        frame.push_back(0x80u | 127u);
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<uint8_t>(
                (static_cast<uint64_t>(payload_len) >> shift) & 0xFFu));
        }
    }

    uint8_t mask[4];
    fill_random_bytes(mask, sizeof(mask));
    frame.insert(frame.end(), mask, mask + sizeof(mask));
    for (size_t i = 0; i < payload_len; ++i)
        frame.push_back(static_cast<uint8_t>(payload[i] ^ mask[i % 4]));

    size_t offset = 0;
    while (offset < frame.size()) {
        if (!websocket_send_bytes(relay, frame.data(), frame.size(), 200, offset))
            return false;
        if (offset < frame.size())
            return false;
    }
    return true;
}

WebSocketParseStatus websocket_try_extract_frame(std::vector<uint8_t> &rx,
                                                 bool &fin, uint8_t &opcode,
                                                 std::vector<uint8_t> &payload) {
    payload.clear();
    if (rx.size() < 2)
        return WebSocketParseStatus::NEED_MORE;

    fin = (rx[0] & 0x80u) != 0;
    opcode = rx[0] & 0x0Fu;
    const bool masked = (rx[1] & 0x80u) != 0;
    uint64_t payload_len = rx[1] & 0x7Fu;
    size_t offset = 2;

    if ((rx[0] & 0x70u) != 0)
        return WebSocketParseStatus::INVALID;

    if (payload_len == 126) {
        if (rx.size() < offset + 2)
            return WebSocketParseStatus::NEED_MORE;
        payload_len = (static_cast<uint64_t>(rx[offset]) << 8) |
                      static_cast<uint64_t>(rx[offset + 1]);
        offset += 2;
    } else if (payload_len == 127) {
        if (rx.size() < offset + 8)
            return WebSocketParseStatus::NEED_MORE;
        payload_len = 0;
        for (unsigned i = 0; i < 8; ++i)
            payload_len = (payload_len << 8) | static_cast<uint64_t>(rx[offset + i]);
        offset += 8;
    }

    if (payload_len > kWebSocketMaxFrame)
        return WebSocketParseStatus::INVALID;
    if (masked)
        return WebSocketParseStatus::INVALID;
    if (rx.size() < offset + payload_len)
        return WebSocketParseStatus::NEED_MORE;

    payload.assign(rx.begin() + offset, rx.begin() + offset + payload_len);
    rx.erase(rx.begin(), rx.begin() + offset + payload_len);
    return WebSocketParseStatus::OK;
}

bool websocket_handle_received_frame(StreamRelayConnection &relay,
                                     bool fin, uint8_t opcode,
                                     const std::vector<uint8_t> &payload) {
    switch (opcode) {
    case 0x0:
        if (!relay.ws_message_active)
            return false;
        relay.ws_message.insert(relay.ws_message.end(), payload.begin(), payload.end());
        if (fin) {
            relay.frames.push_back(relay.ws_message);
            relay.ws_message.clear();
            relay.ws_message_active = false;
        }
        return true;
    case 0x2:
        if (relay.ws_message_active)
            return false;
        relay.ws_message = payload;
        if (fin) {
            relay.frames.push_back(relay.ws_message);
            relay.ws_message.clear();
            relay.ws_message_active = false;
        } else {
            relay.ws_message_active = true;
        }
        return true;
    case 0x8:
        return false;
    case 0x9:
        return websocket_send_raw_frame(relay, 0xAu, payload.data(), payload.size(), true);
    case 0xA:
        return true;
    default:
        return false;
    }
}

}  // namespace

bool stream_relay_connected(const StreamRelayConnection &relay) {
    if (relay.mode == StreamRelayMode::WEBSOCKET)
        return relay.ws_open && relay.ws_easy != nullptr;
    return tcp_socket_valid(relay.socket);
}

bool stream_relay_is_websocket(const StreamRelayConnection &relay) {
    return relay.mode == StreamRelayMode::WEBSOCKET;
}

TransportKind stream_relay_transport_kind(const StreamRelayConnection &relay) {
    if (relay.mode == StreamRelayMode::WEBSOCKET)
        return TransportKind::WS_RELAY;
    if (relay.mode == StreamRelayMode::TCP)
        return TransportKind::TCP_RELAY;
    return TransportKind::NONE;
}

TcpSocket stream_relay_native_socket(const StreamRelayConnection &relay) {
    return relay.socket;
}

void stream_relay_reset(StreamRelayConnection &relay) {
    if (relay.mode == StreamRelayMode::WEBSOCKET)
        cleanup_websocket(relay);
    else
        tcp_close(relay.socket);
    relay.mode = StreamRelayMode::NONE;
    relay.socket = kInvalidTcpSocket;
    relay.rx.clear();
    relay.frame_len = 0;
    relay.frames.clear();
    relay.ws_message.clear();
    relay.ws_message_active = false;
}

void stream_relay_adopt_socket(StreamRelayConnection &relay, TcpSocket socket) {
    if (relay.socket == socket)
        return;
    stream_relay_reset(relay);
    relay.mode = StreamRelayMode::TCP;
    relay.socket = socket;
}

bool stream_relay_connect_timeout(const std::string &host, Uint16 port, Uint32 timeout_ms,
                                  StreamRelayConnection &relay) {
    stream_relay_reset(relay);
    TcpSocket socket = kInvalidTcpSocket;
    if (!tcp_connect_timeout(host, port, timeout_ms, socket))
        return false;
    relay.mode = StreamRelayMode::TCP;
    relay.socket = socket;
    return true;
}

bool stream_relay_begin_websocket_connect(const std::string &url, Uint32 timeout_ms,
                                          StreamRelayConnection &relay) {
    stream_relay_reset(relay);
    if (url.empty())
        return false;

    const auto relay_url = multiplayer::ResolveWebSocketRelayUrl(url);
    if (!relay_url.is_valid())
        return false;

    CURL *easy = curl_easy_init();
    CURLM *multi = curl_multi_init();
    if (!easy || !multi) {
        if (easy)
            curl_easy_cleanup(easy);
        if (multi)
            curl_multi_cleanup(multi);
        return false;
    }

    if (!websocket_prepare_handshake(relay_url, relay)) {
        curl_easy_cleanup(easy);
        curl_multi_cleanup(multi);
        return false;
    }

    const std::string connect_url = websocket_connect_url(relay_url);
    curl_easy_setopt(easy, CURLOPT_URL, connect_url.c_str());
    curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));

    if (curl_multi_add_handle(multi, easy) != CURLM_OK) {
        curl_easy_cleanup(easy);
        curl_multi_cleanup(multi);
        relay.ws_handshake_request.clear();
        relay.ws_accept_value.clear();
        return false;
    }

    relay.mode = StreamRelayMode::WEBSOCKET;
    relay.ws_easy = easy;
    relay.ws_multi = multi;
    relay.ws_connecting = true;
    relay.ws_open = false;
    relay.ws_message_active = false;

    bool completed = false;
    bool success = false;
    if (!stream_relay_poll_connect(relay, 0, completed)) {
        stream_relay_reset(relay);
        return false;
    }
    if (completed && !stream_relay_connected(relay)) {
        stream_relay_reset(relay);
        return false;
    }
    (void)success;
    return true;
}

bool stream_relay_connect_websocket_timeout(const std::string &url, Uint32 timeout_ms,
                                            StreamRelayConnection &relay) {
    stream_relay_reset(relay);
    if (url.empty())
        return false;

    const auto relay_url = multiplayer::ResolveWebSocketRelayUrl(url);
    if (!relay_url.is_valid())
        return false;

    CURL *easy = curl_easy_init();
    if (!easy)
        return false;

    if (!websocket_prepare_handshake(relay_url, relay)) {
        curl_easy_cleanup(easy);
        return false;
    }

    const std::string connect_url = websocket_connect_url(relay_url);
    curl_easy_setopt(easy, CURLOPT_URL, connect_url.c_str());
    curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));

    relay.mode = StreamRelayMode::WEBSOCKET;
    relay.ws_easy = easy;
    relay.ws_connecting = true;
    relay.ws_open = false;
    relay.ws_message_active = false;

    CURLcode rc = curl_easy_perform(easy);
    if (rc != CURLE_OK) {
        if (debug_enabled())
            debug_log("mp ws relay: connect failed: %s", curl_easy_strerror(rc));
        stream_relay_reset(relay);
        return false;
    }
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 0L);

    if (!refresh_active_socket(relay)) {
        if (debug_enabled())
            debug_log("mp ws relay: failed to query active socket");
        stream_relay_reset(relay);
        return false;
    }

    bool completed = false;
    bool success = false;
    if (!websocket_progress_handshake(relay, timeout_ms, completed, success) ||
        !completed || !success) {
        if (!completed && debug_enabled())
            debug_log("mp ws relay: handshake timeout");
        stream_relay_reset(relay);
        return false;
    }
    return true;
}

bool stream_relay_poll_connect(StreamRelayConnection &relay, Uint32 timeout_ms, bool &connected) {
    connected = false;
    if (relay.mode != StreamRelayMode::WEBSOCKET || relay.ws_easy == nullptr)
        return false;
    if (relay.ws_open) {
        connected = true;
        return true;
    }
    if (!relay.ws_connecting)
        return false;

    bool completed = false;
    bool success = false;
    if (!websocket_progress_connect(relay, timeout_ms, completed, success))
        return false;
    if (completed && !success)
        return false;
    connected = relay.ws_open;
    return true;
}

bool stream_relay_wait_readable(const StreamRelayConnection &relay, Uint32 timeout_ms,
                                bool &ready) {
    return wait_for_socket(relay.socket, timeout_ms, false, ready);
}

bool stream_relay_wait_writable(const StreamRelayConnection &relay, Uint32 timeout_ms,
                                bool &ready) {
    return wait_for_socket(relay.socket, timeout_ms, true, ready);
}

bool stream_relay_finish_connect(const StreamRelayConnection &relay, bool &connected) {
    connected = false;
    if (!tcp_socket_valid(relay.socket))
        return false;

    int err = 0;
#ifdef WIN32
    int errlen = sizeof(err);
#else
    socklen_t errlen = sizeof(err);
#endif
    if (::getsockopt(relay.socket, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char *>(&err), &errlen) != 0) {
        return false;
    }
    connected = (err == 0);
    return true;
}

bool stream_relay_send_frame(StreamRelayConnection &relay, const void *data, size_t len) {
    if (!stream_relay_connected(relay) || len == 0)
        return false;
    if (relay.mode == StreamRelayMode::WEBSOCKET) {
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
        size_t offset = 0;
        while (offset < len) {
            const size_t chunk = std::min(kWebSocketFrameChunk, len - offset);
            const uint8_t opcode = (offset == 0) ? 0x2u : 0x0u;
            const bool fin = (offset + chunk == len);
            if (!websocket_send_raw_frame(relay, opcode, bytes + offset, chunk, fin))
                return false;
            offset += chunk;
        }
        return true;
    }
    return tcp_send_frame(relay.socket, data, len);
}

bool stream_relay_send_host_hello(StreamRelayConnection &relay, Uint32 session_id) {
    return send_hello(relay, RELAY_HELLO_HOST, session_id);
}

bool stream_relay_send_client_hello(StreamRelayConnection &relay, Uint32 session_id) {
    return send_hello(relay, RELAY_HELLO_CLIENT, session_id);
}

bool stream_relay_send_payload(StreamRelayConnection &relay, Uint32 session_id,
                               Uint32 client_id, const ecl::Buffer &payload) {
    return send_payload(relay, session_id, client_id, payload.data(), payload.size());
}

bool stream_relay_send_payload(StreamRelayConnection &relay, Uint32 session_id,
                               Uint32 client_id, const std::vector<uint8_t> &payload) {
    return send_payload(relay, session_id, client_id, payload.data(), payload.size());
}

bool stream_relay_pump(StreamRelayConnection &relay) {
    if (!stream_relay_connected(relay))
        return false;
    if (relay.mode == StreamRelayMode::WEBSOCKET) {
        bool got_data = false;
        if (!websocket_recv_bytes(relay, 0, got_data))
            return false;
        if (!got_data)
            return true;
        while (true) {
            bool fin = false;
            uint8_t opcode = 0;
            std::vector<uint8_t> payload;
            WebSocketParseStatus status =
                websocket_try_extract_frame(relay.rx, fin, opcode, payload);
            if (status == WebSocketParseStatus::NEED_MORE)
                break;
            if (status == WebSocketParseStatus::INVALID)
                return false;
            if (!websocket_handle_received_frame(relay, fin, opcode, payload))
                return false;
        }
        return true;
    }
    return tcp_pump_recv(relay.socket, relay.rx);
}

bool stream_relay_next_frame(StreamRelayConnection &relay, std::vector<uint8_t> &out) {
    if (relay.mode == StreamRelayMode::WEBSOCKET) {
        if (relay.frames.empty()) {
            out.clear();
            return false;
        }
        out = relay.frames.front();
        relay.frames.pop_front();
        return true;
    }
    return tcp_try_extract_frame(relay.rx, relay.frame_len, out);
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
