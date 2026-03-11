#include "multiplayer_state.hh"

#include "actors.hh"
#include "display.hh"
#include "input.hh"
#include "multiplayer_internal.hh"
#include "multiplayer_rollback.hh"
#include "multiplayer_sim_snapshot.hh"
#include "options.hh"
#include "server.hh"

#include <algorithm>
#include <limits>

namespace enigma {
namespace multiplayer {

namespace {

struct PredictionState {
    bool render_active = false;
    bool simulation_active = false;
    uint32_t last_delay_ticks = std::numeric_limits<uint32_t>::max();
    sim_snapshot::Snapshot truth_snapshot_for_render;
};

PredictionState g_prediction;

uint32_t configured_delay_ticks() {
    return static_cast<uint32_t>(std::max(0, options::GetInt("MultiplayerDebugInputDelayTicks")));
}

bool prediction_enabled_now() {
    return IsActive() && input::IsNetworked() &&
           options::GetBool("MultiplayerDebugVisualPrediction");
}

void clear_prediction_cache() {
    (void)0;
}

void maybe_invalidate_for_config_change() {
    const uint32_t delay = configured_delay_ticks();
    if (g_prediction.last_delay_ticks == delay)
        return;
    g_prediction.last_delay_ticks = delay;
    clear_prediction_cache();
}

void refresh_actor_sprites() {
    std::vector<Actor *> actors;
    GetActors(actors);
    for (Actor *actor : actors) {
        if (actor)
            actor->move_screen();
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
        server::SimulateOneTick(timestep);
        display::Tick(timestep);
    }
    g_prediction.simulation_active = false;
    return true;
}

}  // namespace

void VisualPredictionOnBeforeSimTick(double timestep) {
    (void)timestep;
    maybe_invalidate_for_config_change();
}

void VisualPredictionOnAfterSimTick() {
    maybe_invalidate_for_config_change();
}

void VisualPredictionBeginRender() {
    maybe_invalidate_for_config_change();
    if (!prediction_enabled_now())
        return;
    g_prediction.truth_snapshot_for_render = sim_snapshot::Capture();
    if (!rebuild_predicted_world_for_render(input::CurrentTick(), input::TickTimestep())) {
        return;
    }
    g_prediction.render_active = true;
    refresh_actor_sprites();
}

void VisualPredictionEndRender() {
    if (!g_prediction.render_active)
        return;
    sim_snapshot::Restore(g_prediction.truth_snapshot_for_render);
    g_prediction.render_active = false;
    refresh_actor_sprites();
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
