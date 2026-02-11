#include "multiplayer_rollback.hh"

#include "multiplayer_internal.hh"
#include "multiplayer_sim_snapshot.hh"
#include "options.hh"
#include "server.hh"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <deque>
#include <unordered_map>

namespace enigma {
namespace multiplayer {
namespace rollback {

namespace {

struct HistoryTick {
    std::array<input::PlayerInput, input::kMaxPlayers> inputs;
    std::bitset<input::kMaxPlayers> present;
};

struct Frame {
    uint32_t tick = 0;
    sim_snapshot::Snapshot snap;
};

std::unordered_map<uint32_t, HistoryTick> g_history;
std::deque<Frame> g_frames;

uint32_t g_max_history_tick = 0;
uint32_t g_last_snapshot_tick = UINT32_MAX;
uint32_t g_pending_tick = UINT32_MAX;
bool g_replaying = false;

bool equal_input(const input::PlayerInput &a, const input::PlayerInput &b) {
    return a.mouse_force[0] == b.mouse_force[0] && a.mouse_force[1] == b.mouse_force[1] &&
           a.rotate_steps == b.rotate_steps && a.activate_count == b.activate_count;
}

int keep_ticks() {
    int keep = options::GetInt("MultiplayerDebugRollbackKeepTicks");
    if (keep <= 0)
        keep = 200;
    if (keep > 5000)
        keep = 5000;
    return keep;
}

bool should_enable() {
    if (!multiplayer::IsActive())
        return false;
    if (!input::IsNetworked())
        return false;
    if (!input::ZerofillMissingInputsEnabled())
        return false;
    if (!options::GetBool("MultiplayerDebugRollbackEnabled"))
        return false;
    return true;
}

void prune_history() {
    const int keep = keep_ticks();
    if (keep <= 0)
        return;
    uint32_t prune_before = 0;
    if (g_max_history_tick > static_cast<uint32_t>(keep))
        prune_before = g_max_history_tick - static_cast<uint32_t>(keep);
    for (auto it = g_history.begin(); it != g_history.end();) {
        if (it->first < prune_before)
            it = g_history.erase(it);
        else
            ++it;
    }
}

void prune_frames() {
    const int keep = keep_ticks();
    if (keep <= 0)
        return;
    while (g_frames.size() > static_cast<size_t>(keep))
        g_frames.pop_front();
}

void inject_inputs(uint32_t from_tick, uint32_t to_tick) {
    const unsigned players = input::ExpectedPlayers();
    for (uint32_t tick = from_tick; tick < to_tick; ++tick) {
        auto it = g_history.find(tick);
        if (it == g_history.end())
            continue;
        for (unsigned player = 0; player < players && player < input::kMaxPlayers; ++player) {
            if (!it->second.present.test(player))
                continue;
            input::EnqueueInput(tick, player, it->second.inputs[player]);
        }
    }
}

void store_frame(uint32_t tick, sim_snapshot::Snapshot &&snap) {
    // Keep frames unique by tick so lookups are stable even after replay.
    for (auto it = g_frames.rbegin(); it != g_frames.rend(); ++it) {
        if (it->tick == tick) {
            it->snap = std::move(snap);
            return;
        }
        if (it->tick < tick)
            break;
    }
    Frame f;
    f.tick = tick;
    f.snap = std::move(snap);
    g_frames.push_back(std::move(f));
    prune_frames();
}

bool find_frame(uint32_t tick, Frame &out) {
    for (auto it = g_frames.rbegin(); it != g_frames.rend(); ++it) {
        if (it->tick == tick) {
            out = *it;  // copy snapshot (vectors)
            return true;
        }
        if (it->tick < tick)
            break;
    }
    return false;
}

bool oldest_frame_tick(uint32_t &out) {
    if (g_frames.empty())
        return false;
    out = g_frames.front().tick;
    return true;
}

}  // namespace

bool Enabled() {
    return should_enable();
}

bool IsReplaying() {
    return g_replaying;
}

uint32_t EarliestTick(uint32_t current_tick) {
    if (!should_enable())
        return current_tick;
    const int keep = keep_ticks();
    if (keep <= 0)
        return current_tick;
    if (current_tick > static_cast<uint32_t>(keep))
        return current_tick - static_cast<uint32_t>(keep);
    return 0;
}

void Reset() {
    g_history.clear();
    g_frames.clear();
    g_max_history_tick = 0;
    g_last_snapshot_tick = UINT32_MAX;
    g_pending_tick = UINT32_MAX;
    g_replaying = false;
}

void RecordInput(uint32_t tick, unsigned player, const input::PlayerInput &pi) {
    if (!should_enable())
        return;
    if (g_replaying)
        return;
    if (player >= input::kMaxPlayers)
        return;

    HistoryTick &slot = g_history[tick];
    const bool had = slot.present.test(player);
    const input::PlayerInput prev = slot.inputs[player];
    slot.inputs[player] = pi;
    slot.present.set(player);

    if (tick > g_max_history_tick)
        g_max_history_tick = tick;
    prune_history();

    const uint32_t current_tick = input::CurrentTick();
    if (tick >= current_tick)
        return;

    // Late input: if this tick was already simulated, request a rollback to the
    // earliest affected tick.
    if (!had || !equal_input(prev, pi)) {
        if (g_pending_tick == UINT32_MAX || tick < g_pending_tick)
            g_pending_tick = tick;
    }
}

void OnBeforeSimTick(uint32_t tick) {
    if (!should_enable())
        return;
    if (g_replaying)
        return;
    if (tick == g_last_snapshot_tick)
        return;

    store_frame(tick, sim_snapshot::Capture());
    g_last_snapshot_tick = tick;
}

void MaybeRollback(double timestep) {
    if (!should_enable())
        return;
    if (g_replaying)
        return;
    if (g_pending_tick == UINT32_MAX)
        return;

    const uint32_t target_tick = input::CurrentTick();
    if (g_pending_tick >= target_tick) {
        g_pending_tick = UINT32_MAX;
        return;
    }

    uint32_t rollback_tick = g_pending_tick;
    Frame frame;
    if (!find_frame(rollback_tick, frame)) {
        uint32_t oldest = 0;
        if (!oldest_frame_tick(oldest)) {
            g_pending_tick = UINT32_MAX;
            return;
        }
        if (rollback_tick < oldest)
            rollback_tick = oldest;
        if (!find_frame(rollback_tick, frame)) {
            g_pending_tick = UINT32_MAX;
            return;
        }
    }

    // Clear pending first: replay may trigger further late inputs.
    g_pending_tick = UINT32_MAX;

    internal::debug_log("mp rollback: from tick=%u to tick=%u", rollback_tick, target_tick);

    g_replaying = true;
    sim_snapshot::Restore(frame.snap);

    // After restoring, re-inject the best-known inputs (including those that
    // arrived after the snapshot was taken).
    inject_inputs(rollback_tick, target_tick);

    // Replay forward to the original tick.
    while (input::CurrentTick() < target_tick) {
        const uint32_t t = input::CurrentTick();
        store_frame(t, sim_snapshot::Capture());
        g_last_snapshot_tick = t;

        server::SimulateOneTick(timestep);
    }

    g_replaying = false;
}

}  // namespace rollback
}  // namespace multiplayer
}  // namespace enigma
