# Multiplayer Object State Contract

This note proposes a shared semantic contract for:

- local in-process snapshot/restore used by visual prediction
- cross-peer world-resync used to repair lockstep world divergence

The goal is not one identical binary format. The goal is that both mechanisms
restore the **same gameplay-relevant object state**.

## Problem

Right now we have two different restoration models:

- prediction snapshots restore object internals via `MpCapture*ForSnapshot()` /
  `MpRestore*ForSnapshot()`
- world resync sends a bespoke packet with visible `kind/state`, movable-stone
  positions, and a few ad hoc extras like Oxyd color

That is good enough for many cases, but it is not semantically unified:

- prediction can be more faithful than world resync
- world resync can repair visible state while still losing hidden runtime state
- every new problematic class risks getting fixed twice in two different ways

The contract below is meant to stop that drift.

## Current prototype status

The first contract slice is implemented for `Door`:

- `Object` now exposes `MpCaptureSemanticState(...)` /
  `MpApplySemanticState(...)`
- `NET_WORLD_STATE` carries semantic override records in addition to visible
  `kind/state`
- `Door` opts into that path, so world resync now preserves
  `CLOSED` / `OPEN` / `OPENING` / `CLOSING`, not just external open/closed

This is intentionally a narrow first step:

- the network semantic payload currently carries `logical_state` and `flags`
  only
- semantic `fields` / `refs` are still part of the contract design, but not
  encoded on the wire yet
- that is sufficient for `Door`, and it proves the contract shape before
  moving on to richer objects such as `ShogunDot`, `OxydStone`, `Rubberband`,
  or `Wire`

## Design goals

1. One semantic definition of "relevant object state"
2. Two transports:
   - local snapshot transport
   - network world-resync transport
3. Object-specific logic only for object-specific hidden state
4. No prediction-specific hacks in gameplay classes
5. Incremental migration from the current code

## Core idea

Each object can expose a **semantic multiplayer state record**.

That record is the authoritative description of the object's gameplay-relevant
internal state. It is then encoded in two different ways:

- **local snapshot encoding**
  - optimized for in-process restore
  - may use local object identity where appropriate
- **network resync encoding**
  - peer-safe
  - no raw pointers or process-local object ids

## Proposed interface

### Object-level semantic contract

```c++
struct MpObjectRef {
    enum class Kind {
        None,
        GridFloor,
        GridItem,
        GridStone,
        OtherByName,
        ActorByActorId
    };

    Kind kind = Kind::None;
    GridPos pos = GridPos(-1, -1);
    uint16_t actor_id = 0xFFFF;
    std::string name;
};

struct MpSemanticField {
    std::string key;
    Value value;
};

struct MpSemanticRefField {
    std::string key;
    MpObjectRef ref;
};

struct MpSemanticState {
    int logical_state = 0;
    uint32_t flags = 0;
    std::vector<MpSemanticField> fields;
    std::vector<MpSemanticRefField> refs;
};

enum class MpApplyContext {
    SnapshotRestore,
    WorldResync
};
```

On `Object`:

```c++
virtual void MpCaptureSemanticState(MpSemanticState &out) const;
virtual bool MpApplySemanticState(const MpSemanticState &state, MpApplyContext ctx);
```

### Meaning

- `logical_state`
  - the gameplay-relevant internal state, not just external `getAttr("state")`
  - examples:
    - `Door`: `CLOSED`, `OPEN`, `OPENING`, `CLOSING`
    - `ShogunDot`: `ON`, `OFF`
    - `ShogunStone`: current hole composition

- `flags`
  - only runtime flags that materially affect later behavior
  - not every scratch bit automatically belongs here

- `fields`
  - scalar gameplay data that must survive restore
  - examples:
    - `OxydStone`: `oxydcolor`
    - `Rubberband`: `strength`, `length`, `threshold`, `min`, `max`
    - `CoinSlot`: buffered add-time value

- `refs`
  - gameplay-relevant object relationships
  - examples:
    - `Wire`: `anchor1`, `anchor2`
    - `Rubberband`: `anchor1`, `anchor2`
    - `Vortex`: destination object references

## Important rule

Visible `kind/state` remains useful, but it is not the semantic contract.

Visible `kind/state` answers:

- what is on the grid right now

Semantic state answers:

- what internal runtime state determines future behavior

Both are needed.

## Two adapters

### 1. Local snapshot adapter

Used by visual prediction.

Requirements:

- exact enough for repeated capture/restore inside one process
- may use efficient local resolution
- can preserve object identity more directly

Suggested implementation:

- keep the current `sim_snapshot::Snapshot`
- replace the current split
  - `MpCaptureStateForSnapshot`
  - `MpCaptureFlagsForSnapshot`
  - `MpCaptureAttrsForSnapshot`
  with an adapter that fills `MpSemanticState`

Local resolution of refs can be done by:

- object id, if the object is guaranteed to survive within the same process
- or the same `MpObjectRef` resolver used by network code

The important point is semantic equivalence, not the exact encoding choice.

### 2. Network world-resync adapter

Used by lockstep repair.

Requirements:

- peer-safe
- no raw pointers
- no local object ids unless there is an explicit stable cross-peer mapping

Suggested encoding:

- world packet still carries visible grid reconstruction data:
  - floor/item/stone kind
  - logical external state where appropriate
  - movable-stone positions
- plus a list of **semantic override records** for objects that need more

Example:

```c++
struct NetObjectSemanticState {
    uint16_t x;
    uint16_t y;
    GridLayer layer;
    int32_t logical_state;
    uint32_t flags;
    std::vector<NetScalarField> fields;
    std::vector<NetRefField> refs;
};
```

For `Other` objects that are not on the grid:

- use named-object or stable resolver based transport entries

## Resolver rules

`MpObjectRef` must resolve the same gameplay relationship in both transports.

Preferred reference forms:

1. grid layer + position
   - for floor/item/stone references
2. actor id / player actor id
   - for actor references that are already synchronized
3. object name
   - for non-grid named objects

Avoid:

- process-local pointers
- bare runtime object ids in network packets

## Apply semantics

`MpApplySemanticState(...)` must be allowed to:

- restore hidden internal state directly
- rebuild internal topology
- reconnect side tables and registries
- avoid replaying gameplay side effects unintentionally

That means applying semantic state is **not** the same as calling generic
`setAttr("state", ...)`.

Examples:

- `ShogunDot`
  - restore `ON/OFF` without replaying `performAction()`
- `Door`
  - restore `OPENING/CLOSING` animation phase explicitly
- `ShogunStone`
  - rebuild hidden sub-shogun topology
- `Wire`
  - reconnect anchors and fellows/wires lists
- `OxydStone`
  - restore color and internal open/close state consistently

## Migration strategy

### Phase 1: wrap the current snapshot hooks

Implement `MpCaptureSemanticState()` / `MpApplySemanticState()` in terms of the
current snapshot hook split:

- `MpCaptureStateForSnapshot`
- `MpCaptureFlagsForSnapshot`
- `MpCaptureAttrsForSnapshot`

This gives us one semantic abstraction without changing all callers at once.

### Phase 2: use semantic state in local snapshots

Convert `sim_snapshot` capture/restore to use the new semantic contract
internally.

### Phase 3: add semantic payloads to world resync

Keep the current visible world-state packet, but add semantic records for the
objects that need more than visible `kind/state`.

Scalar semantic fields are now on that wire path for current users such as
`OxydStone` `oxydcolor`; reference transport for `Other` objects is still open.

### Phase 4: retire ad hoc world-resync special cases

Once semantic payloads are in place, remove special one-off handling where it
becomes redundant.

## Initial object mapping

These are the first objects that should implement the contract fully:

1. `Door`
2. `ShogunDot`
3. `ShogunStone`
4. `OxydStone`
5. `Rubberband`
6. `Wire`

That matches the current audit backlog and already proven problem cases.

## What this does not try to solve

- a single universal binary packet format for both local and network use
- automatic serialization of arbitrary private state
- prediction policy

This contract only defines:

- which object state matters
- how objects expose it
- how both restore paths can converge on the same semantics

## Recommendation

Do not extend `NET_WORLD_STATE` with more ad hoc object-specific fields.

Instead:

1. introduce the semantic object-state contract
2. adapt current snapshot hooks to it
3. migrate world resync to consume the same semantics incrementally

That gives us one notion of correctness for both prediction restore and
lockstep repair.
