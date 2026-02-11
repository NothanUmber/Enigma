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

#include "enet_ver.hh"
#include "options.hh"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#ifdef WIN32
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

/* -------------------- Multiplayer debug -------------------- */
/*
 * Multiplayer debug logging and on-demand state dumps.
 *
 * This file keeps all "heavy" debug utilities in one place so normal runtime
 * code stays readable.
 */

namespace enigma {
namespace multiplayer {
namespace internal {

namespace {

const char *transport_kind_name(TransportKind kind) {
    switch (kind) {
    case TransportKind::DIRECT:
        return "direct";
    case TransportKind::UDP_RELAY:
        return "udp-relay";
    case TransportKind::TCP_RELAY:
        return "tcp-relay";
    default:
        return "none";
    }
}

}  // namespace

bool debug_enabled() {
    const char *env = std::getenv("ENIGMA_MP_DEBUG");
    if (env && *env)
        return true;
    return options::GetBool("MultiplayerDebugLogging");
}

bool force_relay_enabled() {
    const char *env = std::getenv("ENIGMA_MP_FORCE_RELAY");
    if (env && *env)
        return true;
    return options::GetBool("MultiplayerDebugForceRelay");
}

bool dump_state_enabled() {
    const char *env = std::getenv("ENIGMA_MP_DUMP_STATE");
    if (env && *env)
        return true;
    return options::GetBool("MultiplayerDebugDumpState");
}

void debug_log(const char *fmt, ...) {
    if (!debug_enabled())
        return;

#ifdef WIN32
    // GUI-subsystem binaries often have no attached console, so stderr can be
    // discarded. When ENIGMA_MP_DEBUG is enabled, also write to a temp log file
    // so Windows builds remain debuggable.
    static FILE *log_file = nullptr;
    static bool log_file_init = false;
    if (!log_file_init) {
        log_file_init = true;
        const char *tmp = std::getenv("TEMP");
        if (!tmp || !*tmp)
            tmp = std::getenv("TMP");
        if (!tmp || !*tmp)
            tmp = ".";
        char path[1024] = {0};
        std::snprintf(path, sizeof(path), "%s/enigma-mp-%d.log", tmp, _getpid());
        log_file = std::fopen(path, "a");
    }
#endif

    // Prefix every log line with stable session context so host/client logs
    // can be correlated even when per-level tick counters reset.
    static uint64_t seq = 0;
    seq += 1;
    const unsigned sid = static_cast<unsigned>(g_session.session_id);
    const unsigned rid = static_cast<unsigned>(g_session.restart_id);
    const unsigned epoch = static_cast<unsigned>(g_session.input_epoch);
    const unsigned lp = static_cast<unsigned>(g_session.local_player);
    const int is_host = g_session.host ? 1 : 0;
    const char *via = transport_kind_name(g_session.active_transport);

    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "mp[%llu sid=%u rid=%u epoch=%u host=%d p=%u via=%s] ",
                 static_cast<unsigned long long>(seq), sid, rid, epoch, is_host, lp, via);
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);

#ifdef WIN32
    if (log_file) {
        va_list args2;
        va_copy(args2, args);
        std::fprintf(log_file, "mp[%llu sid=%u rid=%u epoch=%u host=%d p=%u via=%s] ",
                     static_cast<unsigned long long>(seq), sid, rid, epoch, is_host, lp, via);
        std::vfprintf(log_file, fmt, args2);
        std::fprintf(log_file, "\n");
        std::fflush(log_file);
        va_end(args2);
    }
#endif

    va_end(args);
}

std::string address_to_ip_string(const ENetAddress &addr) {
    // Always prefer a numeric IPv4 address here. LAN multiplayer uses this
    // address for direct connects; reverse DNS / hostnames can fail to resolve
    // (notably on Windows).
    in_addr ia;
    ia.s_addr = addr.host;  // ENet stores host in network byte order.
#ifdef WIN32
    const char *s = inet_ntoa(ia);
    return s ? std::string(s) : std::string();
#else
    char buf[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop(AF_INET, &ia, buf, sizeof(buf)))
        return std::string();
    return std::string(buf);
#endif
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma
