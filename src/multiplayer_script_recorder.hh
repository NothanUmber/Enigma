#ifndef MULTIPLAYER_SCRIPT_RECORDER_HH_INCLUDED
#define MULTIPLAYER_SCRIPT_RECORDER_HH_INCLUDED

#include "input.hh"

#include <cstdint>
#include <string>
#include <vector>

namespace enigma {
namespace multiplayer {

namespace setupsnapshot {

struct ActorState {
    unsigned player = 0;
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
};

struct CellState {
    int x = 0;
    int y = 0;
    std::string floor_kind;
    int floor_state = 0;
    std::string item_kind;
    int item_state = 0;
    std::string stone_kind;
    int stone_state = 0;
};

struct Snapshot {
    std::vector<ActorState> actors;
    std::vector<CellState> cells;
};

Snapshot Capture();
bool SaveToFile(const Snapshot &snapshot, const std::string &path);
bool LoadFromFile(const std::string &path, Snapshot &snapshot);
bool Restore(const Snapshot &snapshot);

}  // namespace setupsnapshot

namespace scriptrecorder {

bool Toggle();
bool IsRecording();
std::string StatusMessage();
bool CaptureSetupSnapshot();
void RecordLocalPending(uint32_t current_tick, unsigned local_player, const input::PlayerInput &pending);

}  // namespace scriptrecorder
}  // namespace multiplayer
}  // namespace enigma

#endif
