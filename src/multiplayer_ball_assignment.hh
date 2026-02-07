#ifndef MULTIPLAYER_BALL_ASSIGNMENT_HH_INCLUDED
#define MULTIPLAYER_BALL_ASSIGNMENT_HH_INCLUDED

#include <cstddef>
#include <vector>

namespace enigma {
namespace multiplayer {
namespace internal {

// Returns a per-ball assignment to players, distributing balls as evenly as
// possible in a deterministic round-robin order.
//
// The caller decides whether redistribution is appropriate (e.g. only when the
// authored level provides enough balls for all players in a color-group).
inline std::vector<unsigned> distribute_balls_round_robin(size_t ball_count,
                                                          const std::vector<unsigned> &players) {
    std::vector<unsigned> out;
    out.reserve(ball_count);
    if (players.empty()) {
        for (size_t i = 0; i < ball_count; ++i)
            out.push_back(0);
        return out;
    }
    for (size_t i = 0; i < ball_count; ++i)
        out.push_back(players[i % players.size()]);
    return out;
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma

#endif  // MULTIPLAYER_BALL_ASSIGNMENT_HH_INCLUDED
