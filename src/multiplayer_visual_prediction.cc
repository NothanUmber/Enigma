#include "multiplayer_state.hh"

#include "actors.hh"
#include "display.hh"
#include "input.hh"
#include "multiplayer_internal.hh"
#include "multiplayer_rollback.hh"
#include "multiplayer_sim_snapshot.hh"
#include "options.hh"
#include "player.hh"
#include "server.hh"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>

namespace enigma {
namespace multiplayer {

namespace {

constexpr size_t kTracePlayers = 2;
constexpr uint32_t kTraceKeepTicks = 512;
constexpr double kTracePosEpsilon = 0.01;
constexpr double kTraceVelEpsilon = 0.05;
constexpr double kTraceForceEpsilon = 0.05;
constexpr unsigned kTraceMaxLogs = 64;

struct TraceActorState {
    bool valid = false;
    int object_id = -1;
    int controllers = 0;
    ecl::V2 pos;
    ecl::V2 vel;
    ecl::V2 force;
    ecl::V2 collforce;
    int contacts_count = 0;
    int last_contacts_count = 0;
};

struct PredictedTickTrace {
    uint32_t tick = 0;
    std::array<bool, kTracePlayers> input_present = {};
    std::array<input::PlayerInput, kTracePlayers> inputs = {};
    std::array<TraceActorState, kTracePlayers> pre_actors = {};
    std::array<TraceActorState, kTracePlayers> post_actors = {};
};

struct PredictionState {
    struct GridModelSnapshot {
        GridLayer layer = GRID_COUNT;
        uint16_t x = 0;
        uint16_t y = 0;
        std::unique_ptr<::display::Model> model;

        GridModelSnapshot() = default;
        GridModelSnapshot(const GridModelSnapshot &other)
        : layer(other.layer),
          x(other.x),
          y(other.y),
          model(other.model ? std::unique_ptr<::display::Model>(other.model->clone()) : nullptr) {
        }

        GridModelSnapshot &operator=(const GridModelSnapshot &other) {
            if (this == &other)
                return *this;
            layer = other.layer;
            x = other.x;
            y = other.y;
            model.reset(other.model ? other.model->clone() : nullptr);
            return *this;
        }
    };

    bool render_active = false;
    bool simulation_active = false;
    uint32_t last_delay_ticks = std::numeric_limits<uint32_t>::max();
    sim_snapshot::Snapshot truth_snapshot_for_render;
    std::vector<GridModelSnapshot> truth_grid_models_for_render;
    std::deque<PredictedTickTrace> trace_history;
    uint32_t truth_pre_tick = std::numeric_limits<uint32_t>::max();
    std::array<TraceActorState, kTracePlayers> truth_pre_actors = {};
    unsigned trace_log_count = 0;
};

PredictionState g_prediction;

bool trace_prediction_enabled() {
    const char *env = std::getenv("ENIGMA_MP_TRACE_PREDICT_ACTORS");
    return env && *env;
}

TraceActorState capture_trace_actor_state(unsigned player) {
    TraceActorState state;
    Actor *actor = player::GetMainActor(player);
    if (!actor)
        return state;
    state.valid = true;
    state.object_id = actor->getId();
    state.controllers = actor->get_controllers();
    state.pos = actor->get_pos();
    state.vel = actor->get_vel();
    if (const ActorInfo *ai = actor->get_actorinfo()) {
        state.force = ai->force;
        state.collforce = ai->collforce;
        state.contacts_count = ai->contacts_count;
        state.last_contacts_count = ai->last_contacts_count;
    }
    return state;
}

bool trace_input_present(uint32_t tick, unsigned player, input::PlayerInput &out) {
    out = input::PlayerInput();
    return input::PeekInput(tick, player, out);
}

void prune_trace_history(uint32_t tick_floor) {
    while (!g_prediction.trace_history.empty() &&
           g_prediction.trace_history.front().tick + kTraceKeepTicks < tick_floor) {
        g_prediction.trace_history.pop_front();
    }
}

void store_predicted_tick_trace(const PredictedTickTrace &entry) {
    if (!trace_prediction_enabled())
        return;
    prune_trace_history(entry.tick);
    for (PredictedTickTrace &existing : g_prediction.trace_history) {
        if (existing.tick == entry.tick) {
            existing = entry;
            return;
        }
    }
    g_prediction.trace_history.push_back(entry);
}

const PredictedTickTrace *find_predicted_tick_trace(uint32_t tick) {
    for (auto it = g_prediction.trace_history.rbegin(); it != g_prediction.trace_history.rend(); ++it) {
        if (it->tick == tick)
            return &*it;
    }
    return nullptr;
}

bool differs(const ecl::V2 &a, const ecl::V2 &b, double eps) {
    return std::fabs(a[0] - b[0]) > eps || std::fabs(a[1] - b[1]) > eps;
}

bool actor_trace_differs(const TraceActorState &predicted, const TraceActorState &truth) {
    if (predicted.valid != truth.valid)
        return true;
    if (!predicted.valid)
        return false;
    if (predicted.object_id != truth.object_id || predicted.controllers != truth.controllers)
        return true;
    if (differs(predicted.pos, truth.pos, kTracePosEpsilon))
        return true;
    if (differs(predicted.vel, truth.vel, kTraceVelEpsilon))
        return true;
    if (differs(predicted.force, truth.force, kTraceForceEpsilon))
        return true;
    if (differs(predicted.collforce, truth.collforce, kTraceForceEpsilon))
        return true;
    if (predicted.contacts_count != truth.contacts_count)
        return true;
    if (predicted.last_contacts_count != truth.last_contacts_count)
        return true;
    return false;
}

void log_tick_trace_mismatch(uint32_t tick, const PredictedTickTrace &predicted) {
    if (!trace_prediction_enabled() || !internal::debug_enabled())
        return;
    if (g_prediction.trace_log_count >= kTraceMaxLogs)
        return;

    const input::PlayerInput last0 = predicted.inputs[0];
    const input::PlayerInput last1 = predicted.inputs[1];
    const TraceActorState truth0 = capture_trace_actor_state(0);
    const TraceActorState truth1 = capture_trace_actor_state(1);
    const TraceActorState &pred0 = predicted.post_actors[0];
    const TraceActorState &pred1 = predicted.post_actors[1];
    const TraceActorState &pre0 = predicted.pre_actors[0];
    const TraceActorState &pre1 = predicted.pre_actors[1];
    const TraceActorState &truth_pre0 = g_prediction.truth_pre_actors[0];
    const TraceActorState &truth_pre1 = g_prediction.truth_pre_actors[1];

    internal::debug_log(
        "vp actor mismatch tick=%u "
        "in0=%d fx0=%.3f fy0=%.3f in1=%d fx1=%.3f fy1=%.3f "
        "pred_pre_p0=(%.3f,%.3f|%.3f,%.3f|%.3f,%.3f|%.3f,%.3f c=%d/%d) "
        "pred_post_p0=(%.3f,%.3f|%.3f,%.3f|%.3f,%.3f|%.3f,%.3f c=%d/%d) "
        "truth_pre_p0=(%.3f,%.3f|%.3f,%.3f|%.3f,%.3f|%.3f,%.3f c=%d/%d) "
        "truth_post_p0=(%.3f,%.3f|%.3f,%.3f|%.3f,%.3f|%.3f,%.3f c=%d/%d) "
        "pred_pre_p1=(%.3f,%.3f|%.3f,%.3f|%.3f,%.3f|%.3f,%.3f c=%d/%d) "
        "pred_post_p1=(%.3f,%.3f|%.3f,%.3f|%.3f,%.3f|%.3f,%.3f c=%d/%d) "
        "truth_pre_p1=(%.3f,%.3f|%.3f,%.3f|%.3f,%.3f|%.3f,%.3f c=%d/%d) "
        "truth_post_p1=(%.3f,%.3f|%.3f,%.3f|%.3f,%.3f|%.3f,%.3f c=%d/%d)",
        tick,
        predicted.input_present[0] ? 1 : 0,
        static_cast<double>(last0.mouse_force[0]),
        static_cast<double>(last0.mouse_force[1]),
        predicted.input_present[1] ? 1 : 0,
        static_cast<double>(last1.mouse_force[0]),
        static_cast<double>(last1.mouse_force[1]),
        pre0.pos[0], pre0.pos[1], pre0.vel[0], pre0.vel[1], pre0.force[0], pre0.force[1],
        pre0.collforce[0], pre0.collforce[1], pre0.contacts_count, pre0.last_contacts_count,
        pred0.pos[0], pred0.pos[1], pred0.vel[0], pred0.vel[1], pred0.force[0], pred0.force[1],
        pred0.collforce[0], pred0.collforce[1], pred0.contacts_count, pred0.last_contacts_count,
        truth_pre0.pos[0], truth_pre0.pos[1], truth_pre0.vel[0], truth_pre0.vel[1],
        truth_pre0.force[0], truth_pre0.force[1], truth_pre0.collforce[0], truth_pre0.collforce[1],
        truth_pre0.contacts_count, truth_pre0.last_contacts_count,
        truth0.pos[0], truth0.pos[1], truth0.vel[0], truth0.vel[1], truth0.force[0], truth0.force[1],
        truth0.collforce[0], truth0.collforce[1], truth0.contacts_count, truth0.last_contacts_count,
        pre1.pos[0], pre1.pos[1], pre1.vel[0], pre1.vel[1], pre1.force[0], pre1.force[1],
        pre1.collforce[0], pre1.collforce[1], pre1.contacts_count, pre1.last_contacts_count,
        pred1.pos[0], pred1.pos[1], pred1.vel[0], pred1.vel[1], pred1.force[0], pred1.force[1],
        pred1.collforce[0], pred1.collforce[1], pred1.contacts_count, pred1.last_contacts_count,
        truth_pre1.pos[0], truth_pre1.pos[1], truth_pre1.vel[0], truth_pre1.vel[1],
        truth_pre1.force[0], truth_pre1.force[1], truth_pre1.collforce[0], truth_pre1.collforce[1],
        truth_pre1.contacts_count, truth_pre1.last_contacts_count,
        truth1.pos[0], truth1.pos[1], truth1.vel[0], truth1.vel[1], truth1.force[0], truth1.force[1],
        truth1.collforce[0], truth1.collforce[1], truth1.contacts_count, truth1.last_contacts_count);
    g_prediction.trace_log_count += 1;
}

uint32_t configured_delay_ticks() {
    return static_cast<uint32_t>(std::max(0, options::GetInt("MultiplayerDebugInputDelayTicks")));
}

bool prediction_enabled_now() {
    return IsActive() && input::IsNetworked() &&
           options::GetBool("MultiplayerDebugVisualPrediction");
}

void clear_prediction_cache() {
    g_prediction.trace_history.clear();
    g_prediction.truth_pre_tick = std::numeric_limits<uint32_t>::max();
    g_prediction.truth_pre_actors = {};
    g_prediction.trace_log_count = 0;
}

void maybe_invalidate_for_config_change() {
    const uint32_t delay = configured_delay_ticks();
    if (g_prediction.last_delay_ticks == delay)
        return;
    g_prediction.last_delay_ticks = delay;
    clear_prediction_cache();
}

void capture_grid_models_for_render(std::vector<PredictionState::GridModelSnapshot> &out) {
    out.clear();
    const int w = Width();
    const int h = Height();
    if (w <= 0 || h <= 0)
        return;
    out.reserve(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    const GridLayer layers[] = {GRID_FLOOR, GRID_ITEMS, GRID_STONES};
    for (GridLayer layer : layers) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                ::display::Model *model = ::display::GetModel(GridLoc(layer, GridPos(x, y)));
                if (!model)
                    continue;
                PredictionState::GridModelSnapshot entry;
                entry.layer = layer;
                entry.x = static_cast<uint16_t>(x);
                entry.y = static_cast<uint16_t>(y);
                entry.model.reset(model->clone());
                out.push_back(std::move(entry));
            }
        }
    }
}

void restore_grid_models_for_render(const std::vector<PredictionState::GridModelSnapshot> &snapshot) {
    const int w = Width();
    const int h = Height();
    if (w <= 0 || h <= 0)
        return;
    const GridLayer layers[] = {GRID_FLOOR, GRID_ITEMS, GRID_STONES};
    for (GridLayer layer : layers) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x)
                ::display::KillModel(GridLoc(layer, GridPos(x, y)));
        }
    }
    for (const auto &entry : snapshot) {
        if (!entry.model)
            continue;
        ::display::SetModel(GridLoc(entry.layer,
                                    GridPos(static_cast<int>(entry.x), static_cast<int>(entry.y))),
                            entry.model->clone());
    }
}

bool get_local_predicted_input(uint32_t source_tick, input::PlayerInput &out) {
    out = input::PlayerInput();
    if (!internal::g_session.active || !internal::g_session.local_player_known)
        return false;

    const auto it = internal::g_session.local_history.find(source_tick);
    if (it != internal::g_session.local_history.end()) {
        out = it->second;
        return true;
    }

    if (source_tick != internal::g_session.next_local_tick)
        return false;

    input::PlayerInput pending;
    if (!input::PeekLocalPending(internal::g_session.local_player, pending))
        return false;
    if (pending.empty())
        return false;
    out = pending;
    return true;
}

void rewrite_inputs_for_predicted_tick(uint32_t tick, uint32_t delay_ticks) {
    (void)delay_ticks;
    const unsigned players = input::ExpectedPlayers();
    const unsigned local_player = LocalPlayer();
    for (unsigned player = 0; player < players && player < input::kMaxPlayers; ++player) {
        input::PlayerInput predicted_input;
        if (player == local_player) {
            const uint32_t source_tick = tick;
            if (!get_local_predicted_input(source_tick, predicted_input))
                rollback::GetRecordedInput(source_tick, player, predicted_input);
        }
        input::EnqueueInput(tick, player, predicted_input);
    }
}

bool rebuild_predicted_world_for_render(uint32_t current_tick, double timestep) {
    const uint32_t delay_ticks = configured_delay_ticks();
    if (delay_ticks == 0)
        return false;

    g_prediction.simulation_active = true;
    const uint32_t target_tick = current_tick + delay_ticks;
    while (input::CurrentTick() < target_tick) {
        const uint32_t replay_tick = input::CurrentTick();
        rewrite_inputs_for_predicted_tick(replay_tick, delay_ticks);
        PredictedTickTrace trace_entry;
        if (trace_prediction_enabled()) {
            trace_entry.tick = replay_tick;
            for (unsigned player = 0; player < kTracePlayers; ++player) {
                trace_entry.input_present[player] =
                    trace_input_present(replay_tick, player, trace_entry.inputs[player]);
                trace_entry.pre_actors[player] = capture_trace_actor_state(player);
            }
        }
        server::SimulateOneTick(timestep);
        if (trace_prediction_enabled()) {
            for (unsigned player = 0; player < kTracePlayers; ++player)
                trace_entry.post_actors[player] = capture_trace_actor_state(player);
            store_predicted_tick_trace(trace_entry);
        }
        display::Tick(timestep);
    }
    g_prediction.simulation_active = false;
    return true;
}

}  // namespace

void VisualPredictionOnBeforeSimTick(double timestep) {
    (void)timestep;
    maybe_invalidate_for_config_change();
    if (!trace_prediction_enabled())
        return;
    g_prediction.truth_pre_tick = input::CurrentTick();
    for (unsigned player = 0; player < kTracePlayers; ++player)
        g_prediction.truth_pre_actors[player] = capture_trace_actor_state(player);
}

void VisualPredictionOnAfterSimTick() {
    maybe_invalidate_for_config_change();
    if (!trace_prediction_enabled())
        return;
    const uint32_t current_tick = input::CurrentTick();
    if (current_tick == 0)
        return;
    const uint32_t simulated_tick = current_tick - 1;
    prune_trace_history(simulated_tick);
    const PredictedTickTrace *predicted = find_predicted_tick_trace(simulated_tick);
    if (!predicted)
        return;
    bool mismatch = false;
    for (unsigned player = 0; player < kTracePlayers; ++player) {
        if (actor_trace_differs(predicted->post_actors[player], capture_trace_actor_state(player))) {
            mismatch = true;
            break;
        }
    }
    if (mismatch)
        log_tick_trace_mismatch(simulated_tick, *predicted);
}

void VisualPredictionBeginRender() {
    maybe_invalidate_for_config_change();
    if (!prediction_enabled_now())
        return;
    g_prediction.truth_snapshot_for_render = sim_snapshot::Capture();
    capture_grid_models_for_render(g_prediction.truth_grid_models_for_render);
    if (!rebuild_predicted_world_for_render(input::CurrentTick(), input::TickTimestep())) {
        return;
    }
    g_prediction.render_active = true;
    RefreshRenderState(0.0);
}

void VisualPredictionEndRender() {
    if (!g_prediction.render_active)
        return;
    sim_snapshot::Restore(g_prediction.truth_snapshot_for_render);
    restore_grid_models_for_render(g_prediction.truth_grid_models_for_render);
    g_prediction.render_active = false;
    RefreshRenderState(0.0);
}

bool VisualPredictionRenderActive() {
    return g_prediction.render_active;
}

bool VisualPredictionSimulationActive() {
    return g_prediction.simulation_active;
}

void VisualPredictionInvalidate() {
    g_prediction = PredictionState();
}

bool VisualPredictionEnabled() {
    return prediction_enabled_now();
}

}  // namespace multiplayer
}  // namespace enigma
