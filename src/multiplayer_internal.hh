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

#include <array>
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
// UI/debug override for input delay is expressed in legacy (10ms) ticks.
// This is intentionally larger than kMaxInputLead (which caps extra lead from
// clock drift) so experiments can cover high-latency links.
constexpr uint32_t kMaxInputDelayLegacyTicks = 200;
// Each NET_INPUT_BUNDLE covers a short sequential range of ticks and includes a
// resend back-window. A larger back-window improves robustness on high-jitter /
// lossy links so the host can still receive late client input ticks.
constexpr uint32_t kInputBundleBackTicksDirect = 12;
constexpr uint32_t kInputBundleBackTicksUdpRelay = 24;
constexpr uint32_t kInputBundleBackTicksTcpRelay = 12;
constexpr uint32_t kInputBundleMaxCountDirect = 32;
constexpr uint32_t kInputBundleMaxCountUdpRelay = 48;
constexpr uint32_t kInputBundleMaxCountTcpRelay = 32;
// Keep local input history long enough to cover the largest resend back-window.
constexpr uint32_t kInputHistoryKeepTicks = 256;
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
// Avoid reacting to transient world mismatches (e.g. client-side prediction of
// movable stones). Require multiple consecutive observations before requesting
// an authoritative world-state snapshot from the host.
constexpr unsigned kWorldDesyncStreakForWorldStateRequest = 2;
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

enum class StreamRelayMode {
    NONE = 0,
    TCP = 1,
    WEBSOCKET = 2
};

struct RelayRouteKey {
    HostSource source = HostSource::UDP_RELAY;
    Uint32 client_id = 0;

    bool operator==(const RelayRouteKey &other) const {
        return source == other.source && client_id == other.client_id;
    }
};

struct RelayRouteKeyHash {
    size_t operator()(const RelayRouteKey &key) const {
        return (static_cast<size_t>(key.client_id) << 2) ^
               static_cast<size_t>(key.source);
    }
};

struct RelayRemoteRoute {
    HostSource source = HostSource::UDP_RELAY;
    Uint32 client_id = 0;
    unsigned player_id = 0;
    bool ready = false;
};

#ifdef WIN32
using TcpSocket = SOCKET;
constexpr TcpSocket kInvalidTcpSocket = INVALID_SOCKET;
#else
using TcpSocket = int;
constexpr TcpSocket kInvalidTcpSocket = -1;
#endif

struct StreamRelayConnection {
    StreamRelayMode mode = StreamRelayMode::NONE;
    TcpSocket socket = kInvalidTcpSocket;
    std::vector<uint8_t> rx;
    uint32_t frame_len = 0;
    std::deque<std::vector<uint8_t>> frames;
    std::vector<uint8_t> ws_message;
    std::vector<uint8_t> ws_handshake_request;
    size_t ws_handshake_offset = 0;
    void *ws_easy = nullptr;
    void *ws_multi = nullptr;
    std::string ws_accept_value;
    bool ws_connecting = false;
    bool ws_open = false;
    bool ws_message_active = false;
};

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
		    // Simulation tick duration in milliseconds. This must match across all peers.
		    Uint16 tick_ms = 10;
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
	    // Current level selection for this session (from lobby start or host load broadcasts).
	    // For hosts this is what we announce to joining peers via NET_LOAD_LEVEL.
	    std::string level_pack_name;
	    std::string level_id;
	    ENetHost *host_handle = nullptr;
	    ENetPeer *server_peer = nullptr;
	    ENetHost *relay_handle = nullptr;
	    ENetPeer *relay_peer = nullptr;
	    std::unordered_map<ENetPeer *, unsigned> peer_players;
    std::unordered_map<ENetPeer *, bool> peer_ready;
    StreamRelayConnection stream_relay;
    std::unordered_map<RelayRouteKey, RelayRemoteRoute, RelayRouteKeyHash> relay_routes;
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
    // Tracks late (clamped) mouse samples so out-of-order delivery doesn't cause
    // "random kicks" by overwriting a newer sample with an older one.
    std::array<bool, input::kMaxPlayers> late_mouse_valid = {};
    std::array<uint32_t, input::kMaxPlayers> late_mouse_applied_tick = {};
    std::array<uint32_t, input::kMaxPlayers> late_mouse_src_tick = {};
    uint32_t input_clock_tick = 0;
    double input_clock_accu = 0.0;
    bool input_clock_frozen = false;
    bool client_desync_hold = false;
    double sync_timer = 0.0;
    uint32_t last_host_resync_broadcast_tick = UINT32_MAX;
    uint32_t last_host_world_state_broadcast_tick = UINT32_MAX;
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
		    // While waiting for READY, periodically re-announce the current level load
		    // to handle packet loss / late joins robustly.
		    double load_announce_timer = 0.0;
		    // Host-side connectivity auto-detect (optional).
		    //
		    // When enabled, the host probes RTT to each client before starting the
		    // level and selects one of the connectivity presets based on the worst
		    // observed link.
		    bool auto_detect_active = false;
		    bool auto_detect_done = false;
		    Uint32 auto_detect_next_ping_id = 1;
		    Uint32 auto_detect_next_send_ms = 0;
		    Uint32 auto_detect_end_ms = 0;
		    int auto_detect_selected_preset = -1;  // 0=good,1=mediocre,2=bad
		    std::vector<std::unordered_map<Uint32, Uint32>> auto_detect_inflight_ms;
		    std::vector<std::vector<Uint32>> auto_detect_rtts_ms;
		    std::vector<Uint32> auto_detect_sent;
		    std::vector<Uint32> auto_detect_recv;
		    // Host-side runtime RTT probes (player-indexed). Updated continuously
		    // while a session runs so the in-game overlay can show per-player latency.
		    Uint32 runtime_latency_next_ping_id = 1;
		    Uint32 runtime_latency_next_send_ms = 0;
		    std::vector<std::unordered_map<Uint32, Uint32>> runtime_latency_inflight_ms;
		    std::vector<Uint32> runtime_latency_rtt_ms;
		    std::vector<bool> runtime_latency_valid;
		    // Periodic host -> clients propagation of debug options while RUNNING.
		    // This keeps runtime-mutable MP settings synchronized without restart.
		    double debug_options_broadcast_timer = 0.0;
		    bool has_pending_sync = false;
		    protocol::SyncPacket pending_sync;
    struct ChecksumSample {
        uint32_t tick = 0;
        uint64_t world_checksum = 0;
        uint64_t grid_kind_checksum = 0;
        uint64_t grid_state_checksum = 0;
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
    // Track latest accepted resync snapshot tick to ignore out-of-order delivery
    // (e.g. unreliable resync broadcasts can arrive reordered and would otherwise
    // cause visible "backwards" jumps).
    uint32_t last_accepted_resync_tick = 0;
    // Track latest accepted world-state snapshot tick to ignore out-of-order delivery
    // (e.g. unreliable world-state broadcasts can arrive reordered and would otherwise
    // cause visible "backwards" jumps for movable stones like puzzle stones).
    uint32_t last_accepted_world_state_tick = UINT32_MAX;
    // Track latest accepted client-authoritative actor-state per player.
    std::array<uint32_t, input::kMaxPlayers> last_accepted_owner_state_tick = {};
    // Client-side send gate: avoid spamming owner-state packets every frame for the same tick.
    uint32_t last_sent_owner_state_tick = 0;
    bool resync_inflight = false;
    double resync_inflight_timer = 0.0;
    double resync_cooldown = 0.0;
    double world_state_cooldown = 0.0;
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

        uint64_t world_state_packets_recv = 0;
        uint64_t world_state_state_applied = 0;
        uint64_t world_state_semantic_applied = 0;
    };
    Telemetry telemetry;
};

extern LobbyState g_lobby;
extern SessionState g_session;
extern std::string g_relay_server;
extern std::string g_tcp_relay_server;
extern std::string g_websocket_relay_url;

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

bool stream_relay_connected(const StreamRelayConnection &relay);
bool stream_relay_is_websocket(const StreamRelayConnection &relay);
TransportKind stream_relay_transport_kind(const StreamRelayConnection &relay);
TcpSocket stream_relay_native_socket(const StreamRelayConnection &relay);
void stream_relay_reset(StreamRelayConnection &relay);
void stream_relay_adopt_socket(StreamRelayConnection &relay, TcpSocket socket);
bool stream_relay_connect_timeout(const std::string &host, Uint16 port, Uint32 timeout_ms,
                                  StreamRelayConnection &relay);
bool stream_relay_begin_websocket_connect(const std::string &url, Uint32 timeout_ms,
                                          StreamRelayConnection &relay);
bool stream_relay_connect_websocket_timeout(const std::string &url, Uint32 timeout_ms,
                                            StreamRelayConnection &relay);
bool stream_relay_poll_connect(StreamRelayConnection &relay, Uint32 timeout_ms, bool &connected);
bool stream_relay_wait_readable(const StreamRelayConnection &relay, Uint32 timeout_ms,
                                bool &ready);
bool stream_relay_wait_writable(const StreamRelayConnection &relay, Uint32 timeout_ms,
                                bool &ready);
bool stream_relay_finish_connect(const StreamRelayConnection &relay, bool &connected);
bool stream_relay_send_frame(StreamRelayConnection &relay, const void *data, size_t len);
bool stream_relay_send_host_hello(StreamRelayConnection &relay, Uint32 session_id);
bool stream_relay_send_client_hello(StreamRelayConnection &relay, Uint32 session_id);
bool stream_relay_send_payload(StreamRelayConnection &relay, Uint32 session_id,
                               Uint32 client_id, const ecl::Buffer &payload);
bool stream_relay_send_payload(StreamRelayConnection &relay, Uint32 session_id,
                               Uint32 client_id, const std::vector<uint8_t> &payload);
bool stream_relay_pump(StreamRelayConnection &relay);
bool stream_relay_next_frame(StreamRelayConnection &relay, std::vector<uint8_t> &out);

bool host_source_is_relay(HostSource source);
bool transport_uses_stream_relay(TransportKind transport);
TransportKind transport_kind_for_host_source(HostSource source);
RelayRouteKey make_relay_route_key(HostSource source, Uint32 client_id);
RelayRemoteRoute *find_relay_route(SessionState &session, HostSource source, Uint32 client_id);
const RelayRemoteRoute *find_relay_route(const SessionState &session, HostSource source,
                                         Uint32 client_id);
bool lookup_relay_player(const SessionState &session, HostSource source, Uint32 client_id,
                         unsigned &player_id);
void upsert_relay_route(SessionState &session, HostSource source, Uint32 client_id,
                        unsigned player_id, bool ready);
bool remove_relay_route(SessionState &session, HostSource source, Uint32 client_id,
                        unsigned *player_id);
bool relay_route_ready(const SessionState &session, HostSource source, Uint32 client_id);
void set_relay_route_ready(SessionState &session, HostSource source, Uint32 client_id, bool ready);
unsigned relay_route_count(const SessionState &session, HostSource source);
unsigned relay_ready_count(const SessionState &session, HostSource source);

template <typename Fn>
void for_each_relay_route(const SessionState &session, HostSource source, Fn fn) {
    for (const auto &entry : session.relay_routes) {
        const RelayRemoteRoute &route = entry.second;
        if (route.source != source)
            continue;
        fn(route);
    }
}

void encode_relay_header(ecl::Buffer &buf, RelayMessageType type, Uint32 session_id,
                         Uint32 client_id);
bool decode_relay_header(const char *data, size_t len, RelayMessageType &type,
                         Uint32 &session_id, Uint32 &client_id,
                         const char *&payload, size_t &payload_len);

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma

#endif
