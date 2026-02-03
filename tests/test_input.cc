#include "input.hh"

#include <cassert>
#include <iostream>

int main() {
    using namespace enigma::input;

    Reset();
    SubmitMouseForce(0, ecl::V2(1.0, 2.0));
    SubmitRotateInventory(0, 1);
    SubmitActivateItem(0);

    PlayerInput pending = DrainLocalPending(0);
    assert(pending.mouse_force[0] == 1.0);
    assert(pending.mouse_force[1] == 2.0);
    assert(pending.rotate_steps == 1);
    assert(pending.activate_count == 1);

    PlayerInput empty = DrainLocalPending(0);
    assert(empty.empty());

    Reset();
    SetNetworked(true);
    SetExpectedPlayers(2);

    PlayerInput p0;
    p0.mouse_force = ecl::V2(0.5, -0.5);
    PlayerInput p1;
    p1.rotate_steps = -2;

    EnqueueInput(0, 0, p0);
    assert(!CanAdvanceTick());
    EnqueueInput(0, 1, p1);
    assert(CanAdvanceTick());

    PlayerInput c0 = ConsumeInput(0, 0);
    PlayerInput c1 = ConsumeInput(0, 1);
    assert(c0.mouse_force[0] == 0.5);
    assert(c0.mouse_force[1] == -0.5);
    assert(c1.rotate_steps == -2);

    AdvanceTick();
    assert(CurrentTick() == 1);

    std::cout << "test_input ok\n";
    return 0;
}
