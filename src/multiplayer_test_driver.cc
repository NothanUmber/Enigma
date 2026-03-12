#include "multiplayer_test_driver.hh"

#include "multiplayer.hh"
#include "multiplayer_internal.hh"
#include "multiplayer_protocol.hh"
#include "multiplayer_rollback.hh"
#include "multiplayer_script_recorder.hh"
#include "multiplayer_state.hh"

#include "client.hh"
#include "game.hh"
#include "input.hh"
#include "lev/Proxy.hh"
#include "main.hh"
#include "others/Wire.hh"
#include "options.hh"
#include "player.hh"
#include "Inventory.hh"
#include "server.hh"
#include "stones/OxydStone.hh"
#include "stones/ShogunStone.hh"
#include "world.hh"

#include "SDL.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace enigma {
namespace multiplayer {
namespace testdriver {

namespace {

struct DriverState {
    bool enabled = false;
    std::string role;
    std::string connect_value;

    std::string controller_host;
    Uint16 controller_port = 0;

    internal::TcpSocket sock = internal::kInvalidTcpSocket;
    std::vector<uint8_t> rx;
    uint32_t pending_len = 0;

    bool hello_sent = false;
    Uint32 next_connect_attempt_ms = 0;

    bool join_active = false;
    // Periodic state streaming.
    int stream_state_interval_ms = 0;
    Uint32 next_stream_state_ms = 0;

    // Test-only: force a specific input sample for a number of simulation ticks.
    bool input_override_enabled = false;
    unsigned input_override_player = 0;
    input::PlayerInput input_override_value;
    int input_override_ticks_left = 0;
    uint32_t input_override_last_tick = UINT32_MAX;

    // Test-only: submit a fixed mouse-force delta once per simulation tick.
    bool hold_mouse_force_enabled = false;
    unsigned hold_mouse_force_player = 0;
    ecl::V2 hold_mouse_force_value;
    int hold_mouse_force_ticks_left = 0;
    uint32_t hold_mouse_force_last_tick = UINT32_MAX;

    struct QueuedMouseForce {
        uint32_t tick = 0;
        unsigned player = 0;
        ecl::V2 value;
    };
    std::vector<QueuedMouseForce> queued_mouse_forces;

    struct QueuedLocalInput {
        uint32_t tick = 0;
        unsigned player = 0;
        input::PlayerInput value;
    };
    std::vector<QueuedLocalInput> queued_local_inputs;
};

DriverState g_drv;

static std::string trim(const std::string &s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

static std::vector<std::string> split_ws(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

static std::map<std::string, std::string> parse_kv(const std::vector<std::string> &tokens,
                                                   size_t start) {
    std::map<std::string, std::string> out;
    for (size_t i = start; i < tokens.size(); ++i) {
        const std::string &t = tokens[i];
        std::string::size_type pos = t.find('=');
        if (pos == std::string::npos)
            continue;
        std::string k = t.substr(0, pos);
        std::string v = t.substr(pos + 1);
        if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
            v = v.substr(1, v.size() - 2);
        if (!k.empty())
            out[k] = v;
    }
    return out;
}

static bool parse_u32(const std::map<std::string, std::string> &kv, const char *key, uint32_t &out) {
    auto it = kv.find(key);
    if (it == kv.end())
        return false;
    char *end = nullptr;
    unsigned long v = std::strtoul(it->second.c_str(), &end, 10);
    if (!end || *end != '\0')
        return false;
    out = static_cast<uint32_t>(v);
    return true;
}

static bool parse_i32(const std::map<std::string, std::string> &kv, const char *key, int &out) {
    auto it = kv.find(key);
    if (it == kv.end())
        return false;
    char *end = nullptr;
    long v = std::strtol(it->second.c_str(), &end, 10);
    if (!end || *end != '\0')
        return false;
    out = static_cast<int>(v);
    return true;
}

static bool parse_u16(const std::map<std::string, std::string> &kv, const char *key, Uint16 &out) {
    uint32_t v = 0;
    if (!parse_u32(kv, key, v))
        return false;
    if (v == 0 || v > 65535)
        return false;
    out = static_cast<Uint16>(v);
    return true;
}

static bool parse_u8(const std::map<std::string, std::string> &kv, const char *key, Uint8 &out) {
    uint32_t v = 0;
    if (!parse_u32(kv, key, v))
        return false;
    if (v > 255)
        return false;
    out = static_cast<Uint8>(v);
    return true;
}

static bool parse_f32(const std::map<std::string, std::string> &kv, const char *key, float &out) {
    auto it = kv.find(key);
    if (it == kv.end())
        return false;
    char *end = nullptr;
    float v = std::strtof(it->second.c_str(), &end);
    if (!end || *end != '\0')
        return false;
    out = v;
    return true;
}

static bool resolve_driver_tick(const std::map<std::string, std::string> &kv, uint32_t &tick,
                                std::string &err) {
    tick = 0;
    err.clear();
    if (parse_u32(kv, "tick", tick))
        return true;

    uint32_t base_tick = input::CurrentTick();
    if (multiplayer::IsActive() && internal::g_session.active &&
        internal::g_session.local_player_known && internal::g_session.next_local_tick != 0) {
        base_tick = internal::g_session.next_local_tick;
    }

    uint32_t ticks_ahead = 0;
    if (parse_u32(kv, "ticks_ahead", ticks_ahead)) {
        tick = base_tick + ticks_ahead;
        return true;
    }

    err = "missing_tick";
    return false;
}

static uint32_t local_submission_tick() {
    if (multiplayer::IsActive() && internal::g_session.active &&
        internal::g_session.local_player_known && internal::g_session.next_local_tick != 0) {
        return internal::g_session.next_local_tick;
    }
    return input::CurrentTick();
}

static void send_frame(const std::string &msg) {
    if (!g_drv.enabled)
        return;
    if (!internal::tcp_socket_valid(g_drv.sock))
        return;
    internal::tcp_send_frame(g_drv.sock, msg.data(), msg.size());
}

static void send_ok(const char *cmd, const std::string &extra = std::string()) {
    std::string line = "OK";
    if (cmd && *cmd)
        line += std::string(" cmd=") + cmd;
    if (!extra.empty())
        line += std::string(" ") + extra;
    send_frame(line);
}

static void send_err(const char *cmd, const std::string &msg) {
    std::string line = "ERR";
    if (cmd && *cmd)
        line += std::string(" cmd=") + cmd;
    if (!msg.empty())
        line += std::string(" msg=") + msg;
    send_frame(line);
}

static void send_evt(const std::string &name, const std::string &extra = std::string()) {
    std::string line = "EVT name=" + name;
    if (!extra.empty())
        line += std::string(" ") + extra;
    send_frame(line);
}

static lev::Proxy *find_proxy_by_norm(const std::string &norm_level_path) {
    if (norm_level_path.empty())
        return nullptr;
    for (lev::Proxy *proxy : lev::Proxy::getProxies()) {
        if (!proxy)
            continue;
        if (proxy->getNormLevelPath() == norm_level_path)
            return proxy;
    }
    return nullptr;
}

static void emit_steerable_actors() {
    std::vector<Actor *> actors;
    GetActors(actors);
    unsigned idx = 0;
    for (Actor *a : actors) {
        if (!a || !a->isSteerable())
            continue;
        const ecl::V2 &p = a->get_pos();
        const ecl::V2 &v = a->get_vel();
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(3);
        os << "idx=" << idx
           << " kind=" << a->getKind()
           << " obj=" << a->getId()
           << " ctrl=" << a->get_controllers()
           << " mf=" << a->get_mouseforce()
           << " x=" << p[0] << " y=" << p[1]
           << " vx=" << v[0] << " vy=" << v[1];
        Value owner = a->getAttr("owner");
        if (owner)
            os << " owner=" << static_cast<int>(owner);
        Value color = a->getAttr("color");
        if (color)
            os << " color=" << static_cast<int>(color);
        send_evt("STEERABLE", os.str());
        idx += 1;
    }
    send_ok("LIST_STEERABLE", "count=" + std::to_string(idx));
}

static void append_actor(std::ostringstream &os, const char *prefix, unsigned player) {
    Actor *a = player::GetMainActor(player);
    if (!a) {
        os << " " << prefix << "valid=0";
        return;
    }
    const ecl::V2 &p = a->get_pos();
    const ecl::V2 &v = a->get_vel();
    os.setf(std::ios::fixed);
    os.precision(3);
    const ActorInfo *ai = a->get_actorinfo();
    const VisualPredictionActorMode vp_mode = VisualPredictionGetActorMode(*a);
    const double vp_blend = VisualPredictionGetActorBlendAlpha(*a);
    os << " " << prefix << "valid=1"
       << " " << prefix << "kind=" << a->getKind()
       << " " << prefix << "obj=" << a->getId()
       << " " << prefix << "ctrl=" << a->get_controllers()
       << " " << prefix << "mf=" << a->get_mouseforce()
       << " " << prefix << "vp_mode=" << static_cast<int>(vp_mode)
       << " " << prefix << "vp_blend=" << vp_blend
       << " " << prefix << "grab=" << (ai && ai->grabbed ? 1 : 0)
       << " " << prefix << "mov=" << (a->is_movable() ? 1 : 0)
       << " " << prefix << "dead=" << (a->is_dead() ? 1 : 0)
       << " " << prefix << "fly=" << (a->is_flying() ? 1 : 0)
       << " " << prefix << "x=" << p[0]
       << " " << prefix << "y=" << p[1]
       << " " << prefix << "vx=" << v[0]
       << " " << prefix << "vy=" << v[1];
}

static void maybe_apply_input_override() {
    if (!g_drv.enabled || !g_drv.input_override_enabled)
        return;
    if (g_drv.input_override_ticks_left <= 0) {
        g_drv.input_override_enabled = false;
        send_evt("INPUT_OVERRIDE_DONE");
        return;
    }
    const uint32_t tick = input::CurrentTick();
    // Always (re-)enqueue the override for the current tick so it wins even if
    // transport code enqueues a later value for the same (tick, player).
    input::EnqueueInput(tick, g_drv.input_override_player, g_drv.input_override_value);
    rollback::RecordInput(tick, g_drv.input_override_player, g_drv.input_override_value);

    if (tick != g_drv.input_override_last_tick) {
        g_drv.input_override_last_tick = tick;
        g_drv.input_override_ticks_left -= 1;
    }
}

static void maybe_apply_hold_mouse_force() {
    if (!g_drv.enabled || !g_drv.hold_mouse_force_enabled)
        return;
    if (g_drv.hold_mouse_force_ticks_left <= 0) {
        g_drv.hold_mouse_force_enabled = false;
        send_evt("HOLD_MOUSE_FORCE_DONE");
        return;
    }
    const uint32_t tick = local_submission_tick();
    if (tick == g_drv.hold_mouse_force_last_tick)
        return;
    g_drv.hold_mouse_force_last_tick = tick;
    input::SubmitMouseForce(g_drv.hold_mouse_force_player, g_drv.hold_mouse_force_value);
    g_drv.hold_mouse_force_ticks_left -= 1;
}

static void maybe_apply_queued_mouse_forces() {
    if (!g_drv.enabled || g_drv.queued_mouse_forces.empty())
        return;
    const uint32_t tick = local_submission_tick();
    for (auto it = g_drv.queued_mouse_forces.begin(); it != g_drv.queued_mouse_forces.end();) {
        if (tick > it->tick) {
            it = g_drv.queued_mouse_forces.erase(it);
            continue;
        }
        if (tick == it->tick) {
            input::SubmitMouseForce(it->player, it->value);
            it = g_drv.queued_mouse_forces.erase(it);
            continue;
        }
        ++it;
    }
}

static void maybe_apply_queued_local_inputs() {
    if (!g_drv.enabled || g_drv.queued_local_inputs.empty())
        return;
    const uint32_t tick = local_submission_tick();
    for (auto it = g_drv.queued_local_inputs.begin(); it != g_drv.queued_local_inputs.end();) {
        if (tick > it->tick) {
            it = g_drv.queued_local_inputs.erase(it);
            continue;
        }
        if (tick == it->tick) {
            if (it->value.mouse_force[0] != 0.0 || it->value.mouse_force[1] != 0.0)
                input::SubmitMouseForce(it->player, it->value.mouse_force);
            if (it->value.rotate_steps != 0)
                input::SubmitRotateInventory(it->player, it->value.rotate_steps);
            for (int i = 0; i < it->value.activate_count; ++i)
                input::SubmitActivateItem(it->player);
            it = g_drv.queued_local_inputs.erase(it);
            continue;
        }
        ++it;
    }
}

static bool world_accessible() {
    return server::WorldInitialized && Width() > 0 && Height() > 0;
}

static std::vector<Wire *> sorted_wires() {
    std::vector<Wire *> wires;
    std::vector<Other *> others;
    GetOthers(others);
    for (Other *other : others) {
        if (Wire *wire = dynamic_cast<Wire *>(other))
            wires.push_back(wire);
    }
    return wires;
}

static std::string object_debug_label(Object *obj) {
    if (!obj)
        return "(none)";
    if (Value name = obj->getAttr("name")) {
        const std::string str = name.to_string();
        if (!str.empty())
            return str;
    }
    if (Stone *stone = dynamic_cast<Stone *>(obj)) {
        const GridPos pos = stone->getOwnerPos();
        std::ostringstream os;
        os << "@" << pos.x << "," << pos.y;
        return os.str();
    }
    std::ostringstream os;
    os << "#" << obj->getId();
    return os.str();
}

static void emit_state_snapshot() {
    if (!g_drv.enabled)
        return;
    const uint32_t tick = input::CurrentTick();
    const internal::SessionState &s = internal::g_session;
    unsigned direct_ready = 0;
    for (const auto &kv : s.peer_ready) {
        if (kv.second)
            direct_ready += 1;
    }
    unsigned udp_ready = 0;
    for (const auto &kv : s.relay_ready) {
        if (kv.second)
            udp_ready += 1;
    }
    unsigned tcp_ready = 0;
    for (const auto &kv : s.tcp_relay_ready) {
        if (kv.second)
            tcp_ready += 1;
    }
    std::ostringstream os;
    os << "tick=" << tick
       << " mp_active=" << (multiplayer::IsActive() ? 1 : 0)
       << " mp_host=" << (multiplayer::IsHost() ? 1 : 0)
       << " expected=" << multiplayer::ExpectedPlayers()
       << " local=" << multiplayer::LocalPlayer()
       << " mp_defer=" << (multiplayer::ShouldDeferStart() ? 1 : 0)
       << " mp_phase=" << static_cast<int>(s.phase)
       << " mp_epoch=" << static_cast<unsigned>(s.input_epoch)
       << " mp_load=" << static_cast<unsigned>(s.load_id)
       << " mp_last_load=" << static_cast<unsigned>(s.last_load_id)
       << " mp_direct=" << static_cast<unsigned>(s.peer_players.size())
       << " mp_direct_ready=" << direct_ready
       << " mp_udp=" << static_cast<unsigned>(s.relay_players.size())
       << " mp_udp_ready=" << udp_ready
       << " mp_tcp=" << static_cast<unsigned>(s.tcp_relay_players.size())
       << " mp_tcp_ready=" << tcp_ready
       << " mp_local_ready_sent=" << (s.local_ready_sent ? 1 : 0)
       << " mp_paused=" << (s.paused ? 1 : 0)
       << " sv_world_init=" << (server::WorldInitialized ? 1 : 0)
       << " net=" << (input::IsNetworked() ? 1 : 0)
       << " zerofill=" << (input::ZerofillMissingInputsEnabled() ? 1 : 0)
       << " rb=" << (rollback::Enabled() ? 1 : 0)
       << " rb_replay=" << (rollback::IsReplaying() ? 1 : 0)
       << " ovr=" << (g_drv.input_override_enabled ? 1 : 0)
       << " ovr_p=" << static_cast<unsigned>(g_drv.input_override_player)
       << " ovr_left=" << static_cast<int>(g_drv.input_override_ticks_left)
       << " hold=" << (g_drv.hold_mouse_force_enabled ? 1 : 0)
       << " hold_p=" << static_cast<unsigned>(g_drv.hold_mouse_force_player)
       << " hold_left=" << static_cast<int>(g_drv.hold_mouse_force_ticks_left);

    // Input diagnostics for the current tick (first two players).
    for (unsigned p = 0; p < 2; ++p) {
        input::PlayerInput pi;
        const bool present = input::PeekInput(tick, p, pi);
        os << " in" << p << "_present=" << (present ? 1 : 0);
        if (present) {
            os.setf(std::ios::fixed);
            os.precision(3);
            os << " in" << p << "_fx=" << static_cast<double>(pi.mouse_force[0])
               << " in" << p << "_fy=" << static_cast<double>(pi.mouse_force[1])
               << " in" << p << "_rot=" << static_cast<int>(pi.rotate_steps)
               << " in" << p << "_act=" << static_cast<unsigned>(pi.activate_count);
        }
    }

    // Also report the last consumed input per player. This is often more useful
    // than peeking the current tick because the sim may have just advanced and
    // cleared the queue for the tick that applied movement.
    for (unsigned p = 0; p < 2; ++p) {
        input::PlayerInput last;
        const bool ok = input::GetLastConsumed(p, last);
        os << " lc" << p << "_present=" << (ok ? 1 : 0);
        if (ok) {
            os.setf(std::ios::fixed);
            os.precision(3);
            os << " lc" << p << "_fx=" << static_cast<double>(last.mouse_force[0])
               << " lc" << p << "_fy=" << static_cast<double>(last.mouse_force[1])
               << " lc" << p << "_rot=" << static_cast<int>(last.rotate_steps)
               << " lc" << p << "_act=" << static_cast<unsigned>(last.activate_count);
        }
    }

       os << " mp_clock_tick=" << static_cast<unsigned>(s.input_clock_tick)
       << " mp_input_delay=" << static_cast<unsigned>(s.input_delay)
       << " mp_tick_ms=" << static_cast<unsigned>(s.tick_ms)
       << " mp_world_cs=" << static_cast<unsigned long long>(s.last_world_checksum)
       << " mp_last_resync=" << static_cast<unsigned>(s.last_accepted_resync_tick)
       << " mp_next_local_tick=" << static_cast<unsigned>(s.next_local_tick)
       << " mp_next_send_tick=" << static_cast<unsigned>(s.next_send_tick);
    // Always report the first two players for convenience.
    append_actor(os, "p0_", 0);
    append_actor(os, "p1_", 1);
    send_evt("STATE", os.str());
}

static bool handle_command(const std::string &line) {
    const std::string s = trim(line);
    if (s.empty())
        return true;
    std::vector<std::string> toks = split_ws(s);
    if (toks.empty())
        return true;
    const std::string cmd = toks[0];
    std::map<std::string, std::string> kv = parse_kv(toks, 1);

    if (cmd == "PING") {
        send_ok("PING", "pong=1");
        return true;
    }

    if (cmd == "LIST_STEERABLE") {
        emit_steerable_actors();
        return true;
    }

    if (cmd == "GET_ACTOR_GRID") {
        uint32_t player_u32 = 0;
        if (!parse_u32(kv, "player", player_u32)) {
            send_err("GET_ACTOR_GRID", "missing_player");
            return true;
        }
        Actor *a = player::GetMainActor(static_cast<unsigned>(player_u32));
        if (!a) {
            send_err("GET_ACTOR_GRID", "no_actor");
            return true;
        }
        GridPos p(a->get_pos());
        std::ostringstream os;
        os << "player=" << static_cast<unsigned>(player_u32)
           << " gx=" << static_cast<int>(p.x)
           << " gy=" << static_cast<int>(p.y);
        send_ok("GET_ACTOR_GRID", os.str());
        return true;
    }

    if (cmd == "SET_ACTOR_POS") {
        uint32_t player_u32 = 0;
        if (!parse_u32(kv, "player", player_u32)) {
            send_err("SET_ACTOR_POS", "missing_player");
            return true;
        }
        float x = 0.0f;
        float y = 0.0f;
        if (!parse_f32(kv, "x", x) || !parse_f32(kv, "y", y)) {
            send_err("SET_ACTOR_POS", "missing_xy");
            return true;
        }
        float vx = 0.0f;
        float vy = 0.0f;
        parse_f32(kv, "vx", vx);
        parse_f32(kv, "vy", vy);
        Actor *a = player::GetMainActor(static_cast<unsigned>(player_u32));
        if (!a) {
            send_err("SET_ACTOR_POS", "no_actor");
            return true;
        }
        ActorInfo *ai = a->get_actorinfo();
        ai->pos = ecl::V2(x, y);
        DidMoveActor(a);
        ai->last_gridpos = ai->gridpos;
        ai->vel = ecl::V2(vx, vy);
        ai->frozen_vel = ecl::V2(0, 0);
        ai->forceacc = ecl::V2(0, 0);
        ai->pos_force = ecl::V2(0, 0);
        ai->force = ecl::V2(0, 0);
        ai->collforce = ecl::V2(0, 0);
        ai->render_pos = ai->pos;
        ai->render_initialized = true;
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(3);
        os << "player=" << static_cast<unsigned>(player_u32)
           << " x=" << static_cast<double>(ai->pos[0])
           << " y=" << static_cast<double>(ai->pos[1])
           << " vx=" << static_cast<double>(ai->vel[0])
           << " vy=" << static_cast<double>(ai->vel[1]);
        multiplayer::VisualPredictionInvalidate();
        send_ok("SET_ACTOR_POS", os.str());
        return true;
    }

    if (cmd == "GET_CELL") {
        int x = 0;
        int y = 0;
        if (!parse_i32(kv, "x", x) || !parse_i32(kv, "y", y)) {
            send_err("GET_CELL", "missing_xy");
            return true;
        }
        if (!world_accessible()) {
            send_err("GET_CELL", "no_world");
            return true;
        }
        GridPos p(x, y);
        Object *fl = GetFloor(p);
        Object *st = GetStone(p);
        Object *it = GetItem(p);
        auto kind_or_dash = [](Object *obj) -> std::string {
            return obj ? obj->getKind() : std::string("-");
        };
        auto state_or_dash = [](Object *obj) -> std::string {
            if (!obj)
                return std::string("-");
            Value v = obj->getAttr("state");
            if (!v)
                return std::string("-");
            return std::to_string(static_cast<int>(v));
        };
        auto snapshot_state_or_dash = [](Object *obj) -> std::string {
            if (!obj)
                return std::string("-");
            return std::to_string(obj->MpCaptureStateForSnapshot());
        };
        std::ostringstream os;
        os << "x=" << x << " y=" << y
           << " fl=" << kind_or_dash(fl)
           << " st=" << kind_or_dash(st)
           << " it=" << kind_or_dash(it)
           << " fl_state=" << state_or_dash(fl)
           << " st_state=" << state_or_dash(st)
           << " it_state=" << state_or_dash(it)
           << " fl_snap=" << snapshot_state_or_dash(fl)
           << " st_snap=" << snapshot_state_or_dash(st)
           << " it_snap=" << snapshot_state_or_dash(it);
        send_ok("GET_CELL", os.str());
        return true;
    }

    if (cmd == "GET_CELL_RENDER") {
        int x = 0;
        int y = 0;
        if (!parse_i32(kv, "x", x) || !parse_i32(kv, "y", y)) {
            send_err("GET_CELL_RENDER", "missing_xy");
            return true;
        }
        if (!world_accessible()) {
            send_err("GET_CELL_RENDER", "no_world");
            return true;
        }
        multiplayer::VisualPredictionBeginRender();
        GridPos p(x, y);
        Object *fl = GetFloor(p);
        Object *st = GetStone(p);
        Object *it = GetItem(p);
        auto kind_or_dash = [](Object *obj) -> std::string {
            return obj ? obj->getKind() : std::string("-");
        };
        auto state_or_dash = [](Object *obj) -> std::string {
            if (!obj)
                return std::string("-");
            Value v = obj->getAttr("state");
            if (!v)
                return std::string("-");
            return std::to_string(static_cast<int>(v));
        };
        auto snapshot_state_or_dash = [](Object *obj) -> std::string {
            if (!obj)
                return std::string("-");
            return std::to_string(obj->MpCaptureStateForSnapshot());
        };
        std::ostringstream os;
        os << "x=" << x << " y=" << y
           << " fl=" << kind_or_dash(fl)
           << " st=" << kind_or_dash(st)
           << " it=" << kind_or_dash(it)
           << " fl_state=" << state_or_dash(fl)
           << " st_state=" << state_or_dash(st)
           << " it_state=" << state_or_dash(it)
           << " fl_snap=" << snapshot_state_or_dash(fl)
           << " st_snap=" << snapshot_state_or_dash(st)
           << " it_snap=" << snapshot_state_or_dash(it);
        multiplayer::VisualPredictionEndRender();
        send_ok("GET_CELL_RENDER", os.str());
        return true;
    }

    if (cmd == "SET_STONE") {
        int x = 0;
        int y = 0;
        if (!parse_i32(kv, "x", x) || !parse_i32(kv, "y", y)) {
            send_err("SET_STONE", "missing_xy");
            return true;
        }
        std::string kind;
        {
            auto it = kv.find("kind");
            if (it != kv.end())
                kind = it->second;
        }
        if (kind.empty()) {
            send_err("SET_STONE", "missing_kind");
            return true;
        }
        Stone *st = MakeStone(kind.c_str());
        if (!st) {
            send_err("SET_STONE", "make_failed");
            return true;
        }
        SetStone(GridPos(x, y), st);
        std::ostringstream os;
        os << "x=" << x << " y=" << y << " kind=" << kind << " id=" << st->getId();
        multiplayer::VisualPredictionInvalidate();
        send_ok("SET_STONE", os.str());
        return true;
    }

    if (cmd == "GET_SHOGUN_CHAIN") {
        int x = 0;
        int y = 0;
        if (!parse_i32(kv, "x", x) || !parse_i32(kv, "y", y)) {
            send_err("GET_SHOGUN_CHAIN", "missing_xy");
            return true;
        }
        if (!world_accessible()) {
            send_err("GET_SHOGUN_CHAIN", "no_world");
            return true;
        }
        ShogunStone *shogun = dynamic_cast<ShogunStone *>(GetStone(GridPos(x, y)));
        if (!shogun) {
            send_err("GET_SHOGUN_CHAIN", "no_shogun");
            return true;
        }
        std::ostringstream os;
        os << "x=" << x << " y=" << y
           << " kind=" << shogun->getKind()
           << " holes=" << shogun->MpCaptureStateForSnapshot()
           << " chain=" << shogun->MpDebugChainHoles();
        send_ok("GET_SHOGUN_CHAIN", os.str());
        return true;
    }

    if (cmd == "BREAK_SHOGUN_CHAIN") {
        int x = 0;
        int y = 0;
        if (!parse_i32(kv, "x", x) || !parse_i32(kv, "y", y)) {
            send_err("BREAK_SHOGUN_CHAIN", "missing_xy");
            return true;
        }
        if (!world_accessible()) {
            send_err("BREAK_SHOGUN_CHAIN", "no_world");
            return true;
        }
        ShogunStone *shogun = dynamic_cast<ShogunStone *>(GetStone(GridPos(x, y)));
        if (!shogun) {
            send_err("BREAK_SHOGUN_CHAIN", "no_shogun");
            return true;
        }
        if (!shogun->MpDebugDropSubChain()) {
            send_err("BREAK_SHOGUN_CHAIN", "not_breakable");
            return true;
        }
        multiplayer::VisualPredictionInvalidate();
        std::ostringstream os;
        os << "x=" << x << " y=" << y
           << " kind=" << shogun->getKind()
           << " holes=" << shogun->MpCaptureStateForSnapshot()
           << " chain=" << shogun->MpDebugChainHoles();
        send_ok("BREAK_SHOGUN_CHAIN", os.str());
        return true;
    }

    if (cmd == "GET_OXYD_STATE") {
        int x = 0;
        int y = 0;
        if (!parse_i32(kv, "x", x) || !parse_i32(kv, "y", y)) {
            send_err("GET_OXYD_STATE", "missing_xy");
            return true;
        }
        if (!world_accessible()) {
            send_err("GET_OXYD_STATE", "no_world");
            return true;
        }
        OxydStone *oxyd = dynamic_cast<OxydStone *>(GetStone(GridPos(x, y)));
        if (!oxyd) {
            send_err("GET_OXYD_STATE", "no_oxyd");
            return true;
        }
        std::ostringstream os;
        os << "x=" << x << " y=" << y
           << " kind=" << oxyd->getKind()
           << " internal=" << oxyd->MpDebugInternalState()
           << " external=" << static_cast<int>(oxyd->getAttr("state"))
           << " color=" << static_cast<int>(oxyd->getAttr("oxydcolor"));
        send_ok("GET_OXYD_STATE", os.str());
        return true;
    }

    if (cmd == "FORCE_OXYD_STATE") {
        int x = 0;
        int y = 0;
        int state = 0;
        if (!parse_i32(kv, "x", x) || !parse_i32(kv, "y", y)) {
            send_err("FORCE_OXYD_STATE", "missing_xy");
            return true;
        }
        if (!parse_i32(kv, "state", state)) {
            send_err("FORCE_OXYD_STATE", "missing_state");
            return true;
        }
        if (!world_accessible()) {
            send_err("FORCE_OXYD_STATE", "no_world");
            return true;
        }
        OxydStone *oxyd = dynamic_cast<OxydStone *>(GetStone(GridPos(x, y)));
        if (!oxyd) {
            send_err("FORCE_OXYD_STATE", "no_oxyd");
            return true;
        }
        oxyd->MpDebugForceInternalState(state);
        multiplayer::VisualPredictionInvalidate();
        std::ostringstream os;
        os << "x=" << x << " y=" << y
           << " kind=" << oxyd->getKind()
           << " internal=" << oxyd->MpDebugInternalState()
           << " external=" << static_cast<int>(oxyd->getAttr("state"))
           << " color=" << static_cast<int>(oxyd->getAttr("oxydcolor"));
        send_ok("FORCE_OXYD_STATE", os.str());
        return true;
    }

    if (cmd == "FORCE_OXYD_COLOR") {
        int x = 0;
        int y = 0;
        int color = 0;
        if (!parse_i32(kv, "x", x) || !parse_i32(kv, "y", y)) {
            send_err("FORCE_OXYD_COLOR", "missing_xy");
            return true;
        }
        if (!parse_i32(kv, "color", color)) {
            send_err("FORCE_OXYD_COLOR", "missing_color");
            return true;
        }
        if (!world_accessible()) {
            send_err("FORCE_OXYD_COLOR", "no_world");
            return true;
        }
        OxydStone *oxyd = dynamic_cast<OxydStone *>(GetStone(GridPos(x, y)));
        if (!oxyd) {
            send_err("FORCE_OXYD_COLOR", "no_oxyd");
            return true;
        }
        oxyd->MpForceOxydColor(color);
        multiplayer::VisualPredictionInvalidate();
        std::ostringstream os;
        os << "x=" << x << " y=" << y
           << " kind=" << oxyd->getKind()
           << " internal=" << oxyd->MpDebugInternalState()
           << " external=" << static_cast<int>(oxyd->getAttr("state"))
           << " color=" << static_cast<int>(oxyd->getAttr("oxydcolor"));
        send_ok("FORCE_OXYD_COLOR", os.str());
        return true;
    }

    if (cmd == "GET_WIRE_STATE") {
        int index = 0;
        if (!parse_i32(kv, "index", index)) {
            send_err("GET_WIRE_STATE", "missing_index");
            return true;
        }
        if (!world_accessible()) {
            send_err("GET_WIRE_STATE", "no_world");
            return true;
        }
        const std::vector<Wire *> wires = sorted_wires();
        if (index < 0 || static_cast<size_t>(index) >= wires.size()) {
            send_err("GET_WIRE_STATE", "bad_index");
            return true;
        }
        Wire *wire = wires[static_cast<size_t>(index)];
        Object *anchor1 = wire->getAttr("anchor1");
        Object *anchor2 = wire->getAttr("anchor2");
        ObjectList anchor1_wires;
        ObjectList anchor2_wires;
        ObjectList anchor1_fellows;
        ObjectList anchor2_fellows;
        if (anchor1) {
            anchor1_wires = anchor1->getAttr("wires");
            anchor1_fellows = anchor1->getAttr("fellows");
        }
        if (anchor2) {
            anchor2_wires = anchor2->getAttr("wires");
            anchor2_fellows = anchor2->getAttr("fellows");
        }
        std::ostringstream os;
        os << "index=" << index
           << " id=" << wire->getId()
           << " anchor1=" << object_debug_label(anchor1)
           << " anchor2=" << object_debug_label(anchor2)
           << " anchor1_wires=" << anchor1_wires.size()
           << " anchor2_wires=" << anchor2_wires.size()
           << " anchor1_fellows=" << anchor1_fellows.size()
           << " anchor2_fellows=" << anchor2_fellows.size();
        send_ok("GET_WIRE_STATE", os.str());
        return true;
    }

    if (cmd == "FORCE_WIRE_ANCHORS") {
        int index = 0;
        if (!parse_i32(kv, "index", index)) {
            send_err("FORCE_WIRE_ANCHORS", "missing_index");
            return true;
        }
        const auto it_anchor1 = kv.find("anchor1");
        const auto it_anchor2 = kv.find("anchor2");
        if (it_anchor1 == kv.end() || it_anchor2 == kv.end()) {
            send_err("FORCE_WIRE_ANCHORS", "missing_anchor");
            return true;
        }
        if (!world_accessible()) {
            send_err("FORCE_WIRE_ANCHORS", "no_world");
            return true;
        }
        const std::vector<Wire *> wires = sorted_wires();
        if (index < 0 || static_cast<size_t>(index) >= wires.size()) {
            send_err("FORCE_WIRE_ANCHORS", "bad_index");
            return true;
        }
        Stone *anchor1 = dynamic_cast<Stone *>(GetNamedObject(it_anchor1->second));
        Stone *anchor2 = dynamic_cast<Stone *>(GetNamedObject(it_anchor2->second));
        if (!anchor1 || !anchor2) {
            send_err("FORCE_WIRE_ANCHORS", "bad_anchor");
            return true;
        }
        Wire *wire = wires[static_cast<size_t>(index)];
        wire->setAttr("anchor1", Value(static_cast<Object *>(anchor1)));
        wire->setAttr("anchor2", Value(static_cast<Object *>(anchor2)));
        multiplayer::VisualPredictionInvalidate();
        std::ostringstream os;
        os << "index=" << index
           << " id=" << wire->getId()
           << " anchor1=" << object_debug_label(anchor1)
           << " anchor2=" << object_debug_label(anchor2);
        send_ok("FORCE_WIRE_ANCHORS", os.str());
        return true;
    }

    if (cmd == "MOVE_STONE") {
        int from_x = 0;
        int from_y = 0;
        int to_x = 0;
        int to_y = 0;
        if (!parse_i32(kv, "from_x", from_x) || !parse_i32(kv, "from_y", from_y) ||
            !parse_i32(kv, "to_x", to_x) || !parse_i32(kv, "to_y", to_y)) {
            send_err("MOVE_STONE", "missing_xy");
            return true;
        }
        const GridPos from(from_x, from_y);
        const GridPos to(to_x, to_y);
        Stone *st = GetStone(from);
        if (!st) {
            send_err("MOVE_STONE", "no_stone");
            return true;
        }
        if (GetStone(to)) {
            send_err("MOVE_STONE", "target_occupied");
            return true;
        }
        MoveStone(from, to);
        std::ostringstream os;
        os << "from_x=" << from_x << " from_y=" << from_y
           << " to_x=" << to_x << " to_y=" << to_y
           << " id=" << st->getId() << " kind=" << st->getKind();
        multiplayer::VisualPredictionInvalidate();
        send_ok("MOVE_STONE", os.str());
        return true;
    }

    if (cmd == "CLEAR_MOVABLE_STONES") {
        if (!world_accessible()) {
            send_err("CLEAR_MOVABLE_STONES", "no_world");
            return true;
        }
        int cleared = 0;
        for (int y = 0; y < Height(); ++y) {
            for (int x = 0; x < Width(); ++x) {
                const GridPos p(x, y);
                Stone *st = GetStone(p);
                if (!st || !st->is_movable())
                    continue;
                KillStone(p);
                ++cleared;
            }
        }
        multiplayer::VisualPredictionInvalidate();
        send_ok("CLEAR_MOVABLE_STONES", "count=" + std::to_string(cleared));
        return true;
    }

    if (cmd == "GET_UNDER_PLAYER") {
        uint32_t player_u32 = 0;
        if (!parse_u32(kv, "player", player_u32)) {
            send_err("GET_UNDER_PLAYER", "missing_player");
            return true;
        }
        Actor *a = player::GetMainActor(static_cast<unsigned>(player_u32));
        if (!a) {
            send_err("GET_UNDER_PLAYER", "no_actor");
            return true;
        }
        GridPos gp(a->get_pos());
        Object *fl = GetFloor(gp);
        Object *st = GetStone(gp);
        Object *it = GetItem(gp);
        auto kind_or_dash = [](Object *obj) -> std::string {
            return obj ? obj->getKind() : std::string("-");
        };
        std::ostringstream os;
        os << "player=" << static_cast<unsigned>(player_u32)
           << " gx=" << static_cast<int>(gp.x)
           << " gy=" << static_cast<int>(gp.y)
           << " fl=" << kind_or_dash(fl)
           << " st=" << kind_or_dash(st)
           << " it=" << kind_or_dash(it);
        send_ok("GET_UNDER_PLAYER", os.str());
        return true;
    }

    if (cmd == "GET_INV") {
        uint32_t player_u32 = 0;
        if (!parse_u32(kv, "player", player_u32)) {
            send_err("GET_INV", "missing_player");
            return true;
        }
        Inventory *inv = player::GetInventory(static_cast<unsigned>(player_u32));
        if (!inv) {
            send_err("GET_INV", "no_inventory");
            return true;
        }
        std::ostringstream os;
        os << "player=" << static_cast<unsigned>(player_u32)
           << " size=" << static_cast<unsigned>(inv->size());
        if (inv->size() > 0 && inv->get_item(0)) {
            os << " first=" << inv->get_item(0)->getKind();
        } else {
            os << " first=-";
        }
        send_ok("GET_INV", os.str());
        return true;
    }

    if (cmd == "SUBMIT_LOCAL") {
        const unsigned local_player = multiplayer::LocalPlayer();
        float fx = 0.0f, fy = 0.0f;
        int rot = 0;
        int act = 0;
        parse_f32(kv, "fx", fx);
        parse_f32(kv, "fy", fy);
        parse_i32(kv, "rot", rot);
        parse_i32(kv, "act", act);
        if (fx != 0.0f || fy != 0.0f)
            input::SubmitMouseForce(local_player, ecl::V2(fx, fy));
        if (rot != 0)
            input::SubmitRotateInventory(local_player, rot);
        for (int i = 0; i < act; ++i)
            input::SubmitActivateItem(local_player);
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(3);
        os << "player=" << static_cast<unsigned>(local_player)
           << " fx=" << static_cast<double>(fx)
           << " fy=" << static_cast<double>(fy)
           << " rot=" << rot
           << " act=" << act;
        send_ok("SUBMIT_LOCAL", os.str());
        return true;
    }

    if (cmd == "INJECT_INPUT") {
        uint32_t player_u32 = 0;
        if (!parse_u32(kv, "player", player_u32)) {
            send_err("INJECT_INPUT", "missing_player");
            return true;
        }
        float fx = 0.0f, fy = 0.0f;
        if (!parse_f32(kv, "fx", fx) || !parse_f32(kv, "fy", fy)) {
            send_err("INJECT_INPUT", "missing_force");
            return true;
        }
        int rot = 0;
        int act = 0;
        parse_i32(kv, "rot", rot);
        parse_i32(kv, "act", act);
        input::PlayerInput pi;
        pi.mouse_force = ecl::V2(fx, fy);
        pi.rotate_steps = rot;
        pi.activate_count = act > 0 ? act : 0;
        const uint32_t t = input::CurrentTick();
        input::EnqueueInput(t, static_cast<unsigned>(player_u32), pi);
        rollback::RecordInput(t, static_cast<unsigned>(player_u32), pi);
        input::PlayerInput chk;
        const bool present = input::PeekInput(t, static_cast<unsigned>(player_u32), chk);
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(3);
        os << "tick=" << static_cast<unsigned>(t)
           << " player=" << static_cast<unsigned>(player_u32)
           << " present=" << (present ? 1 : 0);
        if (present) {
            os << " fx=" << static_cast<double>(chk.mouse_force[0])
               << " fy=" << static_cast<double>(chk.mouse_force[1])
               << " rot=" << static_cast<int>(chk.rotate_steps)
               << " act=" << static_cast<unsigned>(chk.activate_count);
        }
        send_ok("INJECT_INPUT", os.str());
        return true;
    }

    if (cmd == "ENQUEUE_INPUT") {
        uint32_t player_u32 = 0;
        if (!parse_u32(kv, "player", player_u32)) {
            send_err("ENQUEUE_INPUT", "missing_player");
            return true;
        }
        uint32_t tick = 0;
        std::string tick_err;
        if (!resolve_driver_tick(kv, tick, tick_err)) {
            send_err("ENQUEUE_INPUT", tick_err);
            return true;
        }
        float fx = 0.0f, fy = 0.0f;
        if (!parse_f32(kv, "fx", fx) || !parse_f32(kv, "fy", fy)) {
            send_err("ENQUEUE_INPUT", "missing_force");
            return true;
        }
        int rot = 0;
        int act = 0;
        parse_i32(kv, "rot", rot);
        parse_i32(kv, "act", act);
        input::PlayerInput pi;
        pi.mouse_force = ecl::V2(fx, fy);
        pi.rotate_steps = rot;
        pi.activate_count = act > 0 ? act : 0;
        input::EnqueueInput(tick, static_cast<unsigned>(player_u32), pi);
        rollback::RecordInput(tick, static_cast<unsigned>(player_u32), pi);
        input::PlayerInput chk;
        const bool present = input::PeekInput(tick, static_cast<unsigned>(player_u32), chk);
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(3);
        os << "tick=" << static_cast<unsigned>(tick)
           << " player=" << static_cast<unsigned>(player_u32)
           << " present=" << (present ? 1 : 0);
        if (present) {
            os << " fx=" << static_cast<double>(chk.mouse_force[0])
               << " fy=" << static_cast<double>(chk.mouse_force[1])
               << " rot=" << static_cast<int>(chk.rotate_steps)
               << " act=" << static_cast<unsigned>(chk.activate_count);
        }
        send_ok("ENQUEUE_INPUT", os.str());
        return true;
    }

    if (cmd == "OVERRIDE_INPUT") {
        uint32_t player_u32 = 0;
        if (!parse_u32(kv, "player", player_u32)) {
            send_err("OVERRIDE_INPUT", "missing_player");
            return true;
        }
        int ticks = 0;
        if (!parse_i32(kv, "ticks", ticks)) {
            send_err("OVERRIDE_INPUT", "missing_ticks");
            return true;
        }
        if (ticks <= 0) {
            g_drv.input_override_enabled = false;
            g_drv.input_override_ticks_left = 0;
            send_ok("OVERRIDE_INPUT", "enabled=0");
            return true;
        }
        float fx = 0.0f, fy = 0.0f;
        if (!parse_f32(kv, "fx", fx) || !parse_f32(kv, "fy", fy)) {
            send_err("OVERRIDE_INPUT", "missing_force");
            return true;
        }
        int rot = 0;
        int act = 0;
        parse_i32(kv, "rot", rot);
        parse_i32(kv, "act", act);

        g_drv.input_override_player = static_cast<unsigned>(player_u32);
        g_drv.input_override_value = input::PlayerInput();
        g_drv.input_override_value.mouse_force = ecl::V2(fx, fy);
        g_drv.input_override_value.rotate_steps = rot;
        g_drv.input_override_value.activate_count = act > 0 ? act : 0;
        g_drv.input_override_ticks_left = ticks;
        g_drv.input_override_last_tick = UINT32_MAX;
        g_drv.input_override_enabled = true;
        // Apply once immediately so a script can query STATE right after.
        maybe_apply_input_override();

        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(3);
        os << "enabled=1"
           << " player=" << static_cast<unsigned>(player_u32)
           << " ticks=" << ticks
           << " fx=" << static_cast<double>(fx)
           << " fy=" << static_cast<double>(fy);
        send_ok("OVERRIDE_INPUT", os.str());
        return true;
    }

    if (cmd == "QUIT") {
        send_ok("QUIT");
        client::Msg_Command("abort");
        return true;
    }

    if (cmd == "START_HOST") {
        protocol::LobbyStart start;
        start.host_id = "mptest-host";
        start.filter_optimized = 0;

        uint32_t session_id = 0;
        uint32_t seed = 0;
        Uint8 expected = 0;
        Uint16 port = 0;
        if (!parse_u32(kv, "session", session_id) ||
            !parse_u32(kv, "seed", seed) ||
            !parse_u8(kv, "expected", expected) ||
            !parse_u16(kv, "port", port)) {
            send_err("START_HOST", "missing_fields");
            return true;
        }
        start.session_id = session_id;
        start.seed = seed;
        start.expected_players = expected;
        start.host_port = port;
        {
            std::map<std::string, std::string>::const_iterator it = kv.find("host_id");
            if (it != kv.end())
                start.host_id = it->second;
        }
        {
            std::map<std::string, std::string>::const_iterator it = kv.find("filter");
            if (it != kv.end())
                start.filter_optimized = static_cast<Uint8>(std::atoi(it->second.c_str()) ? 1 : 0);
        }
        {
            std::map<std::string, std::string>::const_iterator it = kv.find("level_id");
            if (it != kv.end())
                start.level_id = it->second;
        }
        {
            std::map<std::string, std::string>::const_iterator it = kv.find("pack");
            if (it != kv.end())
                start.pack_name = it->second;
        }

        bool ok = multiplayer::StartHostSession(start);
        if (!ok) {
            send_err("START_HOST", "start_failed");
            return true;
        }
        send_ok("START_HOST");
        return true;
    }

    if (cmd == "START_CLIENT") {
        protocol::LobbyStart start;
        start.host_id = "mptest-host";
        start.filter_optimized = 0;

        uint32_t session_id = 0;
        uint32_t seed = 0;
        Uint8 expected = 0;
        Uint16 port = 0;
        if (!parse_u32(kv, "session", session_id) ||
            !parse_u32(kv, "seed", seed) ||
            !parse_u8(kv, "expected", expected) ||
            !parse_u16(kv, "port", port)) {
            send_err("START_CLIENT", "missing_fields");
            return true;
        }
        std::string host_ip;
        {
            std::map<std::string, std::string>::const_iterator it = kv.find("host_ip");
            if (it != kv.end())
                host_ip = it->second;
        }
        if (host_ip.empty()) {
            send_err("START_CLIENT", "missing_host_ip");
            return true;
        }
        start.session_id = session_id;
        start.seed = seed;
        start.expected_players = expected;
        start.host_port = port;
        {
            std::map<std::string, std::string>::const_iterator it = kv.find("host_id");
            if (it != kv.end())
                start.host_id = it->second;
        }
        {
            std::map<std::string, std::string>::const_iterator it = kv.find("filter");
            if (it != kv.end())
                start.filter_optimized = static_cast<Uint8>(std::atoi(it->second.c_str()) ? 1 : 0);
        }
        {
            std::map<std::string, std::string>::const_iterator it = kv.find("level_id");
            if (it != kv.end())
                start.level_id = it->second;
        }
        {
            std::map<std::string, std::string>::const_iterator it = kv.find("pack");
            if (it != kv.end())
                start.pack_name = it->second;
        }

        bool ok = multiplayer::BeginClientJoin(start, host_ip);
        if (!ok) {
            send_err("START_CLIENT", "join_begin_failed");
            return true;
        }
        g_drv.join_active = true;
        send_ok("START_CLIENT");
        return true;
    }

    if (cmd == "LOAD_LEVEL") {
        std::string pack;
        std::string level_id;
        {
            std::map<std::string, std::string>::const_iterator it = kv.find("pack");
            if (it != kv.end())
                pack = it->second;
        }
        {
            std::map<std::string, std::string>::const_iterator it = kv.find("level_id");
            if (it != kv.end())
                level_id = it->second;
        }
        if (level_id.empty()) {
            send_err("LOAD_LEVEL", "missing_level_id");
            return true;
        }
        if (!pack.empty())
            server::Msg_SetLevelPack(pack);
        lev::Proxy *proxy = find_proxy_by_norm(level_id);
        if (!proxy) {
            send_err("LOAD_LEVEL", "level_not_found");
            return true;
        }
        if (multiplayer::IsActive() && multiplayer::IsHost()) {
            // Mirror lobby behavior: tell peers what to load.
            multiplayer::NotifyLoadLevel(pack, level_id);
        }
        server::Msg_LoadLevel(proxy, false);
        send_ok("LOAD_LEVEL");
        return true;
    }

    if (cmd == "START_GAME") {
        server::Msg_StartGame();
        send_ok("START_GAME");
        return true;
    }

    if (cmd == "ABORT_MP") {
        multiplayer::RequestAbort();
        send_ok("ABORT_MP");
        return true;
    }

    if (cmd == "MOUSE_FORCE") {
        uint32_t player_u32 = 0;
        if (!parse_u32(kv, "player", player_u32)) {
            send_err("MOUSE_FORCE", "missing_player");
            return true;
        }
        float fx = 0.0f, fy = 0.0f;
        if (!parse_f32(kv, "fx", fx) || !parse_f32(kv, "fy", fy)) {
            send_err("MOUSE_FORCE", "missing_force");
            return true;
        }
        input::SubmitMouseForce(static_cast<unsigned>(player_u32), ecl::V2(fx, fy));
        send_ok("MOUSE_FORCE");
        return true;
    }

    if (cmd == "HOLD_MOUSE_FORCE") {
        uint32_t player_u32 = 0;
        if (!parse_u32(kv, "player", player_u32)) {
            send_err("HOLD_MOUSE_FORCE", "missing_player");
            return true;
        }
        int ticks = 0;
        if (!parse_i32(kv, "ticks", ticks)) {
            send_err("HOLD_MOUSE_FORCE", "missing_ticks");
            return true;
        }
        if (ticks <= 0) {
            g_drv.hold_mouse_force_enabled = false;
            g_drv.hold_mouse_force_ticks_left = 0;
            send_ok("HOLD_MOUSE_FORCE", "enabled=0");
            return true;
        }
        float fx = 0.0f, fy = 0.0f;
        if (!parse_f32(kv, "fx", fx) || !parse_f32(kv, "fy", fy)) {
            send_err("HOLD_MOUSE_FORCE", "missing_force");
            return true;
        }
        g_drv.hold_mouse_force_player = static_cast<unsigned>(player_u32);
        g_drv.hold_mouse_force_value = ecl::V2(fx, fy);
        g_drv.hold_mouse_force_ticks_left = ticks;
        g_drv.hold_mouse_force_last_tick = UINT32_MAX;
        g_drv.hold_mouse_force_enabled = true;
        // Apply once immediately so a script can query STATE right after.
        maybe_apply_hold_mouse_force();

        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(3);
        os << "enabled=1"
           << " player=" << static_cast<unsigned>(player_u32)
           << " ticks=" << ticks
           << " fx=" << static_cast<double>(fx)
           << " fy=" << static_cast<double>(fy);
        send_ok("HOLD_MOUSE_FORCE", os.str());
        return true;
    }

    if (cmd == "QUEUE_MOUSE_FORCE") {
        uint32_t player_u32 = 0;
        if (!parse_u32(kv, "player", player_u32)) {
            send_err("QUEUE_MOUSE_FORCE", "missing_player");
            return true;
        }
        uint32_t tick = 0;
        std::string tick_err;
        if (!resolve_driver_tick(kv, tick, tick_err)) {
            send_err("QUEUE_MOUSE_FORCE", tick_err);
            return true;
        }
        float fx = 0.0f;
        float fy = 0.0f;
        if (!parse_f32(kv, "fx", fx) || !parse_f32(kv, "fy", fy)) {
            send_err("QUEUE_MOUSE_FORCE", "missing_force");
            return true;
        }
        DriverState::QueuedMouseForce entry;
        entry.tick = tick;
        entry.player = static_cast<unsigned>(player_u32);
        entry.value = ecl::V2(fx, fy);
        g_drv.queued_mouse_forces.push_back(entry);
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(3);
        os << "player=" << static_cast<unsigned>(player_u32)
           << " tick=" << static_cast<unsigned>(tick)
           << " fx=" << static_cast<double>(fx)
           << " fy=" << static_cast<double>(fy);
        send_ok("QUEUE_MOUSE_FORCE", os.str());
        return true;
    }

    if (cmd == "QUEUE_LOCAL_INPUT") {
        uint32_t player_u32 = 0;
        if (!parse_u32(kv, "player", player_u32)) {
            send_err("QUEUE_LOCAL_INPUT", "missing_player");
            return true;
        }
        uint32_t tick = 0;
        std::string tick_err;
        if (!resolve_driver_tick(kv, tick, tick_err)) {
            send_err("QUEUE_LOCAL_INPUT", tick_err);
            return true;
        }
        float fx = 0.0f;
        float fy = 0.0f;
        int rot = 0;
        int act = 0;
        parse_f32(kv, "fx", fx);
        parse_f32(kv, "fy", fy);
        parse_i32(kv, "rot", rot);
        parse_i32(kv, "act", act);
        if (fx == 0.0f && fy == 0.0f && rot == 0 && act == 0) {
            send_err("QUEUE_LOCAL_INPUT", "empty_input");
            return true;
        }
        DriverState::QueuedLocalInput entry;
        entry.tick = tick;
        entry.player = static_cast<unsigned>(player_u32);
        entry.value.mouse_force = ecl::V2(fx, fy);
        entry.value.rotate_steps = rot;
        entry.value.activate_count = act;
        g_drv.queued_local_inputs.push_back(entry);
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(3);
        os << "player=" << static_cast<unsigned>(player_u32)
           << " tick=" << static_cast<unsigned>(tick)
           << " fx=" << static_cast<double>(fx)
           << " fy=" << static_cast<double>(fy)
           << " rot=" << rot
           << " act=" << act;
        send_ok("QUEUE_LOCAL_INPUT", os.str());
        return true;
    }

    if (cmd == "SETUP_SAVE_FILE") {
        std::string path;
        {
            auto it = kv.find("path");
            if (it != kv.end())
                path = it->second;
        }
        if (path.empty()) {
            send_err("SETUP_SAVE_FILE", "missing_path");
            return true;
        }
        const multiplayer::setupsnapshot::Snapshot snapshot = multiplayer::setupsnapshot::Capture();
        if (!multiplayer::setupsnapshot::SaveToFile(snapshot, path)) {
            send_err("SETUP_SAVE_FILE", "save_failed");
            return true;
        }
        send_ok("SETUP_SAVE_FILE", "path=" + path);
        return true;
    }

    if (cmd == "SETUP_LOAD_FILE") {
        std::string path;
        {
            auto it = kv.find("path");
            if (it != kv.end())
                path = it->second;
        }
        if (path.empty()) {
            send_err("SETUP_LOAD_FILE", "missing_path");
            return true;
        }
        multiplayer::setupsnapshot::Snapshot snapshot;
        if (!multiplayer::setupsnapshot::LoadFromFile(path, snapshot)) {
            send_err("SETUP_LOAD_FILE", "load_failed");
            return true;
        }
        if (!multiplayer::setupsnapshot::Restore(snapshot)) {
            send_err("SETUP_LOAD_FILE", "restore_failed");
            return true;
        }
        multiplayer::VisualPredictionInvalidate();
        send_ok("SETUP_LOAD_FILE", "path=" + path);
        return true;
    }

    if (cmd == "SET_INT") {
        std::string key;
        int value = 0;
        {
            std::map<std::string, std::string>::const_iterator it = kv.find("key");
            if (it != kv.end())
                key = it->second;
        }
        if (key.empty() || !parse_i32(kv, "value", value)) {
            send_err("SET_INT", "missing_fields");
            return true;
        }
        options::SetOption(key.c_str(), static_cast<double>(value));
        send_ok("SET_INT");
        return true;
    }

    if (cmd == "SET_BOOL") {
        std::string key;
        int value = 0;
        {
            std::map<std::string, std::string>::const_iterator it = kv.find("key");
            if (it != kv.end())
                key = it->second;
        }
        if (key.empty() || !parse_i32(kv, "value", value)) {
            send_err("SET_BOOL", "missing_fields");
            return true;
        }
        options::SetOption(key.c_str(), value ? 1.0 : 0.0);
        send_ok("SET_BOOL");
        return true;
    }

    if (cmd == "STATE") {
        emit_state_snapshot();
        send_ok("STATE");
        return true;
    }

    if (cmd == "STATE_RENDER") {
        multiplayer::VisualPredictionBeginRender();
        emit_state_snapshot();
        multiplayer::VisualPredictionEndRender();
        send_ok("STATE_RENDER");
        return true;
    }

    if (cmd == "STREAM_STATE") {
        int interval = 0;
        if (!parse_i32(kv, "interval_ms", interval) || interval < 0) {
            send_err("STREAM_STATE", "invalid_interval");
            return true;
        }
        g_drv.stream_state_interval_ms = interval;
        g_drv.next_stream_state_ms = SDL_GetTicks() + static_cast<Uint32>(interval);
        send_ok("STREAM_STATE");
        return true;
    }

    send_err(cmd.c_str(), "unknown_command");
    return true;
}

static void maybe_connect() {
    if (!g_drv.enabled)
        return;
    if (internal::tcp_socket_valid(g_drv.sock))
        return;
    const Uint32 now = SDL_GetTicks();
    if (now < g_drv.next_connect_attempt_ms)
        return;
    g_drv.next_connect_attempt_ms = now + 250;

    if (g_drv.controller_host.empty() || g_drv.controller_port == 0)
        return;
    internal::TcpSocket s = internal::kInvalidTcpSocket;
    if (!internal::tcp_connect_timeout(g_drv.controller_host, g_drv.controller_port, 25, s))
        return;
    g_drv.sock = s;
    g_drv.hello_sent = false;
    g_drv.rx.clear();
    g_drv.pending_len = 0;
    send_evt("CONNECTED");
}

static void send_hello_once() {
    if (!g_drv.enabled)
        return;
    if (!internal::tcp_socket_valid(g_drv.sock))
        return;
    if (g_drv.hello_sent)
        return;
    g_drv.hello_sent = true;
    std::ostringstream os;
    os << "EVT name=HELLO"
       << " role=" << g_drv.role
       << " pid="
#ifdef WIN32
       << static_cast<unsigned long>(::GetCurrentProcessId())
#else
       << static_cast<unsigned long>(::getpid())
#endif
       << " rev=" << ENIGMA_GIT_REV;
    send_frame(os.str());
}

static void pump_rx() {
    if (!g_drv.enabled)
        return;
    if (!internal::tcp_socket_valid(g_drv.sock))
        return;
    if (!internal::tcp_pump_recv(g_drv.sock, g_drv.rx)) {
        internal::tcp_close(g_drv.sock);
        g_drv.hello_sent = false;
        g_drv.join_active = false;
        send_evt("DISCONNECTED");
        return;
    }
    std::vector<uint8_t> frame;
    while (internal::tcp_try_extract_frame(g_drv.rx, g_drv.pending_len, frame)) {
        std::string line(reinterpret_cast<const char *>(frame.data()), frame.size());
        handle_command(line);
    }
}

static void poll_join() {
    if (!g_drv.enabled || !g_drv.join_active)
        return;
    ClientJoinStatus st = multiplayer::PollClientJoin();
    if (st == ClientJoinStatus::JOINED) {
        g_drv.join_active = false;
        send_evt("JOINED");
    } else if (st == ClientJoinStatus::FAILED) {
        g_drv.join_active = false;
        send_evt("JOIN_FAILED");
    }
}

}  // namespace

void Configure(const std::string &role, const std::string &connect_host_port) {
    g_drv = DriverState();
    g_drv.role = role;
    g_drv.connect_value = connect_host_port;
    if (g_drv.role.empty() || g_drv.connect_value.empty())
        return;
    std::string host;
    Uint16 port = 0;
    if (!internal::parse_host_port(g_drv.connect_value, host, port, 0) || port == 0)
        return;
    g_drv.controller_host = host;
    g_drv.controller_port = port;
    g_drv.enabled = true;
}

bool Enabled() {
    return g_drv.enabled;
}

void Tick(double dtime) {
    (void)dtime;
    if (!g_drv.enabled)
        return;
    maybe_connect();
    if (!internal::tcp_socket_valid(g_drv.sock))
        return;
    send_hello_once();
    pump_rx();
    poll_join();

    // Apply any input override before the next simulation tick consumes inputs.
    maybe_apply_hold_mouse_force();
    maybe_apply_queued_mouse_forces();
    maybe_apply_queued_local_inputs();
    maybe_apply_input_override();

    if (g_drv.stream_state_interval_ms > 0) {
        Uint32 now = SDL_GetTicks();
        if (now >= g_drv.next_stream_state_ms) {
            emit_state_snapshot();
            g_drv.next_stream_state_ms = now + static_cast<Uint32>(g_drv.stream_state_interval_ms);
        }
    }
}

void Run() {
    // Minimal deterministic-ish tick loop. We intentionally mirror the game loop
    // ordering used in `game::StartGame()`.
    Uint32 last_tick_time = SDL_GetTicks();
    double dtime = 0.0;

    while (!client::AbortGameP() && !app.bossKeyPressed) {
        Tick(dtime);
        multiplayer::Tick(dtime);
        client::Tick(dtime);
        // Ensure override wins even if multiplayer transport enqueued after Tick().
        maybe_apply_queued_mouse_forces();
        maybe_apply_queued_local_inputs();
        maybe_apply_input_override();
        server::Tick(dtime);

        int sleeptime = 10 - (SDL_GetTicks() - last_tick_time);
        if (sleeptime >= 3)
            SDL_Delay(sleeptime);

        Uint32 current_tick_time = SDL_GetTicks();
        dtime = (current_tick_time - last_tick_time) / 1000.0;
        if (std::abs(1 - dtime / 0.01) < 0.2) {
            dtime = 0.01;
            last_tick_time += 10;
        } else {
            last_tick_time = current_tick_time;
        }
        if (dtime > 500.0)
            dtime = 0.0;
    }
    multiplayer::Shutdown();
}

}  // namespace testdriver
}  // namespace multiplayer
}  // namespace enigma
