#include "multiplayer_script_recorder.hh"

#include "main.hh"
#include "multiplayer.hh"
#include "multiplayer_internal.hh"
#include "options.hh"
#include "player.hh"
#include "server.hh"
#include "items/ShogunDot.hh"
#include "stones/OxydStone.hh"
#include "world.hh"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#ifdef WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace enigma {
namespace multiplayer {
namespace setupsnapshot {

namespace {

static const char *kSnapshotNone = "-";

template <typename T>
std::string to_token(const T &value) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(6);
    os << value;
    return os.str();
}

std::string kind_or_none(Object *obj) {
    return obj ? obj->getKind() : kSnapshotNone;
}

int state_or_zero(Object *obj) {
    if (!obj)
        return 0;
    return obj->MpCaptureStateForSnapshot();
}

bool restore_layer_object(const GridPos &pos, GridLayer layer, const std::string &want_kind, int want_state) {
    const bool want_none = want_kind.empty() || want_kind == kSnapshotNone;
    Object *current = GetObject(GridLoc(layer, pos));
    const std::string current_kind = current ? current->getKind() : std::string(kSnapshotNone);
    if (want_none) {
        if (current) {
            switch (layer) {
            case GRID_FLOOR: KillFloor(pos); break;
            case GRID_ITEMS: KillItem(pos); break;
            case GRID_STONES: KillStone(pos); break;
            default: break;
            }
        }
        return true;
    }

    if (!current || current_kind != want_kind) {
        switch (layer) {
        case GRID_FLOOR:
            KillFloor(pos);
            SetFloor(pos, MakeFloor(want_kind.c_str()));
            break;
        case GRID_ITEMS:
            KillItem(pos);
            SetItem(pos, MakeItem(want_kind.c_str()));
            break;
        case GRID_STONES:
            KillStone(pos);
            SetStone(pos, MakeStone(want_kind.c_str()));
            break;
        default:
            break;
        }
        current = GetObject(GridLoc(layer, pos));
    }

    if (!current)
        return false;
    if (Stone *st = dynamic_cast<Stone *>(current)) {
        if (OxydStone *ox = dynamic_cast<OxydStone *>(st)) {
            ox->MpForceExternalState(want_state);
            return true;
        }
    }
    if (!current->MpRestoreStateForSnapshot(want_state))
        current->setAttr("state", Value(want_state));
    return true;
}

}  // namespace

Snapshot Capture() {
    Snapshot snapshot;
    const int width = Width();
    const int height = Height();
    snapshot.cells.reserve(static_cast<size_t>(std::max(0, width) * std::max(0, height)));

    const unsigned players = std::max<unsigned>(1, player::PlayerCount());
    for (unsigned player_index = 0; player_index < players; ++player_index) {
        Actor *actor = player::GetMainActor(player_index);
        if (!actor)
            continue;
        const ActorInfo *ai = actor->get_actorinfo();
        ActorState actor_state;
        actor_state.player = player_index;
        actor_state.x = ai->pos[0];
        actor_state.y = ai->pos[1];
        actor_state.vx = ai->vel[0];
        actor_state.vy = ai->vel[1];
        snapshot.actors.push_back(actor_state);
    }

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const GridPos pos(x, y);
            CellState cell;
            cell.x = x;
            cell.y = y;
            cell.floor_kind = kind_or_none(GetFloor(pos));
            cell.floor_state = state_or_zero(GetFloor(pos));
            cell.item_kind = kind_or_none(GetItem(pos));
            cell.item_state = state_or_zero(GetItem(pos));
            cell.stone_kind = kind_or_none(GetStone(pos));
            cell.stone_state = state_or_zero(GetStone(pos));
            snapshot.cells.push_back(cell);
        }
    }

    return snapshot;
}

bool SaveToFile(const Snapshot &snapshot, const std::string &path) {
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out)
        return false;

    out << "MPSSETUP 1\n";
    out << "ACTORS " << snapshot.actors.size() << "\n";
    for (const ActorState &actor : snapshot.actors) {
        out << "A " << actor.player
            << " " << to_token(actor.x)
            << " " << to_token(actor.y)
            << " " << to_token(actor.vx)
            << " " << to_token(actor.vy) << "\n";
    }
    out << "CELLS " << snapshot.cells.size() << "\n";
    for (const CellState &cell : snapshot.cells) {
        out << "C " << cell.x
            << " " << cell.y
            << " " << cell.floor_kind
            << " " << cell.floor_state
            << " " << cell.item_kind
            << " " << cell.item_state
            << " " << cell.stone_kind
            << " " << cell.stone_state << "\n";
    }
    return static_cast<bool>(out);
}

bool LoadFromFile(const std::string &path, Snapshot &snapshot) {
    std::ifstream in(path.c_str());
    if (!in)
        return false;

    std::string magic;
    int version = 0;
    if (!(in >> magic >> version) || magic != "MPSSETUP" || version != 1)
        return false;

    std::string actors_label;
    size_t actors_count = 0;
    if (!(in >> actors_label >> actors_count) || actors_label != "ACTORS")
        return false;
    snapshot = Snapshot();
    snapshot.actors.reserve(actors_count);
    for (size_t i = 0; i < actors_count; ++i) {
        char tag = 0;
        ActorState actor;
        if (!(in >> tag >> actor.player >> actor.x >> actor.y >> actor.vx >> actor.vy) || tag != 'A')
            return false;
        snapshot.actors.push_back(actor);
    }

    std::string cells_label;
    size_t cells_count = 0;
    if (!(in >> cells_label >> cells_count) || cells_label != "CELLS")
        return false;
    snapshot.cells.reserve(cells_count);
    for (size_t i = 0; i < cells_count; ++i) {
        char tag = 0;
        CellState cell;
        if (!(in >> tag >> cell.x >> cell.y
                 >> cell.floor_kind >> cell.floor_state
                 >> cell.item_kind >> cell.item_state
                 >> cell.stone_kind >> cell.stone_state) || tag != 'C')
            return false;
        snapshot.cells.push_back(cell);
    }
    return true;
}

bool Restore(const Snapshot &snapshot) {
    struct WorldChangeNotificationGuard {
        WorldChangeNotificationGuard() { SetSuppressWorldChangeNotifications(true); }
        ~WorldChangeNotificationGuard() { SetSuppressWorldChangeNotifications(false); }
    } guard;

    for (const CellState &cell : snapshot.cells)
        restore_layer_object(GridPos(cell.x, cell.y), GRID_FLOOR, cell.floor_kind, cell.floor_state);
    for (const CellState &cell : snapshot.cells)
        restore_layer_object(GridPos(cell.x, cell.y), GRID_STONES, cell.stone_kind, cell.stone_state);
    for (const CellState &cell : snapshot.cells)
        restore_layer_object(GridPos(cell.x, cell.y), GRID_ITEMS, cell.item_kind, cell.item_state);

    for (const ActorState &actor_state : snapshot.actors) {
        Actor *actor = player::GetMainActor(actor_state.player);
        if (!actor)
            continue;
        ActorInfo *ai = actor->get_actorinfo();
        ai->pos = ecl::V2(actor_state.x, actor_state.y);
        DidMoveActor(actor);
        ai->last_gridpos = ai->gridpos;
        ai->vel = ecl::V2(actor_state.vx, actor_state.vy);
        ai->frozen_vel = ecl::V2(0, 0);
        ai->forceacc = ecl::V2(0, 0);
        ai->pos_force = ecl::V2(0, 0);
        ai->force = ecl::V2(0, 0);
        ai->collforce = ecl::V2(0, 0);
        ai->render_pos = ai->pos;
        ai->render_initialized = true;
    }

    for (int y = 0; y < Height(); ++y) {
        for (int x = 0; x < Width(); ++x) {
            const GridPos pos(x, y);
            ShogunDot *dot = dynamic_cast<ShogunDot *>(GetItem(pos));
            if (!dot)
                continue;
            dot->MpRestoreStateForSnapshot(0);
            dot->stone_change(GetStone(pos));
        }
    }

    return true;
}

}  // namespace setupsnapshot

namespace scriptrecorder {

namespace {

uint32_t recorder_tick_base() {
    if (multiplayer::IsActive() && internal::g_session.active && internal::g_session.local_player_known &&
        internal::g_session.next_local_tick != 0) {
        return internal::g_session.next_local_tick;
    }
    return input::CurrentTick();
}

struct RecorderState {
    bool recording = false;
    uint32_t base_tick = 0;
    unsigned local_player = 0;
    bool host = true;
    std::string output_path;
    std::string status_message;
    std::vector<std::string> header_lines;
    struct RecordedInput {
        uint32_t tick = 0;
        unsigned player = 0;
        input::PlayerInput value;
    };
    std::vector<RecordedInput> inputs;
    struct SetupSnapshot {
        bool valid = false;
        uint32_t tick = 0;
        setupsnapshot::Snapshot snapshot;
        std::string file_path;
    } setup_snapshot;
};

RecorderState g_recorder;

void ensure_local_dir() {
#ifdef WIN32
    _mkdir("local");
#else
    mkdir("local", 0777);
#endif
}

void append_bool_option(const char *key) {
    std::ostringstream os;
    os << "both SET_BOOL key=" << key << " value=" << (options::GetBool(key) ? 1 : 0);
    g_recorder.header_lines.push_back(os.str());
}

void append_int_option(const char *key) {
    std::ostringstream os;
    os << "both SET_INT key=" << key << " value=" << options::GetInt(key);
    g_recorder.header_lines.push_back(os.str());
}

std::string current_level_id() {
    if (!internal::g_session.level_id.empty())
        return internal::g_session.level_id;
    if (server::LoadedProxy)
        return server::LoadedProxy->getNormLevelPath();
    return std::string();
}

void build_script_header() {
    g_recorder.header_lines.clear();
    g_recorder.header_lines.push_back("# Recorded by the in-game multiplayer script recorder.");
    g_recorder.header_lines.push_back("# Use Shift+F11 while recording to refresh the setup snapshot.");
    append_bool_option("MultiplayerAutoDetectConnectivity");
    append_bool_option("MultiplayerDebugVisualPrediction");
    append_bool_option("MultiplayerDebugZeroFillInputs");
    append_bool_option("MultiplayerDebugNetSimEnabled");
    append_bool_option("MultiplayerDebugNetSimAll");
    append_int_option("MultiplayerDebugPredictMissingMouseTicks");
    append_int_option("MultiplayerDebugInputDelayTicks");
    append_int_option("MultiplayerDebugTickLengthMs");
    append_int_option("MultiplayerDebugNetSimDelayMs");
    append_int_option("MultiplayerDebugNetSimJitterMs");
    append_int_option("MultiplayerDebugNetSimDropPct");
    append_int_option("MultiplayerDebugNetSimDupPct");

    const Uint32 seed = internal::g_session.seed;
    const unsigned expected = internal::g_session.expected_players ? internal::g_session.expected_players : 2;
    const std::string level_id = current_level_id();

    g_recorder.header_lines.push_back("host START_HOST session=1 seed=" + std::to_string(seed) +
                                      " expected=" + std::to_string(expected) +
                                      " port=23470 host_id=mptest-host");
    g_recorder.header_lines.push_back("client START_CLIENT session=1 seed=" + std::to_string(seed) +
                                      " expected=" + std::to_string(expected) +
                                      " port=23470 host_ip=127.0.0.1 host_id=mptest-host");
    g_recorder.header_lines.push_back("wait role=client contains=\"EVT name=JOINED\" timeout_ms=15000");
    if (!level_id.empty())
        g_recorder.header_lines.push_back("host LOAD_LEVEL level_id=\"" + level_id + "\"");
    g_recorder.header_lines.push_back("sleep 1500");
    g_recorder.header_lines.push_back("both START_GAME");
    g_recorder.header_lines.push_back("sleep 1000");
}

bool write_script() {
    ensure_local_dir();
    g_recorder.output_path = "local/last_mp_recording.txt";
    std::ofstream out(g_recorder.output_path.c_str(), std::ios::out | std::ios::trunc);
    if (!out)
        return false;
    for (const std::string &line : g_recorder.header_lines)
        out << line << "\n";
    out << "\n";

    uint32_t input_base_tick = g_recorder.base_tick;
    if (g_recorder.setup_snapshot.valid) {
        if (!setupsnapshot::SaveToFile(g_recorder.setup_snapshot.snapshot, g_recorder.setup_snapshot.file_path))
            return false;
        out << "# Setup snapshot\n";
        out << "# Capture tick=" << g_recorder.setup_snapshot.tick << "\n";
        out << "both SETUP_LOAD_FILE path=\"" << g_recorder.setup_snapshot.file_path << "\"\n";
        out << "sleep 200\n\n";
        input_base_tick = g_recorder.setup_snapshot.tick;
    }

    out << "# Recorded local inputs\n";
    out << "# role=" << (g_recorder.host ? "host" : "client")
        << " local_player=" << g_recorder.local_player
        << " base_tick=" << input_base_tick << "\n";

    uint32_t max_input_tick = input_base_tick;
    for (size_t i = 0; i < g_recorder.inputs.size(); ++i) {
        const RecorderState::RecordedInput &entry = g_recorder.inputs[i];
        if (entry.tick < input_base_tick)
            continue;
        max_input_tick = std::max(max_input_tick, entry.tick);
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(3);
        os << (g_recorder.host ? "host " : "client ")
           << "QUEUE_LOCAL_INPUT player=" << entry.player
           << " ticks_ahead=" << (entry.tick - input_base_tick)
           << " fx=" << static_cast<double>(entry.value.mouse_force[0])
           << " fy=" << static_cast<double>(entry.value.mouse_force[1]);
        if (entry.value.rotate_steps != 0)
            os << " rot=" << entry.value.rotate_steps;
        if (entry.value.activate_count != 0)
            os << " act=" << entry.value.activate_count;
        out << os.str() << "\n";
    }

    const uint32_t tick_ms = static_cast<uint32_t>(std::max(1, options::GetInt("MultiplayerDebugTickLengthMs")));
    const uint32_t input_delay =
        static_cast<uint32_t>(std::max(0, options::GetInt("MultiplayerDebugInputDelayTicks")));
    const uint32_t settle_ticks =
        (max_input_tick > input_base_tick ? (max_input_tick - input_base_tick) : 0) + input_delay + 50;
    out << "sleep " << (settle_ticks * tick_ms) << "\n";
    out << "both QUIT\n";
    return true;
}

}  // namespace

bool Toggle() {
    if (g_recorder.recording) {
        g_recorder.recording = false;
        if (write_script())
            g_recorder.status_message = "MP script saved to " + g_recorder.output_path;
        else
            g_recorder.status_message = "MP script recorder failed to write local/last_mp_recording.txt";
        return g_recorder.recording;
    }

    if (!multiplayer::IsActive() || !server::WorldInitialized) {
        g_recorder.status_message = "MP script recorder needs an active multiplayer level";
        return false;
    }

    g_recorder.recording = true;
    g_recorder.base_tick = recorder_tick_base();
    g_recorder.local_player = multiplayer::LocalPlayer();
    g_recorder.host = multiplayer::IsHost();
    g_recorder.inputs.clear();
    g_recorder.setup_snapshot = RecorderState::SetupSnapshot();
    build_script_header();
    CaptureSetupSnapshot();
    if (g_recorder.setup_snapshot.valid)
        g_recorder.status_message = "MP script recording started (setup snapshot captured)";
    else
        g_recorder.status_message = "MP script recording started";
    return true;
}

bool IsRecording() {
    return g_recorder.recording;
}

std::string StatusMessage() {
    return g_recorder.status_message;
}

bool CaptureSetupSnapshot() {
    if (!g_recorder.recording) {
        g_recorder.status_message = "MP setup snapshot requires active recording";
        return false;
    }
    if (!multiplayer::IsActive() || !server::WorldInitialized) {
        g_recorder.status_message = "MP setup snapshot needs an active multiplayer level";
        return false;
    }

    g_recorder.setup_snapshot.valid = true;
    g_recorder.setup_snapshot.tick = recorder_tick_base();
    g_recorder.setup_snapshot.snapshot = setupsnapshot::Capture();
    g_recorder.setup_snapshot.file_path = "local/last_mp_recording_setup.mpsetup";
    g_recorder.status_message = "MP setup snapshot captured";
    return true;
}

void RecordLocalInputTick(uint32_t tick, unsigned local_player, const input::PlayerInput &pending) {
    if (!g_recorder.recording)
        return;
    if (local_player != g_recorder.local_player)
        return;
    if (pending.empty())
        return;
    if (tick < g_recorder.base_tick)
        return;

    RecorderState::RecordedInput entry;
    entry.tick = tick;
    entry.player = local_player;
    entry.value = pending;
    g_recorder.inputs.push_back(entry);
}

}  // namespace scriptrecorder
}  // namespace multiplayer
}  // namespace enigma
