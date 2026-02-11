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
 *
 * It does not attempt to serialize/restore the full world object graph (grid
 * composition, arbitrary Lua state, etc.). That is a separate, larger engine
 * project.
 */

#include "multiplayer_sim_snapshot.hh"

#include "actors.hh"
#include "server.hh"
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

}  // namespace

Snapshot Capture() {
    Snapshot snap;
    snap.input = input::CaptureSnapshot();
    snap.random_state = server::RandomState;
    snap.level_time = server::LevelTime;
    snap.game_timer = GameTimer.snapshot();
    snap.pending_actions = CapturePendingActions();

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
    }
}

}  // namespace sim_snapshot
}  // namespace multiplayer
}  // namespace enigma

