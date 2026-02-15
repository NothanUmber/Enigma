#!/usr/bin/env python3
"""
Generate `doc/multiplayer_pr_review.html` from `git diff <base>`.

Audience: upstream maintainers reviewing the multiplayer patch. The output is
organized per-path:
- Description: what the file/module is.
- Where it is used: concrete call sites / dependencies.
- What changed vs upstream/master: the intent of the delta.
- Full red/green diff for the exact code changes.

We intentionally do NOT embed per-hunk notes in the HTML. If a code region needs
extra explanation long-term, prefer adding an in-code comment so the rationale
travels with the code (and not only with this review artifact).
"""

from __future__ import annotations

import argparse
import hashlib
import html
import re
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime
from typing import Dict, List, Optional


def sh(*args: str) -> str:
    return subprocess.check_output(args, stderr=subprocess.STDOUT, text=True)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8", errors="replace")).hexdigest()


def section_id_for_path(path: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "_", path).strip("_")


@dataclass
class FileNarrative:
    description: List[str]
    where_used: List[str]
    what_changed: List[str]


def parse_git_diff(base_ref: str) -> Dict[str, str]:
    """
    Returns map: path -> raw unified diff section for that path.
    Only includes tracked files (including newly staged additions).
    """
    patch = sh("git", "diff", "--no-color", base_ref)
    sections: Dict[str, List[str]] = {}
    current_path: Optional[str] = None

    for line in patch.splitlines(True):
        if line.startswith("diff --git "):
            m = re.match(r"diff --git a/(.*?) b/(.*?)\n?$", line)
            current_path = m.group(2) if m else None
            if current_path is not None:
                sections.setdefault(current_path, []).append(line)
            continue
        if current_path is None:
            continue
        sections[current_path].append(line)

    return {k: "".join(v).rstrip("\n") + "\n" for k, v in sections.items()}


def should_exclude_review_path(path: str) -> bool:
    # Ignore generated interpreter cache artifacts and long-form docs that are
    # better reviewed in their native format.
    if path.endswith(".pyc") or "/__pycache__/" in f"/{path}":
        return True
    if path == "doc/multiplayer_architecture.md":
        return True
    if path == "tools/create_multiplayer_pr_review.py":
        return True
    return False


def infer_change_tag(raw_diff: str) -> str:
    if "\nnew file mode " in raw_diff or "\n--- /dev/null" in raw_diff:
        return "A"
    if "\ndeleted file mode " in raw_diff:
        return "D"
    return "M"


def render_diff_html(raw_diff: str) -> str:
    lines = raw_diff.splitlines()
    out: List[str] = []
    out.append('<div class="diff">')

    old_ln = 0
    new_ln = 0

    hunk_re = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
    meta_prefixes = (
        "diff --git ",
        "index ",
        "new file mode ",
        "deleted file mode ",
        "old mode ",
        "new mode ",
        "rename from ",
        "rename to ",
        "copy from ",
        "copy to ",
        "similarity index ",
        "dissimilarity index ",
        "Binary files ",
    )

    for line in lines:
        if line.startswith(meta_prefixes) or line.startswith("---") or line.startswith("+++"):
            out.append('<div class="line meta"><span class="code">%s</span></div>' % html.escape(line))
            continue
        m = hunk_re.match(line)
        if m:
            old_ln = int(m.group(1))
            new_ln = int(m.group(3))
            out.append('<div class="line hunkhdr"><span class="code">%s</span></div>' % html.escape(line))
            continue
        if not line:
            out.append(
                '<div class="line ctx"><span class="ln old"></span><span class="ln new"></span><span class="code"></span></div>'
            )
            continue

        prefix = line[0]
        text = line[1:] if len(line) > 1 else ""

        if prefix == "+":
            cls = "add"
            o = ""
            n = str(new_ln)
            new_ln += 1
        elif prefix == "-":
            cls = "del"
            o = str(old_ln)
            n = ""
            old_ln += 1
        else:
            cls = "ctx"
            o = str(old_ln)
            n = str(new_ln)
            old_ln += 1
            new_ln += 1

        out.append(
            '<div class="line %s"><span class="ln old">%s</span><span class="ln new">%s</span><span class="code">%s</span></div>'
            % (cls, html.escape(o), html.escape(n), html.escape(text))
        )

    out.append("</div>")
    return "\n".join(out)


def html_list(items: List[str]) -> str:
    if not items:
        return '<div class="empty">(none)</div>'
    out = ["<ul>"]
    for it in items:
        out.append("<li>%s</li>" % html.escape(it))
    out.append("</ul>")
    return "\n".join(out)


def build_narratives() -> Dict[str, FileNarrative]:
    """
    Curated per-file narratives.

    This is intentionally explicit: the review doc is for humans. If a file is
    missing here, the generated HTML fails so we don't ship an under-explained
    diff to maintainers.
    """
    n: Dict[str, FileNarrative] = {}

    def add(path: str, description: List[str], where_used: List[str], what_changed: List[str]) -> None:
        n[path] = FileNarrative(description=description, where_used=where_used, what_changed=what_changed)

    # Repo/meta
    add(
        ".gitignore",
        ["Top-level ignore rules for build artifacts and developer-local files."],
        ["Used by git; affects what can accidentally appear in PR diffs."],
        [
            "Ignores developer-local build/output artifacts (`local/`, `__pycache__/`, `*.pyc`, `*.log`, etc.) to keep review diffs clean.",
        ],
    )
    add(
        "build_enigma.sh",
        ["Developer convenience build script for macOS/Homebrew builds."],
        ["Optional; run manually from the repo root."],
        ["No multiplayer intent: keep this script available for contributors building Enigma on macOS."],
    )
    add(
        "multiplayer_architecture.md",
        ["Legacy multiplayer architecture document (repo root)."],
        ["Was previously used as a design note during development."],
        ["Removed in favor of `doc/multiplayer_architecture.md` (single canonical location)."],
    )
    add(
        "out1.txt",
        ["Developer-local debug output artifact."],
        ["Not used by the build or runtime."],
        ["Removed stray debug output that should not be tracked."],
    )
    add(
        "out2.txt",
        ["Developer-local debug output artifact."],
        ["Not used by the build or runtime."],
        ["Removed stray debug output that should not be tracked."],
    )

    # Containerized backend deployment
    add(
        "Dockerfile",
        ["Minimal container image to run the Internet lobby server plus UDP and TCP relays."],
        ["Used for easy deployment to a public VM/container host for Internet multiplayer testing."],
        [
            "Adds a combined lobby+relay container setup.",
            "Exposes and documents ports: 12347/udp lobby, 12348/udp UDP relay, 12349/tcp TCP relay.",
        ],
    )
    add(
        "tools/docker-entrypoint.sh",
        ["Entrypoint script that starts the lobby server and both relay servers inside the container."],
        ["Used by the `Dockerfile` container image."],
        ["New helper for deploying the multiplayer backend as one container process tree."],
    )
    add(
        "tools/internet_lobby_server.py",
        [
            "Internet lobby service (custom UDP protocol) used for room creation/joining and for distributing session metadata.",
            "This server coordinates who is in a room; it is not on the per-tick gameplay data path.",
        ],
        [
            "Clients in Internet mode talk to this service to create/join/leave rooms and discover peers/transport options.",
            "Deployed alongside the relays for Internet play.",
        ],
        [
            "New optional Internet coordination service for multiplayer.",
            "Tracks room members as id/name pairs, returns member lists in JOIN/POLL responses, and closes the room immediately when the host leaves so stale rooms do not linger.",
        ],
    )
    add(
        "tools/relay_server.cc",
        ["UDP relay server used as a fallback gameplay transport when direct UDP connectivity fails."],
        ["Used in Internet mode when direct connect fails or is disabled."],
        ["New backend component: UDP relay for gameplay packets."],
    )
    add(
        "tools/tcp_relay_server.cc",
        ["TCP relay server used as a last-resort gameplay transport for networks that block UDP."],
        ["Used in Internet mode when both direct connect and UDP relay fail/are disabled."],
        ["New backend component: TCP relay fallback for restrictive networks."],
    )

    # Data + schemas
    add(
        "data/levels/enigma_experimental/index.xml",
        ["Level pack index for experimental levels."],
        ["Consumed by the level pack indexer and the in-game level selection widgets."],
        ["Registers new multiplayer test levels so they appear in the level pack."],
    )
    add(
        "data/levels/enigma_experimental/mp_test_3p.xml",
        ["Minimal 3-player test level used to validate multi-marble session wiring."],
        ["Selectable from the multiplayer lobby (or the experimental pack when filters allow)."],
        ["New multiplayer test map for 3 players (includes Oxyd stones so it is finishable)."],
    )
    add(
        "data/levels/enigma_experimental/mp_test_4p.xml",
        ["Minimal 4-player test level used to validate multi-marble session wiring."],
        ["Selectable from the multiplayer lobby (or the experimental pack when filters allow)."],
        ["New multiplayer test map for 4 players (includes Oxyd stones so it is finishable)."],
    )
    add(
        "data/schemas/enigmarc.xml",
        ["Schema for Enigma configuration (preferences) file."],
        ["Used by config parsing/validation for preferences UI and runtime."],
        ["Adds multiplayer-related configuration keys (server address, ports, transport toggles)."],
    )
    add(
        "data/schemas/level.xsd",
        ["XML schema for level files."],
        ["Used to validate level XML metadata such as player counts and network flags."],
        ["Extends/aligns schema to support the multiplayer metadata used by the lobby filters."],
    )

    # Docs
    add(
        "doc/.gitignore",
        ["Ignore rules specific to generated documentation output under `doc/`."],
        ["Prevents accidental check-in of generated manuals/reference output."],
        ["Ensures `doc/multiplayer_pr_review.html` is kept even though other `*.html` in `doc/` is ignored."],
    )
    add(
        "doc/multiplayer_architecture.md",
        ["Developer documentation for multiplayer architecture and operational modes (LAN vs Internet)."],
        ["Reference for maintainers and contributors reviewing the multiplayer implementation."],
        ["New document describing multiplayer internals and deployment/testing guidance."],
    )

    # Build + entry points
    add(
        "src/Makefile.am",
        ["Autotools build rules for `src/`."],
        ["Used by `./configure && make` on all platforms."],
        ["Adds new multiplayer, input, and multiplayer-specific UI source files to the build."],
    )
    add(
        "src/main.cc",
        ["Program entry point and global initialization."],
        ["Starts the game, loads preferences, enters main menu."],
        [
            "Wires multiplayer into the UI and startup (menus, options, and runtime hooks).",
            "On shutdown, triggers a best-effort Internet room leave so room ownership is released immediately in normal exits.",
        ],
    )
    add(
        "src/main.hh",
        ["Global main-menu/runtime flags and declarations."],
        ["Included across UI and main loop code."],
        ["Adds `ShowDebugOptions` so the debug tabs can be enabled via `--show-debugoptions`."],
    )

    # Menus/UI integration
    add(
        "src/gui/MainMenu.cc",
        ["Main menu UI implementation."],
        ["Displayed on startup; routes to single-player menus and the multiplayer lobby."],
        ["Adds a Network/Multiplayer entry that opens the multiplayer lobby."],
    )
    add(
        "src/gui/MainMenu.hh",
        ["Main menu UI declarations."],
        ["Included by the menu system."],
        ["Declares new actions/entries for multiplayer."],
    )
    add(
        "src/gui/MultiplayerMenu.hh",
        ["Multiplayer lobby UI declarations and shared helper types."],
        ["Included by `src/gui/MultiplayerMenu*.cc` and `src/gui/MainMenu.cc`."],
        ["New multiplayer lobby screen API and widget wiring."],
    )
    add(
        "src/gui/MultiplayerMenu.cc",
        ["Multiplayer lobby UI implementation (high-level glue and drawing)."],
        ["Opened from main menu; controls LAN and Internet room/lobby flows."],
        [
            "New lobby UI: discover peers, choose packs/maps, start sessions, show transport status.",
            "Remembers LAN/Internet mode and Internet room state while reopening the multiplayer menu in the same process session.",
        ],
    )
    add(
        "src/gui/MultiplayerMenu_internal.hh",
        ["Private internal declarations for the multiplayer lobby implementation."],
        ["Included by the `MultiplayerMenu_*.cc` split units."],
        ["New internal header created as part of multiplayer UI implementation."],
    )
    add(
        "src/gui/MultiplayerMenu_common.cc",
        ["Multiplayer lobby shared UI helpers (layout, text formatting, common widgets)."],
        ["Used by the lobby implementation units."],
        ["Splits lobby code into smaller units to separate concerns and reduce file size."],
    )
    add(
        "src/gui/MultiplayerMenu_levels.cc",
        ["Multiplayer lobby level-pack and level-list logic."],
        ["Used by lobby to present filtered, deterministic level ordering and pack selection."],
        [
            "Implements level filtering and deterministic ordering for multiplayer selection.",
            "Improves Internet lobby UX: full-width status/info panel when not showing host level selection, avoids clipped status text/buttons at low resolutions, and renders Internet member names from lobby data.",
        ],
    )
    add(
        "src/gui/MultiplayerMenu_tick.cc",
        ["Multiplayer lobby per-frame update logic (poll lobby, refresh lists, update status)."],
        ["Called from the lobby menu loop."],
        [
            "Encapsulates ticking/polling so the menu stays responsive while networking progresses.",
            "Updates room membership from poll responses (including display names) and handles host-closed rooms by forcing clients out with a clear status message.",
        ],
    )
    add(
        "src/gui/MultiplayerMenu_actions.cc",
        ["Multiplayer lobby button actions (create/join/leave room, start level, toggles)."],
        ["Event handlers for multiplayer menu widgets."],
        [
            "Centralizes action handlers, reducing stateful logic scattered across the UI.",
            "Shows explicit guidance when Internet lobby/relay host is unresolved (`CHANGEME`) instead of a generic invalid-address error.",
        ],
    )
    add(
        "src/gui/Menu.cc",
        ["Base menu/event loop implementation."],
        ["Used by all menu screens (main menu, options, multiplayer lobby, in-game menus)."],
        ["Adds hooks/behavior needed to keep multiplayer UI responsive during network transitions."],
    )
    add(
        "src/gui/Menu.hh",
        ["Base menu declarations."],
        ["Included by menu implementations."],
        ["Extends menu API to support non-blocking updates needed for multiplayer transitions."],
    )
    add(
        "src/gui/GameMenu.cc",
        ["In-game ESC menu (resume/restart/abort/etc)."],
        ["Shown during gameplay when pressing ESC."],
        [
            "Integrates multiplayer pause/abort semantics so pausing is coordinated and abort/restart are propagated.",
            "Keeps Level Info and Options reachable in multiplayer; multiplayer settings editing is handled in the Options screen itself.",
        ],
    )
    add(
        "src/gui/MultiplayerWaitMenu.hh",
        ["Small in-game menu shown when lockstep stalls (\"waiting for player...\")."],
        ["Instantiated by `src/client.cc` while in `cls_multiplayer_waiting_for_players`."],
        ["New UI surface for lockstep stall recovery/abort (countdown + leave button)."],
    )
    add(
        "src/gui/MultiplayerWaitMenu.cc",
        ["Implementation of the lockstep stall wait dialog menu."],
        ["Used by the client state machine while waiting for missing inputs."],
        ["Adds a dedicated wait dialog to make stalls visible and let users abort the session deliberately."],
    )
    add(
        "src/gui/OptionsMenu.cc",
        ["Options/settings menu implementation."],
        ["Allows users to configure video/audio/input and multiplayer preferences."],
        [
            "Adds a Multiplayer tab for default Internet server and transport/port options.",
            "Locks multiplayer settings while a game is ongoing and shows a read-only guidance message instead of editable network fields.",
        ],
    )
    add(
        "src/gui/OptionsMenu.hh",
        ["Options menu declarations."],
        ["Included by options menu implementation."],
        ["Declares new multiplayer-related settings widgets."],
    )
    add(
        "src/gui/TextField.cc",
        ["Editable text field widget."],
        ["Used in OptionsMenu and MultiplayerMenu for server/room code entry."],
        ["Adds a locked mode so fields can be shown but not edited while in-room."],
    )
    add(
        "src/gui/TextField.hh",
        ["Text field widget declarations."],
        ["Included by UI code."],
        ["Declares the locking API used by multiplayer UI."],
    )
    add(
        "src/gui/LevelPackMenu.cc",
        ["Level pack selection UI."],
        ["Used by single-player and multiplayer flows for pack selection."],
        ["Reused by multiplayer lobby for pack selection; minor adjustments for integration."],
    )
    add(
        "src/gui/LevelPackMenu.hh",
        ["Level pack selection UI declarations."],
        ["Included by menu code."],
        ["Minor interface adjustments to support multiplayer lobby integration."],
    )
    add(
        "src/gui/LevelWidget.cc",
        ["Level grid/list widget used to display level icons and metadata."],
        ["Used in single-player and multiplayer selection screens."],
        ["Supports multiplayer-specific filtering/selection behavior and avoids crashes when packs change."],
    )

    # Windows build compatibility (transitive include collisions)
    add(
        "lib-src/enigma-core/ecl_video.hh",
        [
            "enigma-core video primitives (colors, surfaces, scaling, graphics state).",
            "This is upstream engine code, but it is pulled in transitively by platform headers in some builds.",
        ],
        [
            "Used across the engine and UI for basic color and rendering types.",
            "On Windows it can be included after `<windows.h>` via ENet/SDL headers.",
        ],
        [
            "Undefines the `RGB` macro from `<windows.h>` so Enigma's `ecl::RGB` type can be declared reliably on Windows builds.",
        ],
    )

    # Multiplayer public API + core orchestration
    add(
        "src/multiplayer.hh",
        ["Public multiplayer entry points used by UI and runtime to start/stop/poll multiplayer."],
        ["Called from UI and from the main client/game loop when multiplayer is active."],
        [
            "New public multiplayer API surface.",
            "Extends Internet room APIs to return member lists (ids/names), exposes local lobby display name, and adds a shutdown room-leave helper.",
            "Exposes a small runtime control hook (`SetInputClockFrozen`) used to keep input latency stable across long stalls/waits.",
        ],
    )
    add(
        "src/multiplayer.cc",
        [
            "Multiplayer integration layer (high-level orchestration and compatibility glue).",
            "Detailed logic lives in the `multiplayer_*` modules; this file routes between them.",
        ],
        ["Acts as the integration layer between existing Enigma runtime and the new multiplayer session."],
        ["New multiplayer subsystem implementation (including small runtime helpers used by client UI/state transitions)."],
    )
    add(
        "src/multiplayer_config.hh",
        ["Central multiplayer configuration struct shared between UI and runtime."],
        ["Read by OptionsMenu and by multiplayer runtime to decide transports/ports and defaults."],
        ["New config parsing/validation for multiplayer (server host, ports, toggles, env overrides)."],
    )
    add(
        "src/multiplayer_config.cc",
        ["Multiplayer configuration parsing/validation implementation."],
        ["Used by options UI and multiplayer runtime."],
        ["Centralizes configuration logic to avoid drift between UI fields and runtime behavior."],
    )
    add(
        "src/multiplayer_transport.hh",
        ["Transport abstraction used by session logic (direct, UDP relay, TCP relay) via one API."],
        ["Used by multiplayer session logic; implemented by `src/multiplayer_transport.cc`."],
        ["Introduces a transport facade to decouple session logic from socket/relay details."],
    )
    add(
        "src/multiplayer_transport.cc",
        ["Transport factory and common send/recv/poll behavior shared across transport variants."],
        ["Used by session start/connect code to select and drive the active transport."],
        ["Refactors gameplay networking so session logic doesn't care which transport is chosen."],
    )
    add(
        "src/multiplayer_session.hh",
        ["Multiplayer session state machine API (join/start/restart/abort/next/pause)."],
        ["Driven by `src/client.cc` per tick and by UI actions (start, leave, abort)."],
        ["New explicit session state machine interface for multiplayer."],
    )
    add(
        "src/multiplayer_session_impl.hh",
        ["Internal session data structures and invariants for multiplayer state transitions."],
        ["Included by the session implementation units."],
        ["New internal header as part of the multiplayer session implementation."],
    )
    add(
        "src/multiplayer_wait_settings.hh",
        [
            "Central constants for lockstep stall waiting behavior (dialog delays, abort timeouts, and ENet peer timeout tuning when available).",
            "Keeps UI timeout and transport timeout aligned across ENet versions.",
        ],
        [
            "Included by `src/client.cc` for the stall dialog delay/timeout.",
            "Included by session start/transport code to configure ENet peer timeouts on connect/accept.",
        ],
        ["New shared settings header used by both UI and transport to keep stall/timeout behavior consistent."],
    )
    add(
        "src/multiplayer_session_start.cc",
        ["Session start/join handshake logic (host and client) over the selected transport."],
        ["Used when starting a level from the multiplayer lobby."],
        [
            "Implements deterministic start sequencing and readiness gating across peers.",
            "Improves diagnostics and robustness of joins: logs ENet connect events and ignores WELCOME packets that don't match the expected session seed (guards against late/stale packets).",
            "Uses ENet-version-compatible address logging paths so the same code builds with both vendored ENet 1.0 and system ENet 1.3.",
            "When ENet >= 1.3 is available, configures peer timeout to tolerate transient outages and align with the in-game stall wait dialog.",
        ],
    )
    add(
        "src/multiplayer_session_transport.cc",
        ["Session transport plumbing: packet IO, polling, and shutdown behavior."],
        ["Used by session tick to exchange per-tick input and control messages."],
        [
            "Ensures abort/shutdown stops transport polling immediately to avoid use-after-free and UI stalls.",
            "Validates READY/START against the current `session_id`, `epoch`, and per-level `load_id` so late packets cannot unblock the wrong level start.",
            "Implements host-driven level transitions by handling `NET_LOAD_LEVEL` (switch pack + load level by normalized path).",
            "Uses ENet-version-compatible address formatting in debug logs (works with vendored ENet 1.0 and ENet 1.3).",
            "When ENet >= 1.3 is available, configures peer timeout on accept so temporary network stalls can recover without immediate disconnect.",
        ],
    )
    add(
        "src/multiplayer_session_sync.cc",
        ["Per-tick input synchronization and periodic world checksum verification/resync."],
        ["Called from session tick while a level is running."],
        [
            "Adds deterministic lockstep input exchange and divergence detection with soft resync.",
            "Extends drift recovery by requesting and applying host-authoritative world-grid snapshots when world checksums diverge.",
            "Sends READY/START with an additional `load_id` so readiness and start signals are scoped to the currently loading level.",
        ],
    )
    add(
        "src/multiplayer_session_runtime.cc",
        ["Runtime hooks bridging the session state machine to Enigma world/game state."],
        ["Called from `client.cc` and/or server hooks to control pauses/restarts/level transitions."],
        [
            "Integrates session transitions with existing engine-level control flow.",
            "Ensures post-`WorldInitLevel()` world state is compatible with multiplayer expectations (e.g. multi-ball redistribution for meditation levels).",
            "Adds an input-clock freeze/rebase hook so long stalls or pauses do not permanently increase the input lookahead/latency after resuming.",
        ],
    )
    add(
        "src/multiplayer_protocol.hh",
        ["Binary protocol definitions for gameplay session packets (inputs, sync, control messages)."],
        ["Used by host/client session code across all gameplay transports."],
        [
            "New protocol header for multiplayer gameplay networking.",
            "Extends LAN `LobbyStart` with optional `host_ips` candidates to improve direct-connect success on multi-homed hosts (VMs/VPNs).",
            "Adds host-authoritative level transitions via `NET_LOAD_LEVEL` (pack + normalized level id).",
            "Adds host-authoritative world-state snapshots (`NET_WORLD_STATE`) and client requests (`NET_WORLD_STATE_REQUEST`) for repairing world divergence.",
            "Adds host-to-client settings synchronization (`NET_DEBUG_OPTIONS`) plus `NET_PING`/`NET_PONG` for host-side connectivity probing.",
            "Scopes READY/START to the current session+level by carrying `session_id`, `epoch`, and `load_id` (prevents stale packets unblocking the wrong level).",
        ],
    )
    add(
        "src/multiplayer_rollback.hh",
        ["Rollback/replay helper used to make multiplayer recovery/prediction experiments reproducible and testable."],
        ["Used by session sync/runtime when rollback is enabled via debug options."],
        ["New optional module: stores recent snapshots and supports replay to reduce visible correction artifacts under packet loss/latency."],
    )
    add(
        "src/multiplayer_rollback.cc",
        ["Rollback/replay implementation."],
        ["Used by `src/multiplayer_session_sync.cc` and the multiplayer test driver when rollback is enabled."],
        ["New optional rollback/replay implementation built on the snapshot API in `src/multiplayer_sim_snapshot.*`."],
    )
    add(
        "src/multiplayer_sim_snapshot.hh",
        ["Snapshot/restore API for a deterministic subset of simulation state (actors/world/timers/etc.)."],
        ["Used by rollback/replay code paths and by integration tests."],
        ["New module that defines the snapshot surface needed for rollback/replay experiments."],
    )
    add(
        "src/multiplayer_sim_snapshot.cc",
        ["Snapshot/restore implementation."],
        ["Used by rollback/replay when capturing and restoring recent simulation ticks."],
        ["Implements snapshot capture/restore for actors, world grid state, timers, and pending secure actions."],
    )
    add(
        "src/multiplayer_test_driver.hh",
        ["Small TCP control interface used by integration tests to drive a running Enigma instance."],
        ["Enabled only when started with `--mp-test-role` and `--mp-test-connect`."],
        ["New developer-only test harness API to make multiplayer bugs reproducible and regression-testable."],
    )
    add(
        "src/multiplayer_test_driver.cc",
        ["Multiplayer test driver implementation (command parser + event emitter)."],
        ["Used by `tools/mp_test_env.py` to script host/client runs and collect state/logs."],
        ["Adds a deterministic automation surface (join/start/load/move/observe) without affecting normal gameplay."],
    )
    add(
        "src/multiplayer_state.hh",
        ["Small shared enums/structs for multiplayer state representation."],
        ["Used across session, transport, and UI to represent common state."],
        ["New shared state types for multiplayer (including small runtime hooks used by the client for pause/stall transitions)."],
    )
    add(
        "src/multiplayer_debug.cc",
        ["Debug logging helpers for multiplayer (`ENIGMA_MP_DEBUG`, etc)."],
        ["Used by multiplayer code to emit diagnostics without spamming release builds."],
        ["New debug plumbing for multiplayer."],
    )
    add(
        "src/multiplayer_enet_socket.cc",
        ["ENet socket helpers for LAN broadcast/discovery and UDP operations."],
        ["Used by LAN lobby discovery and UDP relay transport implementation."],
        ["New low-level socket helpers for LAN multiplayer and UDP transports."],
    )
    add(
        "src/multiplayer_tcp_socket.cc",
        ["TCP socket helpers used by the TCP relay transport."],
        ["Used by TCP relay connection and message framing."],
        ["New TCP socket helper module."],
    )
    add(
        "src/multiplayer_relay_codec.cc",
        ["Message framing/codec for relay protocols (UDP relay encapsulation, TCP framing)."],
        ["Used by relay transport implementations and servers."],
        ["New codec module for relay message envelopes."],
    )
    add(
        "src/multiplayer_lan_lobby.cc",
        ["LAN lobby discovery and peer list management (broadcast-based)."],
        ["Used by multiplayer lobby in LAN mode."],
        [
            "Enables LAN peer discovery and joining without a central server.",
            "Broadcast `LobbyStart` now includes best-effort local IPv4 candidates; clients try multiple addresses to cope with VMs and multi-NIC setups.",
            "Keeps lobby identity available for Internet-room operations even when LAN discovery is not actively running.",
        ],
    )
    add(
        "src/multiplayer_internet_lobby.cc",
        ["Internet lobby client (custom UDP protocol) used to manage rooms and discover session endpoints."],
        ["Used by multiplayer lobby in Internet mode."],
        [
            "Adds Internet room create/join/leave and peer discovery via lobby service.",
            "Tracks the active Internet room for lifecycle cleanup, propagates member display names in join/poll flows, and performs best-effort room leave on shutdown.",
        ],
    )
    add(
        "src/multiplayer_identity.cc",
        ["Peer identity generation and display names used in lobby lists."],
        ["Used by both LAN and Internet lobby modes to identify peers."],
        ["New identity helper for multiplayer."],
    )
    add(
        "src/multiplayer_globals.cc",
        ["Shared process-global multiplayer state that needs a stable lifetime across menus/levels."],
        ["Used by UI and runtime to coordinate lobby/session lifetimes."],
        ["New globals module, split out to clarify ownership and shutdown order."],
    )
    add(
        "src/multiplayer_internal.hh",
        ["Internal multiplayer declarations not exposed via `src/multiplayer.hh`."],
        ["Included across multiplayer implementation units."],
        ["New internal header created as part of module split (includes internal session state flags such as input clock freeze)."],
    )
    add(
        "src/multiplayer_util.cc",
        ["Misc multiplayer helpers (parsing, formatting, safe conversions)."],
        ["Used by UI and session code."],
        ["New utility module to keep core modules focused."],
    )
    add(
        "src/multiplayer_extra_players.hh",
        [
            "Support for playing levels with more players than the level was designed for (auto-placement and control redistribution).",
            "Includes helper hooks for rebalancing authored multi-ball levels (e.g. meditation) across session players.",
        ],
        ["Used when lobby filter allows 1-N / 2-N / ... levels and extra players join."],
        [
            "New feature: deterministic extra-player placement for non-optimized levels.",
            "New feature: redistribute authored steerable actors across players when the level provides enough balls (e.g. meditation pearls).",
        ],
    )
    add(
        "src/multiplayer_extra_players.cc",
        ["Implementation of extra-player placement logic."],
        ["Used by session start to compute additional spawn points."],
        [
            "Adds auto-placement with line-of-sight heuristics and safe tile selection.",
            "Adds redistribution for authored multi-ball levels so control can be split among players instead of always duplicating balls.",
            "Runs redistribution post-`WorldInitLevel()` to handle levels/compatibility code that overwrite `controllers` during initialization.",
        ],
    )
    add(
        "src/multiplayer_ball_assignment.hh",
        [
            "Helpers for mapping steerable actors (balls/marbles) to session players.",
            "Encapsulates deterministic distribution: add extra balls when players > balls, redistribute balls when balls > players (meditation levels).",
        ],
        ["Used by `src/multiplayer_extra_players.cc` to compute owner/controller assignments consistently across peers."],
        [
            "New multiplayer logic enabling non-optimized maps to be playable by larger groups.",
            "Introduces deterministic round-robin assignment so all instances derive the same per-player ball ownership.",
        ],
    )

    # Input injection for deterministic multiplayer control
    add(
        "src/input.hh",
        ["Input abstraction used by gameplay to read mouse/keyboard state."],
        ["Used by player control logic; in multiplayer it is fed from synchronized inputs."],
        ["Adds a small input API layer so multiplayer can inject per-player inputs deterministically."],
    )
    add(
        "src/input.cc",
        ["Input abstraction implementation."],
        ["Used by gameplay loop."],
        ["Implements injected-input plumbing required for lockstep multiplayer."],
    )

    # Runtime integration
    add(
        "src/client.cc",
        ["Main client state machine (menus, starting levels, in-game loop)."],
        ["Central runtime entry for menu transitions and gameplay state transitions."],
        [
            "Adds multiplayer-specific client states and integrates the multiplayer session tick into the game loop.",
            "Implements deferred level start (waiting-for-peers screen) without breaking level intro/transition text.",
            "Hardens gameplay mouse control across focus loss/regain: restores input grab/relative mode and re-hides the custom cursor to avoid accidental \"wizard cursor\" behavior.",
            "Shows an in-game wait dialog when lockstep stalls (missing inputs), with a countdown and a Leave action that aborts the session for everyone.",
            "Mitigates buffered-input side effects during stalls/pauses by flushing mouse motion and draining pending local input, then restoring/re-centering gameplay mouse control when resuming.",
        ],
    )
    add(
        "src/client_internal.hh",
        ["Client internal state declarations."],
        ["Included by `src/client.cc` and related client implementation."],
        [
            "Adds internal flags/state needed for multiplayer start deferral and abort/restart handling.",
            "Adds client helpers/state for restoring gameplay mouse control after focus and state transitions.",
            "Adds per-client timers/counters used to drive the lockstep stall wait dialog countdown.",
        ],
    )
    add(
        "src/server.cc",
        ["Local server simulation used by single-player and by host instances."],
        ["Hosts authoritative world tick and message distribution."],
        [
            "Integrates deterministic lockstep input consumption (`input::CanAdvanceTick()` gates world ticks; `apply_inputs_for_tick()` applies synchronized inputs).",
            "Hooks multiplayer lifecycle into level load: extra-actor spawning and post-`WorldInitLevel()` controller rebalance/placement for extra players.",
            "Makes restarts and next-level transitions host-authoritative (clients wait for host control messages instead of advancing locally).",
        ],
    )
    add(
        "src/server.hh",
        ["Server declarations."],
        ["Included by client/game code."],
        ["Declares new server-side hooks used for multiplayer."],
    )
    add(
        "src/player.cc",
        ["Player representation and per-player state."],
        ["Used by world/game logic."],
        ["Generalizes player handling to support >2 players for multiplayer sessions."],
    )
    add(
        "src/player.hh",
        ["Player declarations."],
        ["Included across gameplay code."],
        ["Declares API needed for multi-player counts and per-player attributes."],
    )
    add(
        "src/display.cc",
        ["Rendering/display control and overlays."],
        ["Used by gameplay and menus."],
        ["Adds multiplayer-specific overlays/screens (waiting/paused) without breaking existing transitions."],
    )
    add(
        "src/display.hh",
        ["Display declarations."],
        ["Included by client and UI code."],
        ["Declares multiplayer display helpers (waiting overlay)."],
    )
    add(
        "src/display_internal.hh",
        ["Display internals."],
        ["Included by display implementation."],
        ["Internal changes for multiplayer overlays."],
    )
    add(
        "src/MouseCursor.cc",
        ["Software cursor implementation used by Enigma's in-game cursor rendering path."],
        ["Used by input/display code to show/hide and move the custom cursor."],
        [
            "Hardens cursor visibility reference counting: clamps negative values in `show()`/`hide()` to avoid stuck-visible or out-of-sync cursor states after focus/input grab churn.",
        ],
    )
    add(
        "src/game.cc",
        ["Game logic glue between world, player, and UI."],
        ["Called during gameplay."],
        ["Adjusts game flow so multiplayer session control (restart/abort/pause) is deterministic."],
    )
    add(
        "src/Value.cc",
        ["Dynamic value type used across core engine (Lua bindings, XML parsing, scripting attributes)."],
        ["Used by essentially all scripting and attribute access paths."],
        [
            "Fixes locale-dependent numeric parsing (replaces `atof()` with classic-locale parsing).",
            "This prevents subtle cross-platform/cross-locale divergence that can surface as multiplayer desync.",
        ],
    )
    add(
        "src/world.cc",
        ["World simulation core."],
        ["Central physics and object state simulation."],
        ["Adds hooks needed for multiplayer resync/checksum and extra-player marbles."],
    )
    add(
        "src/world.hh",
        ["World declarations."],
        ["Included by gameplay code."],
        ["Declares multiplayer-related world accessors used for checksums/resync."],
    )
    add(
        "src/StateObject.hh",
        ["Base class for objects that expose an integer `state` attribute."],
        ["Used by many stones/items/floors and by the world grid state machinery."],
        ["Adds minimal snapshot/restore hooks so rollback/replay can restore internal state deterministically."],
    )
    add(
        "src/timer.hh",
        ["Global timer/alarms used by gameplay objects."],
        ["Used by stones/items/floors that schedule time-based callbacks."],
        ["Adds a snapshot/restore surface so rollback/replay can restore pending alarms deterministically."],
    )
    add(
        "src/timer.cc",
        ["Timer implementation."],
        ["Ticked from the core simulation loop."],
        ["Implements alarm snapshot/restore keyed by object id (avoids raw-pointer restoration hazards)."],
    )
    add(
        "src/video.cc",
        ["SDL window and rendering backend glue."],
        ["Creates the SDL window and screen surface used by the renderer."],
        ["Honors `SDL_VIDEO_WINDOW_POS` for window positioning (used by integration tests that place host/client windows side-by-side)."],
    )

    # Simulation helpers touched by multiplayer resync
    add(
        "src/others/Rubberband.cc",
        [
            "Rubberband 'other' object implementation (constraints/forces connecting actors).",
            "Meditation levels rely heavily on rubberbands to connect pearls.",
        ],
        ["Instantiated by levels that use rubberband constraints; updated by the physics/world tick."],
        [
            "Adds a multiplayer-only helper message so rubberband internal flags can be recomputed after a soft resync, reducing persistent post-resync drift.",
        ],
    )
    add(
        "src/world_internal.hh",
        ["World internal helpers."],
        ["Included by world implementation units."],
        ["Internal changes needed for multiplayer state capture/checksum."],
    )

    # Determinism / sync coverage
    add(
        "src/actors.hh",
        ["Actor base declarations (moving entities, including rotors)."],
        ["Included by actor implementations."],
        ["Adds small hooks to support deterministic multiplayer checksums/sync."],
    )
    add(
        "src/actors.cc",
        ["Actor base implementation."],
        ["Used by all moving entities; ticked every simulation step."],
        ["Adds optional render-only smoothing for multiplayer resync corrections (does not affect simulation determinism)."],
    )
    add(
        "src/actors/Balls.hh",
        ["Ball/marble actor declarations."],
        ["Used by most levels; core player-controlled actors."],
        ["Adds a small helper used after rollback snapshot restore to keep APPEARING balls playable."],
    )
    add(
        "src/actors/Balls.cc",
        ["Ball/marble actor implementation."],
        ["Core player actor behavior (movement, animations, interactions)."],
        ["Adds a restore helper to finalize APPEARING->NORMAL when rollback restored without animation progress."],
    )
    add(
        "src/actors/Rotors.cc",
        ["Rotor actor implementation."],
        ["Used by levels with moving rotors (common divergence vector)."],
        ["Ensures rotor state is included in multiplayer divergence detection and resync."],
    )
    add(
        "src/floors.cc",
        ["Floor behavior implementations."],
        ["Used by world simulation."],
        ["Minor adjustments for determinism/state capture required by multiplayer checksum."],
    )
    add(
        "src/floors/SimpleFloors.cc",
        ["Simple floor types implementation."],
        ["Used by many levels."],
        [
            "Keeps upstream/master single-player `fl_yinyang` semantics (floor is keyed to the current player), while making multiplayer `fl_yinyang` behave by ball affinity (color/owner parity) so cloned black/white marbles interact with yin/yang mechanics as expected.",
        ],
    )
    add(
        "src/lua.cc",
        ["Lua VM integration and engine bindings (levels, scripting API, object creation)."],
        ["Used when loading and running Lua-authored levels and scripted components."],
        [
            "Adds a narrow, opt-in trace (`ENIGMA_MP_TRACE_WORLDINIT`) to debug rare cross-peer divergences during actor creation in Lua-authored levels.",
        ],
    )

    # Level metadata/filtering
    add(
        "src/lev/Proxy.hh",
        ["Level proxy interface (load levels from packs, index, etc)."],
        ["Used by level selection and world loading."],
        ["Adds support for discovering multiplayer metadata (player count/network flag) for filtering."],
    )
    add(
        "src/lev/Proxy.cc",
        ["Level proxy implementation."],
        ["Used by level list building."],
        ["Implements multiplayer metadata extraction used by the multiplayer lobby filters."],
    )

    # Tests
    add(
        "tests/test_multiplayer_protocol.cc",
        ["Unit tests for multiplayer binary protocol packing/unpacking."],
        ["Run as part of the unit test suite."],
        ["New tests to prevent protocol drift and catch serialization regressions."],
    )
    add(
        "tests/test_input.cc",
        ["Unit tests for the injected input abstraction used by multiplayer."],
        ["Run as part of the unit test suite."],
        ["New tests to ensure deterministic input injection behavior."],
    )
    add(
        "tests/test_multiplayer_ball_assignment.cc",
        ["Unit tests for deterministic ball-to-player assignment (extra players + meditation redistribution)."],
        ["Run as part of the unit test suite."],
        ["New tests for the ball assignment rules used by non-optimized multiplayer play."],
    )

    # Integration tests (ad-hoc, framework-free)
    add(
        "tools/mp_test_env.py",
        ["Integration test runner that spawns two Enigma instances (host/client) and drives them via a script."],
        ["Developer tool used to reproduce multiplayer bugs deterministically and to validate fixes."],
        ["New test harness for multiplayer behavior regression checks (window positioning, scripted input, structured logs)."],
    )
    add(
        "tools/mp_test_scripts/README.md",
        ["Documentation for the multiplayer integration test scripts."],
        ["Read alongside `tools/mp_test_env.py` when adding new regression scripts."],
        ["New README describing the mp test script format and common patterns."],
    )
    add(
        "tools/mp_test_scripts/basic_join_and_move.txt",
        ["Basic integration test script (join session, start a level, apply motion)."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New scripted baseline test for verifying host/client wiring."],
    )
    add(
        "tools/mp_test_scripts/host_ball_moves_in_host_sim.txt",
        ["Integration test script that asserts host-controlled ball motion is reflected in host simulation."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New regression test for a previously observed 'host ball does not move' failure mode."],
    )
    add(
        "tools/mp_test_scripts/join_load_level_handshake_before_ready.txt",
        ["Integration test script that exercises join + load handshake ordering (READY/START scoping)."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New regression test to ensure stale packets cannot unblock the wrong level start."],
    )
    add(
        "tools/mp_test_scripts/peroxyd_open_sesame_client_ball_moves_in_host_sim_netsim.txt",
        ["Per.Oxyd 'Open Sesame' integration test (netsim) focusing on client ball motion reaching the host simulation."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New regression script for latency/loss scenarios affecting client-controlled movement."],
    )
    add(
        "tools/mp_test_scripts/peroxyd_open_sesame_client_drop_item_visible.txt",
        ["Per.Oxyd 'Open Sesame' integration test for inventory drop actions being replicated to clients."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New regression script for host-authoritative world actions (inventory drop visibility)."],
    )
    add(
        "tools/mp_test_scripts/peroxyd_open_sesame_fast_moves_with_netsim.txt",
        ["Per.Oxyd 'Open Sesame' integration test that applies fast movement under netsim."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New stress script for correction jitter/rollback behavior under simulated bad links."],
    )
    add(
        "tools/mp_test_scripts/peroxyd_open_sesame_fast_moves_with_netsim_resync_broadcast.txt",
        ["Per.Oxyd 'Open Sesame' integration test that enables periodic host resync broadcasts under netsim."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New stress script for the host broadcast correction path."],
    )
    add(
        "tools/mp_test_scripts/peroxyd_open_sesame_fast_moves_with_netsim_resync_broadcast_delay12.txt",
        ["Variant of the Per.Oxyd netsim stress script with a larger input delay."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New script to explore delay/stride tuning under poor connectivity."],
    )
    add(
        "tools/mp_test_scripts/peroxyd_open_sesame_fast_moves_with_netsim_resync_broadcast_predict_missing.txt",
        ["Variant of the Per.Oxyd netsim stress script with missing-mouse prediction enabled."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New script to evaluate predictive input fill vs correction artifacts."],
    )
    add(
        "tools/mp_test_scripts/peroxyd_open_sesame_host_ball_moves_with_rollback.txt",
        ["Per.Oxyd 'Open Sesame' integration test for host ball motion with rollback enabled."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New regression script to validate rollback mode doesn't freeze host movement."],
    )
    add(
        "tools/mp_test_scripts/remote_control_local_ball_basic.txt",
        ["Integration test for remote-control mode of the locally-controlled ball."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New regression script used while exploring host-authoritative control variants."],
    )
    add(
        "tools/mp_test_scripts/remote_control_local_ball_netsim_zickzack.txt",
        ["Remote-control mode integration test under netsim (zickzack/jitter reproduction)."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New stress script for remote-control + smoothing behavior under simulated latency."],
    )
    add(
        "tools/mp_test_scripts/tick_length_ms_negotiated.txt",
        ["Integration test for host-selected tick length negotiation."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New regression script ensuring both peers agree on tick length before start."],
    )
    add(
        "tools/mp_test_scripts/tick_length_ms_scales_legacy_ticks.txt",
        ["Integration test ensuring legacy tick-based debug parameters scale correctly with non-10ms tick lengths."],
        ["Executed by `tools/mp_test_env.py`."],
        ["New regression script for tick-length scaling of debug UX (broadcast strides, delays)."],
    )

    return n


def render_file_section(path: str, tag: str, narrative: FileNarrative, diff_html: str) -> str:
    sid = section_id_for_path(path)
    out: List[str] = []
    out.append(f'<div class="section" id="{sid}">')
    out.append(f'<h2><span class="tag">{html.escape(tag)}</span> <code>{html.escape(path)}</code></h2>')

    out.append('<div class="rationale">')
    out.append('<div class="subhead">Description</div>')
    out.append(html_list(narrative.description))
    out.append('<div class="subhead">Where it is used</div>')
    out.append(html_list(narrative.where_used))
    out.append('<div class="subhead">What changed vs upstream/master</div>')
    out.append(html_list(narrative.what_changed))
    out.append("</div>")

    out.append(diff_html)
    out.append("</div>")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="upstream/master", help="Base ref to diff against (default: upstream/master)")
    ap.add_argument("--out", default="doc/multiplayer_pr_review.html", help="Output HTML path")
    args = ap.parse_args()

    base = args.base
    out_path = args.out
    out_rel = out_path.lstrip("./")

    base_full = sh("git", "rev-parse", base).strip()
    base_short = base_full[:8]
    head_full = sh("git", "rev-parse", "HEAD").strip()
    head_short = head_full[:8]

    diffs = parse_git_diff(base)
    if not diffs:
        print("No diff found vs base %s" % base, file=sys.stderr)
        return 2

    # Avoid self-referential output: the review artifact should not embed its own diff.
    diffs.pop(out_rel, None)
    # Avoid generated/cache artifacts; focus the review on source changes.
    diffs = {path: raw for path, raw in diffs.items() if not should_exclude_review_path(path)}

    narratives = build_narratives()

    paths = sorted(diffs.keys())

    # Fail if a diff path lacks narrative.
    missing = [p for p in paths if p not in narratives]
    if missing:
        print("Missing narratives for %d file(s):" % len(missing), file=sys.stderr)
        for p in missing:
            print("  - %s" % p, file=sys.stderr)
        return 3

    # Detect stale narrative keys (present in mapping but no longer diffed).
    stale = sorted(p for p in (set(narratives.keys()) - set(paths)) if not should_exclude_review_path(p))
    if stale:
        # This is a developer hint; do not embed it in the generated HTML since it
        # confuses reviewers when the base ref changes over time.
        print(
            "Note: %d narrative entries are not used for this diff (base=%s)."
            % (len(stale), base),
            file=sys.stderr,
        )

    toc_lines: List[str] = []
    sections_html: List[str] = []

    for path in paths:
        raw = diffs[path]
        tag = infer_change_tag(raw)
        sid = section_id_for_path(path)
        toc_lines.append(
            f'<div><span class="tag">{html.escape(tag)}</span> <a href="#{sid}"><code>{html.escape(path)}</code></a></div>'
        )

    for path in paths:
        raw = diffs[path]
        tag = infer_change_tag(raw)
        diff_html = render_diff_html(raw)
        sections_html.append(render_file_section(path, tag, narratives[path], diff_html))

    generated = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    diff_hash = sha256_text(sh("git", "diff", "--no-color", base))

    html_out = f"""<!doctype html>
<html><head><meta charset="utf-8">
<title>Enigma Multiplayer PR Review Diff</title>
<style>
body {{ font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial; margin: 24px; color: #111; }}
code {{ font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace; }}
h1 {{ margin: 0 0 8px; }}
.small {{ color: #444; font-size: 13px; }}
.section {{ border-top: 1px solid #ddd; padding-top: 16px; margin-top: 16px; }}
.tag {{ display: inline-block; font-size: 12px; padding: 2px 6px; border-radius: 6px; background: #f2f2f2; color: #333; margin-right: 8px; }}
.toc a {{ text-decoration: none; }}
.warn {{ background: #fff7e6; border: 1px solid #ffd27a; padding: 10px; border-radius: 8px; }}
.rationale {{ margin: 8px 0 10px; color: #222; max-width: 1100px; }}
.rationale ul {{ margin: 6px 0 10px; padding-left: 20px; }}
.subhead {{ font-weight: 700; margin-top: 10px; }}
.empty {{ color: #666; font-style: italic; }}
.diff {{ font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace; font-size: 12px; line-height: 1.35; border: 1px solid #ddd; border-radius: 10px; overflow: hidden; }}
.line {{ display: grid; grid-template-columns: 56px 56px 1fr; }}
.line .ln {{ padding: 2px 8px; color: #666; background: #fafafa; border-right: 1px solid #eee; text-align: right; user-select: none; }}
.line .code {{ padding: 2px 10px; white-space: pre; overflow-x: auto; }}
.line.meta {{ grid-template-columns: 1fr; }}
.line.meta .code {{ background: #f8f8f8; color: #444; border-bottom: 1px solid #eee; }}
.line.hunkhdr {{ grid-template-columns: 1fr; }}
.line.hunkhdr .code {{ background: #eef2ff; color: #1f2a63; border-top: 1px solid #dde3ff; border-bottom: 1px solid #dde3ff; }}
.line.ctx .code {{ background: #fff; }}
.line.add .code {{ background: #e6ffed; }}
.line.del .code {{ background: #ffeef0; }}
.line.add .ln {{ background: #f0fff4; }}
.line.del .ln {{ background: #fff5f7; }}
</style>
</head><body>
<h1>Enigma Multiplayer PR Review Diff</h1>
<div class="small">Generated {html.escape(generated)}. Base: <code>{html.escape(base)}</code> ({html.escape(base_short)}) &rarr; Working tree (HEAD {html.escape(head_short)}). Diff hash: <code>{html.escape(diff_hash[:12])}</code>.</div>
<div class="warn" style="margin-top:12px"><b>How to use:</b> Grouped by path. Each file begins with a narrative (Description, Where used, What changed vs upstream) followed by a red/green diff. Per-hunk notes are intentionally not embedded here; long-lived explanations are kept as code comments instead.</div>

<div class="section"><h2>Extension proposals (not applied)</h2>
<div class="rationale">
<div><b>Scope note:</b> The multiplayer branch already includes substantial restructuring (transport facade, explicit session state, centralized config, UI split files) to keep review and maintenance tractable. The items below are intentionally deferred because they add tooling or cross-language coordination not required for the initial multiplayer PR.</div>
<div style="margin-top:10px"><b>4) In-process integration tests with a fake transport</b></div>
<ul>
<li><b>What it is:</b> A deterministic harness that instantiates a host and N clients in-process and drives the multiplayer session with a fake transport that can simulate packet loss/reordering/latency without real sockets.</li>
<li><b>Why it helps:</b> Multiplayer bugs are often timing-sensitive. A fake transport would allow CI to cover join/start/restart/resync/peer-leave reliably and prevent regressions that are otherwise hard to reproduce.</li>
<li><b>Why it is deferred:</b> It adds a non-trivial test framework surface (fake transport, scripted ticking, fixtures) that maintainers may want to evaluate after the core feature set lands.</li>
</ul>
<div style="margin-top:10px"><b>5) Single source of truth for Internet lobby protocol schema</b></div>
<ul>
<li><b>What it is:</b> Share an explicit schema between the C++ client and <code>tools/internet_lobby_server.py</code> so request/response formats cannot drift silently (today: a custom binary UDP lobby protocol implemented in both C++ and Python).</li>
<li><b>Why it helps:</b> Internet mode spans two languages and a separately deployed service. A shared schema reduces compatibility breakage and simplifies future API extensions (capability negotiation, versioned endpoints).</li>
<li><b>Why it is deferred:</b> Schema tooling/codegen increases build and deployment complexity. The initial PR keeps the server implementation intentionally minimal and dependency-light.</li>
</ul>
</div></div>

<div class="section"><h2>Table of contents</h2><div class="toc">
{chr(10).join(toc_lines)}
</div></div>

{chr(10).join(sections_html)}
</body></html>
"""

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(html_out)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
