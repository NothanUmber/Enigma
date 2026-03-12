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
#include <unordered_map>

namespace enigma {
namespace multiplayer {

namespace {

struct PredictionState {
    struct OwnedActorState {
        std::unordered_map<uint32_t, sim_snapshot::ActorSnapshot> history_by_tick;
    };

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
    std::unordered_map<int, OwnedActorState> owned_remote_actors;
    std::unordered_map<int, OwnedActorState> owned_local_actors;
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
    g_prediction.owned_remote_actors.clear();
    g_prediction.owned_local_actors.clear();
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

bool local_predicted_input_active(uint32_t source_tick) {
    input::PlayerInput predicted_input;
    if (get_local_predicted_input(source_tick, predicted_input))
        return !predicted_input.empty();
    rollback::GetRecordedInput(source_tick, LocalPlayer(), predicted_input);
    return !predicted_input.empty();
}

bool actor_is_local_predicted(const Actor &actor) {
    return actor.controlled_by(static_cast<int>(LocalPlayer()));
}

bool actor_state_differs(const Actor &actor, const sim_snapshot::ActorSnapshot &truth) {
    const ActorInfo &ai = actor.get_actorinfo();
    const ecl::V2 dpos = ai.pos - truth.info.pos;
    const ecl::V2 dvel = ai.vel - truth.info.vel;
    const double pos_sq = static_cast<double>(dpos[0]) * static_cast<double>(dpos[0]) +
                          static_cast<double>(dpos[1]) * static_cast<double>(dpos[1]);
    const double vel_sq = static_cast<double>(dvel[0]) * static_cast<double>(dvel[0]) +
                          static_cast<double>(dvel[1]) * static_cast<double>(dvel[1]);
    return pos_sq > 0.02 * 0.02 || vel_sq > 0.05 * 0.05;
}

bool actor_snapshot_still(const sim_snapshot::ActorSnapshot &snap) {
    const ecl::V2 vel = snap.info.vel;
    const double vel_sq = static_cast<double>(vel[0]) * static_cast<double>(vel[0]) +
                          static_cast<double>(vel[1]) * static_cast<double>(vel[1]);
    return vel_sq <= 0.03 * 0.03;
}

bool actor_truth_still(const Actor &actor) {
    const ecl::V2 vel = actor.get_actorinfo().vel;
    const double vel_sq = static_cast<double>(vel[0]) * static_cast<double>(vel[0]) +
                          static_cast<double>(vel[1]) * static_cast<double>(vel[1]);
    return vel_sq <= 0.03 * 0.03;
}

bool remote_actor_in_local_interaction_cone(const Actor &remote_actor,
                                            const std::vector<Actor *> &local_actors) {
    const ecl::V2 remote_pos = remote_actor.get_pos();
    const double remote_radius = remote_actor.get_actorinfo().radius;
    for (Actor *local_actor : local_actors) {
        if (!local_actor)
            continue;
        const ecl::V2 delta = remote_pos - local_actor->get_pos();
        const double allowed = remote_radius + local_actor->get_actorinfo()->radius + 0.35;
        const double dist_sq =
            static_cast<double>(delta[0]) * static_cast<double>(delta[0]) +
            static_cast<double>(delta[1]) * static_cast<double>(delta[1]);
        if (dist_sq <= allowed * allowed)
            return true;
    }
    return false;
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

    std::unordered_map<int, sim_snapshot::ActorSnapshot> truth_actors;
    truth_actors.reserve(g_prediction.truth_snapshot_for_render.actors.size());
    for (const auto &actor : g_prediction.truth_snapshot_for_render.actors)
        truth_actors[actor.object_id] = actor;

    std::vector<int> erase_owned_ids;
    erase_owned_ids.reserve(g_prediction.owned_remote_actors.size());
    for (auto &owned : g_prediction.owned_remote_actors) {
        auto &history = owned.second.history_by_tick;
        for (auto it = history.begin(); it != history.end();) {
            if (it->first < current_tick)
                it = history.erase(it);
            else
                ++it;
        }
        auto it = history.find(current_tick);
        if (it != history.end()) {
            sim_snapshot::RestoreActor(it->second);
        } else if (history.empty()) {
            erase_owned_ids.push_back(owned.first);
        }
    }
    for (int object_id : erase_owned_ids)
        g_prediction.owned_remote_actors.erase(object_id);

    erase_owned_ids.clear();
    erase_owned_ids.reserve(g_prediction.owned_local_actors.size());
    for (auto &owned : g_prediction.owned_local_actors) {
        auto &history = owned.second.history_by_tick;
        for (auto it = history.begin(); it != history.end();) {
            if (it->first < current_tick)
                it = history.erase(it);
            else
                ++it;
        }
        auto it = history.find(current_tick);
        if (it != history.end()) {
            sim_snapshot::RestoreActor(it->second);
        } else if (history.empty()) {
            erase_owned_ids.push_back(owned.first);
        }
    }
    for (int object_id : erase_owned_ids)
        g_prediction.owned_local_actors.erase(object_id);

    g_prediction.simulation_active = true;
    const uint32_t target_tick = current_tick + delay_ticks;
    while (input::CurrentTick() < target_tick) {
        const uint32_t replay_tick = input::CurrentTick();
        rewrite_inputs_for_predicted_tick(replay_tick, delay_ticks);
        server::SimulateOneTick(timestep);
        display::Tick(timestep);

        std::vector<Actor *> actors;
        GetActors(actors);
        std::vector<Actor *> local_actors;
        local_actors.reserve(2);
        for (Actor *actor : actors) {
            if (actor && actor_is_local_predicted(*actor))
                local_actors.push_back(actor);
        }
        const bool local_input_active = local_predicted_input_active(replay_tick);
        const uint32_t simulated_tick = input::CurrentTick();
        for (Actor *actor : local_actors) {
            if (!actor)
                continue;
            const bool keep_local_owned =
                local_input_active ||
                g_prediction.owned_local_actors.find(actor->getId()) !=
                    g_prediction.owned_local_actors.end();
            if (!keep_local_owned)
                continue;
            g_prediction.owned_local_actors[actor->getId()]
                .history_by_tick[simulated_tick] = sim_snapshot::CaptureActor(*actor);
        }
        for (Actor *actor : actors) {
            if (!actor || actor_is_local_predicted(*actor))
                continue;
            const bool locally_affected =
                remote_actor_in_local_interaction_cone(*actor, local_actors) ||
                g_prediction.owned_remote_actors.find(actor->getId()) !=
                    g_prediction.owned_remote_actors.end();
            if (!locally_affected)
                continue;
            g_prediction.owned_remote_actors[actor->getId()]
                .history_by_tick[simulated_tick] = sim_snapshot::CaptureActor(*actor);
        }
    }
    g_prediction.simulation_active = false;

    std::vector<Actor *> actors;
    GetActors(actors);
    std::vector<Actor *> local_actors;
    local_actors.reserve(2);
    for (Actor *actor : actors) {
        if (actor && actor_is_local_predicted(*actor))
            local_actors.push_back(actor);
    }

    for (Actor *actor : actors) {
        if (!actor || actor_is_local_predicted(*actor))
            continue;
        auto truth_it = truth_actors.find(actor->getId());
        if (truth_it == truth_actors.end())
            continue;
        const bool affected_now = actor_state_differs(*actor, truth_it->second) &&
                                  remote_actor_in_local_interaction_cone(*actor, local_actors);
        if (affected_now) {
            g_prediction.owned_remote_actors[actor->getId()]
                .history_by_tick[target_tick] = sim_snapshot::CaptureActor(*actor);
        }
    }

    erase_owned_ids.clear();
    for (Actor *actor : actors) {
        if (!actor)
            continue;
        auto truth_it = truth_actors.find(actor->getId());
        if (truth_it == truth_actors.end())
            continue;
        if (actor_is_local_predicted(*actor)) {
            auto owned_it = g_prediction.owned_local_actors.find(actor->getId());
            if (owned_it != g_prediction.owned_local_actors.end()) {
                auto current_it = owned_it->second.history_by_tick.find(current_tick);
                if (current_it != owned_it->second.history_by_tick.end() &&
                    actor_snapshot_still(current_it->second) && actor_truth_still(*actor)) {
                    erase_owned_ids.push_back(actor->getId());
                }
                continue;
            }
            sim_snapshot::RestoreActor(truth_it->second);
            continue;
        }
        auto owned_it = g_prediction.owned_remote_actors.find(actor->getId());
        if (owned_it != g_prediction.owned_remote_actors.end()) {
            auto current_it = owned_it->second.history_by_tick.find(current_tick);
            if (current_it != owned_it->second.history_by_tick.end() &&
                actor_snapshot_still(current_it->second) && actor_truth_still(*actor)) {
                erase_owned_ids.push_back(actor->getId());
            }
            continue;
        }
        sim_snapshot::RestoreActor(truth_it->second);
    }
    for (int object_id : erase_owned_ids) {
        g_prediction.owned_remote_actors.erase(object_id);
        g_prediction.owned_local_actors.erase(object_id);
    }

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
