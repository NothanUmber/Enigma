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
#ifndef ENIGMA_MULTIPLAYER_CONNECTIVITY_PRESETS_HH
#define ENIGMA_MULTIPLAYER_CONNECTIVITY_PRESETS_HH

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace enigma {
namespace multiplayer {
namespace connectivity {

// Auto-detect classification thresholds based on worst-link p90 RTT.
// Keep in sync with display overlays and documentation.
constexpr uint32_t kConnectivityGoodMaxP90Ms = 70;
constexpr uint32_t kConnectivityNormalMaxP90Ms = 180;

struct PresetSpec {
    const char *name = "Custom";
    bool zerofill = false;
    bool rollback = false;
    bool remote_local_ball = false;
    bool client_auth_pos = false;
    bool host_world_only = false;
    int tick_ms = 10;
    int input_delay_legacy_ticks = 4;
    int predict_mouse_ticks = 0;
    int host_resync_stride_legacy_ticks = 50;
    int host_world_stride_legacy_ticks = 50;
    int rollback_keep_ticks = 200;
};

inline const std::array<PresetSpec, 3> &presets() {
    // Keep in sync with OptionsMenu::apply_mp_debug_preset() UI labels.
    static const std::array<PresetSpec, 3> k = {{
        PresetSpec{
            /*name=*/"Good",
            /*zerofill=*/false,
            /*rollback=*/false,
            /*remote_local_ball=*/false,
            /*client_auth_pos=*/false,
            /*host_world_only=*/false,
            /*tick_ms=*/10,
            /*input_delay_legacy_ticks=*/4,
            /*predict_mouse_ticks=*/0,
            /*host_resync_stride_legacy_ticks=*/1,
            /*host_world_stride_legacy_ticks=*/50,
            /*rollback_keep_ticks=*/200,
        },
        PresetSpec{
            /*name=*/"Normal",
            /*zerofill=*/true,
            /*rollback=*/false,
            /*remote_local_ball=*/false,
            /*client_auth_pos=*/false,
            /*host_world_only=*/false,
            /*tick_ms=*/20,
            /*input_delay_legacy_ticks=*/8,
            /*predict_mouse_ticks=*/0,
            /*host_resync_stride_legacy_ticks=*/1,
            /*host_world_stride_legacy_ticks=*/25,
            /*rollback_keep_ticks=*/200,
        },
        PresetSpec{
            /*name=*/"Bad",
            /*zerofill=*/true,
            /*rollback=*/false,
            /*remote_local_ball=*/false,
            /*client_auth_pos=*/true,
            /*host_world_only=*/true,
            /*tick_ms=*/50,
            /*input_delay_legacy_ticks=*/16,
            /*predict_mouse_ticks=*/0,
            /*host_resync_stride_legacy_ticks=*/30,
            /*host_world_stride_legacy_ticks=*/10,
            /*rollback_keep_ticks=*/200,
        },
    }};
    return k;
}

template <class GetBoolFn, class GetIntFn>
inline bool options_match_preset(const PresetSpec &p, GetBoolFn get_bool, GetIntFn get_int) {
    return get_bool("MultiplayerDebugSmoothRender") &&
           get_bool("MultiplayerDebugZeroFillInputs") == p.zerofill &&
           get_bool("MultiplayerDebugRollbackEnabled") == p.rollback &&
           get_bool("MultiplayerDebugRemoteControlLocalBall") == p.remote_local_ball &&
           get_bool("MultiplayerDebugClientAuthBallPos") == p.client_auth_pos &&
           get_bool("MultiplayerDebugHostOnlyWorldInteractions") == p.host_world_only &&
           get_int("MultiplayerDebugTickLengthMs") == p.tick_ms &&
           get_int("MultiplayerDebugInputDelayTicks") == p.input_delay_legacy_ticks &&
           get_int("MultiplayerDebugPredictMissingMouseTicks") == p.predict_mouse_ticks &&
           get_int("MultiplayerDebugHostBroadcastResyncStrideTicks") == p.host_resync_stride_legacy_ticks &&
           get_int("MultiplayerDebugHostBroadcastWorldStateStrideTicks") == p.host_world_stride_legacy_ticks;
}

template <class GetBoolFn, class GetIntFn>
inline std::string profile_name_or_custom(GetBoolFn get_bool, GetIntFn get_int) {
    for (const auto &p : presets()) {
        if (options_match_preset(p, get_bool, get_int))
            return p.name;
    }
    return "Custom";
}

template <class SetBoolFn, class SetIntFn>
inline void apply_preset(const PresetSpec &p, SetBoolFn set_bool, SetIntFn set_int) {
    set_bool("MultiplayerDebugSmoothRender", true);
    // Conservative: avoid surprising transport/bind behavior when switching presets.
    set_bool("MultiplayerDebugForceRelay", false);
    set_bool("MultiplayerDebugBindLocal", false);

    set_bool("MultiplayerDebugZeroFillInputs", p.zerofill);
    set_bool("MultiplayerDebugRollbackEnabled", p.rollback);
    set_bool("MultiplayerDebugRemoteControlLocalBall", p.remote_local_ball);
    set_bool("MultiplayerDebugClientAuthBallPos", p.client_auth_pos);
    set_bool("MultiplayerDebugHostOnlyWorldInteractions", p.host_world_only);

    set_int("MultiplayerDebugTickLengthMs", p.tick_ms);
    set_int("MultiplayerDebugInputDelayTicks", p.input_delay_legacy_ticks);
    set_int("MultiplayerDebugPredictMissingMouseTicks", p.predict_mouse_ticks);
    set_int("MultiplayerDebugHostBroadcastResyncStrideTicks", p.host_resync_stride_legacy_ticks);
    set_int("MultiplayerDebugHostBroadcastWorldStateStrideTicks", p.host_world_stride_legacy_ticks);
    set_int("MultiplayerDebugRollbackKeepTicks", p.rollback_keep_ticks);
}

template <class SetBoolFn, class SetIntFn>
inline bool apply_preset_id(int preset_id, SetBoolFn set_bool, SetIntFn set_int) {
    if (preset_id < 0 || preset_id >= static_cast<int>(presets().size()))
        return false;
    apply_preset(presets()[static_cast<size_t>(preset_id)], set_bool, set_int);
    return true;
}

}  // namespace connectivity
}  // namespace multiplayer
}  // namespace enigma

#endif  // ENIGMA_MULTIPLAYER_CONNECTIVITY_PRESETS_HH
