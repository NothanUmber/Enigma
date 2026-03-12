# Target Visual Prediction Model

This note captures the intended target behavior for the high-latency visual
prediction experiment. It exists so the implementation can be judged against a
stable target instead of drifting across iterations.

## Decision

The current unstaged actor interpolation experiment should be reverted before
continuing. The committed baseline is well-defined and playable; the current
experiment is not. It is easier to implement the target model cleanly from that
baseline than to repair the current partial actor-state machine in place.

## Core timeline

Let:

- `T` be the current authoritative lockstep tick
- `D` be the configured input delay in ticks

Then:

- the authoritative world represents tick `T`
- the local visual prediction represents `T` plus the pending local input window
- the local pending input window covers the local inputs that truth has not yet
  applied

The predictor therefore always starts from the **current lockstep state**, not
from an older snapshot. It then reapplies the still-pending local inputs on top
of that truth state.

## Actor behavior

### Local actor

The locally controlled actor is driven by the replayed local input window. This
gives immediate local control.

### Remote actor by default

A remote-controlled actor is truth-driven by default.

That means:

- it should apply its own remote input only when the authoritative lockstep sim
  would also apply it
- free remote motion should therefore look like the sender's motion, only
  delayed by approximately `D` ticks
- no telepathic remote movement is allowed

### Local interaction with a remote actor

If the local predicted actor directly affects a remote actor, the remote actor
must react immediately in the predicted world as well.

Example:

- local ball kicks a stationary remote ball

Expected result:

- the remote ball starts moving immediately in the local predicted world
- the resulting rollout persists smoothly
- once truth catches up `D` ticks later, the same remote motion should already
  exist on the truth side
- if no conflicting remote input arrived in the meantime, the handoff back to
  truth should be invisible

This is the critical case where a reset-to-truth each frame is wrong.

### Handoff back to truth

For the initial target implementation, the handoff from `LocalOwned` back to
truth-driven mode should use a strict and simple criterion:

- the remote actor in the locally owned predicted sim stands still
- and the corresponding remote actor in the authoritative lockstep sim also
  stands still

Only then should the actor be pulled out of `LocalOwned` mode again.

The intention is:

- while the predicted rollout is still evolving, keep the predicted actor alive
- once both trajectories have naturally come to rest, the truth-driven actor
  should have converged closely enough that the handoff can happen invisibly

This is the default release rule unless a conflict state was entered earlier.

## Required actor modes

The intended actor model is:

1. **Truth-driven**
   - default for remote actors
   - remote motion follows authoritative truth timing

2. **Locally-owned**
   - entered when a local predicted interaction materially changes a remote
     actor
   - the actor keeps its predicted state across frames instead of being rebuilt
     from current truth each render pass

3. **Conflict / Blend-to-truth**
   - entered if a remotely controlled actor is locally-owned and then receives
     real remote input before truth has caught up
   - this is a presentation mode for visually reconciling incompatible
     influences

## Ownership rule

Ownership is not a gameplay concept. It is a persistence rule:

- while truth has not yet caught up to the locally predicted interaction, the
  replay-affected remote actor must not be reset from the current truth
  snapshot
- once both predicted and truth-driven versions of that actor are standing
  still, ownership can be released

The release criterion must be based on the actor state that truth reaches for
the same interaction, not on arbitrary frame timing.

## Conflict rule

If a locally-owned remote actor receives real remote input before the local
prediction has naturally handed off to truth, then the predictor is in
conflict.

Expected behavior:

- do not snap abruptly
- interpolate visually toward the truth-driven state
- blend both position and velocity, not position alone

This blend is only a presentation policy. It does not change authoritative
truth.

## World behavior

The same semantic principle applies to world objects:

- start from current truth
- replay pending local inputs
- if a local predicted interaction changes a world element, that change must
  persist until truth catches up or conflict resolution is required

For this to work reliably, prediction snapshots and world resync must preserve
the same gameplay-relevant internal object state.

See also:

- `doc/multiplayer_object_state_contract.md`
- `doc/multiplayer_snapshot_audit.md`

## Explicit non-goals

The target model should **not** do any of the following:

- rebuild remote actor behavior every frame purely from current truth, if that
  actor was already changed by local prediction
- replay future remote input early just because the packet is already known
- use a global predicted-vs-truth world digest as the primary control mechanism
  for this mixed-time model
- rely on object-specific prediction hacks for gameplay behavior

## Acceptance criteria

The target implementation is correct if all of the following hold:

1. Local actor feels immediate.
2. Freely moving remote actors look like delayed truth, including natural
   rollout.
3. A local kick on a stationary remote actor causes immediate remote-ball
   reaction in the predicted view.
4. That kicked remote ball does not:
   - snap back
   - teleport to the final position
   - rerun the same rollout multiple times
5. If both players influence the same actor during the delay window, the actor
   may require correction, but the transition is visually blended instead of
   abrupt.

## Implementation roadmap

Use this as the working TODO list. Each step should remain reviewable on its
own and should end in a clearly testable intermediate behavior.

Status markers:

- `[ ]` not started
- `[~]` started / partial
- `[x]` done

### Step 1 — Persist actor prediction state across frames

- Status: `[~]`
- Goal:
  - stop rebuilding replay-affected remote actors from current truth every
    render frame
  - keep enough actor state alive across frames so a locally kicked remote ball
    can continue its predicted rollout instead of snapping back
- Files:
  - `src/multiplayer_visual_prediction.cc`
    - add persistent per-remote-actor state storage
    - keep remote actors truth-driven by default
    - keep locally affected remote actors in a persistent locally-owned state
      across frames
  - `src/multiplayer_sim_snapshot.hh`
  - `src/multiplayer_sim_snapshot.cc`
    - keep or extend narrow actor capture/restore helpers needed for
      per-actor persistence
- Expected behavior after this step:
  - free remote movement still looks like delayed truth
  - when the local ball kicks a stationary remote ball, that remote ball no
    longer rolls forward, snaps back, and rolls again
  - handoff back to truth may still be rough or missing
- Test:
  - use a simple two-ball level such as `It Takes Two`
  - move one ball into the other while the remote mouse is not moving
  - verify that the kicked remote ball keeps one continuous locally predicted
    rollout

### Step 2 — Release locally-owned remote actors only when both sides stand still

- Status: `[ ]`
- Goal:
  - make ownership release deterministic and simple
  - avoid premature handoff while predicted rollout is still evolving
- Files:
  - `src/multiplayer_visual_prediction.cc`
    - implement the documented release rule:
      - predicted locally-owned remote actor stands still
      - corresponding truth actor also stands still
    - once both are idle, hand the actor back to truth-driven mode
- Expected behavior after this step:
  - kicked remote actors keep their predicted rollout until it naturally
    settles
  - once both predicted and truth versions are idle, the actor falls back to
    truth-driven mode invisibly or near-invisibly
- Test:
  - repeat the single-kick scenario
  - verify that the remote ball rolls out once and then remains stable
  - verify that after the rollout the actor resumes truth-driven updates

### Step 3 — Enter conflict mode when real remote input arrives during local ownership

- Status: `[ ]`
- Goal:
  - handle the case where a remote actor is already locally owned and then the
    remote player starts influencing it before truth has caught up
- Files:
  - `src/multiplayer_visual_prediction.cc`
    - track the three actor modes explicitly:
      - `Truth`
      - `LocalOwned`
      - `BlendToTruth`
    - detect truth-time remote input for a locally-owned remote actor
    - switch that actor into conflict/blend mode
- Expected behavior after this step:
  - the actor no longer snaps when local and remote influences overlap
  - instead it transitions to a blended handoff toward truth
- Test:
  - use a coupled level or arrange a case where:
    - local player kicks or drags the remote ball
    - remote player starts moving shortly afterwards
  - verify that the remote ball does not snap abruptly at the first real remote
    input

### Step 4 — Blend both position and velocity in conflict mode

- Status: `[ ]`
- Goal:
  - make conflict presentation visually plausible instead of mushy or abrupt
- Files:
  - `src/multiplayer_visual_prediction.cc`
    - implement render-only blending of:
      - position
      - velocity
    - do not mutate authoritative truth as part of this blend
- Expected behavior after this step:
  - conflict handoff feels smoother
  - remote actor no longer teleports when transitioning from locally-owned to
    truth-driven
- Test:
  - same conflict scenario as Step 3
  - compare before/after visual transition quality

### Step 5 — Extend the same persistence rule from actors to locally changed world elements

- Status: `[ ]`
- Goal:
  - make locally changed world state follow the same mixed-time semantics as
    actors
  - prevent doors, movable stones, and similar elements from being rebuilt from
    truth each frame while they are still locally predicted
- Files:
  - `src/multiplayer_visual_prediction.cc`
    - track locally owned world elements in addition to actors
  - `src/world.hh`
  - `src/world.cc`
  - object classes covered by the snapshot audit as needed
  - `doc/multiplayer_snapshot_audit.md`
    - mark which classes are now prediction-owned cleanly
- Expected behavior after this step:
  - locally changed world elements persist smoothly until truth catches up
  - world interactions no longer need per-frame truth resets to stay visible
- Test:
  - `Open Sesame` for doors and shogun stacks
  - `Good Company` for rubberbanded movement and world updates

### Step 6 — Align lockstep world resync with the same semantic state contract

- Status: `[ ]`
- Goal:
  - make prediction snapshot restore and lockstep world resync restore the same
    gameplay-relevant object state
- Files:
  - `src/Object.hh`
  - `src/Object.cc`
  - `src/multiplayer_protocol.hh`
  - `src/multiplayer_session_sync.cc`
  - `src/multiplayer_session_transport.cc`
  - object classes as needed (`Door`, `ShogunDot`, `ShogunStone`,
    `OxydStone`, `Rubberband`, `Wire`, ...)
  - `doc/multiplayer_object_state_contract.md`
  - `doc/multiplayer_snapshot_audit.md`
- Expected behavior after this step:
  - prediction restore and world resync are semantically aligned
  - fewer object-specific edge cases where resync repairs visible state but not
    future behavior
- Test:
  - provoke a world divergence intentionally
  - verify that the repaired peer behaves the same afterwards, not just looks
    the same immediately
