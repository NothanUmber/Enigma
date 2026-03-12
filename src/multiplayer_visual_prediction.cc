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
#include <unordered_set>

namespace enigma {
namespace multiplayer {

namespace {

struct PredictionState {
    struct OwnedActorState {
        std::unordered_map<uint32_t, sim_snapshot::ActorSnapshot> history_by_tick;
    };

    struct RemoteActorState {
        VisualPredictionActorMode mode = VisualPredictionActorMode::Truth;
        std::unordered_map<uint32_t, sim_snapshot::ActorSnapshot> history_by_tick;
        uint32_t blend_start_tick = 0;
        uint32_t last_truth_input_tick_checked = 0;
    };

    struct TruthGridObjectState {
        GridLayer layer = GRID_COUNT;
        uint16_t x = 0;
        uint16_t y = 0;
        ObjectStateSnapshot snapshot;
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
    std::unordered_map<int, RemoteActorState> remote_actor_states;
    std::unordered_map<int, OwnedActorState> owned_local_actors;
    std::unordered_set<int> owned_world_objects;
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
    g_prediction.remote_actor_states.clear();
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

bool attr_snapshots_equal(const Object::MpAttrSnapshot &a, const Object::MpAttrSnapshot &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].first != b[i].first || a[i].second != b[i].second)
            return false;
    }
    return true;
}

bool object_state_matches_truth(Object *obj, const ObjectStateSnapshot &truth) {
    if (!obj)
        return false;
    if (obj->MpCaptureStateForSnapshot() != truth.state)
        return false;
    if (obj->MpCaptureFlagsForSnapshot() != truth.flags)
        return false;
    Object::MpAttrSnapshot attrs;
    obj->MpCaptureAttrsForSnapshot(attrs);
    return attr_snapshots_equal(attrs, truth.attrs);
}

bool other_state_matches_truth(Other *other, const OtherStateSnapshot &truth) {
    if (!other)
        return false;
    if (other->MpCaptureStateForSnapshot() != truth.state)
        return false;
    if (other->MpCaptureFlagsForSnapshot() != truth.flags)
        return false;
    Object::MpAttrSnapshot attrs;
    other->MpCaptureAttrsForSnapshot(attrs);
    return attr_snapshots_equal(attrs, truth.attrs);
}

void restore_object_truth_state(const ObjectStateSnapshot &truth) {
    Object *obj = Object::getObject(truth.object_id);
    if (!obj)
        return;
    obj->MpRestoreFlagsForSnapshot(truth.flags);
    obj->MpRestoreAttrsForSnapshot(truth.attrs);
    const int want = truth.state;
    if (obj->MpRestoreStateForSnapshot(want))
        return;
    const int have = static_cast<int>(obj->getAttr("state"));
    if (have != want)
        obj->setAttr("state", Value(want));
}

uint64_t grid_model_key(GridLayer layer, uint16_t x, uint16_t y) {
    return (static_cast<uint64_t>(static_cast<unsigned>(layer)) << 32) |
           (static_cast<uint64_t>(x) << 16) |
           static_cast<uint64_t>(y);
}

void restore_truth_grid_model(
    const PredictionState::TruthGridObjectState &truth,
    const std::unordered_map<uint64_t, size_t> &truth_grid_model_index) {
    const uint64_t key = grid_model_key(truth.layer, truth.x, truth.y);
    auto it = truth_grid_model_index.find(key);
    if (it == truth_grid_model_index.end())
        return;
    const auto &entry = g_prediction.truth_grid_models_for_render[it->second];
    ::display::KillModel(GridLoc(truth.layer, GridPos(static_cast<int>(truth.x), static_cast<int>(truth.y))));
    if (!entry.model)
        return;
    ::display::SetModel(GridLoc(entry.layer,
                                GridPos(static_cast<int>(entry.x), static_cast<int>(entry.y))),
                        entry.model->clone());
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

int actor_authoritative_player(const Actor &actor) {
    const unsigned players = std::min(input::ExpectedPlayers(), input::kMaxPlayers);
    for (unsigned player = 0; player < players; ++player) {
        if (!actor.controlled_by(static_cast<int>(player)))
            continue;
        return static_cast<int>(player);
    }
    return -1;
}

void prune_actor_history(std::unordered_map<uint32_t, sim_snapshot::ActorSnapshot> &history,
                        uint32_t current_tick) {
    for (auto it = history.begin(); it != history.end();) {
        if (it->first < current_tick)
            it = history.erase(it);
        else
            ++it;
    }
}

bool authoritative_remote_input_active(const Actor &actor,
                                       PredictionState::RemoteActorState &state,
                                       uint32_t current_tick) {
    if (current_tick == 0)
        return false;
    const int player = actor_authoritative_player(actor);
    if (player < 0 || static_cast<unsigned>(player) == LocalPlayer())
        return false;

    const uint32_t end_tick = current_tick - 1;
    if (state.last_truth_input_tick_checked > end_tick)
        return false;

    input::PlayerInput truth_input;
    for (uint32_t tick = state.last_truth_input_tick_checked; tick <= end_tick; ++tick) {
        if (!rollback::GetRecordedInput(tick, static_cast<unsigned>(player), truth_input))
            continue;
        if (truth_input.empty())
            continue;
        state.last_truth_input_tick_checked = tick + 1;
        return true;
    }

    state.last_truth_input_tick_checked = end_tick + 1;
    return false;
}

double remote_actor_blend_alpha(const PredictionState::RemoteActorState &state,
                                uint32_t current_tick) {
    const uint32_t duration_ticks = std::max<uint32_t>(1u, configured_delay_ticks());
    if (current_tick <= state.blend_start_tick)
        return 0.0;
    double alpha = static_cast<double>(current_tick - state.blend_start_tick) /
                   static_cast<double>(duration_ticks);
    if (alpha < 0.0)
        alpha = 0.0;
    if (alpha > 1.0)
        alpha = 1.0;
    return alpha;
}

void apply_actor_blend_toward_truth(Actor &actor,
                                    const sim_snapshot::ActorSnapshot &truth,
                                    double alpha) {
    if (alpha <= 0.0)
        return;
    if (alpha >= 1.0) {
        sim_snapshot::RestoreActor(truth);
        return;
    }

    ActorInfo *ai = actor.get_actorinfo();
    if (!ai)
        return;

    ai->pos = ai->pos + (truth.info.pos - ai->pos) * alpha;
    DidMoveActor(&actor);
    ai->last_gridpos = ai->gridpos;
    ai->vel = ai->vel + (truth.info.vel - ai->vel) * alpha;
    ai->frozen_vel = ai->frozen_vel + (truth.info.frozen_vel - ai->frozen_vel) * alpha;
    ai->render_pos = ai->pos;
    ai->render_initialized = true;
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

    std::unordered_map<int, PredictionState::TruthGridObjectState> truth_grid_objects;
    truth_grid_objects.reserve(g_prediction.truth_snapshot_for_render.object_states.size());
    {
        const int w = Width();
        const int h = Height();
        size_t state_index = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                GridPos p(x, y);
                auto capture_truth_object = [&](GridLayer layer, Object *obj) {
                    if (!obj)
                        return;
                    if (state_index >= g_prediction.truth_snapshot_for_render.object_states.size())
                        return;
                    const ObjectStateSnapshot &snap =
                        g_prediction.truth_snapshot_for_render.object_states[state_index++];
                    if (layer == GRID_STONES) {
                        Stone *stone = dynamic_cast<Stone *>(obj);
                        if (stone && stone->is_movable())
                            return;
                    }
                    PredictionState::TruthGridObjectState truth;
                    truth.layer = layer;
                    truth.x = static_cast<uint16_t>(x);
                    truth.y = static_cast<uint16_t>(y);
                    truth.snapshot = snap;
                    truth_grid_objects[snap.object_id] = std::move(truth);
                };
                capture_truth_object(GRID_FLOOR, GetFloor(p));
                capture_truth_object(GRID_ITEMS, GetItem(p));
                capture_truth_object(GRID_STONES, GetStone(p));
            }
        }
    }

    std::unordered_map<int, OtherStateSnapshot> truth_other_objects;
    truth_other_objects.reserve(g_prediction.truth_snapshot_for_render.other_states.size());
    for (const auto &snap : g_prediction.truth_snapshot_for_render.other_states)
        truth_other_objects[snap.object_id] = snap;

    std::unordered_map<uint64_t, size_t> truth_grid_model_index;
    truth_grid_model_index.reserve(g_prediction.truth_grid_models_for_render.size());
    for (size_t i = 0; i < g_prediction.truth_grid_models_for_render.size(); ++i) {
        const auto &entry = g_prediction.truth_grid_models_for_render[i];
        truth_grid_model_index[grid_model_key(entry.layer, entry.x, entry.y)] = i;
    }

    std::vector<int> erase_remote_ids;
    erase_remote_ids.reserve(g_prediction.remote_actor_states.size());
    for (auto &entry : g_prediction.remote_actor_states) {
        auto &state = entry.second;
        prune_actor_history(state.history_by_tick, current_tick);
        auto it = state.history_by_tick.find(current_tick);
        if (it != state.history_by_tick.end()) {
            sim_snapshot::RestoreActor(it->second);
        } else if (state.history_by_tick.empty()) {
            erase_remote_ids.push_back(entry.first);
        }
    }
    for (int object_id : erase_remote_ids)
        g_prediction.remote_actor_states.erase(object_id);

    std::vector<int> erase_local_ids;
    erase_local_ids.reserve(g_prediction.owned_local_actors.size());
    for (auto &owned : g_prediction.owned_local_actors) {
        auto &history = owned.second.history_by_tick;
        prune_actor_history(history, current_tick);
        auto it = history.find(current_tick);
        if (it != history.end()) {
            sim_snapshot::RestoreActor(it->second);
        } else if (history.empty()) {
            erase_local_ids.push_back(owned.first);
        }
    }
    for (int object_id : erase_local_ids)
        g_prediction.owned_local_actors.erase(object_id);

    g_prediction.simulation_active = true;
    const uint32_t target_tick = current_tick + delay_ticks;
    bool local_world_prediction_active = false;
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
        local_world_prediction_active = local_world_prediction_active || local_input_active;
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
            auto state_it = g_prediction.remote_actor_states.find(actor->getId());
            const bool locally_affected =
                remote_actor_in_local_interaction_cone(*actor, local_actors) ||
                (state_it != g_prediction.remote_actor_states.end() &&
                 state_it->second.mode != VisualPredictionActorMode::Truth);
            if (!locally_affected)
                continue;
            auto &state = g_prediction.remote_actor_states[actor->getId()];
            if (state.mode == VisualPredictionActorMode::Truth) {
                state.mode = VisualPredictionActorMode::LocalOwned;
                state.last_truth_input_tick_checked = current_tick;
            }
            state.history_by_tick[simulated_tick] = sim_snapshot::CaptureActor(*actor);
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
            auto &state = g_prediction.remote_actor_states[actor->getId()];
            if (state.mode == VisualPredictionActorMode::Truth) {
                state.mode = VisualPredictionActorMode::LocalOwned;
                state.last_truth_input_tick_checked = current_tick;
            }
            state.history_by_tick[target_tick] = sim_snapshot::CaptureActor(*actor);
        }

        auto state_it = g_prediction.remote_actor_states.find(actor->getId());
        if (state_it == g_prediction.remote_actor_states.end())
            continue;
        auto &state = state_it->second;
        if (state.mode != VisualPredictionActorMode::LocalOwned)
            continue;
        if (!actor_state_differs(*actor, truth_it->second))
            continue;
        if (!authoritative_remote_input_active(*actor, state, current_tick))
            continue;
        state.mode = VisualPredictionActorMode::BlendToTruth;
        state.blend_start_tick = current_tick;
        if (state.history_by_tick.find(target_tick) == state.history_by_tick.end()) {
            state.history_by_tick[target_tick] = sim_snapshot::CaptureActor(*actor);
        }
    }

    erase_remote_ids.clear();
    erase_local_ids.clear();
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
                    erase_local_ids.push_back(actor->getId());
                }
                continue;
            }
            sim_snapshot::RestoreActor(truth_it->second);
            continue;
        }

        auto state_it = g_prediction.remote_actor_states.find(actor->getId());
        if (state_it != g_prediction.remote_actor_states.end()) {
            auto &state = state_it->second;
            if (state.mode == VisualPredictionActorMode::BlendToTruth) {
                const double alpha = remote_actor_blend_alpha(state, current_tick);
                if (alpha >= 1.0) {
                    sim_snapshot::RestoreActor(truth_it->second);
                    erase_remote_ids.push_back(actor->getId());
                } else {
                    apply_actor_blend_toward_truth(*actor, truth_it->second, alpha);
                }
                continue;
            }

            auto current_it = state.history_by_tick.find(current_tick);
            if (current_it != state.history_by_tick.end() &&
                actor_snapshot_still(current_it->second) && actor_truth_still(*actor)) {
                erase_remote_ids.push_back(actor->getId());
            }
            continue;
        }
        sim_snapshot::RestoreActor(truth_it->second);
    }
    for (int object_id : erase_remote_ids)
        g_prediction.remote_actor_states.erase(object_id);
    for (int object_id : erase_local_ids)
        g_prediction.owned_local_actors.erase(object_id);

    std::unordered_set<int> next_owned_world_objects;
    next_owned_world_objects.reserve(g_prediction.owned_world_objects.size() + 8);
    for (const auto &entry : truth_grid_objects) {
        Object *obj = Object::getObject(entry.first);
        if (!obj)
            continue;
        const bool owned_before = g_prediction.owned_world_objects.find(entry.first) !=
                                  g_prediction.owned_world_objects.end();
        const bool differs_from_truth = !object_state_matches_truth(obj, entry.second.snapshot);
        const bool keep_owned =
            differs_from_truth && (owned_before || local_world_prediction_active);
        if (keep_owned) {
            next_owned_world_objects.insert(entry.first);
            continue;
        }
        restore_object_truth_state(entry.second.snapshot);
        restore_truth_grid_model(entry.second, truth_grid_model_index);
    }
    for (const auto &entry : truth_other_objects) {
        Other *other = dynamic_cast<Other *>(Object::getObject(entry.first));
        if (!other)
            continue;
        const bool owned_before = g_prediction.owned_world_objects.find(entry.first) !=
                                  g_prediction.owned_world_objects.end();
        const bool keep_owned =
            !other_state_matches_truth(other, entry.second) &&
            (owned_before || local_world_prediction_active);
        if (keep_owned) {
            next_owned_world_objects.insert(entry.first);
            continue;
        }
        other->MpRestoreFlagsForSnapshot(entry.second.flags);
        other->MpRestoreAttrsForSnapshot(entry.second.attrs);
        const int want = entry.second.state;
        if (!other->MpRestoreStateForSnapshot(want)) {
            const int have = static_cast<int>(other->getAttr("state"));
            if (have != want)
                other->setAttr("state", Value(want));
        }
    }
    g_prediction.owned_world_objects = std::move(next_owned_world_objects);

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

VisualPredictionActorMode VisualPredictionGetActorMode(const Actor &actor) {
    if (actor_is_local_predicted(actor))
        return VisualPredictionActorMode::Truth;
    auto it = g_prediction.remote_actor_states.find(actor.getId());
    if (it == g_prediction.remote_actor_states.end())
        return VisualPredictionActorMode::Truth;
    return it->second.mode;
}

double VisualPredictionGetActorBlendAlpha(const Actor &actor) {
    auto it = g_prediction.remote_actor_states.find(actor.getId());
    if (it == g_prediction.remote_actor_states.end())
        return 0.0;
    if (it->second.mode != VisualPredictionActorMode::BlendToTruth)
        return 0.0;
    return remote_actor_blend_alpha(it->second, input::CurrentTick());
}

}  // namespace multiplayer
}  // namespace enigma
