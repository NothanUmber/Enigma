#ifndef MULTIPLAYER_SIM_SNAPSHOT_HH_INCLUDED
#define MULTIPLAYER_SIM_SNAPSHOT_HH_INCLUDED

#include "input.hh"
#include "timer.hh"
#include "world.hh"

#include "ecl_math.hh"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace enigma {

class Actor;

namespace multiplayer {
namespace sim_snapshot {

struct ContactSnapshot {
    ecl::V2 pos;
    ecl::V2 normal;
};

struct ActorInfoSnapshot {
    ecl::V2 pos;
    ecl::V2 render_pos;
    bool render_initialized = false;
    ecl::V2 vel;
    ecl::V2 frozen_vel;
    ecl::V2 pos_force;
    ecl::V2 forceacc;
    double charge = 0.0;
    double mass = 0.0;
    double radius = 0.0;
    bool grabbed = false;
    bool created = false;
    bool ignore_contacts = false;

    ecl::V2 force;
    ecl::V2 collforce;
    double friction = 0.0;

    std::array<ContactSnapshot, 7> contacts_a;
    std::array<ContactSnapshot, 7> contacts_b;
    int contacts_count = 0;
    int last_contacts_count = 0;
    // Which contact buffer is currently active (0=a, 1=b).
    int contacts_sel = 0;
    int last_contacts_sel = 1;
};

struct ActorSnapshot {
    int object_id = -1;
    int internal_state = 0;
    ActorInfoSnapshot info;
};

struct Snapshot {
    input::Snapshot input;
    int32_t random_state = 0;
    double level_time = 0.0;
    Timer::Snapshot game_timer;
    std::vector<Action> pending_actions;
    // GridObject "state" attribute snapshot for all floors/items/stones.
    std::vector<int> object_state_ids;
    std::vector<int> object_state_values;
    std::vector<ActorSnapshot> actors;
};

Snapshot Capture();
void Restore(const Snapshot &snap);

}  // namespace sim_snapshot
}  // namespace multiplayer

}  // namespace enigma

#endif
