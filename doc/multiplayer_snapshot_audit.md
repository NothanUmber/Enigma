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
- movable-stone positions
- animated grid models
- actors
- `GameTimer`
- pending secure actions

That is enough for classes whose full runtime state is represented by:

- `state`
- current grid position
- current model animation state
- active timer alarms

It is **not** enough for classes that additionally depend on:

- hidden sub-objects or hidden topology
- runtime-only attributes such as `$foo`
- dynamic `objFlags` bits that are not derivable from `state`
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

### Current parity table

| Object | Snapshot hooks | World resync parity | Notes |
| --- | --- | --- | --- |
| `Door` | Yes | Yes | `NET_WORLD_STATE` now carries a semantic override record for `Door`, and focused probes verified that `OPENING` / `OPEN` are restored on both peers. |
| `ShogunDot` | Yes | Yes | World resync now carries the same ON/OFF logical state through the semantic override path, matching snapshot restore without replaying target actions. |
| `ShogunStone` | Yes | Yes | World resync now uses the same hole-mask semantic restore path, so same-kind repairs rebuild hidden sub-shogun topology instead of relying only on visible `kind`. |
| `OxydStone` | Yes | Yes | World resync now preserves internal `CLOSED` / `OPEN_PAIR` / `OPENING` / `CLOSING` / `OPEN_SINGLE` via semantic `logical_state`, and `oxydcolor` now rides the same semantic field path instead of the ad hoc color transport. |
| `Rubberband` | Yes | Yes | World resync now carries semantic `Other` records for `Rubberband`, including stable actor-anchor refs and scalar runtime parameters, so same-kind repairs reconnect anchors and restore later band behavior. |
| `Wire` | Yes | Yes | World resync now carries semantic `Other` records for `Wire`, so same-kind repairs reconnect both stone anchors and rebuild the corresponding fellows/wires lists. |

## High-confidence candidates for custom hooks

These are the next classes I would treat as requiring custom
capture/restore support.

- `src/stones/OxydStone.cc`
  - hidden/internal states (`OPENING`, `CLOSING`, `OPEN_SINGLE`, `OPEN_PAIR`)
  - static per-level registry `levelOxyds`
  - color/pairing state is not fully represented by plain external `state`

- `src/items/Vortex.cc`
  - runtime attrs `$dest_idx`, `$dest_vortex`, `$grabbed_actor`
  - internal busy states (`SWALLOWING`, `WARPING`, `EMITTING`)
  - prediction can snapshot it while an actor is half-way through warp handling

- `src/floors/ThiefFloor.cc`
  - private fields `victimId` and `bag`
  - inventory/bag ownership is not represented by external `state`

- `src/floors/ScalesFloor.cc`
  - runtime attr `$mass` accumulates mass across messages
  - not represented by external `state`

- `src/stones/ChessStone.cc`
  - runtime attr `$destination`
  - deferred state bits in `objFlags` (`NEWCOLOR`, `FALL`, `SINK`, capture retry counter)
  - plain `state` is insufficient

- `src/stones/CoinSlot.cc`
  - runtime attr `$addTime` buffers extra timer delay
  - if omitted, replay of coin insertions can drift

- `src/stones/StoneImpulse.cc`
  - runtime attrs `$incoming` and `$impulse_source`
  - dynamic behavior bits in `objFlags`
  - used across later callbacks / impulse propagation

- `src/stones/WindowStone.cc`
  - dynamic scratch / secure bits in `objFlags`
  - window damage state is not stored in `state`

- `src/stones/SpitterStone.cc`
  - runtime attrs `$hitdestindex` and `$ball_velocity`
  - used later when creating / launching cannonballs

- `src/stones/ActorImpulseStone.cc`
  - runtime attrs / counters such as `$signalidx`
  - action sequencing depends on data outside `state`

## Review next when prediction hits them

These classes are not as clear-cut as the list above, but they do maintain
runtime state outside the generic snapshot path and should be checked before
relying on prediction for them.

- `src/stones/PuzzleStone.cc`
  - dynamic `objFlags` (`HOLLOW`, `VISITED`, `SINGLE`)
  - cluster logic and pending explosion state may rely on those flags

- `src/items/WormHole.cc`
  - state encodes engage/warp progress, which is good
  - but verify carefully because teleport timing and force-field registration are delicate

- `src/floors/FloodStream.cc`
  - mostly looks covered by `state` + `GameTimer`
  - still worth checking because flood propagation depends on callbacks and surrounding cells

- `src/stones/MonoFlopStone.cc`
  - mostly looks covered by `state` + `GameTimer`
  - laser-specific mode bits in `objFlags` should still be sanity-checked

- `src/stones/TimerStone.cc`
  - mostly looks covered by `state` + `GameTimer`
  - should be verified, but does not obviously need custom hooks yet

## Dedicated non-grid snapshot scope

These are stateful world objects that are restored by the dedicated
`CaptureOtherStates()` / `RestoreOtherStates()` pass rather than the grid-object
snapshot path. They should be audited with that in mind.

- `src/others/TimerGadget.cc`
  - stateful `Other` with `GameTimer` behavior
  - **Snapshot hooks:** probably covered by generic `Other` + `GameTimer`, still unverified
  - **World resync parity:** no current `Other` transport

- more generally: `src/others/*`
  - the dedicated non-grid snapshot pass now exists
  - remaining work is per-class fidelity, not a missing top-level mechanism

## Likely fine with generic handling

These classes currently look as if generic `StateObject` + `GameTimer` +
runtime-model snapshotting should be enough:

- `src/items/Bomb.cc`
- `src/items/SeedItem.cc`
- `src/floors/ForwardFloor.cc`
  - verified by `tools/mp_test_scripts/forwardfloor_sim_snapshot_baseline_probe.txt`
    and `tools/mp_test_scripts/forwardfloor_sim_snapshot_restore_probe.txt`
  - runtime attr `$stoneabove` and the pending `ALARM_PUSH` already survive via
    generic object-attr + `GameTimer` sim snapshots
- `src/stones/DispenserStone.cc`
- `src/stones/ScissorsStone.cc`
- `src/stones/BoulderStone.cc`
- `src/stones/BreakStone.cc`
- `src/stones/LightPassengerStone.cc`
  - verified by `tools/mp_test_scripts/lightpassenger_sim_snapshot_restore_probe.txt`
  - dynamic `objFlags` plus `GameTimer` are sufficient once movable-stone
    snapshot restore avoids lifecycle callbacks and rebuilds the laser graph
  - exact verified case: blocked laser push retry restores at the saved cell and
    matches the baseline one-cell move timing after 45 ticks

They still need testing, but they do not currently show the same kind of hidden
state as `ShogunStone`, `Vortex`, or `ThiefFloor`.

## Recommended order

If we continue extending prediction/replay coverage, the next order should be:

1. `Vortex`
2. `ThiefFloor`
3. `ChessStone`
4. the `$...`-attribute stones (`CoinSlot`, `StoneImpulse`, `SpitterStone`, `ActorImpulseStone`)
5. remaining `Other` classes with custom runtime references beyond the generic pass

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
