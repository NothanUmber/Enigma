#ifndef MULTIPLAYER_EXTRA_PLAYERS_HH_INCLUDED
#define MULTIPLAYER_EXTRA_PLAYERS_HH_INCLUDED

#include "multiplayer_internal.hh"

#include "world.hh"

namespace enigma {
namespace multiplayer {
namespace internal {

unsigned compute_level_players();

bool placement_required_for_player(unsigned player);
bool is_valid_placement(const GridPos &pos, unsigned player);

void apply_placement(unsigned player, const GridPos &pos);
void broadcast_placement(const protocol::PlacementPacket &msg);

void send_existing_placements_to_peer(ENetPeer *peer);
void send_existing_placements_to_relay(Uint32 client_id);
void send_existing_placements_to_tcp_relay(Uint32 client_id);
void send_placement_to_host(const GridPos &pos);

void add_extra_actors(unsigned level_players, unsigned expected_players);
void auto_place_extra_players();

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma

#endif

