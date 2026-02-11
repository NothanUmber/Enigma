#include "input.hh"

#include <bitset>
#include <cstdlib>
#include <map>

namespace enigma {
namespace input {

namespace {

struct TickInputs {
    std::array<PlayerInput, kMaxPlayers> inputs;
    std::bitset<kMaxPlayers> present;
};

bool g_networked = false;
bool g_zerofill_missing_inputs = false;
unsigned g_expected_players = 1;
uint32_t g_current_tick = 0;
std::array<PlayerInput, kMaxPlayers> g_local_pending;
std::map<uint32_t, TickInputs> g_queue;

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
    g_expected_players = 1;
    for (auto &pending : g_local_pending)
        pending = PlayerInput();
}

void SetNetworked(bool enabled) {
    g_networked = enabled;
    g_zerofill_missing_inputs = enabled && env_bool_numeric("ENIGMA_MP_ZEROFILL_INPUTS");
}

bool IsNetworked() {
    return g_networked;
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
    if (it == g_queue.end() || !it->second.present.test(player))
        return empty_input();
    PlayerInput input = it->second.inputs[player];
    it->second.inputs[player] = PlayerInput();
    it->second.present.reset(player);
    if (it->second.present.none())
        g_queue.erase(it);
    return input;
}

void AdvanceTick() {
    ++g_current_tick;
}

}  // namespace input
}  // namespace enigma
