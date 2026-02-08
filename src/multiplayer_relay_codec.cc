/*
 * Copyright (C) 2026 Ferdinand Strixner (LLM collaboration)
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

/* -------------------- Multiplayer relay codec -------------------- */
/*
 * Relay message encoding/decoding.
 *
 * Shared between UDP relay and TCP relay to keep message framing identical.
 */

namespace enigma {
namespace multiplayer {
namespace internal {

void encode_relay_header(ecl::Buffer &buf, RelayMessageType type, Uint32 session_id,
                         Uint32 client_id) {
    buf << Uint32(kRelayMagic) << Uint8(kRelayVersion) << Uint8(type)
        << Uint32(session_id) << Uint32(client_id);
}

bool decode_relay_header(const char *data, size_t len, RelayMessageType &type,
                         Uint32 &session_id, Uint32 &client_id,
                         const char *&payload, size_t &payload_len) {
    ecl::Buffer buf;
    buf.assign(const_cast<char *>(data), len);
    Uint32 magic = 0;
    Uint8 version = 0;
    Uint8 raw_type = 0;
    if (!(buf >> magic >> version >> raw_type >> session_id >> client_id))
        return false;
    if (magic != kRelayMagic || version != kRelayVersion)
        return false;
    type = static_cast<RelayMessageType>(raw_type);
    size_t offset = static_cast<size_t>(buf.get_rpos());
    if (offset > len)
        return false;
    payload = data + offset;
    payload_len = len - offset;
    return true;
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
