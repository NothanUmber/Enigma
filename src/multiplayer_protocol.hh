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
    NET_ABORT = 12,
    // Host -> clients: instruct clients to load a fully-qualified level.
    // Used for level transitions (advance to next level) to avoid relying on
    // each client advancing locally (which can diverge across level packs).
    NET_LOAD_LEVEL = 13,
    // Unreliable, redundant input transport: sends a short range of sequential
    // per-tick inputs (including a few repeats) to reduce lockstep stalls under
    // packet loss/jitter.
    NET_INPUT_BUNDLE = 14,
	    // Host -> clients: authoritative world state snapshot (grid object states).
	    NET_WORLD_STATE = 15,
	    // Client -> host: request an authoritative world state snapshot.
	    NET_WORLD_STATE_REQUEST = 16,
	    // Host -> clients: host-selected debug/session settings (authoritative for the session).
	    NET_DEBUG_OPTIONS = 17,
	    // Host <-> clients: lightweight RTT probe for auto-detecting connectivity.
	    NET_PING = 18,
	    NET_PONG = 19,
	    // Client -> host -> clients: experimental client-authoritative actor state.
	    NET_OWNER_ACTOR_STATE = 20
	};

struct LobbyAnnounce {
    std::string id;
    std::string name;
    std::string level_id;
    // Optional: human-readable pack name (best-effort, used for UI/debug only).
    std::string pack_name;
    Uint8 player_count;
};

struct LobbyStart {
    Uint32 session_id;
    std::string level_id;
    // Fully qualifies `level_id` across packs (clients switch to this pack before loading).
    std::string pack_name;
    // Optional: list of host IP addresses clients can try for direct connect.
    // This helps with multi-homed hosts (VPNs, VMs, multiple NICs).
    std::vector<std::string> host_ips;
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

struct InputBundleEntry {
    float mouse_x;
    float mouse_y;
    int16_t rotate_steps;
    Uint8 activate_count;
};

struct InputBundlePacket {
    Uint32 epoch;
    Uint32 first_tick;
    Uint8 player;
    std::vector<InputBundleEntry> entries;
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
    // Optional extensions (trailing fields):
    // - `grid_kind_checksum`: floor/stone/item kinds per cell
    // - `grid_state_checksum`: external "state" attrs per cell
    Uint64 grid_kind_checksum = 0;
    Uint64 grid_state_checksum = 0;
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

struct WorldStatePacket {
    Uint32 epoch;
    Uint32 tick;
    Uint16 width;
    Uint16 height;
    std::vector<Uint16> floor_state;
    std::vector<Uint16> stone_state;
    std::vector<Uint16> item_state;
    struct MovableStone {
        Uint32 object_id;
        Uint16 x;
        Uint16 y;
    };
    // Optional extension: authoritative positions of movable stones (puzzle stones, doors, etc).
    std::vector<MovableStone> movable_stones;
    struct OxydColor {
        Uint16 x;
        Uint16 y;
        // Encodes signed `oxydcolor` as uint16_t (two's complement of int16_t).
        Uint16 color_raw;
    };
    // Optional extension: authoritative oxydcolor assignments.
    std::vector<OxydColor> oxyd_colors;
    // Optional extension: authoritative per-cell kinds.
    //
    // Some objects change their kind based on attributes (see Object::getKind()) and may
    // diverge even when a simple "state" attribute matches. When present, the receiver
    // can rebuild the grid kinds from this snapshot to force convergence.
    std::vector<std::string> kind_dict;
    std::vector<Uint16> floor_kind;
    std::vector<Uint16> stone_kind;
    std::vector<Uint16> item_kind;
    struct SemanticState {
        Uint8 layer = 0;
        Uint16 x = 0;
        Uint16 y = 0;
        Uint32 logical_state = 0;
        Uint32 flags = 0;
    };
    std::vector<SemanticState> semantic_states;
    enum SemanticFieldType : Uint8 {
        SEM_FIELD_NIL = 0,
        SEM_FIELD_BOOL = 1,
        SEM_FIELD_DOUBLE = 2,
        SEM_FIELD_STRING = 3,
    };
    struct SemanticField {
        std::string key;
        Uint8 type = SEM_FIELD_NIL;
        Uint8 bool_value = 0;
        double double_value = 0.0;
        std::string string_value;
    };
    struct SemanticFieldState {
        Uint8 layer = 0;
        Uint16 x = 0;
        Uint16 y = 0;
        std::vector<SemanticField> fields;
    };
    std::vector<SemanticFieldState> semantic_field_states;
};

struct WorldStateRequest {
    Uint32 epoch;
    Uint32 tick;
};

struct DebugOptionsPacket {
    // Packet format version (allows extending fields without breaking older builds).
    Uint8 version = 1;
    Uint16 tick_ms = 10;
    Uint32 bool_mask = 0;
    Uint16 predict_missing_mouse_ticks = 0;
    Uint16 input_delay_legacy_ticks = 0;
    Uint16 host_resync_stride_legacy_ticks = 0;
    Uint16 host_world_stride_legacy_ticks = 0;
    Uint16 rollback_keep_ticks = 0;
    Uint16 netsim_delay_ms = 0;
    Uint16 netsim_jitter_ms = 0;
    Uint8 netsim_drop_pct = 0;
    Uint8 netsim_dup_pct = 0;
};

struct OwnerActorStatePacket {
    Uint32 epoch = 0;
    Uint32 tick = 0;
    Uint8 player = 0;
    Uint32 object_id = 0;
    Uint16 actor_id = 0;
    Uint32 name_hash = 0;
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
};

struct PingPacket {
    Uint32 ping_id = 0;
};

struct PongPacket {
    Uint32 ping_id = 0;
};

struct RestartPacket {
    Uint32 restart_id;
    Uint8 level_restart;
};

struct LoadLevelPacket {
    Uint32 load_id;
    std::string pack_name;
    std::string level_id;
};

struct PlacementPacket {
    Uint32 restart_id;
    Uint8 player;
    Uint16 x;
    Uint16 y;
};

inline void encode_lobby_announce(ecl::Buffer &buf, const LobbyAnnounce &msg) {
    buf << kLobbyMagic << kLobbyVersion << Uint8(LOBBY_ANNOUNCE)
        << msg.id << msg.name << msg.level_id << Uint8(msg.player_count) << msg.pack_name;
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
    msg.pack_name.clear();
    if (buf.get_rpos() < static_cast<std::ptrdiff_t>(buf.size())) {
        if (!(buf >> msg.pack_name))
            return false;
    }
    return true;
}

inline void encode_lobby_start(ecl::Buffer &buf, const LobbyStart &msg) {
    buf << kLobbyMagic << kLobbyVersion << Uint8(LOBBY_START)
        << Uint32(msg.session_id) << msg.level_id << Uint32(msg.seed)
        << Uint8(msg.expected_players) << Uint16(msg.host_port) << msg.host_id
        << Uint8(msg.filter_optimized) << msg.pack_name;
    // Optional extension: additional host IP candidates. Older decoders ignore
    // trailing bytes, so extending the payload keeps LAN interop with older builds.
    Uint8 count = static_cast<Uint8>(std::min<size_t>(msg.host_ips.size(), 255));
    buf << count;
    for (Uint8 i = 0; i < count; ++i)
        buf << msg.host_ips[i];
}

inline void encode_world_state(ecl::Buffer &buf, const WorldStatePacket &msg) {
    buf << Uint8(NET_WORLD_STATE) << Uint32(msg.epoch) << Uint32(msg.tick)
        << Uint16(msg.width) << Uint16(msg.height);
    const Uint32 count = static_cast<Uint32>(msg.floor_state.size());
    buf << count;
    for (Uint32 i = 0; i < count; ++i)
        buf << Uint16(msg.floor_state[i]);
    for (Uint32 i = 0; i < count; ++i)
        buf << Uint16(msg.stone_state[i]);
    for (Uint32 i = 0; i < count; ++i)
        buf << Uint16(msg.item_state[i]);
    // Optional extension: movable stone positions.
    Uint16 mcount = static_cast<Uint16>(std::min<size_t>(msg.movable_stones.size(), 0xFFFF));
    buf << mcount;
    for (Uint16 i = 0; i < mcount; ++i) {
        const auto &e = msg.movable_stones[i];
        buf << Uint32(e.object_id) << Uint16(e.x) << Uint16(e.y);
    }
    // Optional extension v1: authoritative kinds.
    const bool have_kinds =
        (msg.floor_kind.size() == count && msg.stone_kind.size() == count && msg.item_kind.size() == count &&
         !msg.kind_dict.empty());
    if (have_kinds) {
        buf << Uint8(1);
        Uint16 dict_count = static_cast<Uint16>(std::min<size_t>(msg.kind_dict.size(), 0xFFFF));
        buf << dict_count;
        for (Uint16 i = 0; i < dict_count; ++i)
            buf << msg.kind_dict[i];
        for (Uint32 i = 0; i < count; ++i)
            buf << Uint16(msg.floor_kind[i]);
        for (Uint32 i = 0; i < count; ++i)
            buf << Uint16(msg.stone_kind[i]);
        for (Uint32 i = 0; i < count; ++i)
            buf << Uint16(msg.item_kind[i]);
    }
    // Optional extension v2: authoritative oxyd colors.
    if (!msg.oxyd_colors.empty()) {
        buf << Uint8(2);
        Uint16 ccount = static_cast<Uint16>(std::min<size_t>(msg.oxyd_colors.size(), 0xFFFF));
        buf << ccount;
        for (Uint16 i = 0; i < ccount; ++i) {
            const auto &e = msg.oxyd_colors[i];
            buf << Uint16(e.x) << Uint16(e.y) << Uint16(e.color_raw);
        }
    }
    if (!msg.semantic_states.empty()) {
        buf << Uint8(3);
        Uint16 scount = static_cast<Uint16>(std::min<size_t>(msg.semantic_states.size(), 0xFFFF));
        buf << scount;
        for (Uint16 i = 0; i < scount; ++i) {
            const auto &e = msg.semantic_states[i];
            buf << Uint8(e.layer) << Uint16(e.x) << Uint16(e.y)
                << Uint32(e.logical_state) << Uint32(e.flags);
        }
    }
    if (!msg.semantic_field_states.empty()) {
        buf << Uint8(4);
        Uint16 fcount =
            static_cast<Uint16>(std::min<size_t>(msg.semantic_field_states.size(), 0xFFFF));
        buf << fcount;
        for (Uint16 i = 0; i < fcount; ++i) {
            const auto &entry = msg.semantic_field_states[i];
            buf << Uint8(entry.layer) << Uint16(entry.x) << Uint16(entry.y);
            Uint16 ecount =
                static_cast<Uint16>(std::min<size_t>(entry.fields.size(), 0xFFFF));
            buf << ecount;
            for (Uint16 j = 0; j < ecount; ++j) {
                const auto &field = entry.fields[j];
                buf << field.key << Uint8(field.type);
                switch (field.type) {
                case WorldStatePacket::SEM_FIELD_NIL:
                    break;
                case WorldStatePacket::SEM_FIELD_BOOL:
                    buf << Uint8(field.bool_value);
                    break;
                case WorldStatePacket::SEM_FIELD_DOUBLE:
                    buf << field.double_value;
                    break;
                case WorldStatePacket::SEM_FIELD_STRING:
                    buf << field.string_value;
                    break;
                default:
                    break;
                }
            }
        }
    }
}

inline bool decode_world_state(ecl::Buffer &buf, WorldStatePacket &msg) {
    Uint8 type = 0;
    Uint32 epoch = 0;
    Uint32 tick = 0;
    Uint16 w = 0;
    Uint16 h = 0;
    Uint32 count = 0;
    if (!(buf >> type >> epoch >> tick >> w >> h >> count))
        return false;
    if (type != NET_WORLD_STATE)
        return false;
    if (w == 0 || h == 0)
        return false;
    const Uint32 expected = static_cast<Uint32>(w) * static_cast<Uint32>(h);
    if (count != expected)
        return false;
    msg.epoch = epoch;
    msg.tick = tick;
    msg.width = w;
    msg.height = h;
    msg.floor_state.assign(count, 0xFFFF);
    msg.stone_state.assign(count, 0xFFFF);
    msg.item_state.assign(count, 0xFFFF);
    for (Uint32 i = 0; i < count; ++i) {
        Uint16 v = 0;
        if (!(buf >> v))
            return false;
        msg.floor_state[i] = v;
    }
    for (Uint32 i = 0; i < count; ++i) {
        Uint16 v = 0;
        if (!(buf >> v))
            return false;
        msg.stone_state[i] = v;
    }
    for (Uint32 i = 0; i < count; ++i) {
        Uint16 v = 0;
        if (!(buf >> v))
            return false;
        msg.item_state[i] = v;
    }
    msg.movable_stones.clear();
    // Optional extension: movable stone positions.
    if (buf.get_rpos() < static_cast<std::ptrdiff_t>(buf.size())) {
        Uint16 mcount = 0;
        if (!(buf >> mcount))
            return false;
        msg.movable_stones.reserve(mcount);
        for (Uint16 i = 0; i < mcount; ++i) {
            WorldStatePacket::MovableStone e;
            if (!(buf >> e.object_id >> e.x >> e.y))
                return false;
            msg.movable_stones.push_back(e);
        }
    }
    msg.oxyd_colors.clear();
    msg.kind_dict.clear();
    msg.floor_kind.clear();
    msg.stone_kind.clear();
    msg.item_kind.clear();
    msg.semantic_states.clear();
    msg.semantic_field_states.clear();
    // Optional extensions: kinds (v1), oxyd colors (v2), semantic states (v3),
    // and semantic scalar fields (v4). Older decoders ignore trailing bytes.
    // Newer decoders support multiple extension blocks in sequence.
    while (buf.get_rpos() < static_cast<std::ptrdiff_t>(buf.size())) {
        const size_t rpos = static_cast<size_t>(buf.get_rpos());
        const size_t sz = static_cast<size_t>(buf.size());
        if (rpos >= sz)
            break;
        const Uint8 ext = static_cast<Uint8>(buf.data()[rpos]);
        if (ext == 1) {
            Uint8 consumed_ext = 0;
            Uint16 dict_count = 0;
            if (!(buf >> consumed_ext >> dict_count))
                return false;
            msg.kind_dict.reserve(dict_count);
            for (Uint16 i = 0; i < dict_count; ++i) {
                std::string k;
                if (!(buf >> k))
                    return false;
                msg.kind_dict.push_back(k);
            }
            msg.floor_kind.assign(count, 0);
            msg.stone_kind.assign(count, 0);
            msg.item_kind.assign(count, 0);
            for (Uint32 i = 0; i < count; ++i) {
                Uint16 v = 0;
                if (!(buf >> v))
                    return false;
                msg.floor_kind[i] = v;
            }
            for (Uint32 i = 0; i < count; ++i) {
                Uint16 v = 0;
                if (!(buf >> v))
                    return false;
                msg.stone_kind[i] = v;
            }
            for (Uint32 i = 0; i < count; ++i) {
                Uint16 v = 0;
                if (!(buf >> v))
                    return false;
                msg.item_kind[i] = v;
            }
            continue;
        }
        if (ext == 2) {
            Uint8 consumed_ext = 0;
            Uint16 ccount = 0;
            if (!(buf >> consumed_ext >> ccount))
                return false;
            msg.oxyd_colors.reserve(ccount);
            for (Uint16 i = 0; i < ccount; ++i) {
                WorldStatePacket::OxydColor e;
                if (!(buf >> e.x >> e.y >> e.color_raw))
                    return false;
                msg.oxyd_colors.push_back(e);
            }
            continue;
        }
        if (ext == 3) {
            Uint8 consumed_ext = 0;
            Uint16 scount = 0;
            if (!(buf >> consumed_ext >> scount))
                return false;
            msg.semantic_states.reserve(scount);
            for (Uint16 i = 0; i < scount; ++i) {
                WorldStatePacket::SemanticState e;
                if (!(buf >> e.layer >> e.x >> e.y >> e.logical_state >> e.flags))
                    return false;
                msg.semantic_states.push_back(e);
            }
            continue;
        }
        if (ext == 4) {
            Uint8 consumed_ext = 0;
            Uint16 fcount = 0;
            if (!(buf >> consumed_ext >> fcount))
                return false;
            msg.semantic_field_states.reserve(fcount);
            for (Uint16 i = 0; i < fcount; ++i) {
                WorldStatePacket::SemanticFieldState entry;
                Uint16 ecount = 0;
                if (!(buf >> entry.layer >> entry.x >> entry.y >> ecount))
                    return false;
                entry.fields.reserve(ecount);
                for (Uint16 j = 0; j < ecount; ++j) {
                    WorldStatePacket::SemanticField field;
                    if (!(buf >> field.key >> field.type))
                        return false;
                    switch (field.type) {
                    case WorldStatePacket::SEM_FIELD_NIL:
                        break;
                    case WorldStatePacket::SEM_FIELD_BOOL:
                        if (!(buf >> field.bool_value))
                            return false;
                        break;
                    case WorldStatePacket::SEM_FIELD_DOUBLE:
                        if (!(buf >> field.double_value))
                            return false;
                        break;
                    case WorldStatePacket::SEM_FIELD_STRING:
                        if (!(buf >> field.string_value))
                            return false;
                        break;
                    default:
                        return false;
                    }
                    entry.fields.push_back(field);
                }
                msg.semantic_field_states.push_back(entry);
            }
            continue;
        }
        // Unknown extension marker: ignore remaining bytes for forward compatibility.
        break;
    }
    return true;
}

inline void encode_world_state_request(ecl::Buffer &buf, const WorldStateRequest &msg) {
    buf << Uint8(NET_WORLD_STATE_REQUEST) << Uint32(msg.epoch) << Uint32(msg.tick);
}

inline bool decode_world_state_request(ecl::Buffer &buf, WorldStateRequest &msg) {
    Uint8 type = 0;
    Uint32 epoch = 0;
    Uint32 tick = 0;
    if (!(buf >> type >> epoch >> tick))
        return false;
    if (type != NET_WORLD_STATE_REQUEST)
        return false;
    msg.epoch = epoch;
    msg.tick = tick;
    return true;
}

inline void encode_debug_options(ecl::Buffer &buf, const DebugOptionsPacket &msg) {
    buf << Uint8(NET_DEBUG_OPTIONS) << Uint8(msg.version) << Uint16(msg.tick_ms) << Uint32(msg.bool_mask)
        << Uint16(msg.predict_missing_mouse_ticks) << Uint16(msg.input_delay_legacy_ticks)
        << Uint16(msg.host_resync_stride_legacy_ticks) << Uint16(msg.host_world_stride_legacy_ticks)
        << Uint16(msg.rollback_keep_ticks) << Uint16(msg.netsim_delay_ms) << Uint16(msg.netsim_jitter_ms)
        << Uint8(msg.netsim_drop_pct) << Uint8(msg.netsim_dup_pct);
}

inline bool decode_debug_options(ecl::Buffer &buf, DebugOptionsPacket &msg) {
    Uint8 type = 0;
    Uint8 ver = 0;
    if (!(buf >> type >> ver))
        return false;
    if (type != NET_DEBUG_OPTIONS)
        return false;
    msg.version = ver;
    if (!(buf >> msg.tick_ms >> msg.bool_mask >> msg.predict_missing_mouse_ticks >>
          msg.input_delay_legacy_ticks >> msg.host_resync_stride_legacy_ticks >>
          msg.host_world_stride_legacy_ticks >> msg.rollback_keep_ticks >> msg.netsim_delay_ms >>
          msg.netsim_jitter_ms >> msg.netsim_drop_pct >> msg.netsim_dup_pct))
        return false;
    return true;
}

inline void encode_owner_actor_state(ecl::Buffer &buf, const OwnerActorStatePacket &msg) {
    buf << Uint8(NET_OWNER_ACTOR_STATE) << Uint32(msg.epoch) << Uint32(msg.tick) << Uint8(msg.player)
        << Uint32(msg.object_id) << Uint16(msg.actor_id) << Uint32(msg.name_hash) << msg.x << msg.y << msg.vx
        << msg.vy;
}

inline bool decode_owner_actor_state(ecl::Buffer &buf, OwnerActorStatePacket &msg) {
    Uint8 type = 0;
    Uint32 epoch = 0;
    Uint32 tick = 0;
    Uint8 player = 0;
    Uint32 object_id = 0;
    Uint16 actor_id = 0;
    Uint32 name_hash = 0;
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    if (!(buf >> type >> epoch >> tick >> player >> object_id >> actor_id >> name_hash >> x >> y >> vx >> vy))
        return false;
    if (type != NET_OWNER_ACTOR_STATE)
        return false;
    msg.epoch = epoch;
    msg.tick = tick;
    msg.player = player;
    msg.object_id = object_id;
    msg.actor_id = actor_id;
    msg.name_hash = name_hash;
    msg.x = x;
    msg.y = y;
    msg.vx = vx;
    msg.vy = vy;
    return true;
}

inline void encode_ping(ecl::Buffer &buf, const PingPacket &msg) {
    buf << Uint8(NET_PING) << Uint32(msg.ping_id);
}

inline bool decode_ping(ecl::Buffer &buf, PingPacket &msg) {
    Uint8 type = 0;
    Uint32 ping_id = 0;
    if (!(buf >> type >> ping_id))
        return false;
    if (type != NET_PING)
        return false;
    msg.ping_id = ping_id;
    return true;
}

inline void encode_pong(ecl::Buffer &buf, const PongPacket &msg) {
    buf << Uint8(NET_PONG) << Uint32(msg.ping_id);
}

inline bool decode_pong(ecl::Buffer &buf, PongPacket &msg) {
    Uint8 type = 0;
    Uint32 ping_id = 0;
    if (!(buf >> type >> ping_id))
        return false;
    if (type != NET_PONG)
        return false;
    msg.ping_id = ping_id;
    return true;
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
    msg.pack_name.clear();
    msg.host_ips.clear();
    if (buf.get_rpos() < static_cast<std::ptrdiff_t>(buf.size())) {
        if (!(buf >> msg.pack_name))
            return false;
    }
    // Optional extension: host IP candidates.
    if (buf.get_rpos() < static_cast<std::ptrdiff_t>(buf.size())) {
        Uint8 count = 0;
        if (!(buf >> count))
            return false;
        for (Uint8 i = 0; i < count; ++i) {
            std::string ip;
            if (!(buf >> ip))
                return false;
            msg.host_ips.push_back(ip);
        }
    }
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

inline void encode_input_bundle(ecl::Buffer &buf, const InputBundlePacket &msg) {
    Uint8 count = static_cast<Uint8>(msg.entries.size());
    buf << Uint8(NET_INPUT_BUNDLE) << Uint32(msg.epoch) << Uint32(msg.first_tick) << Uint8(msg.player)
        << Uint8(count);
    for (const auto &e : msg.entries) {
        buf << float(e.mouse_x) << float(e.mouse_y)
            << Uint16(static_cast<uint16_t>(e.rotate_steps)) << Uint8(e.activate_count);
    }
}

inline bool decode_input_bundle(ecl::Buffer &buf, InputBundlePacket &msg) {
    Uint8 type = 0;
    Uint32 epoch = 0;
    Uint32 first_tick = 0;
    Uint8 player = 0;
    Uint8 count = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_INPUT_BUNDLE)
        return false;
    if (!(buf >> epoch >> first_tick >> player >> count))
        return false;
    msg.epoch = epoch;
    msg.first_tick = first_tick;
    msg.player = player;
    msg.entries.clear();
    msg.entries.reserve(count);
    for (Uint8 i = 0; i < count; ++i) {
        float mx = 0.0f;
        float my = 0.0f;
        Uint16 rotate_raw = 0;
        Uint8 activate = 0;
        if (!(buf >> mx >> my >> rotate_raw >> activate))
            return false;
        InputBundleEntry e;
        e.mouse_x = mx;
        e.mouse_y = my;
        e.rotate_steps = static_cast<int16_t>(rotate_raw);
        e.activate_count = activate;
        msg.entries.push_back(e);
    }
    return true;
}

	inline void encode_welcome(ecl::Buffer &buf, Uint8 player_id, Uint8 expected_players, Uint32 seed,
	                           Uint16 tick_ms) {
	    buf << Uint8(NET_WELCOME) << Uint8(player_id) << Uint8(expected_players) << Uint32(seed)
	        << Uint16(tick_ms);
	}

	inline bool decode_welcome(ecl::Buffer &buf, Uint8 &player_id, Uint8 &expected_players, Uint32 &seed,
	                           Uint16 *tick_ms = nullptr) {
	    Uint8 type = 0;
	    if (!(buf >> type))
	        return false;
	    if (type != NET_WELCOME)
	        return false;
	    if (!(buf >> player_id >> expected_players >> seed))
	        return false;
	    // Optional extension: negotiated tick duration (ms).
	    if (buf.get_rpos() < static_cast<std::ptrdiff_t>(buf.size())) {
	        Uint16 ms = 0;
	        if (!(buf >> ms))
	            return false;
	        if (tick_ms)
	            *tick_ms = ms;
	    } else if (tick_ms) {
	        *tick_ms = 0;
	    }
	    return true;
	}

inline void encode_sync(ecl::Buffer &buf, const SyncPacket &msg) {
    buf << Uint8(NET_SYNC) << Uint32(msg.epoch) << Uint32(msg.tick) << Uint32(msg.random_state)
        << float(msg.p0_x) << float(msg.p0_y) << float(msg.p1_x) << float(msg.p1_y)
        << Uint64(msg.world_checksum) << Uint64(msg.actor_checksum)
        << Uint64(msg.grid_kind_checksum) << Uint64(msg.grid_state_checksum);
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
    Uint64 grid_kind_checksum = 0;
    Uint64 grid_state_checksum = 0;
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
    // Optional extension: grid checksums.
    if (buf.get_rpos() < static_cast<std::ptrdiff_t>(buf.size())) {
        if (!(buf >> grid_kind_checksum >> grid_state_checksum))
            return false;
        msg.grid_kind_checksum = grid_kind_checksum;
        msg.grid_state_checksum = grid_state_checksum;
    } else {
        msg.grid_kind_checksum = 0;
        msg.grid_state_checksum = 0;
    }
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

inline void encode_ready(ecl::Buffer &buf, Uint32 session_id, Uint32 epoch, Uint32 load_id) {
    buf << Uint8(NET_READY) << Uint32(session_id) << Uint32(epoch) << Uint32(load_id);
}

inline bool decode_ready(ecl::Buffer &buf, Uint32 &session_id, Uint32 &epoch, Uint32 &load_id) {
    Uint8 type = 0;
    Uint32 parsed_session = 0;
    Uint32 parsed_epoch = 0;
    Uint32 parsed_load_id = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_READY)
        return false;
    if (!(buf >> parsed_session >> parsed_epoch >> parsed_load_id))
        return false;
    session_id = parsed_session;
    epoch = parsed_epoch;
    load_id = parsed_load_id;
    return true;
}

inline void encode_start(ecl::Buffer &buf, Uint32 epoch, Uint32 load_id) {
    buf << Uint8(NET_START) << Uint32(epoch) << Uint32(load_id);
}

inline bool decode_start(ecl::Buffer &buf, Uint32 &epoch, Uint32 &load_id) {
    Uint8 type = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_START)
        return false;
    if (!(buf >> epoch >> load_id))
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

inline void encode_load_level(ecl::Buffer &buf, const LoadLevelPacket &msg) {
    buf << Uint8(NET_LOAD_LEVEL) << Uint32(msg.load_id) << msg.pack_name << msg.level_id;
}

inline bool decode_load_level(ecl::Buffer &buf, LoadLevelPacket &msg) {
    Uint8 type = 0;
    Uint32 load_id = 0;
    if (!(buf >> type))
        return false;
    if (type != NET_LOAD_LEVEL)
        return false;
    if (!(buf >> load_id >> msg.pack_name >> msg.level_id))
        return false;
    msg.load_id = load_id;
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
