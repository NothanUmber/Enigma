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
from typing import Dict, List, Optional, Tuple


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


def _spawn_enigma(
    enigma_bin: str,
    role: str,
    connect_host: str,
    pref_dir: str,
    extra_args: List[str],
    log_path: str,
) -> Tuple[subprocess.Popen, object]:
    argv = [
        enigma_bin,
        "--window",
        "--nograb",
        "--nosound",
        "--nomusic",
        "--pref",
        pref_dir,
        "--mp-test-role",
        role,
        "--mp-test-connect",
        connect_host,
    ] + extra_args
    logf = open(log_path, "wb")
    return subprocess.Popen(argv, stdout=logf, stderr=subprocess.STDOUT), logf


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
        "--workdir",
        default="",
        help="Directory for prefs/logs (default: create a temp dir and keep it)",
    )
    ns = ap.parse_args(argv)

    if not (os.path.isfile(ns.enigma) and os.access(ns.enigma, os.X_OK)):
        raise SystemExit(f"enigma binary not found/executable: {ns.enigma}")

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
        ph, lfh = _spawn_enigma(ns.enigma, "host", connect, pref_host, ns.extra_arg, log_host)
        pc, lfc = _spawn_enigma(ns.enigma, "client", connect, pref_client, ns.extra_arg, log_client)
        procs.extend([ph, pc])
        logs.extend([lfh, lfc])

        ctrl.wait_for_roles(timeout_s=ns.timeout_ms / 1000.0)
        _run_script(ctrl, ns.script)
        return 0
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
