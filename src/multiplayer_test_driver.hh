#ifndef MULTIPLAYER_TEST_DRIVER_HH_INCLUDED
#define MULTIPLAYER_TEST_DRIVER_HH_INCLUDED

#include <string>

namespace enigma {
namespace multiplayer {
namespace testdriver {

// Enables the multiplayer test driver when both values are non-empty.
// `connect_host_port` must be in "host:port" form.
void Configure(const std::string &role, const std::string &connect_host_port);

bool Enabled();

// Runs a minimal tick loop that services:
// - test driver control socket
// - multiplayer session tick
// - client tick (events/render)
// - server tick (simulation)
//
// Intended to be used when Enigma is started with `--mp-test-*` flags.
void Run();

// Processes incoming control commands and emits events back to the controller.
// Safe to call even when not Enabled().
void Tick(double dtime);

}  // namespace testdriver
}  // namespace multiplayer
}  // namespace enigma

#endif  // MULTIPLAYER_TEST_DRIVER_HH_INCLUDED

