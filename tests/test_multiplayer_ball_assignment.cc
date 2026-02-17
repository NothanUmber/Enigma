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

#include "multiplayer_ball_assignment.hh"

#include <cassert>
#include <iostream>
#include <vector>

/* -------------------- Multiplayer ball assignment tests -------------------- */
/*
 * Minimal unit tests for deterministic ball assignment helpers.
 */

int main() {
    using enigma::multiplayer::internal::distribute_balls_round_robin;

    {
        std::vector<unsigned> players = {0, 1};
        auto a = distribute_balls_round_robin(4, players);
        assert((a == std::vector<unsigned>({0, 1, 0, 1})));
    }

    {
        std::vector<unsigned> players = {0, 1, 2};
        auto a = distribute_balls_round_robin(4, players);
        assert((a == std::vector<unsigned>({0, 1, 2, 0})));
    }

    {
        std::vector<unsigned> players = {0, 2};
        auto a = distribute_balls_round_robin(2, players);
        assert((a == std::vector<unsigned>({0, 2})));
    }

    std::cout << "test_multiplayer_ball_assignment ok\n";
    return 0;
}
