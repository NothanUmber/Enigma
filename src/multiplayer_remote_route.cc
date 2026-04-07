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

#include "multiplayer_internal.hh"

/* -------------------- Multiplayer relay routes -------------------- */
/*
 * Shared bookkeeping for non-direct remotes so additional fallback transports
 * do not require another copy of player/ready maps.
 */

namespace enigma {
namespace multiplayer {
namespace internal {

bool host_source_is_relay(HostSource source) {
    return source == HostSource::UDP_RELAY || source == HostSource::TCP_RELAY;
}

bool transport_uses_stream_relay(TransportKind transport) {
    return transport == TransportKind::TCP_RELAY || transport == TransportKind::WS_RELAY;
}

TransportKind transport_kind_for_host_source(HostSource source) {
    switch (source) {
    case HostSource::DIRECT:
        return TransportKind::DIRECT;
    case HostSource::UDP_RELAY:
        return TransportKind::UDP_RELAY;
    case HostSource::TCP_RELAY:
        return TransportKind::TCP_RELAY;
    }
    return TransportKind::NONE;
}

RelayRouteKey make_relay_route_key(HostSource source, Uint32 client_id) {
    RelayRouteKey key;
    key.source = source;
    key.client_id = client_id;
    return key;
}

RelayRemoteRoute *find_relay_route(SessionState &session, HostSource source, Uint32 client_id) {
    if (!host_source_is_relay(source))
        return nullptr;
    auto it = session.relay_routes.find(make_relay_route_key(source, client_id));
    if (it == session.relay_routes.end())
        return nullptr;
    return &it->second;
}

const RelayRemoteRoute *find_relay_route(const SessionState &session, HostSource source,
                                         Uint32 client_id) {
    if (!host_source_is_relay(source))
        return nullptr;
    auto it = session.relay_routes.find(make_relay_route_key(source, client_id));
    if (it == session.relay_routes.end())
        return nullptr;
    return &it->second;
}

bool lookup_relay_player(const SessionState &session, HostSource source, Uint32 client_id,
                         unsigned &player_id) {
    const RelayRemoteRoute *route = find_relay_route(session, source, client_id);
    if (!route)
        return false;
    player_id = route->player_id;
    return true;
}

void upsert_relay_route(SessionState &session, HostSource source, Uint32 client_id,
                        unsigned player_id, bool ready) {
    if (!host_source_is_relay(source))
        return;
    RelayRemoteRoute &route = session.relay_routes[make_relay_route_key(source, client_id)];
    route.source = source;
    route.client_id = client_id;
    route.player_id = player_id;
    route.ready = ready;
}

bool remove_relay_route(SessionState &session, HostSource source, Uint32 client_id,
                        unsigned *player_id) {
    if (!host_source_is_relay(source))
        return false;
    auto it = session.relay_routes.find(make_relay_route_key(source, client_id));
    if (it == session.relay_routes.end())
        return false;
    if (player_id)
        *player_id = it->second.player_id;
    session.relay_routes.erase(it);
    return true;
}

bool relay_route_ready(const SessionState &session, HostSource source, Uint32 client_id) {
    const RelayRemoteRoute *route = find_relay_route(session, source, client_id);
    return route != nullptr && route->ready;
}

void set_relay_route_ready(SessionState &session, HostSource source, Uint32 client_id, bool ready) {
    RelayRemoteRoute *route = find_relay_route(session, source, client_id);
    if (!route)
        return;
    route->ready = ready;
}

unsigned relay_route_count(const SessionState &session, HostSource source) {
    unsigned count = 0;
    for_each_relay_route(session, source, [&count](const RelayRemoteRoute &) {
        count += 1;
    });
    return count;
}

unsigned relay_ready_count(const SessionState &session, HostSource source) {
    unsigned count = 0;
    for_each_relay_route(session, source, [&count](const RelayRemoteRoute &route) {
        if (route.ready)
            count += 1;
    });
    return count;
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
