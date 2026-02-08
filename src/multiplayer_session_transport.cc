#include "multiplayer_session.hh"

#include "multiplayer_extra_players.hh"
#include "multiplayer_session_impl.hh"
#include "multiplayer_transport.hh"

#include "client.hh"
#include "errors.hh"
#include "input.hh"
#include "options.hh"
#include "player.hh"
#include "server.hh"
#include "world.hh"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace enigma {
namespace multiplayer {
namespace internal {

bool abort_session_with_message(const char *message) {
    if (!client::AbortGameP())
        client::Msg_ShowText(message, true, 3.0);
    client::Msg_Command("abort");
    SessionShutdown();
    return false;
}

bool local_can_send_ready() {
    if (!g_session.active || g_session.host)
        return false;
    if (!g_session.local_player_known)
        return false;
    return true;
}

bool has_remote_peers() {
    return !g_session.peer_players.empty() || !g_session.relay_players.empty() ||
           !g_session.tcp_relay_players.empty();
}

bool can_accept_more_remote_players() {
    return g_session.next_player_id < g_session.expected_players;
}

namespace {

bool lookup_remote_player(HostSource source, ENetPeer *peer, Uint32 relay_client_id,
                          unsigned &player_id) {
    if (source == HostSource::UDP_RELAY) {
        auto it = g_session.relay_players.find(relay_client_id);
        if (it != g_session.relay_players.end()) {
            player_id = it->second;
            return true;
        }
        return false;
    }
    if (source == HostSource::TCP_RELAY) {
        auto it = g_session.tcp_relay_players.find(relay_client_id);
        if (it != g_session.tcp_relay_players.end()) {
            player_id = it->second;
            return true;
        }
        return false;
    }
    auto it = g_session.peer_players.find(peer);
    if (it != g_session.peer_players.end()) {
        player_id = it->second;
        return true;
    }
    return false;
}

void mark_remote_ready(HostSource source, ENetPeer *peer, Uint32 relay_client_id) {
    if (source == HostSource::UDP_RELAY)
        g_session.relay_ready[relay_client_id] = true;
    else if (source == HostSource::TCP_RELAY)
        g_session.tcp_relay_ready[relay_client_id] = true;
    else
        g_session.peer_ready[peer] = true;
}

void send_input_to_peer(ENetPeer *peer, const protocol::InputPacket &pkt) {
    ecl::Buffer buf;
    protocol::encode_input(buf, pkt);
    g_transport.HostSendDirect(peer, buf);
}

void broadcast_input(const protocol::InputPacket &pkt, ENetPeer *exclude, Uint32 exclude_udp_relay,
                     Uint32 exclude_tcp_relay) {
    for (const auto &entry : g_session.peer_players) {
        if (entry.first == exclude)
            continue;
        send_input_to_peer(entry.first, pkt);
    }
    ecl::Buffer buf;
    protocol::encode_input(buf, pkt);
    if (!g_session.relay_players.empty())
        g_transport.HostBroadcastUdpRelay(buf, exclude_udp_relay);
    if (!g_session.tcp_relay_players.empty())
        g_transport.HostBroadcastTcpRelay(buf, exclude_tcp_relay);
}

bool handle_host_input_packet(const char *data, size_t len, ENetPeer *peer, HostSource source,
                              Uint32 relay_client_id) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::InputPacket input_msg;
    if (!protocol::decode_input(buf, input_msg))
        return false;
    if (input_msg.epoch != g_session.input_epoch) {
        debug_log("mp drop input: tick=%u epoch=%u local epoch=%u", input_msg.tick, input_msg.epoch,
                  g_session.input_epoch);
        return true;
    }
    if (input_msg.tick < 20) {
        debug_log("mp recv input: tick=%u player=%u mouse=(%.2f,%.2f) rot=%d act=%u",
                  input_msg.tick, input_msg.player, input_msg.mouse_x, input_msg.mouse_y,
                  static_cast<int>(input_msg.rotate_steps),
                  static_cast<unsigned>(input_msg.activate_count));
    }
    if (input_msg.tick < input::CurrentTick())
        return true;
    unsigned player_id = 0;
    if (!lookup_remote_player(source, peer, relay_client_id, player_id))
        return true;
    input::PlayerInput pi;
    pi.mouse_force = ecl::V2(input_msg.mouse_x, input_msg.mouse_y);
    pi.rotate_steps = input_msg.rotate_steps;
    pi.activate_count = input_msg.activate_count;
    input::EnqueueInput(input_msg.tick, player_id, pi);
    protocol::InputPacket forward = input_msg;
    forward.player = static_cast<Uint8>(player_id);
    broadcast_input(forward, source == HostSource::DIRECT ? peer : nullptr,
                    source == HostSource::UDP_RELAY ? relay_client_id : 0,
                    source == HostSource::TCP_RELAY ? relay_client_id : 0);
    return true;
}

bool handle_host_resync_request_packet(const char *data, size_t len, HostSource source,
                                       Uint32 relay_client_id, ENetPeer *peer) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::ResyncRequest req;
    if (!protocol::decode_resync_request(buf, req))
        return false;
    if (req.epoch == g_session.input_epoch) {
        if (source == HostSource::UDP_RELAY)
            send_resync_state_to_relay(relay_client_id);
        else if (source == HostSource::TCP_RELAY)
            send_resync_state_to_tcp_relay(relay_client_id);
        else
            send_resync_state(peer);
    }
    return true;
}

bool handle_host_ready_packet(const char *data, size_t len, ENetPeer *peer, HostSource source,
                              Uint32 relay_client_id) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    if (!protocol::decode_ready(buf))
        return false;
    unsigned player_id = 0;
    if (lookup_remote_player(source, peer, relay_client_id, player_id)) {
        debug_log("mp host: ready player=%u source=%s", player_id, host_source_name(source));
        mark_remote_ready(source, peer, relay_client_id);
        debug_log("mp host: peer ready");
    }
    return true;
}

bool handle_host_pause_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 epoch = 0;
    bool paused = false;
    if (!protocol::decode_pause(buf, epoch, paused))
        return false;
    if (epoch != g_session.input_epoch)
        return true;
    SessionRequestPause(paused);
    return true;
}

bool handle_host_menu_packet(const char *data, size_t len, ENetPeer *peer, HostSource source,
                             Uint32 relay_client_id) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 epoch = 0;
    Uint8 ignored_player = 0;
    bool open = false;
    if (!protocol::decode_menu(buf, epoch, ignored_player, open))
        return false;
    if (epoch != g_session.input_epoch)
        return true;
    unsigned player_id = 0;
    if (!lookup_remote_player(source, peer, relay_client_id, player_id))
        return true;
    if (player_id >= g_session.menu_open.size())
        return true;
    if (g_session.menu_open[player_id] == open)
        return true;
    g_session.menu_open[player_id] = open;

    bool want_pause = false;
    for (bool any : g_session.menu_open) {
        if (any) {
            want_pause = true;
            break;
        }
    }
    SessionRequestPause(want_pause);
    return true;
}

bool handle_host_abort_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 epoch = 0;
    if (!protocol::decode_abort(buf, epoch))
        return false;
    if (epoch != g_session.input_epoch)
        return true;
    // Abort is global: one player aborts => end the session for all.
    send_abort_to_peers();
    begin_abort_after_grace("Game aborted. Ending session.");
    return true;
}

bool handle_host_placement_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::PlacementPacket place;
    if (!protocol::decode_place(buf, place))
        return false;
    if (place.restart_id == g_session.restart_id && place.player < g_session.expected_players) {
        GridPos pos(static_cast<int>(place.x), static_cast<int>(place.y));
        if (placement_required_for_player(place.player) && is_valid_placement(pos, place.player)) {
            apply_placement(place.player, pos);
            if (g_session.placement_received.size() > place.player)
                g_session.placement_received[place.player] = true;
            broadcast_placement(place);
        }
    }
    return true;
}

bool handle_host_packet(const char *data, size_t len, ENetPeer *peer, HostSource source,
                        Uint32 relay_client_id) {
    if (handle_host_input_packet(data, len, peer, source, relay_client_id))
        return true;
    if (handle_host_resync_request_packet(data, len, source, relay_client_id, peer))
        return true;
    if (handle_host_ready_packet(data, len, peer, source, relay_client_id))
        return true;
    if (handle_host_menu_packet(data, len, peer, source, relay_client_id))
        return true;
    if (handle_host_pause_packet(data, len))
        return true;
    if (handle_host_abort_packet(data, len))
        return true;
    if (handle_host_placement_packet(data, len))
        return true;
    return false;
}

bool handle_client_input_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::InputPacket input_msg;
    if (!protocol::decode_input(buf, input_msg))
        return false;
    if (input_msg.epoch != g_session.input_epoch) {
        debug_log("mp drop input: tick=%u epoch=%u local epoch=%u", input_msg.tick, input_msg.epoch,
                  g_session.input_epoch);
        return true;
    }
    if (input_msg.tick < 20) {
        debug_log("mp recv input: tick=%u player=%u mouse=(%.2f,%.2f) rot=%d act=%u",
                  input_msg.tick, input_msg.player, input_msg.mouse_x, input_msg.mouse_y,
                  static_cast<int>(input_msg.rotate_steps),
                  static_cast<unsigned>(input_msg.activate_count));
    }
    if (input_msg.tick < input::CurrentTick())
        return true;
    input::PlayerInput pi;
    pi.mouse_force = ecl::V2(input_msg.mouse_x, input_msg.mouse_y);
    pi.rotate_steps = input_msg.rotate_steps;
    pi.activate_count = input_msg.activate_count;
    input::EnqueueInput(input_msg.tick, input_msg.player, pi);
    return true;
}

bool handle_client_welcome_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint8 player_id = 0;
    Uint8 expected_players = 0;
    Uint32 seed = 0;
    if (!protocol::decode_welcome(buf, player_id, expected_players, seed))
        return false;
    g_session.local_player = player_id;
    g_session.local_player_known = true;
    g_session.expected_players = expected_players;
    g_session.seed = seed;
    input::SetExpectedPlayers(expected_players);
    debug_log("mp client: welcome player=%u expected=%u seed=%u", player_id, expected_players, seed);
    if (debug_enabled())
        debug_log("mp client: transport=%s", transport_name(g_session.active_transport));
    if (g_session.phase == SessionState::Phase::WAITING_FOR_READY && !g_session.local_ready_sent) {
        send_ready_to_host();
        g_session.local_ready_sent = true;
        g_session.ready_timer = 0.0;
    }
    return true;
}

bool handle_client_sync_packet(const char *data, size_t len) {
    if (g_session.abort_pending)
        return true;
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::SyncPacket sync;
    if (!protocol::decode_sync(buf, sync))
        return false;
    uint32_t local_tick = input::CurrentTick();
    if (local_tick < sync.tick) {
        g_session.pending_sync = sync;
        g_session.has_pending_sync = true;
    } else if (local_tick == sync.tick) {
        handle_sync_current(sync);
    } else {
        SessionState::ChecksumSample sample;
        if (lookup_checksum_sample(sync.tick, sample)) {
            handle_sync_sample(sync, sample);
        } else {
            debug_log("mp sync skip: sync tick=%u local tick=%u (no history)", sync.tick,
                      local_tick);
        }
    }
    return true;
}

bool handle_client_resync_state_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::ResyncState state;
    if (!protocol::decode_resync_state(buf, state))
        return false;
    apply_resync_state(state);
    return true;
}

bool handle_client_start_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 epoch = 0;
    if (!protocol::decode_start(buf, epoch))
        return false;
    g_session.input_epoch = epoch;
    g_session.debug_state_dumped = false;
    configure_input_session(g_session.expected_players);
    g_session.phase = SessionState::Phase::READY_TO_START;
    debug_log("mp client: start allowed");
    return true;
}

bool handle_client_pause_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 epoch = 0;
    bool paused = false;
    if (!protocol::decode_pause(buf, epoch, paused))
        return false;
    if (epoch != g_session.input_epoch)
        return true;
    if (g_session.paused == paused)
        return true;
    g_session.paused = paused;
    server::Msg_Pause(paused);
    return true;
}

bool handle_client_abort_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 epoch = 0;
    if (!protocol::decode_abort(buf, epoch))
        return false;
    if (epoch != g_session.input_epoch)
        return true;
    return abort_session_with_message("Game aborted. Ending session.");
}

bool handle_client_restart_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::RestartPacket restart;
    if (!protocol::decode_restart(buf, restart))
        return false;
    if (restart.restart_id > g_session.last_restart_id) {
        g_session.last_restart_id = restart.restart_id;
        g_session.restart_id = restart.restart_id;
        if (restart.level_restart)
            server::RestartLevelFromNetwork();
        else
            server::Msg_RestartGameFromNetwork();
    }
    return true;
}

bool handle_client_placement_packet(const char *data, size_t len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    protocol::PlacementPacket place;
    if (!protocol::decode_place(buf, place))
        return false;
    if (place.restart_id == g_session.restart_id && place.player < g_session.expected_players) {
        GridPos pos(static_cast<int>(place.x), static_cast<int>(place.y));
        apply_placement(place.player, pos);
    }
    return true;
}

void handle_client_payload(const char *data, size_t len) {
    if (handle_client_input_packet(data, len))
        return;
    if (handle_client_welcome_packet(data, len))
        return;
    if (handle_client_sync_packet(data, len))
        return;
    if (handle_client_resync_state_packet(data, len))
        return;
    if (handle_client_start_packet(data, len))
        return;
    if (handle_client_pause_packet(data, len))
        return;
    if (handle_client_abort_packet(data, len))
        return;
    if (handle_client_restart_packet(data, len))
        return;
    handle_client_placement_packet(data, len);
}

bool host_register_relay_client(Uint32 client_id, HostSource source);
bool host_unregister_relay_client(Uint32 client_id, HostSource source);

bool host_register_udp_relay_client(Uint32 client_id) {
    return host_register_relay_client(client_id, HostSource::UDP_RELAY);
}

bool host_register_tcp_relay_client(Uint32 client_id) {
    return host_register_relay_client(client_id, HostSource::TCP_RELAY);
}

bool host_unregister_udp_relay_client(Uint32 client_id) {
    return host_unregister_relay_client(client_id, HostSource::UDP_RELAY);
}

bool host_unregister_tcp_relay_client(Uint32 client_id) {
    return host_unregister_relay_client(client_id, HostSource::TCP_RELAY);
}

bool host_register_relay_client(Uint32 client_id, HostSource source) {
    if (!can_accept_more_remote_players())
        return false;

    unsigned player_id = g_session.next_player_id++;
    ecl::Buffer welcome;
    protocol::encode_welcome(welcome, static_cast<Uint8>(player_id),
                             static_cast<Uint8>(g_session.expected_players), g_session.seed);

    if (source == HostSource::UDP_RELAY) {
        g_session.relay_players[client_id] = player_id;
        g_session.relay_ready[client_id] = false;
        debug_log("mp host: relay client -> player %u", player_id);
        g_transport.HostSendUdpRelay(client_id, welcome);
        send_existing_placements_to_relay(client_id);
        return true;
    }

    if (source == HostSource::TCP_RELAY) {
        g_session.tcp_relay_players[client_id] = player_id;
        g_session.tcp_relay_ready[client_id] = false;
        debug_log("mp host: tcp relay client -> player %u", player_id);
        g_transport.HostSendTcpRelay(client_id, welcome);
        send_existing_placements_to_tcp_relay(client_id);
        return true;
    }

    return false;
}

bool host_unregister_relay_client(Uint32 client_id, HostSource source) {
    if (source == HostSource::UDP_RELAY) {
        g_session.relay_players.erase(client_id);
        g_session.relay_ready.erase(client_id);
    } else if (source == HostSource::TCP_RELAY) {
        g_session.tcp_relay_players.erase(client_id);
        g_session.tcp_relay_ready.erase(client_id);
    } else {
        return true;
    }
    if (g_session.active)
        return abort_session_with_message("Player disconnected. Ending session.");
    return true;
}

bool handle_direct_connect_event(ENetPeer *peer) {
    if (!g_session.host)
        return true;
    if (!can_accept_more_remote_players()) {
        enet_peer_disconnect(peer, 0);
        return true;
    }

    unsigned player_id = g_session.next_player_id++;
    g_session.peer_players[peer] = player_id;
    g_session.peer_ready[peer] = false;
    peer->data = reinterpret_cast<void *>(static_cast<uintptr_t>(player_id));
    debug_log("mp host: peer connected -> player %u", player_id);

    ecl::Buffer buf;
    protocol::encode_welcome(buf, static_cast<Uint8>(player_id),
                             static_cast<Uint8>(g_session.expected_players), g_session.seed);
    g_transport.HostSendDirect(peer, buf);
    send_existing_placements_to_peer(peer);
    return true;
}

bool handle_relay_payload_as_host(const char *data, size_t len, HostSource source,
                                  bool close_on_decode_error, bool (*on_connect)(Uint32),
                                  bool (*on_disconnect)(Uint32)) {
    RelayMessageType type = RELAY_ERROR;
    Uint32 session_id = 0;
    Uint32 client_id = 0;
    const char *payload = nullptr;
    size_t payload_len = 0;
    if (!decode_relay_header(data, len, type, session_id, client_id, payload, payload_len)) {
        if (close_on_decode_error)
            tcp_close(g_session.tcp_relay_socket);
        return true;
    }
    if (session_id != g_session.session_id)
        return true;
    if (type == RELAY_CLIENT_CONNECT) {
        return on_connect(client_id);
    }
    if (type == RELAY_CLIENT_DISCONNECT)
        return on_disconnect(client_id);
    if (type == RELAY_DATA) {
        // Propagate abort/disconnect requests back to the poll loop.
        if (!handle_host_packet(payload, payload_len, nullptr, source, client_id))
            return false;
    }
    return true;
}

bool handle_udp_relay_payload(const char *data, size_t len) {
    return handle_relay_payload_as_host(data, len, HostSource::UDP_RELAY, false,
                                        host_register_udp_relay_client,
                                        host_unregister_udp_relay_client);
}

bool handle_tcp_relay_payload(const char *data, size_t len) {
    if (!g_session.host) {
        if (g_session.active_transport == TransportKind::TCP_RELAY)
            handle_client_payload(data, len);
        // The client can receive a session-ending message (e.g. NET_ABORT) via relay.
        // If that happens, stop polling immediately because SessionShutdown may have
        // destroyed sockets/ENet hosts still being serviced by the transport loop.
        if (!g_session.active)
            return false;
        return true;
    }
    return handle_relay_payload_as_host(data, len, HostSource::TCP_RELAY, true,
                                        host_register_tcp_relay_client,
                                        host_unregister_tcp_relay_client);
}

}  // namespace

void process_network_events() {
    if (!g_session.active)
        return;
    // Poll all active transports via one facade.
    class Sink : public ITransportSink {
    public:
        bool OnConnect(HostSource source, ENetPeer *peer) override {
            if (source != HostSource::DIRECT)
                return true;
            return handle_direct_connect_event(peer);
        }
        bool OnDisconnect(HostSource source, ENetPeer *peer) override {
            if (source == HostSource::UDP_RELAY) {
                if (g_session.relay_peer == peer) {
                    g_session.relay_peer = nullptr;
                    if (!g_session.relay_players.empty())
                        return abort_session_with_message("Relay disconnected. Ending session.");
                }
                return true;
            }
            if (source == HostSource::TCP_RELAY) {
                if (g_session.host) {
                    if (!g_session.tcp_relay_players.empty())
                        return abort_session_with_message("TCP relay disconnected. Ending session.");
                    return true;
                }
                if (g_session.active_transport == TransportKind::TCP_RELAY)
                    return abort_session_with_message("Disconnected from TCP relay.");
                return true;
            }
            return handle_direct_disconnect_event(peer);
        }
        bool OnPayload(HostSource source, ENetPeer *peer, const char *data, size_t len) override {
            // Any packet counts as progress (avoid false "host disconnected" aborts).
            g_session.no_payload_timer = 0.0;
            if (source == HostSource::UDP_RELAY) {
                return handle_udp_relay_payload(data, len);
            }
            if (source == HostSource::TCP_RELAY) {
                return handle_tcp_relay_payload(data, len);
            }
            return handle_direct_payload(peer, data, len);
        }
    private:
        bool handle_direct_payload(ENetPeer *peer, const char *data, size_t len) {
            if (g_session.host)
                return handle_host_packet(data, len, peer, HostSource::DIRECT, 0);
            if (g_session.active_transport != TransportKind::TCP_RELAY)
                handle_client_payload(data, len);
            if (!g_session.active)
                return false;
            return true;
        }
        bool handle_direct_disconnect_event(ENetPeer *peer) {
            return handle_direct_disconnect_event_impl(peer);
        }
        static bool handle_direct_disconnect_event_impl(ENetPeer *peer) {
            if (g_session.host) {
                g_session.peer_players.erase(peer);
                g_session.peer_ready.erase(peer);
                if (g_session.active)
                    return abort_session_with_message("Player disconnected. Ending session.");
                return true;
            }
            return abort_session_with_message("Disconnected from host.");
        }
    };

    Sink sink;
    g_transport.Poll(sink);
}

void send_local_inputs() {
    if (!g_session.active || !g_session.local_player_known)
        return;
    uint32_t current_tick = input::CurrentTick();
    uint32_t delay = g_session.input_delay ? g_session.input_delay : kInputDelay;
    uint32_t target_tick = current_tick + delay;
    if (g_session.input_clock_tick > current_tick) {
        uint32_t extra = g_session.input_clock_tick - current_tick;
        if (extra > kMaxInputLead)
            extra = kMaxInputLead;
        target_tick += extra;
    }
    input::PlayerInput pending = input::DrainLocalPending(g_session.local_player);
    bool applied = false;
    bool sent_packets = false;

    while (g_session.next_local_tick <= target_tick) {
        input::PlayerInput send = applied ? input::PlayerInput() : pending;
        if (!send.empty()) {
            float fx = static_cast<float>(send.mouse_force[0]);
            float fy = static_cast<float>(send.mouse_force[1]);
            send.mouse_force = ecl::V2(fx, fy);
        }
        protocol::InputPacket pkt;
        pkt.epoch = g_session.input_epoch;
        pkt.tick = g_session.next_local_tick;
        pkt.player = static_cast<Uint8>(g_session.local_player);
        pkt.mouse_x = static_cast<float>(send.mouse_force[0]);
        pkt.mouse_y = static_cast<float>(send.mouse_force[1]);
        pkt.rotate_steps = static_cast<int16_t>(send.rotate_steps);
        pkt.activate_count = static_cast<Uint8>(send.activate_count);

        input::EnqueueInput(pkt.tick, g_session.local_player, send);
        g_session.local_history[pkt.tick] = send;
        applied = true;
        ++g_session.next_local_tick;
    }

    if (g_session.host) {
        if (!has_remote_peers())
            return;
    } else if (!g_session.server_peer) {
        if (!(g_session.active_transport == TransportKind::TCP_RELAY &&
              tcp_socket_valid(g_session.tcp_relay_socket))) {
            return;
        }
    }

    uint32_t send_tick = g_session.next_send_tick;
    if (send_tick < current_tick)
        send_tick = current_tick;
    while (send_tick < g_session.next_local_tick) {
        protocol::InputPacket pkt;
        pkt.epoch = g_session.input_epoch;
        pkt.tick = send_tick;
        pkt.player = static_cast<Uint8>(g_session.local_player);
        auto it = g_session.local_history.find(pkt.tick);
        if (it != g_session.local_history.end()) {
            const input::PlayerInput &queued = it->second;
            pkt.mouse_x = static_cast<float>(queued.mouse_force[0]);
            pkt.mouse_y = static_cast<float>(queued.mouse_force[1]);
            pkt.rotate_steps = static_cast<int16_t>(queued.rotate_steps);
            pkt.activate_count = static_cast<Uint8>(queued.activate_count);
            g_session.local_history.erase(it);
        } else {
            pkt.mouse_x = 0.0f;
            pkt.mouse_y = 0.0f;
            pkt.rotate_steps = 0;
            pkt.activate_count = 0;
        }
        if (pkt.tick < 20) {
            debug_log("mp send input: tick=%u player=%u mouse=(%.2f,%.2f) rot=%d act=%u",
                      pkt.tick, pkt.player, pkt.mouse_x, pkt.mouse_y,
                      static_cast<int>(pkt.rotate_steps),
                      static_cast<unsigned>(pkt.activate_count));
        }
        if (g_session.host)
            broadcast_input(pkt, nullptr, 0, 0);
        else {
            ecl::Buffer payload;
            protocol::encode_input(payload, pkt);
            g_transport.ClientSend(payload);
        }
        sent_packets = true;
        ++send_tick;
    }
    g_session.next_send_tick = send_tick;

    if (sent_packets)
        g_transport.Flush();
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
