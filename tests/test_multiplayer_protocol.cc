#include "multiplayer_protocol.hh"

#include <cassert>
#include <iostream>

int main() {
    using namespace enigma::multiplayer::protocol;

    LobbyAnnounce announce;
    announce.id = "abc";
    announce.name = "Player";
    announce.level_id = "level/path";
    announce.player_count = 2;

    ecl::Buffer buf;
    encode_lobby_announce(buf, announce);
    LobbyAnnounce decoded_announce;
    ecl::Buffer buf_copy;
    buf_copy.assign(const_cast<char *>(buf.data()), buf.size());
    assert(decode_lobby_announce(buf_copy, decoded_announce));
    assert(decoded_announce.id == announce.id);
    assert(decoded_announce.name == announce.name);
    assert(decoded_announce.level_id == announce.level_id);
    assert(decoded_announce.player_count == announce.player_count);

    LobbyStart start;
    start.session_id = 42;
    start.level_id = "level/path";
    start.seed = 123;
    start.expected_players = 2;
    start.host_port = 12345;
    start.host_id = "host";
    start.filter_optimized = 0;

    buf.clear();
    encode_lobby_start(buf, start);
    LobbyStart decoded_start;
    buf_copy.assign(const_cast<char *>(buf.data()), buf.size());
    assert(decode_lobby_start(buf_copy, decoded_start));
    assert(decoded_start.session_id == start.session_id);
    assert(decoded_start.level_id == start.level_id);
    assert(decoded_start.seed == start.seed);
    assert(decoded_start.expected_players == start.expected_players);
    assert(decoded_start.host_port == start.host_port);
    assert(decoded_start.host_id == start.host_id);
    assert(decoded_start.filter_optimized == start.filter_optimized);

    InputPacket input;
    input.tick = 77;
    input.player = 1;
    input.mouse_x = 1.5f;
    input.mouse_y = -2.5f;
    input.rotate_steps = -1;
    input.activate_count = 3;

    buf.clear();
    encode_input(buf, input);
    InputPacket decoded_input;
    buf_copy.assign(const_cast<char *>(buf.data()), buf.size());
    assert(decode_input(buf_copy, decoded_input));
    assert(decoded_input.tick == input.tick);
    assert(decoded_input.player == input.player);
    assert(decoded_input.mouse_x == input.mouse_x);
    assert(decoded_input.mouse_y == input.mouse_y);
    assert(decoded_input.rotate_steps == input.rotate_steps);
    assert(decoded_input.activate_count == input.activate_count);

    SyncPacket sync;
    sync.tick = 5;
    sync.random_state = 777;
    sync.p0_x = 1.0f;
    sync.p0_y = 2.0f;
    sync.p1_x = 3.0f;
    sync.p1_y = 4.0f;

    buf.clear();
    encode_sync(buf, sync);
    SyncPacket decoded_sync;
    buf_copy.assign(const_cast<char *>(buf.data()), buf.size());
    assert(decode_sync(buf_copy, decoded_sync));
    assert(decoded_sync.tick == sync.tick);
    assert(decoded_sync.random_state == sync.random_state);
    assert(decoded_sync.p0_x == sync.p0_x);
    assert(decoded_sync.p0_y == sync.p0_y);
    assert(decoded_sync.p1_x == sync.p1_x);
    assert(decoded_sync.p1_y == sync.p1_y);

    PlacementPacket place;
    place.restart_id = 3;
    place.player = 2;
    place.x = 17;
    place.y = 9;

    buf.clear();
    encode_place(buf, place);
    PlacementPacket decoded_place;
    buf_copy.assign(const_cast<char *>(buf.data()), buf.size());
    assert(decode_place(buf_copy, decoded_place));
    assert(decoded_place.restart_id == place.restart_id);
    assert(decoded_place.player == place.player);
    assert(decoded_place.x == place.x);
    assert(decoded_place.y == place.y);

    std::cout << "test_multiplayer_protocol ok\n";
    return 0;
}
