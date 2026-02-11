#!/usr/bin/env python3
#
# Multiplayer test environment runner:
# - starts a local TCP controller
# - spawns two Enigma instances (host/client) in --mp-test-* mode
# - drives them via a simple script and collects EVT/OK/ERR frames
#
# Protocol framing matches src/multiplayer_tcp_socket.cc: 4-byte little-endian length prefix.

from __future__ import annotations

import argparse
import os
import selectors
import shlex
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple


def _now_ms() -> int:
    return int(time.time() * 1000)


def _frame_encode(payload: str) -> bytes:
    b = payload.encode("utf-8", errors="strict")
    if len(b) == 0 or len(b) > (1 << 20):
        raise ValueError(f"invalid frame size: {len(b)}")
    return struct.pack("<I", len(b)) + b


def _frame_try_decode(buf: bytearray) -> Optional[str]:
    if len(buf) < 4:
        return None
    (n,) = struct.unpack("<I", bytes(buf[:4]))
    if n == 0 or n > (1 << 20):
        raise ValueError(f"invalid frame length prefix: {n}")
    if len(buf) < 4 + n:
        return None
    payload = bytes(buf[4 : 4 + n])
    del buf[: 4 + n]
    return payload.decode("utf-8", errors="replace")


def _parse_kv(tokens: List[str]) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for t in tokens:
        if "=" not in t:
            continue
        k, v = t.split("=", 1)
        if k:
            out[k] = v
    return out


def _try_parse_float(s: str) -> Optional[float]:
    try:
        return float(s)
    except Exception:
        return None


def _try_parse_int(s: str) -> Optional[int]:
    try:
        return int(s, 10)
    except Exception:
        return None


def _parse_evt_line(line: str) -> Optional[Dict[str, str]]:
    if not line.startswith("EVT "):
        return None
    toks = line.split()
    kv = _parse_kv(toks[1:])
    if "name" not in kv:
        return None
    return kv


def _parse_state_evt(line: str) -> Optional[Dict[str, Any]]:
    kv = _parse_evt_line(line)
    if not kv or kv.get("name") != "STATE":
        return None
    out: Dict[str, Any] = {"name": "STATE"}
    for k, v in kv.items():
        if k == "name":
            continue
        # Try int, then float, else string.
        iv = _try_parse_int(v)
        if iv is not None:
            out[k] = iv
            continue
        fv = _try_parse_float(v)
        if fv is not None:
            out[k] = fv
            continue
        out[k] = v
    return out


@dataclass
class PeerConn:
    sock: socket.socket
    addr: Tuple[str, int]
    rx: bytearray = field(default_factory=bytearray)
    role: Optional[str] = None  # "host"/"client"
    pid: Optional[str] = None

    def send_line(self, line: str) -> None:
        self.sock.sendall(_frame_encode(line))


class Controller:
    def __init__(self, expected: int) -> None:
        self.expected = expected
        self.sel = selectors.DefaultSelector()
        self.listener: Optional[socket.socket] = None
        self.conns: List[PeerConn] = []
        self.by_role: Dict[str, PeerConn] = {}
        self.events: List[Tuple[int, str, str]] = []  # (ts_ms, role, line)
        self.last_state: Dict[str, Dict[str, Any]] = {}

    def listen(self, host: str, port: int) -> int:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((host, port))
        s.listen()
        s.setblocking(False)
        self.listener = s
        self.sel.register(s, selectors.EVENT_READ, data=("listen", None))
        return s.getsockname()[1]

    def close(self) -> None:
        for c in list(self.conns):
            try:
                self.sel.unregister(c.sock)
            except Exception:
                pass
            try:
                c.sock.close()
            except Exception:
                pass
        self.conns.clear()
        self.by_role.clear()
        if self.listener:
            try:
                self.sel.unregister(self.listener)
            except Exception:
                pass
            try:
                self.listener.close()
            except Exception:
                pass
            self.listener = None

    def _accept_ready(self) -> None:
        assert self.listener is not None
        while True:
            try:
                conn, addr = self.listener.accept()
            except BlockingIOError:
                return
            conn.setblocking(False)
            pc = PeerConn(sock=conn, addr=(addr[0], addr[1]))
            self.conns.append(pc)
            self.sel.register(conn, selectors.EVENT_READ, data=("conn", pc))

    def pump(self, timeout_s: float) -> None:
        for key, _mask in self.sel.select(timeout=timeout_s):
            kind, pc = key.data
            if kind == "listen":
                self._accept_ready()
                continue
            assert kind == "conn" and pc is not None
            try:
                data = pc.sock.recv(4096)
            except BlockingIOError:
                continue
            if not data:
                # peer closed
                try:
                    self.sel.unregister(pc.sock)
                except Exception:
                    pass
                try:
                    pc.sock.close()
                except Exception:
                    pass
                if pc in self.conns:
                    self.conns.remove(pc)
                if pc.role and self.by_role.get(pc.role) is pc:
                    del self.by_role[pc.role]
                continue
            pc.rx.extend(data)
            while True:
                line = _frame_try_decode(pc.rx)
                if line is None:
                    break
                role = pc.role or "unknown"
                ts = _now_ms()
                self.events.append((ts, role, line))
                print(f"[{ts}] {role}: {line}", flush=True)

                # Auto-learn role from HELLO.
                if pc.role is None and line.startswith("EVT "):
                    toks = line.split()
                    kv = _parse_kv(toks[1:])
                    if kv.get("name") == "HELLO":
                        r = kv.get("role")
                        if r:
                            pc.role = r
                            pc.pid = kv.get("pid")
                            self.by_role[r] = pc
                            role = r

                st = _parse_state_evt(line)
                if st is not None and role != "unknown":
                    self.last_state[role] = st

    def wait_for_roles(self, timeout_s: float) -> None:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            self.pump(timeout_s=0.05)
            if "host" in self.by_role and "client" in self.by_role:
                return
        raise RuntimeError("timeout waiting for host+client HELLO")

    def send(self, role: str, cmdline: str) -> None:
        pc = self.by_role.get(role)
        if not pc:
            raise RuntimeError(f"no peer with role={role}")
        pc.send_line(cmdline)

    def send_both(self, cmdline: str) -> None:
        self.send("host", cmdline)
        self.send("client", cmdline)

    def wait_line_contains(self, role: str, needle: str, timeout_s: float) -> str:
        deadline = time.time() + timeout_s
        idx = 0
        while time.time() < deadline:
            # Search any newly appended events first.
            while idx < len(self.events):
                _ts, r, line = self.events[idx]
                idx += 1
                if role != "any" and r != role:
                    continue
                if needle in line:
                    return line
            self.pump(timeout_s=0.05)
        raise RuntimeError(f"timeout waiting for role={role} contains={needle!r}")


def _tail_text(path: str, max_bytes: int = 16 * 1024) -> str:
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            start = max(0, size - max_bytes)
            f.seek(start, os.SEEK_SET)
            b = f.read()
        return b.decode("utf-8", errors="replace")
    except Exception as e:
        return f"<unable to read {path}: {e}>"


def _find_default_enigma_bin() -> str:
    # Prefer repo-local build output.
    candidates = [
        os.path.join(os.getcwd(), "src", "enigma"),
        os.path.join(os.getcwd(), "enigma"),
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return candidates[0]

def _find_repo_root_from_script() -> str:
    # tools/mp_test_env.py -> repo root
    return os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def _spawn_enigma(
    enigma_bin: str,
    role: str,
    connect_host: str,
    pref_dir: str,
    data_dir: str,
    extra_args: List[str],
    log_path: str,
    window_pos: Optional[Tuple[int, int]],
) -> Tuple[subprocess.Popen, object]:
    env = dict(os.environ)
    if window_pos is not None:
        env["SDL_VIDEO_CENTERED"] = "0"
        env["SDL_VIDEO_WINDOW_POS"] = f"{window_pos[0]},{window_pos[1]}"
    argv = [
        enigma_bin,
        "--log",
        "--window",
        "--nograb",
        "--nosound",
        "--nomusic",
        "--pref",
        pref_dir,
        "--data",
        data_dir,
        "--mp-test-role",
        role,
        "--mp-test-connect",
        connect_host,
    ] + extra_args
    logf = open(log_path, "wb")
    return subprocess.Popen(argv, stdout=logf, stderr=subprocess.STDOUT, env=env), logf


def _run_script(ctrl: Controller, script_path: str) -> None:
    with open(script_path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            toks = shlex.split(line, comments=False, posix=True)
            if not toks:
                continue
            op = toks[0].lower()

            if op in ("host", "client"):
                cmdline = " ".join(toks[1:])
                if not cmdline:
                    raise RuntimeError(f"{script_path}:{lineno}: missing command after {op}")
                ctrl.send(op, cmdline)
                continue

            if op == "both":
                cmdline = " ".join(toks[1:])
                if not cmdline:
                    raise RuntimeError(f"{script_path}:{lineno}: missing command after both")
                ctrl.send_both(cmdline)
                continue

            if op == "sleep":
                if len(toks) == 2 and toks[1].isdigit():
                    ms = int(toks[1])
                else:
                    kv = _parse_kv(toks[1:])
                    ms = int(kv.get("ms", "0"))
                time.sleep(ms / 1000.0)
                ctrl.pump(timeout_s=0.01)
                continue

            if op == "wait":
                kv = _parse_kv(toks[1:])
                role = kv.get("role", "any").lower()
                needle = kv.get("contains")
                timeout_ms = int(kv.get("timeout_ms", "5000"))
                if not needle:
                    raise RuntimeError(f"{script_path}:{lineno}: wait requires contains=...")
                ctrl.wait_line_contains(role=role, needle=needle, timeout_s=timeout_ms / 1000.0)
                continue

            if op == "wait_state_change":
                kv = _parse_kv(toks[1:])
                role = kv.get("role", "").lower()
                field = kv.get("field", "")
                min_abs_delta = float(kv.get("min_abs_delta", "0"))
                timeout_ms = int(kv.get("timeout_ms", "5000"))
                if role not in ("host", "client"):
                    raise RuntimeError(f"{script_path}:{lineno}: wait_state_change role=host|client required")
                if not field:
                    raise RuntimeError(f"{script_path}:{lineno}: wait_state_change field=... required")
                if min_abs_delta <= 0:
                    raise RuntimeError(f"{script_path}:{lineno}: wait_state_change min_abs_delta>0 required")

                # Ensure we have a baseline state.
                deadline = time.time() + (timeout_ms / 1000.0)
                while time.time() < deadline and role not in ctrl.last_state:
                    ctrl.pump(timeout_s=0.05)
                baseline = ctrl.last_state.get(role)
                if not baseline or field not in baseline:
                    raise RuntimeError(f"{script_path}:{lineno}: no baseline STATE for {role} (missing {field})")
                base_val = baseline[field]
                if not isinstance(base_val, (int, float)):
                    raise RuntimeError(f"{script_path}:{lineno}: field {field} not numeric in baseline")

                while time.time() < deadline:
                    ctrl.pump(timeout_s=0.05)
                    cur = ctrl.last_state.get(role)
                    if not cur:
                        continue
                    cur_val = cur.get(field)
                    if not isinstance(cur_val, (int, float)):
                        continue
                    if abs(float(cur_val) - float(base_val)) >= min_abs_delta:
                        break
                else:
                    raise RuntimeError(
                        f"{script_path}:{lineno}: timeout waiting for {role} {field} change >= {min_abs_delta}"
                    )
                continue

            raise RuntimeError(f"{script_path}:{lineno}: unknown op {op!r}")


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--enigma", default=_find_default_enigma_bin(), help="Path to enigma binary")
    ap.add_argument("--script", required=True, help="Script file to execute")
    ap.add_argument("--host", default="127.0.0.1", help="Controller listen host")
    ap.add_argument("--port", type=int, default=0, help="Controller listen port (0=auto)")
    ap.add_argument("--timeout-ms", type=int, default=15000, help="HELLO wait timeout")
    ap.add_argument("--extra-arg", action="append", default=[], help="Extra arg passed to both instances")
    ap.add_argument(
        "--data-dir",
        default="",
        help="Enigma data directory (default: auto-detect repo ./data; use 'none' to skip)",
    )
    ap.add_argument(
        "--side-by-side",
        action="store_true",
        help="Position host/client windows side-by-side (best-effort via SDL env vars)",
    )
    ap.add_argument("--window-x0", type=int, default=40, help="Left window X when --side-by-side")
    ap.add_argument("--window-y0", type=int, default=40, help="Window Y when --side-by-side")
    ap.add_argument("--window-dx", type=int, default=820, help="Delta X between windows when --side-by-side")
    ap.add_argument(
        "--workdir",
        default="",
        help="Directory for prefs/logs (default: create a temp dir and keep it)",
    )
    ns = ap.parse_args(argv)

    if not (os.path.isfile(ns.enigma) and os.access(ns.enigma, os.X_OK)):
        raise SystemExit(f"enigma binary not found/executable: {ns.enigma}")

    if ns.data_dir.lower() == "none":
        data_dir = ""
    else:
        data_dir = ns.data_dir or os.path.join(_find_repo_root_from_script(), "data")
    if data_dir and not os.path.isdir(data_dir):
        raise SystemExit(f"data dir not found: {data_dir} (use --data-dir to override)")

    ctrl = Controller(expected=2)
    port = ctrl.listen(ns.host, ns.port)
    connect = f"{ns.host}:{port}"

    # Separate preference roots so two processes don't fight over config files.
    tmp_root = ns.workdir or tempfile.mkdtemp(prefix="enigma-mptest-")
    os.makedirs(tmp_root, exist_ok=True)
    pref_host = os.path.join(tmp_root, "pref-host")
    pref_client = os.path.join(tmp_root, "pref-client")
    os.makedirs(pref_host, exist_ok=True)
    os.makedirs(pref_client, exist_ok=True)
    log_host = os.path.join(tmp_root, "host.log")
    log_client = os.path.join(tmp_root, "client.log")

    procs: List[subprocess.Popen] = []
    logs: List[object] = []
    try:
        print(f"mp test workdir: {tmp_root}", flush=True)
        host_pos = None
        client_pos = None
        if ns.side_by_side:
            host_pos = (ns.window_x0, ns.window_y0)
            client_pos = (ns.window_x0 + ns.window_dx, ns.window_y0)
        ph, lfh = _spawn_enigma(
            ns.enigma, "host", connect, pref_host, data_dir, ns.extra_arg, log_host, host_pos
        )
        pc, lfc = _spawn_enigma(
            ns.enigma, "client", connect, pref_client, data_dir, ns.extra_arg, log_client, client_pos
        )
        procs.extend([ph, pc])
        logs.extend([lfh, lfc])

        hello_deadline = time.time() + (ns.timeout_ms / 1000.0)
        while time.time() < hello_deadline:
            ctrl.pump(timeout_s=0.05)
            if "host" in ctrl.by_role and "client" in ctrl.by_role:
                break
            if ph.poll() is not None or pc.poll() is not None:
                raise RuntimeError("child process exited before sending HELLO")
        if "host" not in ctrl.by_role or "client" not in ctrl.by_role:
            raise RuntimeError("timeout waiting for host+client HELLO")

        _run_script(ctrl, ns.script)
        return 0
    except Exception as e:
        # Helpful diagnostics: show logs if something prevented startup/HELLO.
        try:
            print(f"mp test error: {e}", flush=True)
            if os.path.exists(log_host):
                print("----- host.log (tail) -----", flush=True)
                print(_tail_text(log_host), flush=True)
            if os.path.exists(log_client):
                print("----- client.log (tail) -----", flush=True)
                print(_tail_text(log_client), flush=True)
        except Exception:
            pass
        raise
    finally:
        # Best-effort: request clean shutdown via protocol, then kill.
        try:
            if "host" in ctrl.by_role:
                ctrl.send("host", "QUIT")
            if "client" in ctrl.by_role:
                ctrl.send("client", "QUIT")
            end = time.time() + 1.0
            while time.time() < end:
                ctrl.pump(timeout_s=0.05)
        except Exception:
            pass

        for p in procs:
            if p.poll() is None:
                try:
                    p.terminate()
                except Exception:
                    pass
        for p in procs:
            try:
                p.wait(timeout=2.0)
            except Exception:
                pass
        for p in procs:
            if p.poll() is None:
                try:
                    p.kill()
                except Exception:
                    pass

        for lf in logs:
            try:
                lf.close()
            except Exception:
                pass

        ctrl.close()


if __name__ == "__main__":
    # Don't crash on Ctrl+C while leaving children running.
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    raise SystemExit(main(sys.argv[1:]))
