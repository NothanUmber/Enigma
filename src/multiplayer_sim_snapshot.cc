/*
 * Multiplayer rollback/replay snapshot utilities.
 *
 * This snapshot is intentionally limited to "simulation tick state" that is
 * needed for client prediction experiments:
 * - input tick + queued inputs
 * - RNG seed/state
 * - active GameTimer alarms
 * - pending secure delayed actions
 * - actor physics state
 * - positions of movable stones (puzzle stones, doors, etc.)
 *
 * It does not attempt to serialize/restore the full world object graph (grid
 * composition, arbitrary Lua state, etc.). That is a separate, larger engine
 * project.
 */

#include "multiplayer_sim_snapshot.hh"

#include "actors.hh"
#include "actors/Balls.hh"
#include "d_models.hh"
#include "server.hh"
#include "stones/ShogunStone.hh"
#include "world.hh"

#include <cmath>
#include <unordered_map>

namespace enigma {
namespace multiplayer {
namespace sim_snapshot {

namespace {

ActorInfoSnapshot capture_actorinfo(const ActorInfo &ai) {
    ActorInfoSnapshot s;
    s.pos = ai.pos;
    s.render_pos = ai.render_pos;
    s.render_initialized = ai.render_initialized;
    s.vel = ai.vel;
    s.frozen_vel = ai.frozen_vel;
    s.pos_force = ai.pos_force;
    s.forceacc = ai.forceacc;
    s.charge = ai.charge;
    s.mass = ai.mass;
    s.radius = ai.radius;
    s.grabbed = ai.grabbed;
    s.created = ai.created;
    s.ignore_contacts = ai.ignore_contacts;
    s.force = ai.force;
    s.collforce = ai.collforce;
    s.friction = ai.friction;
    for (int i = 0; i < MAX_CONTACTS; ++i) {
        s.contacts_a[static_cast<size_t>(i)].pos = ai.contacts_a[i].pos;
        s.contacts_a[static_cast<size_t>(i)].normal = ai.contacts_a[i].normal;
        s.contacts_b[static_cast<size_t>(i)].pos = ai.contacts_b[i].pos;
        s.contacts_b[static_cast<size_t>(i)].normal = ai.contacts_b[i].normal;
    }
    s.contacts_count = ai.contacts_count;
    s.last_contacts_count = ai.last_contacts_count;
    s.contacts_sel = (ai.contacts == ai.contacts_a) ? 0 : 1;
    s.last_contacts_sel = (ai.last_contacts == ai.contacts_a) ? 0 : 1;
    return s;
}

void restore_actorinfo(Actor &actor, const ActorInfoSnapshot &s) {
    ActorInfo *ai = actor.get_actorinfo();
    ai->pos = s.pos;
    // Update spatial index / field pointers based on position.
    DidMoveActor(&actor);
    ai->last_gridpos = ai->gridpos;

    ai->render_pos = s.render_pos;
    ai->render_initialized = s.render_initialized;
    ai->vel = s.vel;
    ai->frozen_vel = s.frozen_vel;
    ai->pos_force = s.pos_force;
    ai->forceacc = s.forceacc;
    ai->charge = s.charge;
    ai->mass = s.mass;
    ai->radius = s.radius;
    ai->grabbed = s.grabbed;
    ai->created = s.created;
    ai->ignore_contacts = s.ignore_contacts;
    ai->force = s.force;
    ai->collforce = s.collforce;
    ai->friction = s.friction;

    for (int i = 0; i < MAX_CONTACTS; ++i) {
        ai->contacts_a[i].pos = s.contacts_a[static_cast<size_t>(i)].pos;
        ai->contacts_a[i].normal = s.contacts_a[static_cast<size_t>(i)].normal;
        ai->contacts_b[i].pos = s.contacts_b[static_cast<size_t>(i)].pos;
        ai->contacts_b[i].normal = s.contacts_b[static_cast<size_t>(i)].normal;
    }
    ai->contacts_count = s.contacts_count;
    ai->last_contacts_count = s.last_contacts_count;
    ai->contacts = (s.contacts_sel == 0) ? ai->contacts_a : ai->contacts_b;
    ai->last_contacts = (s.last_contacts_sel == 0) ? ai->contacts_a : ai->contacts_b;
}

void restore_movable_stone_positions(const std::vector<Snapshot::MovableStone> &wanted) {
    const int w = Width();
    const int h = Height();
    if (w <= 0 || h <= 0)
        return;

    auto key = [](int x, int y) -> uint32_t {
        return (static_cast<uint32_t>(x) << 16) | static_cast<uint32_t>(y);
    };
    auto key_to_pos = [](uint32_t k) -> GridPos {
        int x = static_cast<int>((k >> 16) & 0xFFFFu);
        int y = static_cast<int>(k & 0xFFFFu);
        return GridPos(x, y);
    };

    std::unordered_map<uint32_t, uint32_t> wanted_pos_by_id;
    wanted_pos_by_id.reserve(wanted.size() * 2);
    for (const auto &e : wanted) {
        if (e.object_id < 0)
            continue;
        wanted_pos_by_id[static_cast<uint32_t>(e.object_id)] =
            key(static_cast<int>(e.x), static_cast<int>(e.y));
    }

    std::unordered_map<uint32_t, uint32_t> current_pos_by_id;
    current_pos_by_id.reserve(std::max<size_t>(wanted.size() * 2, 32));
    std::vector<uint32_t> extra_ids_on_grid;
    extra_ids_on_grid.reserve(8);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            GridPos p(x, y);
            Stone *st = GetStone(p);
            if (!st || !st->is_movable())
                continue;
            const uint32_t object_id = static_cast<uint32_t>(st->getId());
            current_pos_by_id[object_id] = key(x, y);
            if (wanted_pos_by_id.find(object_id) == wanted_pos_by_id.end())
                extra_ids_on_grid.push_back(object_id);
        }
    }

    for (const auto &e : wanted) {
        if (e.object_id < 0)
            continue;
        const uint32_t object_id = static_cast<uint32_t>(e.object_id);
        if (current_pos_by_id.find(object_id) != current_pos_by_id.end())
            continue;
        GridPos dst(static_cast<int>(e.x), static_cast<int>(e.y));
        if (GetStone(dst) != nullptr)
            continue;
        ShogunStone *hidden = dynamic_cast<ShogunStone *>(Object::getObject(e.object_id));
        if (!hidden)
            continue;
        if (!hidden->MpRestoreToGridForSnapshot(dst))
            continue;
        current_pos_by_id[object_id] = key(dst.x, dst.y);
    }

    std::unordered_map<uint32_t, uint32_t> src_to_dst;
    std::unordered_map<uint32_t, uint32_t> src_by_dst;
    src_to_dst.reserve(wanted.size());
    src_by_dst.reserve(wanted.size());
    for (const auto &e : wanted) {
        if (e.object_id < 0)
            continue;
        auto it = current_pos_by_id.find(static_cast<uint32_t>(e.object_id));
        if (it == current_pos_by_id.end())
            continue;
        const uint32_t src = it->second;
        const uint32_t dst = key(static_cast<int>(e.x), static_cast<int>(e.y));
        if (src == dst)
            continue;
        src_to_dst[src] = dst;
        src_by_dst[dst] = src;
    }

    // Resolve moves into empty destination cells first.
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (auto it = src_to_dst.begin(); it != src_to_dst.end(); ++it) {
            const uint32_t src = it->first;
            const uint32_t dst = it->second;
            GridPos dst_pos = key_to_pos(dst);
            if (GetStone(dst_pos) != nullptr)
                continue;
            MoveStone(key_to_pos(src), dst_pos);
            src_by_dst.erase(dst);
            src_to_dst.erase(it);
            progressed = true;
            break;
        }
    }

    // Remaining moves are cycles. Break them by holding one stone in memory.
    while (!src_to_dst.empty()) {
        const uint32_t start_src = src_to_dst.begin()->first;
        const uint32_t start_dst = src_to_dst.begin()->second;
        GridPos start_src_pos = key_to_pos(start_src);
        Stone *held = YieldStone(start_src_pos);
        if (!held) {
            src_by_dst.erase(start_dst);
            src_to_dst.erase(start_src);
            continue;
        }

        uint32_t empty = start_src;
        while (empty != start_dst) {
            auto it_prev = src_by_dst.find(empty);
            if (it_prev == src_by_dst.end()) {
                // Unexpected permutation shape; restore what we can and abort.
                SetStone(start_src_pos, held);
                held = nullptr;
                src_to_dst.clear();
                src_by_dst.clear();
                break;
            }
            const uint32_t prev_src = it_prev->second;
            MoveStone(key_to_pos(prev_src), key_to_pos(empty));
            auto it_dst = src_to_dst.find(prev_src);
            if (it_dst != src_to_dst.end())
                src_to_dst.erase(it_dst);
            src_by_dst.erase(it_prev);
            empty = prev_src;
        }
        if (held) {
            SetStone(key_to_pos(start_dst), held);
            src_by_dst.erase(start_dst);
            src_to_dst.erase(start_src);
        }
    }

    // Predicted replay can temporarily materialize movable stones that are hidden in the
    // authoritative world (e.g. sub-shoguns yielded from a stack). If the truth snapshot
    // does not contain that stone on any grid cell, remove the predicted-only grid
    // instance again so object-state restoration can rebuild the authoritative topology.
    for (uint32_t object_id : extra_ids_on_grid) {
        auto it = current_pos_by_id.find(object_id);
        if (it == current_pos_by_id.end())
            continue;
        GridPos pos = key_to_pos(it->second);
        Stone *st = GetStone(pos);
        if (!st || !st->is_movable() || static_cast<uint32_t>(st->getId()) != object_id)
            continue;
        Stone *extra = YieldStone(pos);
        if (!extra)
            continue;
        extra->setOwnerPos(GridPos(-1, -1));
        DisposeObject(extra);
    }
}

}  // namespace

Snapshot::AnimatedGridModel::AnimatedGridModel(const AnimatedGridModel &other)
: layer(other.layer),
  x(other.x),
  y(other.y),
  model(other.model ? std::unique_ptr<::display::Model>(other.model->clone()) : nullptr) {
}

Snapshot::AnimatedGridModel &Snapshot::AnimatedGridModel::operator=(const AnimatedGridModel &other) {
    if (this == &other)
        return *this;
    layer = other.layer;
    x = other.x;
    y = other.y;
    model.reset(other.model ? other.model->clone() : nullptr);
    return *this;
}

Snapshot::AnimatedGridModel::~AnimatedGridModel() = default;

Snapshot Capture() {
    Snapshot snap;
    snap.input = input::CaptureSnapshot();
    snap.random_state = server::RandomState;
    snap.level_time = server::LevelTime;
    snap.game_timer = GameTimer.snapshot();
    snap.pending_actions = CapturePendingActions();
    CaptureObjectStates(snap.object_states);
    CaptureOtherStates(snap.other_states);

    // Capture movable-stone layout for rollback correctness.
    {
        const int w = Width();
        const int h = Height();
        if (w > 0 && h > 0) {
            snap.movable_stones.clear();
            snap.movable_stones.reserve(64);
            snap.animated_grid_models.clear();
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    GridPos p(x, y);
                    Stone *st = GetStone(p);
                    if (!st || !st->is_movable())
                        continue;
                    Snapshot::MovableStone ms;
                    ms.object_id = st->getId();
                    ms.x = static_cast<uint16_t>(x);
                    ms.y = static_cast<uint16_t>(y);
                    snap.movable_stones.push_back(ms);
                }
            }

            const GridLayer layers[] = {GRID_FLOOR, GRID_ITEMS, GRID_STONES};
            for (GridLayer layer : layers) {
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        if (layer == GRID_STONES) {
                            Stone *st = GetStone(GridPos(x, y));
                            if (st && st->is_movable())
                                continue;
                        }
                        ::display::Model *model = ::display::GetModel(GridLoc(layer, GridPos(x, y)));
                        if (!model || !model->needs_runtime_snapshot())
                            continue;
                        Snapshot::AnimatedGridModel entry;
                        entry.layer = layer;
                        entry.x = static_cast<uint16_t>(x);
                        entry.y = static_cast<uint16_t>(y);
                        entry.model.reset(model ? model->clone() : nullptr);
                        snap.animated_grid_models.push_back(std::move(entry));
                    }
                }
            }
        }
    }

    std::vector<Actor *> actors;
    GetActors(actors);
    snap.actors.reserve(actors.size());
    for (Actor *a : actors) {
        if (!a)
            continue;
        ActorSnapshot as;
        as.object_id = a->getId();
        as.internal_state = a->snapshot_internal_state();
        as.info = capture_actorinfo(*a->get_actorinfo());
        snap.actors.push_back(as);
    }
    return snap;
}

void Restore(const Snapshot &snap) {
    input::RestoreSnapshot(snap.input);
    server::RandomState = snap.random_state;
    server::LevelTime = snap.level_time;
    GameTimer.restore(snap.game_timer);
    RestorePendingActions(snap.pending_actions);
    RestoreObjectStates(snap.object_states);
    RestoreOtherStates(snap.other_states);
    restore_movable_stone_positions(snap.movable_stones);
    for (const auto &entry : snap.animated_grid_models) {
        if (!entry.model)
            continue;
        ::display::SetModel(GridLoc(entry.layer, GridPos(static_cast<int>(entry.x), static_cast<int>(entry.y))),
                            entry.model->clone());
    }

    std::unordered_map<int, ActorSnapshot> by_id;
    by_id.reserve(snap.actors.size());
    for (const auto &a : snap.actors)
        by_id[a.object_id] = a;

    std::vector<Actor *> actors;
    GetActors(actors);
    for (Actor *a : actors) {
        if (!a)
            continue;
        auto it = by_id.find(a->getId());
        if (it == by_id.end())
            continue;
        a->restore_internal_state(it->second.internal_state);
        restore_actorinfo(*a, it->second.info);
        if (BasicBall *ball = dynamic_cast<BasicBall *>(a)) {
            ball->MpFinishAppearingAfterSnapshotRestore();
        }
    }
}

}  // namespace sim_snapshot
}  // namespace multiplayer
}  // namespace enigma
