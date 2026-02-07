#include "multiplayer_internal.hh"

#include "main.hh"

#include <random>
#include <sstream>

namespace enigma {
namespace multiplayer {
namespace internal {

std::string make_id() {
    std::random_device rd;
    uint64_t value = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    std::ostringstream out;
    out << std::hex << value;
    return out.str();
}

std::string resolve_local_name() {
    std::string name = app.state->getString("UserName");
    if (name.empty())
        name = "Player";
    return name;
}

void ensure_lobby_identity() {
    if (!g_lobby.local_id.empty())
        return;
    g_lobby.local_id = make_id();
    g_lobby.local_name = resolve_local_name();
}

}  // namespace internal
}  // namespace multiplayer
}  // namespace enigma

