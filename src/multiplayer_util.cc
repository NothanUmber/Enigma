#include "multiplayer_internal.hh"

#include <cstdlib>

namespace enigma {
namespace multiplayer {
namespace internal {

bool parse_host_port(const std::string &value, std::string &host, Uint16 &port,
                     Uint16 default_port) {
    host.clear();
    port = default_port;
    if (value.empty())
        return false;
    std::string::size_type pos = value.rfind(':');
    if (pos == std::string::npos) {
        host = value;
    } else {
        host = value.substr(0, pos);
        std::string port_str = value.substr(pos + 1);
        if (!port_str.empty()) {
            char *end = nullptr;
            long parsed = std::strtol(port_str.c_str(), &end, 10);
            if (end && *end == '\0' && parsed > 0 && parsed <= 65535)
                port = static_cast<Uint16>(parsed);
        }
    }

    if (host == "localhost" || host == "::1")
        host = "127.0.0.1";
    return !host.empty();
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma

