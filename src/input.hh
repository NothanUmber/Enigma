#ifndef INPUT_HH_INCLUDED
#define INPUT_HH_INCLUDED

#include "ecl_math.hh"

#include <array>
#include <cstdint>

namespace enigma {
namespace input {

constexpr unsigned kMaxPlayers = 8;

struct PlayerInput {
    ecl::V2 mouse_force;
    int rotate_steps;
    int activate_count;

    PlayerInput() : mouse_force(0, 0), rotate_steps(0), activate_count(0) {}

    bool empty() const {
        return mouse_force[0] == 0.0 && mouse_force[1] == 0.0 &&
               rotate_steps == 0 && activate_count == 0;
    }

    void merge(const PlayerInput &other) {
        mouse_force += other.mouse_force;
        rotate_steps += other.rotate_steps;
        activate_count += other.activate_count;
    }
};

void Reset();
void SetNetworked(bool enabled);
bool IsNetworked();
bool ZerofillMissingInputsEnabled();
void SetExpectedPlayers(unsigned count);
unsigned ExpectedPlayers();
uint32_t CurrentTick();

void SubmitMouseForce(unsigned player, const ecl::V2 &force);
void SubmitRotateInventory(unsigned player, int dir);
void SubmitActivateItem(unsigned player);

PlayerInput DrainLocalPending(unsigned player);

void EnqueueInput(uint32_t tick, unsigned player, const PlayerInput &input);
bool HasInput(uint32_t tick, unsigned player);
bool PeekInput(uint32_t tick, unsigned player, PlayerInput &out);
bool CanAdvanceTick();
PlayerInput ConsumeInput(uint32_t tick, unsigned player);
void AdvanceTick();

}  // namespace input
}  // namespace enigma

#endif
