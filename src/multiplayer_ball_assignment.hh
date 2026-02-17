/*
 * 2026 LLM generated contribution - concept, review and revision by Ferdinand Strixner
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef MULTIPLAYER_BALL_ASSIGNMENT_HH_INCLUDED
#define MULTIPLAYER_BALL_ASSIGNMENT_HH_INCLUDED

/* -------------------- Multiplayer ball assignment -------------------- */
/*
 * Helpers for deterministic ball-to-player assignment.
 *
 * Used by the "more balls than players" path (meditation-like levels) to split
 * authored actors across controllers while keeping the level's rubberbands and
 * other connections intact.
 */

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
