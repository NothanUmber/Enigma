# Multiplayer Snapshot Audit

This note lists world-object classes that need more than the generic
`StateObject::state` snapshot used by `multiplayer_sim_snapshot`.

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

- `src/others/Rubberband.cc`
  - currently outside the grid-object snapshot path, but practically required
  - anchor references, violation flags and the rendered band geometry all evolve at runtime
  - without a non-grid snapshot/update pass, the band trails behind predicted actor motion

- `src/floors/ThiefFloor.cc`
  - private fields `victimId` and `bag`
  - inventory/bag ownership is not represented by external `state`

- `src/floors/BridgeFloor.cc`
  - runtime flag `OBJBIT_EXPLICIT` changes closing/opening behavior
  - external `state` alone does not tell whether the bridge should reopen

- `src/floors/ForwardFloor.cc`
  - runtime attr `$stoneabove` is used between `ALARM_PREPARE` and `ALARM_PUSH`
  - plain `state` + timer snapshot is not enough for exact replay

- `src/floors/ScalesFloor.cc`
  - runtime attr `$mass` accumulates mass across messages
  - not represented by external `state`

- `src/stones/LightPassengerStone.cc`
  - dynamic `objFlags` encode skate direction, blocked state and visibility
  - these bits directly affect later movement decisions

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

## Outside current grid-object snapshot scope

These are stateful world objects, but they are **not** restored by
`CaptureObjectStates()` / `RestoreObjectStates()` at all, because they are not
grid objects.

- `src/others/TimerGadget.cc`
  - stateful `Other` with `GameTimer` behavior

- more generally: `src/others/*`
  - if prediction starts depending on `Other` state, the snapshot system needs a
    dedicated non-grid object pass rather than more object-local hacks

## Likely fine with generic handling

These classes currently look as if generic `StateObject` + `GameTimer` +
runtime-model snapshotting should be enough:

- `src/items/Bomb.cc`
- `src/items/SeedItem.cc`
- `src/stones/DispenserStone.cc`
- `src/stones/ScissorsStone.cc`
- `src/stones/BoulderStone.cc`
- `src/stones/BreakStone.cc`

They still need testing, but they do not currently show the same kind of hidden
state as `ShogunStone`, `Vortex`, or `ThiefFloor`.

## Recommended order

If we continue extending prediction/replay coverage, the next order should be:

1. `OxydStone`
2. a dedicated snapshot/update pass for `Other` objects, starting with `Rubberband`
3. `Vortex`
4. `ThiefFloor`
5. `BridgeFloor`
6. `ForwardFloor`
7. `LightPassengerStone`
8. `ChessStone`
9. the `$...`-attribute stones (`CoinSlot`, `StoneImpulse`, `SpitterStone`, `ActorImpulseStone`)
