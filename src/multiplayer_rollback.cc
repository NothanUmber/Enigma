#include "multiplayer_rollback.hh"

#include "multiplayer_internal.hh"
#include "multiplayer_protocol.hh"
#include "multiplayer_session_impl.hh"
#include "multiplayer_sim_snapshot.hh"
#include "options.hh"
#include "actors.hh"
#include "player.hh"
#include "server.hh"
#include "display.hh"

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

uint32_t stable_name_hash(Actor *actor) {
    if (!actor)
        return 0;
    Value name = actor->getAttr("name");
    if (name.getType() != Value::STRING)
        return 0;
    const std::string &s = name.get_string();
    if (s.empty())
        return 0;
    // FNV-1a 32-bit (keep in sync with multiplayer_session_sync.cc).
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= static_cast<uint32_t>(c);
        h *= 16777619u;
    }
    return h ? h : 1u;
}

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
bool g_pending_resync = false;
protocol::ResyncState g_pending_resync_state;
bool g_replaying = false;

uint32_t g_last_auto_delay_bump_tick = 0;

bool equal_input(const input::PlayerInput &a, const input::PlayerInput &b) {
    return a.mouse_force[0] == b.mouse_force[0] && a.mouse_force[1] == b.mouse_force[1] &&
           a.rotate_steps == b.rotate_steps && a.activate_count == b.activate_count;
}

int keep_ticks() {
    int keep = options::GetInt("MultiplayerDebugRollbackKeepTicks");
    if (keep <= 0)
        keep = 200;
    // Rollback must cover (1) the input delay we stamp into the future, plus
    // (2) the resend back-window for input bundles, plus a small jitter margin.
    // Otherwise the host will frequently receive client inputs "too late to replay",
    // simulate with zero inputs, and the client will snap back on resync.
    int min_keep = 0;
    if (input::IsNetworked() && input::ZerofillMissingInputsEnabled()) {
        const uint32_t delay = internal::g_session.input_delay ? internal::g_session.input_delay : internal::kInputDelay;
        uint32_t back = internal::kInputBundleBackTicksDirect;
        switch (internal::g_session.active_transport) {
        case TransportKind::UDP_RELAY:
            back = internal::kInputBundleBackTicksUdpRelay;
            break;
        case TransportKind::TCP_RELAY:
            back = internal::kInputBundleBackTicksTcpRelay;
            break;
        case TransportKind::DIRECT:
        default:
            back = internal::kInputBundleBackTicksDirect;
            break;
        }
        min_keep = static_cast<int>(delay + back + 10);
    }
    if (min_keep > 0 && keep < min_keep)
        keep = min_keep;
    if (keep > 5000)
        keep = 5000;
    return keep;
}

bool should_enable() {
    if (!multiplayer::IsActive())
        return false;
    if (!input::IsNetworked())
        return false;
    // In an authoritative-host model the host must remain monotonic (no rewinding).
    // Only clients use rollback/replay for resync reconciliation.
    if (internal::g_session.host)
        return false;
    if (!input::ZerofillMissingInputsEnabled())
        return false;
    if (!options::GetBool("MultiplayerDebugRollbackEnabled"))
        return false;
    return true;
}

const Frame *find_frame_ptr(uint32_t tick) {
    for (auto it = g_frames.rbegin(); it != g_frames.rend(); ++it) {
        if (it->tick == tick)
            return &(*it);
        if (it->tick < tick)
            break;
    }
    return nullptr;
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
    g_pending_resync = false;
    g_replaying = false;
}

bool TryQueueReconcileResyncState(const protocol::ResyncState &state) {
    if (!should_enable())
        return false;
    if (internal::g_session.host)
        return false;
    if (g_replaying)
        return false;
    if (state.epoch != internal::g_session.input_epoch)
        return false;
    if (!internal::g_session.local_player_known ||
        internal::g_session.local_player >= internal::g_session.expected_players) {
        return false;
    }

    const uint32_t local_tick = input::CurrentTick();
    if (state.tick > local_tick)
        return false;

    const Frame *f = find_frame_ptr(state.tick);
    if (!f)
        return false;

    Actor *local_actor = player::GetMainActor(internal::g_session.local_player);
    if (!local_actor)
        return false;
    const int local_id = local_actor->getId();

    const sim_snapshot::ActorSnapshot *pred = nullptr;
    for (const auto &a : f->snap.actors) {
        if (a.object_id == local_id) {
            pred = &a;
            break;
        }
    }
    if (!pred)
        return false;

    const float pred_x = static_cast<float>(pred->info.pos[0]);
    const float pred_y = static_cast<float>(pred->info.pos[1]);

    const protocol::ResyncActorState *auth = nullptr;
    const char *auth_match = nullptr;
    // Pass 1: match by object_id (best fidelity, but can diverge across peers).
    for (const auto &a : state.actors) {
        if (static_cast<int>(a.object_id) == local_id) {
            auth = &a;
            auth_match = "object_id";
            break;
        }
    }
    // Pass 2: match by (kind, stable name hash) when available (more robust).
    if (!auth) {
        const uint32_t nh = stable_name_hash(local_actor);
        if (nh) {
            const uint16_t kind = static_cast<uint16_t>(get_id(local_actor));
            const protocol::ResyncActorState *best = nullptr;
            double best_d2 = 0.0;
            for (const auto &a : state.actors) {
                if (a.name_hash != nh || a.actor_id != kind)
                    continue;
                const double dx = static_cast<double>(pred_x) - static_cast<double>(a.x);
                const double dy = static_cast<double>(pred_y) - static_cast<double>(a.y);
                const double d2 = dx * dx + dy * dy;
                if (!best || d2 < best_d2) {
                    best = &a;
                    best_d2 = d2;
                }
            }
            auth = best;
            if (auth)
                auth_match = "name";
        }
    }
    // Pass 3: match by (kind, owner) (fallback for unnamed multi-actor levels).
    if (!auth) {
        const uint16_t kind = static_cast<uint16_t>(get_id(local_actor));
        const int owner = static_cast<int>(internal::g_session.local_player);
        const protocol::ResyncActorState *best = nullptr;
        double best_d2 = 0.0;
        for (const auto &a : state.actors) {
            if (a.actor_id != kind)
                continue;
            int a_owner = (a.owner == 0xFFFF) ? -1 : static_cast<int>(a.owner);
            if (a_owner != owner)
                continue;
            const double dx = static_cast<double>(pred_x) - static_cast<double>(a.x);
            const double dy = static_cast<double>(pred_y) - static_cast<double>(a.y);
            const double d2 = dx * dx + dy * dy;
            if (!best || d2 < best_d2) {
                best = &a;
                best_d2 = d2;
            }
        }
        auth = best;
        if (auth)
            auth_match = "owner";
    }
    // Pass 4: match by kind only.
    if (!auth) {
        const uint16_t kind = static_cast<uint16_t>(get_id(local_actor));
        const protocol::ResyncActorState *best = nullptr;
        double best_d2 = 0.0;
        for (const auto &a : state.actors) {
            if (a.actor_id != kind)
                continue;
            const double dx = static_cast<double>(pred_x) - static_cast<double>(a.x);
            const double dy = static_cast<double>(pred_y) - static_cast<double>(a.y);
            const double d2 = dx * dx + dy * dy;
            if (!best || d2 < best_d2) {
                best = &a;
                best_d2 = d2;
            }
        }
        auth = best;
        if (auth)
            auth_match = "kind";
    }
    if (!auth)
        return false;

    const float dx = pred_x - auth->x;
    const float dy = pred_y - auth->y;
    const float d2 = dx * dx + dy * dy;
    const float err = static_cast<float>(std::sqrt(d2));

    // Auto-tune input delay for this client when we observe large authoritative
    // corrections for the locally controlled actor. This reduces late/missing
    // inputs at the host (and thus visible "kicks" and rubber-banding), at the
    // cost of higher input latency. Only increases during a session.
    {
        const uint32_t now = input::CurrentTick();
        const uint32_t min_bump_interval_ticks = 100;  // ~1s at 10ms/tick.
        const uint32_t min_tick = 50;                  // ignore early join noise.
        const uint32_t max_delay = std::min<uint32_t>(internal::kMaxInputLead, 30u);
        if (now > min_tick && err >= 0.75f && internal::g_session.input_delay < max_delay &&
            (g_last_auto_delay_bump_tick == 0 ||
             now - g_last_auto_delay_bump_tick >= min_bump_interval_ticks)) {
            uint32_t step = (err >= 1.5f) ? 2u : 1u;
            uint32_t old = internal::g_session.input_delay;
            uint32_t next = old + step;
            if (next > max_delay)
                next = max_delay;
            if (next != old) {
                internal::g_session.input_delay = next;
                g_last_auto_delay_bump_tick = now;
                internal::debug_log("mp auto input delay bump: %u -> %u (err=%.3f)",
                                    static_cast<unsigned>(old),
                                    static_cast<unsigned>(next),
                                    static_cast<double>(err));
            }
        }
    }

    // Only pay the cost of rollback+replay when the local prediction at that tick
    // meaningfully differs from the authoritative snapshot.
    if (d2 < (0.25f * 0.25f))
        return false;

    // Coalesce: keep the newest queued authoritative snapshot.
    if (!g_pending_resync || state.tick >= g_pending_resync_state.tick) {
        g_pending_resync_state = state;
        g_pending_resync = true;
    }
    internal::debug_log("mp resync reconcile queued: tick=%u match=%s err=%.3f",
                        static_cast<unsigned>(state.tick),
                        auth_match ? auth_match : "unknown",
                        static_cast<double>(err));
    return true;
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

    (void)had;
    (void)prev;
    // Do not roll back on late inputs: under lossy/jittery links this causes the
    // host (and clients) to constantly rewrite history and "jump around". Instead,
    // reconcile only when authoritative snapshots arrive (g_pending_resync).
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
    if (g_pending_tick == UINT32_MAX && !g_pending_resync)
        return;

    const uint32_t target_tick = input::CurrentTick();
    uint32_t rollback_tick = g_pending_tick;
    bool apply_authoritative = false;
    protocol::ResyncState authoritative;
    if (g_pending_resync) {
        if (rollback_tick == UINT32_MAX || g_pending_resync_state.tick <= rollback_tick) {
            rollback_tick = g_pending_resync_state.tick;
            authoritative = g_pending_resync_state;
            apply_authoritative = true;
        }
    }
    if (rollback_tick == UINT32_MAX)
        return;
    if (rollback_tick >= target_tick) {
        if (rollback_tick == g_pending_tick)
            g_pending_tick = UINT32_MAX;
        if (apply_authoritative)
            g_pending_resync = false;
        return;
    }

    Frame frame;
    if (!find_frame(rollback_tick, frame)) {
        uint32_t oldest = 0;
        if (!oldest_frame_tick(oldest)) {
            g_pending_tick = UINT32_MAX;
            g_pending_resync = false;
            return;
        }
        if (rollback_tick < oldest)
            rollback_tick = oldest;
        if (!find_frame(rollback_tick, frame)) {
            g_pending_tick = UINT32_MAX;
            g_pending_resync = false;
            return;
        }
    }

    // Clear pending first: replay may trigger further late inputs.
    if (rollback_tick == g_pending_tick)
        g_pending_tick = UINT32_MAX;
    if (apply_authoritative)
        g_pending_resync = false;

    internal::debug_log("mp rollback: from tick=%u to tick=%u", rollback_tick, target_tick);

    g_replaying = true;
    sim_snapshot::Restore(frame.snap);

    if (apply_authoritative) {
        // Apply the authoritative host snapshot at the rollback tick, then replay
        // locally known inputs forward to "now".
        internal::apply_resync_state(authoritative);
    }

    // After restoring, re-inject the best-known inputs (including those that
    // arrived after the snapshot was taken).
    inject_inputs(rollback_tick, target_tick);

    // Replay forward to the original tick.
    while (input::CurrentTick() < target_tick) {
        const uint32_t t = input::CurrentTick();
        store_frame(t, sim_snapshot::Capture());
        g_last_snapshot_tick = t;

        // Mirror the main loop order used by the test driver and typical gameplay:
        // advance model animations (and their callbacks) before the next sim tick.
        // This matters because some objects (e.g. balls) still couple gameplay state
        // transitions to animation callbacks.
        display::Tick(timestep);
        server::SimulateOneTick(timestep);
    }

    g_replaying = false;
}

}  // namespace rollback
}  // namespace multiplayer
}  // namespace enigma
