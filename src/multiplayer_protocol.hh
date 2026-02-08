#ifndef MULTIPLAYER_PROTOCOL_HH_INCLUDED
#define MULTIPLAYER_PROTOCOL_HH_INCLUDED

#include "ecl_buffer.hh"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace enigma {
namespace multiplayer {
namespace protocol {

constexpr Uint32 kLobbyMagic = 0x454E4C42;  // "ENLB"
constexpr Uint8 kLobbyVersion = 1;

enum LobbyMessageType : Uint8 {
    LOBBY_ANNOUNCE = 1,
    LOBBY_START = 2
};

enum NetMessageType : Uint8 {
    NET_WELCOME = 1,
    NET_INPUT = 2,
    NET_SYNC = 3,
    NET_READY = 4,
    NET_START = 5,
    NET_RESTART = 6,
    NET_RESYNC_REQUEST = 7,
    NET_RESYNC_STATE = 8,
    NET_PLACE = 9,
    NET_PAUSE = 10,
    NET_MENU = 11,
    NET_ABORT = 12
};

struct LobbyAnnounce {
    std::string id;
    std::string name;
    std::string level_id;
    Uint8 player_count;
};

struct LobbyStart {
    Uint32 session_id;
    std::string level_id;
    Uint32 seed;
    Uint8 expected_players;
    Uint16 host_port;
    std::string host_id;
    Uint8 filter_optimized = 1;
};

struct InputPacket {
    Uint32 epoch;
    Uint32 tick;
    Uint8 player;
    float mouse_x;
    float mouse_y;
    int16_t rotate_steps;
    Uint8 activate_count;
};

struct SyncPacket {
    Uint32 epoch;
    Uint32 tick;
    Uint32 random_state;
    float p0_x;
    float p0_y;
    float p1_x;
    float p1_y;
    Uint64 world_checksum;
    Uint64 actor_checksum;
};

struct ResyncRequest {
    Uint32 epoch;
    Uint32 tick;
};

struct ResyncActorState {
    Uint32 object_id;
    Uint16 actor_id;
    Uint16 owner;
    Uint32 controllers = 0;
    Uint16 color = 0xFFFF;
    // Stable identifier for resync matching when object ids diverge.
    Uint32 name_hash = 0;
    float x;
    float y;
    float vx;
    float vy;
};

struct ResyncState {
    Uint32 epoch;
    Uint32 tick;
    Uint32 random_state;
    std::vector<ResyncActorState> actors;
};

struct RestartPacket {
    Uint32 restart_id;
    Uint8 level_restart;
};

struct PlacementPacket {
    Uint32 restart_id;
    Uint8 player;
    Uint16 x;
    Uint16 y;
};

inline void encode_lobby_announce(ecl::Buffer &buf, const LobbyAnnounce &msg) {
    buf << kLobbyMagic << kLobbyVersion << Uint8(LOBBY_ANNOUNCE)
        << msg.id << msg.name << msg.level_id << Uint8(msg.player_count);
}

inline bool decode_lobby_announce(ecl::Buffer &buf, LobbyAnnounce &msg) {
    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 type = 0;
    if (!(buf >> magic >> version >> type))
        return false;
    if (magic != kLobbyMagic || type != LOBBY_ANNOUNCE)
        return false;
    Uint8 count = 0;
    if (version != kLobbyVersion)
        return false;
    if (!(buf >> msg.id >> msg.name >> msg.level_id >> count))
        return false;
    msg.player_count = count;
    return true;
}

inline void encode_lobby_start(ecl::Buffer &buf, const LobbyStart &msg) {
    buf << kLobbyMagic << kLobbyVersion << Uint8(LOBBY_START)
        << Uint32(msg.session_id) << msg.level_id << Uint32(msg.seed)
        << Uint8(msg.expected_players) << Uint16(msg.host_port) << msg.host_id
        << Uint8(msg.filter_optimized);
}

inline bool decode_lobby_start(ecl::Buffer &buf, LobbyStart &msg) {
    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 type = 0;
    Uint32 session_id = 0;
    Uint32 seed = 0;
    Uint8 expected = 0;
    Uint16 host_port = 0;
    if (!(buf >> magic >> version >> type))
        return false;
    if (magic != kLobbyMagic || version != kLobbyVersion || type != LOBBY_START)
        return false;
    if (!(buf >> session_id >> msg.level_id >> seed >> expected >> host_port >> msg.host_id))
        return false;
    msg.session_id = session_id;
    msg.seed = seed;
    msg.expected_players = expected;
    msg.host_port = host_port;
    if (!(buf >> msg.filter_optimized))
        return false;
    return true;
}

inline void encode_input(ecl::Buffer &buf, const InputPacket &msg) {
    buf << Uint8(NET_INPUT) << Uint32(msg.epoch) << Uint32(msg.tick) << Uint8(msg.player)
        << float(msg.mouse_x) << float(msg.mouse_y)
        << Uint16(static_cast<uint16_t>(msg.rotate_steps)) << Uint8(msg.activate_count);
}

inline bool decode_input(ecl::Buffer &buf, InputPacket &msg) {
    Uint8 type = 0;
    Uint32 epoch = 0;
    Uint32 tick = 0;
    Uint8 player = 0;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    Uint16 rotate_steps_raw = 0;
    Uint8 activate_count = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_INPUT)
        return false;
    if (!(buf >> epoch >> tick >> player >> mouse_x >> mouse_y >> rotate_steps_raw >> activate_count))
        return false;
    msg.epoch = epoch;
    msg.tick = tick;
    msg.player = player;
    msg.mouse_x = mouse_x;
    msg.mouse_y = mouse_y;
    msg.rotate_steps = static_cast<int16_t>(rotate_steps_raw);
    msg.activate_count = activate_count;
    return true;
}

inline void encode_welcome(ecl::Buffer &buf, Uint8 player_id, Uint8 expected_players, Uint32 seed) {
    buf << Uint8(NET_WELCOME) << Uint8(player_id) << Uint8(expected_players) << Uint32(seed);
}

inline bool decode_welcome(ecl::Buffer &buf, Uint8 &player_id, Uint8 &expected_players, Uint32 &seed) {
    Uint8 type = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_WELCOME)
        return false;
    if (!(buf >> player_id >> expected_players >> seed))
        return false;
    return true;
}

inline void encode_sync(ecl::Buffer &buf, const SyncPacket &msg) {
    buf << Uint8(NET_SYNC) << Uint32(msg.epoch) << Uint32(msg.tick) << Uint32(msg.random_state)
        << float(msg.p0_x) << float(msg.p0_y) << float(msg.p1_x) << float(msg.p1_y)
        << Uint64(msg.world_checksum) << Uint64(msg.actor_checksum);
}

inline bool decode_sync(ecl::Buffer &buf, SyncPacket &msg) {
    Uint8 type = 0;
    Uint32 epoch = 0;
    Uint32 tick = 0;
    Uint32 random_state = 0;
    float p0_x = 0.0f;
    float p0_y = 0.0f;
    float p1_x = 0.0f;
    float p1_y = 0.0f;
    Uint64 world_checksum = 0;
    Uint64 actor_checksum = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_SYNC)
        return false;
    if (!(buf >> epoch >> tick >> random_state >> p0_x >> p0_y >> p1_x >> p1_y >> world_checksum >>
          actor_checksum))
        return false;
    msg.epoch = epoch;
    msg.tick = tick;
    msg.random_state = random_state;
    msg.p0_x = p0_x;
    msg.p0_y = p0_y;
    msg.p1_x = p1_x;
    msg.p1_y = p1_y;
    msg.world_checksum = world_checksum;
    msg.actor_checksum = actor_checksum;
    return true;
}

inline void encode_resync_request(ecl::Buffer &buf, const ResyncRequest &msg) {
    buf << Uint8(NET_RESYNC_REQUEST) << Uint32(msg.epoch) << Uint32(msg.tick);
}

inline bool decode_resync_request(ecl::Buffer &buf, ResyncRequest &msg) {
    Uint8 type = 0;
    Uint32 epoch = 0;
    Uint32 tick = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_RESYNC_REQUEST)
        return false;
    if (!(buf >> epoch >> tick))
        return false;
    msg.epoch = epoch;
    msg.tick = tick;
    return true;
}

inline void encode_resync_state(ecl::Buffer &buf, const ResyncState &msg) {
    Uint16 count = static_cast<Uint16>(msg.actors.size());
    buf << Uint8(NET_RESYNC_STATE) << Uint32(msg.epoch) << Uint32(msg.tick) << Uint32(msg.random_state)
        << Uint16(count);
    for (const auto &actor : msg.actors) {
        buf << Uint32(actor.object_id) << Uint16(actor.actor_id) << Uint16(actor.owner)
            << float(actor.x) << float(actor.y) << float(actor.vx) << float(actor.vy)
            << Uint32(actor.controllers) << Uint16(actor.color) << Uint32(actor.name_hash);
    }
}

inline bool decode_resync_state(ecl::Buffer &buf, ResyncState &msg) {
    Uint8 type = 0;
    Uint32 epoch = 0;
    Uint32 tick = 0;
    Uint32 random_state = 0;
    Uint16 count = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_RESYNC_STATE)
        return false;
    if (!(buf >> epoch >> tick >> random_state >> count))
        return false;
    msg.epoch = epoch;
    msg.tick = tick;
    msg.random_state = random_state;
    msg.actors.clear();
    msg.actors.reserve(count);
    for (Uint16 i = 0; i < count; ++i) {
        ResyncActorState actor;
        Uint32 object_id = 0;
        Uint16 actor_id = 0;
        Uint16 owner = 0;
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        Uint32 controllers = 0;
        Uint16 color = 0xFFFF;
        Uint32 name_hash = 0;
        if (!(buf >> object_id >> actor_id >> owner >> x >> y >> vx >> vy >> controllers >> color >>
              name_hash))
            return false;
        actor.object_id = object_id;
        actor.actor_id = actor_id;
        actor.owner = owner;
        actor.x = x;
        actor.y = y;
        actor.vx = vx;
        actor.vy = vy;
        actor.controllers = controllers;
        actor.color = color;
        actor.name_hash = name_hash;
        msg.actors.push_back(actor);
    }
    return true;
}

inline void encode_ready(ecl::Buffer &buf) {
    buf << Uint8(NET_READY);
}

inline bool decode_ready(ecl::Buffer &buf) {
    Uint8 type = 0;
    if (!(buf >> type))
        return false;
    return type == NET_READY;
}

inline void encode_start(ecl::Buffer &buf, Uint32 epoch) {
    buf << Uint8(NET_START) << Uint32(epoch);
}

inline bool decode_start(ecl::Buffer &buf, Uint32 &epoch) {
    Uint8 type = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_START)
        return false;
    if (!(buf >> epoch))
        return false;
    return true;
}

inline void encode_pause(ecl::Buffer &buf, Uint32 epoch, bool paused) {
    buf << Uint8(NET_PAUSE) << Uint32(epoch) << Uint8(paused ? 1 : 0);
}

inline bool decode_pause(ecl::Buffer &buf, Uint32 &epoch, bool &paused) {
    Uint8 type = 0;
    Uint32 parsed_epoch = 0;
    Uint8 parsed_paused = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_PAUSE)
        return false;
    if (!(buf >> parsed_epoch >> parsed_paused))
        return false;
    epoch = parsed_epoch;
    paused = parsed_paused != 0;
    return true;
}

inline void encode_menu(ecl::Buffer &buf, Uint32 epoch, Uint8 player, bool open) {
    buf << Uint8(NET_MENU) << Uint32(epoch) << Uint8(player) << Uint8(open ? 1 : 0);
}

inline bool decode_menu(ecl::Buffer &buf, Uint32 &epoch, Uint8 &player, bool &open) {
    Uint8 type = 0;
    Uint32 parsed_epoch = 0;
    Uint8 parsed_player = 0;
    Uint8 parsed_open = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_MENU)
        return false;
    if (!(buf >> parsed_epoch >> parsed_player >> parsed_open))
        return false;
    epoch = parsed_epoch;
    player = parsed_player;
    open = parsed_open != 0;
    return true;
}

inline void encode_abort(ecl::Buffer &buf, Uint32 epoch) {
    buf << Uint8(NET_ABORT) << Uint32(epoch);
}

inline bool decode_abort(ecl::Buffer &buf, Uint32 &epoch) {
    Uint8 type = 0;
    Uint32 parsed_epoch = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_ABORT)
        return false;
    if (!(buf >> parsed_epoch))
        return false;
    epoch = parsed_epoch;
    return true;
}

inline void encode_restart(ecl::Buffer &buf, const RestartPacket &msg) {
    buf << Uint8(NET_RESTART) << Uint32(msg.restart_id) << Uint8(msg.level_restart);
}

inline bool decode_restart(ecl::Buffer &buf, RestartPacket &msg) {
    Uint8 type = 0;
    Uint32 restart_id = 0;
    Uint8 level_restart = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_RESTART)
        return false;
    if (!(buf >> restart_id >> level_restart))
        return false;
    msg.restart_id = restart_id;
    msg.level_restart = level_restart;
    return true;
}

inline void encode_place(ecl::Buffer &buf, const PlacementPacket &msg) {
    buf << Uint8(NET_PLACE) << Uint32(msg.restart_id) << Uint8(msg.player)
        << Uint16(msg.x) << Uint16(msg.y);
}

inline bool decode_place(ecl::Buffer &buf, PlacementPacket &msg) {
    Uint8 type = 0;
    Uint32 restart_id = 0;
    Uint8 player = 0;
    Uint16 x = 0;
    Uint16 y = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_PLACE)
        return false;
    if (!(buf >> restart_id >> player >> x >> y))
        return false;
    msg.restart_id = restart_id;
    msg.player = player;
    msg.x = x;
    msg.y = y;
    return true;
}

}  // namespace protocol
}  // namespace multiplayer
}  // namespace enigma

#endif
