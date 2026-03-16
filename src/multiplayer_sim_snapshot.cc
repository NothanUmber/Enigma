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
#include "laser.hh"
#include "server.hh"
#include "stones/ShogunStone.hh"
#include "world.hh"

#include <cmath>
#include <unordered_map>

namespace enigma {
namespace multiplayer {
namespace sim_snapshot {

namespace {

std::unordered_map<int, int> g_retained_grid_snapshot_refs;
std::unordered_map<int, GridObject *> g_preserved_disposed_grid_objects;

void release_retained_grid_ids(const std::vector<int> &object_ids) {
    for (int object_id : object_ids) {
        auto it_ref = g_retained_grid_snapshot_refs.find(object_id);
        if (it_ref == g_retained_grid_snapshot_refs.end())
            continue;
        if (--it_ref->second == 0) {
            g_retained_grid_snapshot_refs.erase(it_ref);
            auto it_preserved = g_preserved_disposed_grid_objects.find(object_id);
            if (it_preserved == g_preserved_disposed_grid_objects.end())
                continue;
            GridObject *preserved = it_preserved->second;
            g_preserved_disposed_grid_objects.erase(it_preserved);
            if (preserved)
                preserved->dispose();
        }
    }
}

void retain_snapshot_grid_objects(Snapshot &snap) {
    if (snap.object_states.empty()) {
        snap.retained_grid_objects.reset();
        return;
    }
    std::shared_ptr<Snapshot::RetainedGridObjects> retained =
        std::make_shared<Snapshot::RetainedGridObjects>();
    retained->object_ids.reserve(snap.object_states.size());
    for (const auto &obj : snap.object_states) {
        if (obj.object_id < 0)
            continue;
        retained->object_ids.push_back(obj.object_id);
        ++g_retained_grid_snapshot_refs[obj.object_id];
    }
    snap.retained_grid_objects = retained;
}

GridObject *take_preserved_disposed_grid_object(int object_id) {
    auto it = g_preserved_disposed_grid_objects.find(object_id);
    if (it == g_preserved_disposed_grid_objects.end())
        return nullptr;
    GridObject *obj = it->second;
    g_preserved_disposed_grid_objects.erase(it);
    if (obj)
        obj->MpReattachToRepositoryForSnapshotPreserve();
    return obj;
}

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
        Stone *missing = dynamic_cast<Stone *>(Object::getObject(e.object_id));
        if (!missing)
            missing = dynamic_cast<Stone *>(take_preserved_disposed_grid_object(e.object_id));
        if (!missing || !missing->is_movable())
            continue;
        if (ShogunStone *hidden = dynamic_cast<ShogunStone *>(missing)) {
            if (!hidden->MpRestoreToGridForSnapshot(dst))
                continue;
        } else {
            SetStoneForSnapshotRestore(dst, missing);
            if (Value name = missing->getAttr("name"))
                NameObject(missing, name.to_string());
        }
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
            MoveStoneForSnapshotRestore(key_to_pos(src), dst_pos);
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
        Stone *held = YieldStoneForSnapshotRestore(start_src_pos);
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
                SetStoneForSnapshotRestore(start_src_pos, held);
                held = nullptr;
                src_to_dst.clear();
                src_by_dst.clear();
                break;
            }
            const uint32_t prev_src = it_prev->second;
            MoveStoneForSnapshotRestore(key_to_pos(prev_src), key_to_pos(empty));
            auto it_dst = src_to_dst.find(prev_src);
            if (it_dst != src_to_dst.end())
                src_to_dst.erase(it_dst);
            src_by_dst.erase(it_prev);
            empty = prev_src;
        }
        if (held) {
            SetStoneForSnapshotRestore(key_to_pos(start_dst), held);
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

GridObject *get_grid_object_at(GridLayer layer, GridPos pos) {
    switch (layer) {
    case GRID_FLOOR:
        return GetFloor(pos);
    case GRID_ITEMS:
        return GetItem(pos);
    case GRID_STONES:
        return GetStone(pos);
    default:
        return nullptr;
    }
}

GridObject *yield_grid_object_for_snapshot_restore(GridLayer layer, GridPos pos) {
    switch (layer) {
    case GRID_FLOOR:
        return YieldFloorForSnapshotRestore(pos);
    case GRID_ITEMS:
        return YieldItemForSnapshotRestore(pos);
    case GRID_STONES:
        return YieldStoneForSnapshotRestore(pos);
    default:
        return nullptr;
    }
}

void set_grid_object_for_snapshot_restore(GridLayer layer, GridPos pos, GridObject *obj) {
    if (!obj)
        return;
    switch (layer) {
    case GRID_FLOOR:
        SetFloorForSnapshotRestore(pos, dynamic_cast<Floor *>(obj));
        return;
    case GRID_ITEMS:
        SetItemForSnapshotRestore(pos, dynamic_cast<Item *>(obj));
        return;
    case GRID_STONES:
        SetStoneForSnapshotRestore(pos, dynamic_cast<Stone *>(obj));
        return;
    default:
        return;
    }
}

void preserve_or_dispose_grid_object(GridObject *obj) {
    if (!obj)
        return;
    if (!TryPreserveDisposedGridObject(obj))
        DisposeObject(obj);
}

void restore_grid_object_layout(const std::vector<ObjectStateSnapshot> &wanted) {
    for (const auto &snap : wanted) {
        if (snap.object_id < 0 || snap.layer == GRID_COUNT || snap.x < 0 || snap.y < 0)
            continue;
        const GridPos pos(snap.x, snap.y);
        GridObject *current = get_grid_object_at(snap.layer, pos);
        if (current && current->getId() == snap.object_id)
            continue;

        if (current)
            preserve_or_dispose_grid_object(yield_grid_object_for_snapshot_restore(snap.layer, pos));

        GridObject *want = dynamic_cast<GridObject *>(Object::getObject(snap.object_id));
        if (!want)
            want = take_preserved_disposed_grid_object(snap.object_id);
        if (!want)
            continue;

        if (want->isDisplayable()) {
            const GridPos current_pos = want->get_pos();
            if (current_pos != pos) {
                GridObject *lifted = yield_grid_object_for_snapshot_restore(snap.layer, current_pos);
                if (lifted && lifted != want)
                    preserve_or_dispose_grid_object(lifted);
            }
        }

        if (!get_grid_object_at(snap.layer, pos))
            set_grid_object_for_snapshot_restore(snap.layer, pos, want);
    }
}

}  // namespace

Snapshot::AnimatedGridModel::AnimatedGridModel(const AnimatedGridModel &other)
: object_id(other.object_id),
  layer(other.layer),
  x(other.x),
  y(other.y),
  model(other.model ? std::unique_ptr<::display::Model>(other.model->clone()) : nullptr) {
}

Snapshot::AnimatedGridModel &Snapshot::AnimatedGridModel::operator=(const AnimatedGridModel &other) {
    if (this == &other)
        return *this;
    object_id = other.object_id;
    layer = other.layer;
    x = other.x;
    y = other.y;
    model.reset(other.model ? other.model->clone() : nullptr);
    return *this;
}

Snapshot::AnimatedGridModel::~AnimatedGridModel() = default;

Snapshot::RetainedGridObjects::~RetainedGridObjects() {
    release_retained_grid_ids(object_ids);
}

ActorSnapshot CaptureActor(const Actor &actor) {
    ActorSnapshot snap;
    snap.object_id = actor.getId();
    snap.internal_state = actor.snapshot_internal_state();
    snap.info = capture_actorinfo(actor.get_actorinfo());
    return snap;
}

void RestoreActor(const ActorSnapshot &snap) {
    Actor *actor = dynamic_cast<Actor *>(Object::getObject(snap.object_id));
    if (!actor)
        return;
    actor->restore_internal_state(snap.internal_state);
    restore_actorinfo(*actor, snap.info);
    if (BasicBall *ball = dynamic_cast<BasicBall *>(actor))
        ball->MpFinishAppearingAfterSnapshotRestore();
}

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
                        const GridPos p(x, y);
                        Snapshot::AnimatedGridModel entry;
                        if (layer == GRID_STONES) {
                            Stone *st = GetStone(p);
                            if (st && st->is_movable())
                                entry.object_id = st->getId();
                        }
                        ::display::Model *model = ::display::GetModel(GridLoc(layer, p));
                        if (!model || !model->needs_runtime_snapshot())
                            continue;
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
    retain_snapshot_grid_objects(snap);

    std::vector<Actor *> actors;
    GetActors(actors);
    snap.actors.reserve(actors.size());
    for (Actor *a : actors) {
        if (!a)
            continue;
        snap.actors.push_back(CaptureActor(*a));
    }
    return snap;
}

void Restore(const Snapshot &snap) {
    input::RestoreSnapshot(snap.input);
    server::RandomState = snap.random_state;
    server::LevelTime = snap.level_time;
    restore_movable_stone_positions(snap.movable_stones);
    restore_grid_object_layout(snap.object_states);
    RestoreObjectStates(snap.object_states);
    RestoreOtherStates(snap.other_states);
    // Laser beams are runtime graph objects, not part of the grid-object snapshot.
    // Rebuild them after grid/object state is back in place, but suppress
    // light-change actions; snapshot restore must not synthesize new gameplay.
    RecalcLight();
    PerformRecalcLight(true);
    GameTimer.restore(snap.game_timer);
    RestorePendingActions(snap.pending_actions);
    for (const auto &entry : snap.animated_grid_models) {
        if (!entry.model)
            continue;
        GridPos pos(static_cast<int>(entry.x), static_cast<int>(entry.y));
        if (entry.object_id >= 0) {
            Stone *st = dynamic_cast<Stone *>(Object::getObject(entry.object_id));
            if (!st || !st->isDisplayable())
                continue;
            pos = st->get_pos();
        }
        ::display::SetModel(GridLoc(entry.layer, pos), entry.model->clone());
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

bool TryPreserveDisposedGridObject(GridObject *obj) {
    if (!obj)
        return false;
    const int object_id = obj->getId();
    auto it = g_retained_grid_snapshot_refs.find(object_id);
    if (it == g_retained_grid_snapshot_refs.end() || it->second <= 0)
        return false;
    UnnameObject(obj);
    if (TimeHandler *th = dynamic_cast<TimeHandler *>(obj))
        GameTimer.remove_all_alarms(th);
    obj->MpDetachFromRepositoryForSnapshotPreserve();
    g_preserved_disposed_grid_objects[object_id] = obj;
    return true;
}

}  // namespace sim_snapshot
}  // namespace multiplayer
}  // namespace enigma
