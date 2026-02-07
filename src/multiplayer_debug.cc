#include "multiplayer_internal.hh"

#include "enet_ver.hh"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#ifdef WIN32
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace enigma {
namespace multiplayer {
namespace internal {

bool debug_enabled() {
    const char *env = std::getenv("ENIGMA_MP_DEBUG");
    return env && *env;
}

bool force_relay_enabled() {
    const char *env = std::getenv("ENIGMA_MP_FORCE_RELAY");
    return env && *env;
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

    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);

#ifdef WIN32
    if (log_file) {
        va_list args2;
        va_copy(args2, args);
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

