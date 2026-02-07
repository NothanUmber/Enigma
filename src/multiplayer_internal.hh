#ifndef MULTIPLAYER_INTERNAL_HH_INCLUDED
#define MULTIPLAYER_INTERNAL_HH_INCLUDED

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
constexpr double kPeerTimeout = 2.0;
constexpr uint32_t kInputDelay = 4;
constexpr uint32_t kMaxInputLead = 32;
constexpr double kInputTimestep = 0.01;
constexpr Uint32 kJoinTimeoutMs = 15000;
constexpr double kSyncInterval = 0.5;
constexpr double kResyncCooldown = 1.0;
constexpr unsigned kResyncMaxAttempts = 3;
constexpr size_t kChecksumHistory = 512;
constexpr float kSyncPosEpsilon = 0.05f;

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
    double last_seen;
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
    std::string pending_host_ip;
    Uint32 last_session_id = 0;
};

struct SessionState {
    bool active = false;
    bool host = false;
    bool local_player_known = false;
    unsigned local_player = 0;
    unsigned expected_players = 1;
    TransportKind active_transport = TransportKind::NONE;
    Uint32 session_id = 0;
    Uint32 seed = 0;
    Uint32 input_epoch = 0;
    Uint32 restart_id = 0;
    Uint32 last_restart_id = 0;
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
    unsigned next_player_id = 1;
    uint32_t next_local_tick = 0;
    uint32_t next_send_tick = 0;
    std::unordered_map<uint32_t, input::PlayerInput> local_history;
    uint32_t input_clock_tick = 0;
    double input_clock_accu = 0.0;
    double sync_timer = 0.0;
    bool desync_reported = false;
    bool paused = false;
    std::vector<bool> menu_open;
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
    double resync_cooldown = 0.0;
    unsigned resync_attempts = 0;
    unsigned level_players = 0;
    std::vector<bool> placement_received;
    // Tracks whether a given session player needs an additional start position
    // placement (only for extra actors we spawned). Extra players that take
    // over an existing authored actor do not need placement.
    std::vector<bool> needs_placement;
};

extern LobbyState g_lobby;
extern SessionState g_session;
extern std::string g_relay_server;
extern std::string g_tcp_relay_server;

bool debug_enabled();
bool force_relay_enabled();
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
