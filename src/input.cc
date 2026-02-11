#include "input.hh"

#include "options.hh"

#include <bitset>
#include <cstdlib>
#include <map>
#include <vector>

namespace enigma {
namespace input {

namespace {

struct TickInputs {
    std::array<PlayerInput, kMaxPlayers> inputs;
    std::bitset<kMaxPlayers> present;
};

bool g_networked = false;
bool g_zerofill_missing_inputs = false;
unsigned g_predict_missing_mouse_ticks = 0;
unsigned g_expected_players = 1;
uint32_t g_current_tick = 0;
std::array<PlayerInput, kMaxPlayers> g_local_pending;
std::map<uint32_t, TickInputs> g_queue;
std::array<PlayerInput, kMaxPlayers> g_last_consumed;
std::array<unsigned, kMaxPlayers> g_missing_streak;

bool env_bool_numeric(const char *name) {
    const char *value = std::getenv(name);
    if (!value || !*value)
        return false;
    char *end = nullptr;
    long v = std::strtol(value, &end, 10);
    if (end == value)
        return false;
    return v != 0;
}

unsigned env_uint(const char *name, unsigned minv, unsigned maxv) {
    const char *value = std::getenv(name);
    if (!value || !*value)
        return 0;
    char *end = nullptr;
    unsigned long v = std::strtoul(value, &end, 10);
    if (end == value)
        return 0;
    if (v < minv)
        v = minv;
    if (v > maxv)
        v = maxv;
    return static_cast<unsigned>(v);
}

PlayerInput empty_input() {
    return PlayerInput();
}

bool valid_player(unsigned player) {
    return player < kMaxPlayers;
}

}  // namespace

void Reset() {
    g_current_tick = 0;
    g_queue.clear();
    g_networked = false;
    g_zerofill_missing_inputs = false;
    g_predict_missing_mouse_ticks = 0;
    g_expected_players = 1;
    for (auto &pending : g_local_pending)
        pending = PlayerInput();
    for (auto &entry : g_last_consumed)
        entry = PlayerInput();
    g_missing_streak.fill(0);
}

void SetNetworked(bool enabled) {
    g_networked = enabled;
    bool zerofill = false;
    if (const char *env = std::getenv("ENIGMA_MP_ZEROFILL_INPUTS")) {
        (void)env;
        zerofill = env_bool_numeric("ENIGMA_MP_ZEROFILL_INPUTS");
    } else {
        zerofill = options::GetBool("MultiplayerDebugZeroFillInputs");
    }
    g_zerofill_missing_inputs = enabled && zerofill;
    // When the lockstep does not stall on missing inputs, allow predicting missing
    // mouse-force inputs by holding the last consumed value for a few ticks.
    // This reduces visible stutter for a player on a lossy connection, without
    // repeating discrete actions like rotate/activate.
    g_predict_missing_mouse_ticks =
        (enabled && g_zerofill_missing_inputs)
            ? (std::getenv("ENIGMA_MP_PREDICT_MISSING_MOUSE_TICKS")
                   ? env_uint("ENIGMA_MP_PREDICT_MISSING_MOUSE_TICKS", 0, 20)
                   : static_cast<unsigned>(options::GetInt("MultiplayerDebugPredictMissingMouseTicks")))
            : 0;
}

bool IsNetworked() {
    return g_networked;
}

bool ZerofillMissingInputsEnabled() {
    return g_networked && g_zerofill_missing_inputs;
}

void SetExpectedPlayers(unsigned count) {
    g_expected_players = count;
}

unsigned ExpectedPlayers() {
    return g_expected_players;
}

uint32_t CurrentTick() {
    return g_current_tick;
}

void SubmitMouseForce(unsigned player, const ecl::V2 &force) {
    if (!valid_player(player))
        return;
    g_local_pending[player].mouse_force += force;
}

void SubmitRotateInventory(unsigned player, int dir) {
    if (!valid_player(player))
        return;
    g_local_pending[player].rotate_steps += dir;
}

void SubmitActivateItem(unsigned player) {
    if (!valid_player(player))
        return;
    g_local_pending[player].activate_count += 1;
}

PlayerInput DrainLocalPending(unsigned player) {
    if (!valid_player(player))
        return empty_input();
    PlayerInput pending = g_local_pending[player];
    g_local_pending[player] = PlayerInput();
    return pending;
}

void EnqueueInput(uint32_t tick, unsigned player, const PlayerInput &input) {
    if (!valid_player(player))
        return;
    TickInputs &slot = g_queue[tick];
    // In multiplayer each (tick, player) should be treated as a single input
    // sample. When transport redundancy is used (re-sending a few ticks),
    // duplicates must not accumulate.
    slot.inputs[player] = input;
    slot.present.set(player);
}

bool HasInput(uint32_t tick, unsigned player) {
    if (!valid_player(player))
        return false;
    auto it = g_queue.find(tick);
    if (it == g_queue.end())
        return false;
    return it->second.present.test(player);
}

bool PeekInput(uint32_t tick, unsigned player, PlayerInput &out) {
    if (!valid_player(player))
        return false;
    auto it = g_queue.find(tick);
    if (it == g_queue.end())
        return false;
    if (!it->second.present.test(player))
        return false;
    out = it->second.inputs[player];
    return true;
}

bool CanAdvanceTick() {
    if (!g_networked)
        return true;
    if (g_expected_players == 0)
        return false;
    if (g_zerofill_missing_inputs)
        return true;
    for (unsigned player = 0; player < g_expected_players; ++player) {
        if (!HasInput(g_current_tick, player))
            return false;
    }
    return true;
}

PlayerInput ConsumeInput(uint32_t tick, unsigned player) {
    if (!valid_player(player))
        return empty_input();
    if (!g_networked)
        return DrainLocalPending(player);
    auto it = g_queue.find(tick);
    if (it == g_queue.end() || !it->second.present.test(player)) {
        if (!g_zerofill_missing_inputs)
            return empty_input();
        PlayerInput predicted;
        // Only predict continuous mouse force, never repeat discrete actions.
        predicted.rotate_steps = 0;
        predicted.activate_count = 0;
        if (g_predict_missing_mouse_ticks == 0) {
            predicted.mouse_force = ecl::V2(0, 0);
        } else {
            unsigned streak = g_missing_streak[player] + 1;
            g_missing_streak[player] = streak;
            float factor = 0.0f;
            if (streak <= g_predict_missing_mouse_ticks) {
                // Linear decay to zero over N missing ticks.
                factor = static_cast<float>(g_predict_missing_mouse_ticks - (streak - 1)) /
                         static_cast<float>(g_predict_missing_mouse_ticks);
            }
            predicted.mouse_force = g_last_consumed[player].mouse_force * factor;
        }
        return predicted;
    }
    PlayerInput input = it->second.inputs[player];
    it->second.inputs[player] = PlayerInput();
    it->second.present.reset(player);
    if (it->second.present.none())
        g_queue.erase(it);
    g_last_consumed[player] = input;
    g_missing_streak[player] = 0;
    return input;
}

void AdvanceTick() {
    ++g_current_tick;
}

Snapshot CaptureSnapshot() {
    Snapshot snap;
    snap.networked = g_networked;
    snap.zerofill_missing_inputs = g_zerofill_missing_inputs;
    snap.predict_missing_mouse_ticks = g_predict_missing_mouse_ticks;
    snap.expected_players = g_expected_players;
    snap.current_tick = g_current_tick;
    snap.local_pending = g_local_pending;
    snap.last_consumed = g_last_consumed;
    snap.missing_streak = g_missing_streak;
    snap.queue.clear();
    snap.queue.reserve(g_queue.size());
    for (const auto &kv : g_queue) {
        TickInputsSnapshot e;
        e.tick = kv.first;
        e.inputs = kv.second.inputs;
        e.present = kv.second.present;
        snap.queue.push_back(e);
    }
    return snap;
}

void RestoreSnapshot(const Snapshot &snap) {
    g_networked = snap.networked;
    g_zerofill_missing_inputs = snap.zerofill_missing_inputs;
    g_predict_missing_mouse_ticks = snap.predict_missing_mouse_ticks;
    g_expected_players = snap.expected_players;
    g_current_tick = snap.current_tick;
    g_local_pending = snap.local_pending;
    g_last_consumed = snap.last_consumed;
    g_missing_streak = snap.missing_streak;
    g_queue.clear();
    for (const auto &e : snap.queue) {
        TickInputs &slot = g_queue[e.tick];
        slot.inputs = e.inputs;
        slot.present = e.present;
    }
}

}  // namespace input
}  // namespace enigma
