#include "multiplayer_ball_assignment.hh"

#include <cassert>
#include <iostream>
#include <vector>

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

