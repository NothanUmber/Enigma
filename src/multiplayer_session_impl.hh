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

#ifndef MULTIPLAYER_SESSION_IMPL_HH_INCLUDED
#define MULTIPLAYER_SESSION_IMPL_HH_INCLUDED

/* -------------------- Multiplayer session internals -------------------- */
/*
 * Internal session helpers shared across session translation units.
 *
 * Split out to avoid circular dependencies between the session modules.
 */

#include "multiplayer_internal.hh"

namespace enigma {
namespace multiplayer {
namespace internal {

// Shared helpers used across the session implementation translation units.
const char *transport_name(TransportKind t);
const char *host_source_name(HostSource s);

bool abort_session_with_message(const char *message);
void begin_abort_after_grace(const char *message);
bool has_remote_peers();
bool can_accept_more_remote_players();
bool local_can_send_ready();

void configure_input_session(unsigned expected_players);
void process_network_events();
void send_local_inputs();

    void send_ready_to_host();
    void send_start_to_peers();
    void send_restart_to_peers(bool level_restart);
    void send_load_level_to_peers(const std::string &pack_name, const std::string &level_id);
    void send_pause_to_host(bool paused);
    void send_pause_to_peers(bool paused);
    void send_menu_to_host(bool open);
    void send_abort_to_host();
    void send_abort_to_peers();
bool host_ready_to_start();
bool lookup_checksum_sample(uint32_t tick, SessionState::ChecksumSample &out);
void handle_sync_current(const protocol::SyncPacket &sync);
void handle_sync_sample(const protocol::SyncPacket &sync,
                        const SessionState::ChecksumSample &sample);
void record_checksum_sample();
void send_sync_to_peers();
void broadcast_resync_state_unreliable();
void broadcast_world_state_unreliable();
void broadcast_world_state_reliable();
void apply_resync_state(const protocol::ResyncState &state);
void send_resync_state(ENetPeer *peer);
void send_resync_state_to_relay(Uint32 client_id);
void send_resync_state_to_tcp_relay(Uint32 client_id);

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma

#endif
