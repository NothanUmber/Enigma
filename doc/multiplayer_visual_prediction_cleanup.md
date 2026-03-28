# Multiplayer Visual Prediction Cleanup

This note tracks the work that is still useful after the six implementation
steps in `doc/multiplayer_visual_prediction_target.md` were completed.

It is not a replacement target model. The target document remains the design
reference. This document is for:

- cleanup work
- probe hardening
- regression-test promotion
- soak-testing coverage
- later architectural follow-up

## Priorities

1. Turn the currently useful probes into stronger regression tests.
2. Run a broader manual soak matrix on the key gameplay levels.
3. Clean up the remaining timed semantic-state transport issue.
4. Trim or document test-harness sprawl so future work stays reviewable.

## Status On March 28, 2026

- The core acceptance probes were hardened into assertive pass/fail regressions:
  - `it_takes_two_visual_prediction_conflict_blend.txt`
  - `good_company_rubberband_state_probe.txt`
  - `way_to_go_door_render_state_probe.txt`
  - `open_sesame_door_render_probe.txt`
  - `client_desync_hold_probe.txt`
- The lifecycle probes were also upgraded so they now check explicit post-load
  or post-finish end states instead of relying on manual crash inspection:
  - `handle_with_care_finish_crash_probe.txt`
  - `handle_with_care_recorded_finish_crash_probe.txt`
  - `handle_with_care_restart_crash_probe.txt`
  - `inventory_reload_document_crash_probe.txt`
  - `inventory_reload_weight_crash_probe.txt`
- The harness/docs cleanup in this pass added future-only line waits plus
  numeric state comparison helpers and documented the newer setup/desync
  helpers in `tools/mp_test_scripts/README.md`.
- Setup snapshots now preserve stable puzzle-stone kinds during save/load, and
  `puzzlestone_setup_snapshot_probe.txt` covers the old orientation-remap bug.
- One legacy artifact remains:
  - `local/last_mp_recording_setup.mpsetup` was captured before the stable-kind
    fix and still loses puzzle orientation, so it should be refreshed before
    using that replay for visual inspection.
- Several overlapping scratch probes were retired after stronger maintained
  versions existed:
  - `open_sesame_shogun_render_probe.txt`
  - `open_sesame_true_door_timeline.txt`
  - `way_to_go_door_render_probe.txt`
  - `way_to_go_truth_timeline_probe.txt`
  - `it_takes_two_oxyd_anim_dense_probe.txt`
  - `lightpassenger_direction_probe.txt`
- Remaining follow-up after this pass:
  - broaden the manual soak matrix
  - decide whether `it_takes_two_oxyd_anim_probe.txt` should become a maintained
    oxyd-specific regression or be retired
  - optionally add the Good Company near-simultaneous two-player rubberband
    variant mentioned below

## Probe Hardening

The current committed scripts are valuable, but several still behave more like
sample collectors than hard pass/fail regression tests.

The intended direction is:

- keep focused repro scripts small
- add explicit assertions where possible
- prefer deterministic end conditions over manual log inspection
- consolidate overlapping scripts once a stronger version exists

The relevant script format and driver commands are documented in
`tools/mp_test_scripts/README.md`.

### High-priority acceptance probes

- `tools/mp_test_scripts/it_takes_two_visual_prediction_conflict_blend.txt`
  - keep as the canonical actor-conflict regression
  - strengthen it further by asserting the full mode sequence:
    `Truth -> LocalOwned -> BlendToTruth -> Truth`
  - add a final convergence check instead of stopping after blend-alpha motion

- `tools/mp_test_scripts/good_company_rubberband_state_probe.txt`
  - keep as the canonical coupled shared-state stress case
  - add explicit late equality assertions for both peers after the long delay
    window
  - consider adding one variant where both players actively influence the band
    at nearly the same time

- `tools/mp_test_scripts/way_to_go_door_render_state_probe.txt`
  - keep as the cleaner world-persistence regression
  - add explicit early divergence plus later convergence assertions for the
    shogun/door state instead of only render sampling

- `tools/mp_test_scripts/open_sesame_door_render_probe.txt`
  - keep only if it adds coverage beyond `Way To Go`
  - either strengthen it into a real regression for a door-coupled Per.Oxyd
    setup, or demote/remove it to avoid duplicate weak probes

- `tools/mp_test_scripts/client_desync_hold_probe.txt`
  - keep as the intentional divergence/regain-truth probe
  - add assertions for:
    - client diverges while hold is active
    - host remains authoritative and unchanged by held local input
    - client converges back after the hold is released

### Stability and lifecycle probes worth hardening

- `tools/mp_test_scripts/handle_with_care_finish_crash_probe.txt`
- `tools/mp_test_scripts/handle_with_care_recorded_finish_crash_probe.txt`
- `tools/mp_test_scripts/handle_with_care_restart_crash_probe.txt`
- `tools/mp_test_scripts/inventory_reload_document_crash_probe.txt`
- `tools/mp_test_scripts/inventory_reload_weight_crash_probe.txt`

These should become a small restart/reload/finalize stability suite with clear
expected end states, not just “did not crash during manual inspection”.

### Probe review backlog

This pass already folded or removed the obvious duplicates listed in the
status section above.

The remaining item worth an explicit follow-up decision is:

- `tools/mp_test_scripts/it_takes_two_oxyd_anim_probe.txt`
  - either promote it into a maintained oxyd-animation regression
  - or remove it if the maintained acceptance set is judged sufficient without
    a dedicated oxyd-specific visual-prediction probe

## Manual Soak Matrix

Even with the focused scripts above, manual play coverage is still useful.

### Levels

- `It Takes Two`
  - single local kick on stationary remote actor
  - delayed remote free motion
  - overlapping local/remote influence on the same actor

- `Good Company`
  - rubberband-coupled motion under long delay
  - simultaneous two-player influence on a shared coupled situation

- `Way To Go`
  - locally changed world state persistence
  - eventual handoff back to truth after the delay window

- `Open Sesame`
  - shogun/door coupled world interactions
  - high-latency persistence of locally changed world elements

- `Handle with Care`
  - finish / restart / next-level transition stability
  - setup replay and restore sanity

### Conditions

- low delay
  - roughly normal playable settings
- medium delay
  - enough to expose the mixed-time model clearly
- high delay
  - around the current 120-tick stress profile
- with and without netsim/jitter
- with and without intentional client desync hold

### Things to watch for

- free remote motion must still look like delayed truth, not telepathic future
  motion
- locally affected remote actors must not:
  - snap back
  - teleport to their final resting place
  - replay the same rollout multiple times
- coupled conflict cases should blend rather than pop
- locally changed world elements should persist until truth catches up
- both peers should converge again after the delay window closes
- restart / reload / finish transitions must stay crash-free

## Timed Semantic-State Cleanup

This is the remaining architectural follow-up already noted in
`doc/multiplayer_visual_prediction_target.md`.

Status on March 28, 2026:

- explicitly deferred after the test-hardening/cleanup pass
- current risk:
  - under jitter/load, tiny timer drift can still trigger unnecessary semantic
    re-apply and timer re-arm churn for timer-backed objects
- why deferred:
  - the right fix still wants dedicated timer-infrastructure work rather than a
    probe-only patch, and it is separable from the regression hardening work

### Problem

Some objects currently transport live countdown state as ordinary semantic
fields. That is good enough for semantic parity, but it is not the ideal
long-term model.

The side effect is that a tiny timer difference can trigger a semantic reapply
and re-arm a repeating timer more often than necessary.

### Cleanup direction

- keep durable semantic state in the existing semantic channel
- move timed phase transport toward host-tick-anchored data
  - `next_alarm_tick`
  - or `phase_start_tick + interval`
- extend apply context so timed objects can reconcile packet time against local
  time instead of restoring stale raw `timeleft`
- add a drift deadband so tiny countdown mismatches do not rebuild alarms
- implement this generically in timer infrastructure first, then reuse it for:
  - `TimerGadget`
  - `TimerStone`
  - `MonoFlopStone`
  - any similar timer-backed object found later

### Follow-up testing for timed cleanup

- rerun semantic world-resync probes for timed objects under jitter/load
- add one dedicated jitter-focused probe where timer countdown differs slightly
  but should not trigger a semantic rebuild
- verify that repeated host world-state broadcasts do not cause visible timer
  wobble on the client

## Harness and Documentation Cleanup

- document the newer test-driver helpers that were added during this branch in
  `tools/mp_test_scripts/README.md`
  - especially destination setup helpers, setup snapshot helpers, and desync
    hold usage
- review whether some one-off driver commands can be grouped or better named in
  `src/multiplayer_test_driver.cc`
- keep the target doc focused on design/acceptance and move future maintenance
  backlog here rather than reopening the six-step roadmap

## Exit Criteria For This Cleanup Note

This follow-up work is in a good state when:

1. the core visual-prediction acceptance probes are assertive and reliable
2. the overlapping scratch probes have been reviewed and reduced to a smaller
   maintained set
3. the manual soak matrix has been run on the key levels under multiple delay
   profiles
4. the timed semantic-state cleanup is either implemented or explicitly
   deferred with its risk clearly documented
