# Multiplayer Snapshot Audit

This note lists world-object classes that need more than the generic
`StateObject::state` snapshot used by `multiplayer_sim_snapshot`.

See also `doc/multiplayer_object_state_contract.md` for the proposed shared
semantic contract between prediction snapshots and lockstep world resync.

## Status tracking

This file now tracks two separate capabilities:

- **Snapshot hooks**
  - whether local in-process prediction snapshots can capture/apply the
    object's internal runtime state faithfully enough
- **World resync parity**
  - whether the host-driven lockstep world-resync path has been verified to
    restore the same semantics, not just a similar visible result

Status legend:

- **Yes**: implemented / verified
- **Partial**: works only via visible `kind/state`, ad hoc extras, or other
  approximations; not semantically equivalent yet
- **No**: known missing
- **Unknown**: not reviewed yet

## Current generic coverage

The current snapshot layer already captures:

- grid-object `MpCaptureStateForSnapshot()` / `MpRestoreStateForSnapshot()`
- object flags plus generic captured attrs
- movable-stone positions, including retaining disposed movable stones while a
  live snapshot still references them
- animated grid models, including movable-stone runtime models restored by
  object id
- actors
- `GameTimer`
- pending secure actions

That is enough for classes whose full runtime state is represented by:

- `state`, `objFlags`, and captured attrs
- current grid position
- current model animation state
- active timer alarms
- and object instances that can stay alive until outstanding snapshots are dropped

It is **not** enough for classes that additionally depend on:

- hidden sub-objects or hidden topology
- object pointers / object ids stored outside the grid
- static registries that must stay in sync with object state

## Already handled

These classes already need and already have custom snapshot treatment:

- `src/stones/Door.cc`
  - internal states `OPENING` / `CLOSING` matter, not only external open/closed
- `src/items/ShogunDot.cc`
  - snapshot restore must set ON/OFF without replaying target actions
- `src/stones/ShogunStone.cc`
  - hidden sub-shogun topology is not represented by plain grid state
- `src/others/Wire.cc`
  - anchor references live outside generic `$...` attr capture
  - snapshot restore must reconnect both anchors to rebuild fellows/wires lists
- `src/floors/ThiefFloor.cc`
  - hidden `victimId` and private off-grid `it_bag` contents are now serialized
    into the snapshot path via custom attrs
  - `tools/mp_test_scripts/thieffloor_sim_snapshot_bag_baseline_probe.txt` and
    `tools/mp_test_scripts/thieffloor_sim_snapshot_bag_restore_probe.txt`
    verify that restore can still drop the saved bag after a post-save mutation
    replaced the dropped bag on the grid

### Current parity table

| Object | Snapshot hooks | World resync parity | Notes |
| --- | --- | --- | --- |
| `Door` | Yes | Yes | `NET_WORLD_STATE` now carries a semantic override record for `Door`, and focused probes verified that `OPENING` / `OPEN` are restored on both peers. |
| `ShogunDot` | Yes | Yes | World resync now carries the same ON/OFF logical state through the semantic override path, matching snapshot restore without replaying target actions. |
| `ShogunStone` | Yes | Yes | World resync now uses the same hole-mask semantic restore path, so same-kind repairs rebuild hidden sub-shogun topology instead of relying only on visible `kind`. |
| `OxydStone` | Yes | Yes | World resync now preserves internal `CLOSED` / `OPEN_PAIR` / `OPENING` / `CLOSING` / `OPEN_SINGLE` via semantic `logical_state`, and `oxydcolor` now rides the same semantic field path instead of the ad hoc color transport. |
| `Rubberband` | Yes | Yes | World resync now carries semantic `Other` records for `Rubberband`, including stable actor-anchor refs and scalar runtime parameters, so same-kind repairs reconnect anchors and restore later band behavior. |
| `ThiefFloor` | Yes | No | Sim snapshots now restore private `victimId` plus detached `it_bag` contents recursively; lockstep world resync still has no semantic transport for the hidden bag state. |
| `Wire` | Yes | Yes | World resync now carries semantic `Other` records for `Wire`, so same-kind repairs reconnect both stone anchors and rebuild the corresponding fellows/wires lists. |

## High-confidence candidates for custom hooks

These are the next classes I would treat as requiring custom
capture/restore support.

## Review next when prediction hits them

These classes are not as clear-cut as the list above, but they do maintain
runtime state outside the generic snapshot path and should be checked before
relying on prediction for them.

- `src/stones/PuzzleStone.cc`
  - dynamic `objFlags` (`HOLLOW`, `VISITED`, `SINGLE`)
  - cluster logic and pending explosion state may rely on those flags

- `src/items/WormHole.cc`
  - verified by `tools/mp_test_scripts/wormhole_sim_snapshot_forcefield_probe.txt`
  - exact verified case: in `enigma_experimental/mptest_wormhole_snapshot_1`,
    save while an `it_wormhole_on` is idle, then send `close` so it becomes
    `it_wormhole_off` and removes its force field; after `SIM_SNAPSHOT_LOAD`,
    the same restored `it_wormhole_on` again pulls an off-center marble from
    `(4.25,2.5)` and teleports it to `(8.5,2.5)` just like baseline
  - this needed a small restore hook because force-field registration lives in
    `setState` / `on_creation`, while generic snapshot restore only writes the
    internal state and reinitializes the model
  - nonzero `interval` engaged timing is still unverified

- `src/floors/FloodStream.cc`
  - verified by `tools/mp_test_scripts/floodstream_sim_snapshot_restore_probe.txt`
  - exact verified case: in `enigma_experimental/mptest_floodstream_snapshot_1`,
    send `open` to an idle `fl_water`, save while it is `FLOODING`
    (`fl_snap=1`) with one pending 0.500s alarm and the adjacent
    `floodable=true` `fl_rough` still dry, then let baseline flood-replace that
    neighbor with `fl_water`; after `SIM_SNAPSHOT_LOAD`, the source returns to
    `FLOODING`, the adjacent floor is restored to `fl_rough`, and the next
    520ms interval floods that same neighbor again
  - this needed a generic snapshot-restore fix: replaced grid objects are now
    retained and can be reinserted at their saved layer/position before state
    restore, instead of staying stuck as post-save replacements
  - vortex / wormhole flood spread via `warpSpreadPos(true)` is still unverified

- `src/stones/MonoFlopStone.cc`
  - non-laser timer path verified by
    `tools/mp_test_scripts/monoflop_sim_snapshot_restore_probe.txt`
  - exact verified case: in `enigma_experimental/mptest_monoflop_snapshot_1`,
    `CALL_CELL_ACTOR_HIT` puts `st_monoflop` into `ON_TIMER`
    (`st_snap=3`, model `st_monoflop_anim`) with a single pending 0.200s
    alarm; after `SIM_SNAPSHOT_LOAD`, the same animated `ON_TIMER` state and
    pending alarm are restored, and 220ms later the stone again settles to
    `st_snap=0` with model `st_monoflop` and no remaining alarm
  - no gameplay hook was needed here; generic internal-state snapshots plus
    `GameTimer` restore already preserve the touch-triggered timer path
  - laser-specific `ON_LASER` / light-dir handling in `objFlags` is still
    unverified

## Dedicated non-grid snapshot scope

These are stateful world objects that are restored by the dedicated
`CaptureOtherStates()` / `RestoreOtherStates()` pass rather than the grid-object
snapshot path. They should be audited with that in mind.

- `src/others/TimerGadget.cc`
  - verified by `tools/mp_test_scripts/timergadget_sim_snapshot_restore_probe.txt`
  - exact verified case: in `enigma_experimental/mptest_timergadget_snapshot_1`,
    a looping `ot_timer` (`interval=0.2`, `target=sw`, `action=signal`) has
    already fired once against the target `st_switch`; after settling that
    first toggle, `SIM_SNAPSHOT_SAVE` captures the switch at `st_snap=0` with
    one repeating alarm pending, `SIM_SNAPSHOT_LOAD` restores the same settled
    switch state plus queued alarm, and the next 220ms interval flips the
    switch again on the same timeline as baseline
  - no gameplay hook was needed here; the dedicated `Other` snapshot pass plus
    `GameTimer` restore already preserves this repeating-alarm phase case
  - **World resync parity:** no current `Other` transport

- more generally: `src/others/*`
  - the dedicated non-grid snapshot pass now exists
  - remaining work is per-class fidelity, not a missing top-level mechanism

## Likely fine with generic handling

These classes currently look as if generic `StateObject` + `GameTimer` +
runtime-model snapshotting should be enough:

- `src/items/Bomb.cc`
- `src/items/SeedItem.cc`
- `src/items/Vortex.cc`
  - verified by `tools/mp_test_scripts/vortex_sim_snapshot_restore_probe.txt`
  - exact verified case: saving during source `EMITTING` (`it_snap=5`) in
    `enigma_demolevels/ralD006_1` after forcing destination vortex `right`
    closed; restore preserves source `$grabbed_actor`, `$dest_vortex=right`,
    destination reopening (`it_snap=2`), and the pending `GameTimer` alarm
  - after `SIM_SNAPSHOT_LOAD`, the restored sample matches the baseline 700ms
    later: both vortices return to `it_snap=0`, `GET_ACTOR_GRID player=0`
    reports `gx=6 gy=11`, and `GET_GAME_TIMER_ALARMS count=0`
  - sticky-destination redirect via `$dest_idx > 0` remains unverified, but
    this key direct vortex-to-vortex handoff no longer looks like a custom-hook
    gap
- `src/floors/ForwardFloor.cc`
  - verified by `tools/mp_test_scripts/forwardfloor_sim_snapshot_baseline_probe.txt`
    and `tools/mp_test_scripts/forwardfloor_sim_snapshot_restore_probe.txt`
  - runtime attr `$stoneabove` and the pending `ALARM_PUSH` already survive via
    generic object-attr + `GameTimer` sim snapshots
- `src/stones/DispenserStone.cc`
- `src/stones/ScissorsStone.cc`
- `src/stones/BoulderStone.cc`
- `src/stones/BreakStone.cc`
- `src/stones/SpitterStone.cc`
  - verified by `tools/mp_test_scripts/spitter_sim_snapshot_restore_probe.txt`
  - exact verified case: save while `st_spitter` is in `LOADING`
    (`st_snap=2`, model `st_spitter_loading`) after `spit` with explicit
    grid target `(8,2)` in
    `enigma_experimental/mptest_spitter_snapshot_1`; after
    `SIM_SNAPSHOT_LOAD`, the restored `$ball_velocity=5.455,0.000` produces the
    same `CALL_CELL_ANIMCB` outcome and the target item again ends as
    `it_debris`
  - automatic destination cycling via `$hitdestindex` is still unverified, but
    the core "save before cannonball spawn, restore, then launch" path looks
    covered by generic `$...` attr snapshots
- `src/stones/StoneImpulse.cc`
  - verified by `tools/mp_test_scripts/stoneimpulse_sim_snapshot_restore_probe.txt`
  - exact verified case: `SEND_CELL_IMPULSE dir=east` puts an oriented
    `st_stoneimpulse` into `EXPANDING` with saved `$incoming=2` in
    `enigma_experimental/mptest_stoneimpulse_snapshot_1`; after
    `SIM_SNAPSHOT_LOAD`, the stone is immediately back in `st_snap=1` /
    model `st_stoneimpulse_anim1`, and the same two `CALL_CELL_ANIMCB` steps
    again move only the east-side `st_box_wood` from `(5,2)` to `(6,2)` while
    the west-side box at `(3,2)` stays put
  - the direct backfire-suppression path looks covered by generic state +
    flags + `$...` attr snapshots; fellow/wire propagation through
    `$impulse_source` is still unverified
- `src/stones/CoinSlot.cc`
  - verified by `tools/mp_test_scripts/coinslot_sim_snapshot_restore_probe.txt`
  - exact verified case: saving in `enigma_experimental/mptest_coinslot_snapshot_1`
    after one `CALL_CELL_ACTOR_HIT` while non-instant `st_coinslot` is still in
    `INSERT_OFF` (`st_snap=2`, `$addTime=0.7`, model `st_coinslot_insert`,
    no alarm); after `SIM_SNAPSHOT_LOAD`, the same insert-phase model and
    buffered `$addTime` are restored, and the follow-up `CALL_CELL_ANIMCB`
    re-arms the same `0.700` timer with matching baseline countdown
  - this needed a small custom restore hook because the generic state restore
    handled the buffered timer semantics but left insert-phase visuals on the
    wrong static model
- `src/stones/ActorImpulseStone.cc`
  - verified by `tools/mp_test_scripts/actorimpulse_sim_snapshot_restore_probe.txt`
  - exact verified case: in `enigma_experimental/mptest_actorimpulse_snapshot_1`,
    force `GameCompatibility=per.oxyd`, send `_init` to seed the first
    destination, rotate once with `signal`, then save with `$signalidx=1` while
    the middle switch is on; after `SIM_SNAPSHOT_LOAD`, the same `$signalidx=1`
    and switch pattern are restored, and the next `signal` again advances to
    `$signalidx=2` with only the last switch on
  - no gameplay hook was needed here; the generic `$...` attr snapshot already
    preserves the non-Enigma signal-multiplier counter once it is exercised by
    a compatibility-aware probe
- `src/stones/WindowStone.cc`
  - verified by `tools/mp_test_scripts/window_sim_snapshot_restore_probe.txt`
  - exact verified case: in `enigma_experimental/mptest_window_snapshot_1`,
    a secure scratched `st_window_ew` receives `_explosion` from the west so it
    enters `BREAK` with only the east face remaining; after `SIM_SNAPSHOT_LOAD`,
    the same `st_snap=1`, empty `scratches`, and break model
    `st_window_green4_0_anim` are restored, and the next `CALL_CELL_ANIMCB`
    again settles to the static `st_window_green4_0`
  - this needed a small custom restore hook because generic state/flags restore
    preserved the broken-face semantics but left the window on the settled
    static model instead of the pending break animation
- `src/floors/ScalesFloor.cc`
  - verified by `tools/mp_test_scripts/scales_sim_snapshot_restore_probe.txt`
  - exact verified case: in `enigma_experimental/mptest_scales_snapshot_1`,
    send `_add_mass 0.6`, save while the floor is still released, then add
    `_add_mass 0.5` to cross the `min=1.0` threshold; after `SIM_SNAPSHOT_LOAD`,
    the same `$mass=0.6` and released model are restored, and the next
    `_add_mass 0.5` again yields `$mass=1.1` with
    `fl_scales_darkgray_pressed`
  - no gameplay hook was needed here; generic `$...` attr snapshots already
    preserve the accumulated mass and recomputed state/model path
- `src/stones/TimerStone.cc`
  - verified by `tools/mp_test_scripts/timerstone_sim_snapshot_restore_probe.txt`
  - exact verified case: in `enigma_experimental/mptest_timerstone_snapshot_1`,
    save a looping `st_timer` after its first sampled fire, with external
    `st_state=1` but internal `st_snap=3` and the target instant switch still
    in its matching transitional on-state; after `SIM_SNAPSHOT_LOAD`, the same
    internal timer phase and target switch state are restored, and the next
    220ms interval again advances the timer to `st_snap=2` while the target
    switch flips back off on the same timeline as baseline
  - no gameplay hook was needed here; generic internal-state snapshots plus
    `GameTimer` restore already preserve the alternating ON_TRUE/ON_FALSE phase
- `src/stones/LightPassengerStone.cc`
  - verified by `tools/mp_test_scripts/lightpassenger_sim_snapshot_restore_probe.txt`
  - dynamic `objFlags` plus `GameTimer` are sufficient once movable-stone
    snapshot restore avoids lifecycle callbacks and rebuilds the laser graph
  - exact verified case: blocked laser push retry restores at the saved cell and
    matches the baseline one-cell move timing after 45 ticks
- `src/stones/ChessStone.cc`
  - verified by `tools/mp_test_scripts/chessstone_capture_timeline_probe.txt`
    and `tools/mp_test_scripts/chessstone_sim_snapshot_restore_probe.txt`
  - generic flags/attrs + `GameTimer` are sufficient once sim snapshots retain
    disposed movable stones and restore movable runtime models by object id
  - exact verified case: saving during black `CAPTURING` / white `CAPTURE`
    restores both stones immediately after `SIM_SNAPSHOT_LOAD`, including
    black `$destination=4,2`, and 1200ms later matches the same sampled
    baseline progression (`black state=DISAPPEARING` at `(2,1)`, white gone)

They still need testing, but they do not currently show the same kind of hidden
state as `ShogunStone`, `Vortex`, or `ThiefFloor`.

## Recommended order

If we continue extending prediction/replay coverage, the next order should be:

1. remaining `Other` classes with custom runtime references beyond the generic pass

## World-resync alignment backlog

The initial handled-object slice is now aligned for:

- `Door`
- `ShogunDot`
- `ShogunStone`
- `OxydStone`
- `Rubberband`
- `Wire`

The next backlog is extending that same semantic contract to the remaining
high-risk classes in the recommended-order list above.
