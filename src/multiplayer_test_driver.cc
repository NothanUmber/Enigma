#include "multiplayer_test_driver.hh"

#include "multiplayer.hh"
#include "multiplayer_internal.hh"
#include "multiplayer_protocol.hh"
#include "multiplayer_rollback.hh"

#include "client.hh"
#include "game.hh"
#include "input.hh"
#include "lev/Proxy.hh"
#include "main.hh"
#include "options.hh"
#include "player.hh"
#include "server.hh"
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
    os << " " << prefix << "valid=1"
       << " " << prefix << "kind=" << a->getKind()
       << " " << prefix << "obj=" << a->getId()
       << " " << prefix << "ctrl=" << a->get_controllers()
       << " " << prefix << "mf=" << a->get_mouseforce()
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
       << " ovr_left=" << static_cast<int>(g_drv.input_override_ticks_left);

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

    os << " mp_clock_tick=" << static_cast<unsigned>(s.input_clock_tick)
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
