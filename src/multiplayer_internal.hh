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

#ifndef MULTIPLAYER_INTERNAL_HH_INCLUDED
#define MULTIPLAYER_INTERNAL_HH_INCLUDED

/* -------------------- Multiplayer internal state -------------------- */
/*
 * Internal multiplayer types, constants, and global state.
 *
 * This header is private to the multiplayer implementation and should not be
 * included by non-multiplayer code.
 */

#include "enet_ver.hh"
#include "input.hh"
#include "multiplayer.hh"

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef WIN32
#include <winsock2.h>
#endif

namespace enigma {
namespace multiplayer {
namespace internal {

constexpr Uint16 kLobbyPort = 12346;
constexpr Uint16 kGamePort = 12345;
constexpr Uint16 kInternetLobbyPort = 12347;
constexpr double kAnnounceInterval = 0.5;
// Drop peers from the LAN lobby after a short absence of announces. Keep this
// tolerant enough for WiFi/VM broadcast loss so the UI doesn't flicker.
constexpr double kPeerTimeout = 5.0;
constexpr uint32_t kInputDelay = 4;
// TCP relay adds latency and jitter compared to direct/UDP. Use a larger input
// delay to reduce "missing input" situations that otherwise force frequent
// resyncs (or eventually a restart prompt).
constexpr uint32_t kInputDelayTcpRelay = 10;
constexpr uint32_t kMaxInputLead = 32;
constexpr double kInputTimestep = 0.01;
constexpr Uint32 kJoinTimeoutMs = 15000;
// Allow extra time for direct connect handshakes while the host is still
// transitioning/loading the level and not pumping ENet yet (common on slower
// machines / VMs). ENet server replies are application-driven.
// Direct ENet connect can be slow to fail on some networks. For Internet play we
// prefer a fast fallback to relays, while LAN play can tolerate a longer window
// (clients may still be switching packs / loading after receiving START).
constexpr Uint32 kDirectConnectTimeoutMsLan = 12000;
constexpr Uint32 kDirectConnectTimeoutMsInternet = 1200;
constexpr double kSyncInterval = 0.5;
constexpr double kResyncCooldown = 1.0;
// If a resync request is in-flight for too long (packet loss, transport stall),
// clear the in-flight flag so we can retry. Without this, a single lost resync
// response can permanently disable recovery.
constexpr double kResyncInflightTimeout = 2.5;
// When aborting a session, keep the transport alive briefly so reliable abort
// packets have a chance to reach peers (avoids prolonged desync spam on clients
// if the host shuts down immediately).
constexpr double kAbortDeliveryGrace = 0.25;
// Legacy: client-side "no payload" disconnect timeout used to abort quickly
// when the host vanished without sending a reliable abort. We now handle
// transport stalls via a UI countdown and explicit abort request.
[[maybe_unused]] constexpr double kClientNoPayloadDisconnectTimeout = 2.0;
// Require at least N consecutive mismatch observations before triggering a resync
// request. This reduces "jumpy mode" when a single late/out-of-order sync sample
// briefly disagrees but the world would converge again naturally.
constexpr unsigned kDesyncStreakForResync = 2;
// Actor checksum mismatches can be caused by platform drift. Only trigger a
// resync from actor checksums when the mismatch persists for multiple sync
// intervals.
constexpr unsigned kActorDesyncStreakForResync = 6;
constexpr unsigned kResyncMaxAttempts = 3;
constexpr size_t kChecksumHistory = 512;
// Position drift tolerance (in tile units) for sync packets. The simulation is
// not bit-identical across platforms, especially in physics-heavy levels (e.g.
// rubberbands/meditation pearls). Keep this loose enough to avoid constant
// resync churn, while still catching meaningful divergence.
constexpr float kSyncPosEpsilon = 0.10f;

constexpr Uint32 kInternetMagic = 0x52494E45;  // "ENIR"
constexpr Uint8 kInternetVersion = 1;
constexpr Uint32 kRelayMagic = 0x4C524E45;  // "ENRL"
constexpr Uint8 kRelayVersion = 1;

enum InternetMessageType : Uint8 {
    INET_CREATE = 1,
    INET_JOIN = 2,
    INET_LEAVE = 3,
    INET_START = 4,
    INET_POLL = 5,
    INET_CREATE_OK = 101,
    INET_JOIN_OK = 102,
    INET_POLL_OK = 103,
    INET_START_OK = 104,
    INET_ERROR = 105
};

enum RelayMessageType : Uint8 {
    RELAY_HELLO_HOST = 1,
    RELAY_HELLO_CLIENT = 2,
    RELAY_CLIENT_CONNECT = 3,
    RELAY_CLIENT_DISCONNECT = 4,
    RELAY_SEND = 5,
    RELAY_DATA = 6,
    RELAY_ERROR = 7
};

enum class HostSource {
    DIRECT = 0,
    UDP_RELAY = 1,
    TCP_RELAY = 2
};

#ifdef WIN32
using TcpSocket = SOCKET;
constexpr TcpSocket kInvalidTcpSocket = INVALID_SOCKET;
#else
using TcpSocket = int;
constexpr TcpSocket kInvalidTcpSocket = -1;
#endif

struct LobbyPeerEntry {
    LobbyPeer peer;
    ENetAddress addr = {0, 0};  // last seen source address (for unicast announces)
    double last_seen = 0.0;
    double last_unicast_sent = 0.0;
};

struct LobbyState {
    bool active = false;
    ENetSocket socket = ENET_SOCKET_NULL;
    double time = 0.0;
    double announce_timer = 0.0;
    std::string local_id;
    std::string local_name;
    std::string selected_level;
    std::unordered_map<std::string, LobbyPeerEntry> peers;
    bool has_pending_start = false;
    protocol::LobbyStart pending_start;
    // Candidate host IPs to try for direct connect (multi-homed hosts, VMs).
    // The first entry is the preferred address; additional entries are fallbacks.
    std::vector<std::string> pending_host_ips;
    Uint32 last_session_id = 0;
};

struct SessionState {
    bool active = false;
    bool host = false;
    bool local_player_known = false;
    unsigned local_player = 0;
    unsigned expected_players = 1;
    TransportKind active_transport = TransportKind::NONE;
    uint32_t input_delay = kInputDelay;
    Uint32 session_id = 0;
    Uint32 seed = 0;
    Uint32 input_epoch = 0;
    Uint32 restart_id = 0;
    Uint32 last_restart_id = 0;
    // Host-incrementing id for level loads (advance to next level). Used by clients to
    // ignore duplicates/out-of-order load notifications.
    Uint32 load_id = 0;
    Uint32 last_load_id = 0;
    ENetHost *host_handle = nullptr;
    ENetPeer *server_peer = nullptr;
    ENetHost *relay_handle = nullptr;
    ENetPeer *relay_peer = nullptr;
    std::unordered_map<ENetPeer *, unsigned> peer_players;
    std::unordered_map<ENetPeer *, bool> peer_ready;
    std::unordered_map<Uint32, unsigned> relay_players;
    std::unordered_map<Uint32, bool> relay_ready;
    TcpSocket tcp_relay_socket = kInvalidTcpSocket;
    std::vector<uint8_t> tcp_relay_rx;
    uint32_t tcp_relay_frame_len = 0;
    std::unordered_map<Uint32, unsigned> tcp_relay_players;
    std::unordered_map<Uint32, bool> tcp_relay_ready;
    // Host-side player id allocation.
    //
    // We deliberately do not assume that direct connects succeed bidirectionally:
    // on some LAN/VM setups the host can observe an ENet CONNECT event while the
    // client never receives WELCOME. To allow retries (and to avoid getting
    // stuck forever in "waiting for players"), we allocate player ids from a
    // reusable pool and can replace unready peers during the pre-start phase.
    std::vector<bool> player_in_use;
    uint32_t next_local_tick = 0;
    uint32_t next_send_tick = 0;
    std::unordered_map<uint32_t, input::PlayerInput> local_history;
    uint32_t input_clock_tick = 0;
    double input_clock_accu = 0.0;
    bool input_clock_frozen = false;
    double sync_timer = 0.0;
    bool desync_reported = false;
    bool paused = false;
    std::vector<bool> menu_open;
    // Time since last received transport payload (reset on any packet).
    double no_payload_timer = 0.0;
    // Local abort grace window to deliver NET_ABORT reliably.
    bool abort_pending = false;
    double abort_timer = 0.0;
    std::string abort_message;
    // Session lifecycle phase for start/restart transitions.
    //
    // Invariants:
    // - WAITING_FOR_START: level is loaded or loading; server may call Msg_StartGame() which will be deferred.
    // - WAITING_FOR_READY: server requested start; host waits for all peers to be ready (and placements if needed).
    // - READY_TO_START: start conditions are satisfied; Msg_StartGame() must not be deferred anymore.
    // - RUNNING: game is running (until the next restart/next-level resets the phase).
    enum class Phase {
        WAITING_FOR_START = 0,
        WAITING_FOR_READY = 1,
        READY_TO_START = 2,
        RUNNING = 3
    };
    Phase phase = Phase::WAITING_FOR_START;

    bool local_ready_sent = false;
    double ready_timer = 0.0;
    bool has_pending_sync = false;
    protocol::SyncPacket pending_sync;
    struct ChecksumSample {
        uint32_t tick = 0;
        uint64_t world_checksum = 0;
        uint64_t actor_checksum = 0;
        uint32_t random_state = 0;
        float p0_x = 0.0f;
        float p0_y = 0.0f;
        float p1_x = 0.0f;
        float p1_y = 0.0f;
        bool world_valid = false;
    };
    std::deque<ChecksumSample> checksum_history;
    uint32_t last_checksum_tick = UINT32_MAX;
    uint64_t last_world_checksum = 0;
    bool resync_inflight = false;
    double resync_inflight_timer = 0.0;
    double resync_cooldown = 0.0;
    unsigned resync_attempts = 0;
    unsigned desync_streak = 0;
    unsigned actor_desync_streak = 0;
    unsigned world_only_desync_streak = 0;
    unsigned level_players = 0;
    std::vector<bool> placement_received;
    // Tracks whether a given session player needs an additional start position
    // placement (only for extra actors we spawned). Extra players that take
    // over an existing authored actor do not need placement.
    std::vector<bool> needs_placement;
    int last_defer_start_log = -1;
    // When ENIGMA_MP_DUMP_STATE is enabled, we dump a deterministic actor digest
    // once per epoch on the first observed desync. This helps diagnose whether
    // a mismatch is present at start or introduced by simulation drift.
    bool debug_state_dumped = false;

    // Lightweight counters to understand how often desync signals occur and
    // whether they are expected to auto-heal via soft resync. Intended for
    // telemetry via ENIGMA_MP_DEBUG logs, not gameplay logic.
    struct Telemetry {
        uint64_t sync_current_total = 0;
        uint64_t sync_sample_total = 0;

        uint64_t mismatch_pos = 0;
        uint64_t mismatch_rand = 0;
        uint64_t mismatch_actor = 0;
        uint64_t mismatch_world = 0;

        // Actor/world checksum mismatch without position/RNG mismatch.
        uint64_t mismatch_diagnostic_only = 0;
        // RNG mismatch without any other mismatch (fixed by syncing RNG only).
        uint64_t mismatch_rng_only = 0;
        // Position and/or RNG mismatch (eligible for soft resync).
        uint64_t mismatch_soft_resync_candidate = 0;

        uint64_t rng_resync_applied = 0;

        uint64_t resync_requests_sent = 0;
        uint64_t resync_responses_recv = 0;
        uint64_t resync_applied = 0;
        uint64_t resync_inflight_timeouts = 0;
        uint64_t resync_giveups = 0;
    };
    Telemetry telemetry;
};

extern LobbyState g_lobby;
extern SessionState g_session;
extern std::string g_relay_server;
extern std::string g_tcp_relay_server;

bool debug_enabled();
bool force_relay_enabled();
bool dump_state_enabled();
void debug_log(const char *fmt, ...);

std::string address_to_ip_string(const ENetAddress &addr);

// Local identity used for lobby and internet rooms.
std::string make_id();
std::string resolve_local_name();
void ensure_lobby_identity();

// Networking helpers implemented in dedicated translation units to keep the
// session/lobby logic files focused.
ENetSocket enet_socket_create_compat(ENetSocketType type);
bool bind_lobby_socket(ENetSocket socket, Uint16 port);
void tune_enet_socket(ENetSocket socket, bool broadcast);

bool parse_host_port(const std::string &value, std::string &host, Uint16 &port,
                     Uint16 default_port);
inline bool parse_host_port(const std::string &value, std::string &host, Uint16 &port) {
    return parse_host_port(value, host, port, kInternetLobbyPort);
}

bool tcp_socket_valid(TcpSocket s);
void tcp_close(TcpSocket &s);
bool tcp_set_nonblocking(TcpSocket s);
bool tcp_send_frame(TcpSocket s, const void *data, size_t len);
bool tcp_pump_recv(TcpSocket s, std::vector<uint8_t> &rx);
bool tcp_try_extract_frame(std::vector<uint8_t> &rx, uint32_t &pending_len,
                           std::vector<uint8_t> &out);
bool tcp_connect_timeout(const std::string &host, Uint16 port, Uint32 timeout_ms,
                         TcpSocket &out);

void encode_relay_header(ecl::Buffer &buf, RelayMessageType type, Uint32 session_id,
                         Uint32 client_id);
bool decode_relay_header(const char *data, size_t len, RelayMessageType &type,
                         Uint32 &session_id, Uint32 &client_id,
                         const char *&payload, size_t &payload_len);

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma

#endif
