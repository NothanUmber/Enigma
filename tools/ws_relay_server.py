#!/usr/bin/env python3

import argparse
import base64
import hashlib
import selectors
import socket
import struct
from dataclasses import dataclass, field
from typing import Dict, Optional


RELAY_MAGIC = 0x4C524E45
RELAY_VERSION = 1

RELAY_HELLO_HOST = 1
RELAY_HELLO_CLIENT = 2
RELAY_CLIENT_CONNECT = 3
RELAY_CLIENT_DISCONNECT = 4
RELAY_SEND = 5
RELAY_DATA = 6

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


@dataclass
class Conn:
    sock: socket.socket
    addr: tuple
    handshake_done: bool = False
    http_rx: bytearray = field(default_factory=bytearray)
    ws_rx: bytearray = field(default_factory=bytearray)
    role: str = "unknown"
    session_id: int = 0
    client_id: int = 0
    msg_opcode: Optional[int] = None
    msg_buf: bytearray = field(default_factory=bytearray)


@dataclass
class Session:
    host: Optional[Conn] = None
    next_client_id: int = 1
    clients: Dict[int, Conn] = field(default_factory=dict)


def write_u32(value: int) -> bytes:
    return struct.pack("<I", value)


def parse_relay_header(data: bytes):
    if len(data) < 14:
        return None
    magic, version, rtype, session_id, client_id = struct.unpack("<IBBII", data[:14])
    if magic != RELAY_MAGIC or version != RELAY_VERSION:
        return None
    return rtype, session_id, client_id, data[14:]


def make_relay_packet(rtype: int, session_id: int, client_id: int, payload: bytes) -> bytes:
    return (
        write_u32(RELAY_MAGIC)
        + bytes([RELAY_VERSION, rtype])
        + write_u32(session_id)
        + write_u32(client_id)
        + payload
    )


def websocket_accept_value(key: str) -> str:
    digest = hashlib.sha1((key + WS_GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def send_ws_frame(sock: socket.socket, opcode: int, payload: bytes = b"", fin: bool = True):
    first = (0x80 if fin else 0x00) | (opcode & 0x0F)
    length = len(payload)
    if length < 126:
        header = bytes([first, length])
    elif length < (1 << 16):
        header = bytes([first, 126]) + struct.pack("!H", length)
    else:
        header = bytes([first, 127]) + struct.pack("!Q", length)
    sock.sendall(header + payload)


def try_extract_ws_frame(buf: bytearray):
    if len(buf) < 2:
        return None
    b0 = buf[0]
    b1 = buf[1]
    fin = (b0 & 0x80) != 0
    opcode = b0 & 0x0F
    masked = (b1 & 0x80) != 0
    length = b1 & 0x7F
    offset = 2
    if length == 126:
        if len(buf) < offset + 2:
            return None
        length = struct.unpack("!H", bytes(buf[offset:offset + 2]))[0]
        offset += 2
    elif length == 127:
        if len(buf) < offset + 8:
            return None
        length = struct.unpack("!Q", bytes(buf[offset:offset + 8]))[0]
        offset += 8
    mask = b""
    if masked:
        if len(buf) < offset + 4:
            return None
        mask = bytes(buf[offset:offset + 4])
        offset += 4
    if len(buf) < offset + length:
        return None
    payload = bytes(buf[offset:offset + length])
    del buf[:offset + length]
    if masked:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return fin, opcode, payload


def close_conn(
    selector: selectors.BaseSelector,
    conns: Dict[socket.socket, Conn],
    sessions: Dict[int, Session],
    conn: Conn,
):
    try:
        selector.unregister(conn.sock)
    except Exception:
        pass
    conns.pop(conn.sock, None)
    try:
        conn.sock.close()
    except Exception:
        pass

    if conn.role == "client":
        session = sessions.get(conn.session_id)
        if not session:
            return
        session.clients.pop(conn.client_id, None)
        if session.host is not None:
            payload = make_relay_packet(RELAY_CLIENT_DISCONNECT, conn.session_id, conn.client_id, b"")
            try:
                send_ws_frame(session.host.sock, 0x2, payload)
            except Exception:
                close_conn(selector, conns, sessions, session.host)
        if session.host is None and not session.clients:
            sessions.pop(conn.session_id, None)
        return

    if conn.role == "host":
        session = sessions.get(conn.session_id)
        if not session or session.host is not conn:
            return
        for client in list(session.clients.values()):
            close_conn(selector, conns, sessions, client)
        sessions.pop(conn.session_id, None)


def process_relay_message(
    selector: selectors.BaseSelector,
    conns: Dict[socket.socket, Conn],
    sessions: Dict[int, Session],
    conn: Conn,
    payload: bytes,
):
    if conn.role == "unknown":
        header = parse_relay_header(payload)
        if not header:
            close_conn(selector, conns, sessions, conn)
            return
        rtype, session_id, _client_id, _inner = header
        session = sessions.setdefault(session_id, Session())
        if rtype == RELAY_HELLO_HOST:
            conn.role = "host"
            conn.session_id = session_id
            session.host = conn
            for client_id in session.clients:
                pkt = make_relay_packet(RELAY_CLIENT_CONNECT, session_id, client_id, b"")
                send_ws_frame(conn.sock, 0x2, pkt)
            return
        if rtype == RELAY_HELLO_CLIENT:
            conn.role = "client"
            conn.session_id = session_id
            conn.client_id = session.next_client_id
            session.next_client_id += 1
            session.clients[conn.client_id] = conn
            if session.host is not None:
                pkt = make_relay_packet(RELAY_CLIENT_CONNECT, session_id, conn.client_id, b"")
                send_ws_frame(session.host.sock, 0x2, pkt)
            return
        close_conn(selector, conns, sessions, conn)
        return

    session = sessions.get(conn.session_id)
    if conn.role == "client":
        if session is None or session.host is None:
            return
        pkt = make_relay_packet(RELAY_DATA, conn.session_id, conn.client_id, payload)
        try:
            send_ws_frame(session.host.sock, 0x2, pkt)
        except Exception:
            close_conn(selector, conns, sessions, session.host)
        return

    if conn.role == "host":
        header = parse_relay_header(payload)
        if not header:
            return
        rtype, _session_id, client_id, inner = header
        if rtype != RELAY_SEND or session is None:
            return
        client = session.clients.get(client_id)
        if client is None:
            return
        try:
            send_ws_frame(client.sock, 0x2, inner)
        except Exception:
            close_conn(selector, conns, sessions, client)


def process_ws_frame(
    selector: selectors.BaseSelector,
    conns: Dict[socket.socket, Conn],
    sessions: Dict[int, Session],
    conn: Conn,
    fin: bool,
    opcode: int,
    payload: bytes,
):
    if opcode == 0x8:
        close_conn(selector, conns, sessions, conn)
        return
    if opcode == 0x9:
        send_ws_frame(conn.sock, 0xA, payload)
        return
    if opcode == 0xA:
        return

    if opcode in (0x1, 0x2):
        conn.msg_opcode = opcode
        conn.msg_buf = bytearray(payload)
    elif opcode == 0x0 and conn.msg_opcode is not None:
        conn.msg_buf.extend(payload)
    else:
        close_conn(selector, conns, sessions, conn)
        return

    if not fin:
        return
    if conn.msg_opcode != 0x2:
        close_conn(selector, conns, sessions, conn)
        return
    process_relay_message(selector, conns, sessions, conn, bytes(conn.msg_buf))
    conn.msg_opcode = None
    conn.msg_buf.clear()


def perform_handshake(conn: Conn):
    data = conn.http_rx
    marker = data.find(b"\r\n\r\n")
    if marker < 0:
        return False
    request = data[:marker + 4].decode("iso-8859-1", errors="replace")
    request_line = request.split("\r\n", 1)[0]
    print(f"ws relay: handshake from {conn.addr} request={request_line}", flush=True)
    del data[:marker + 4]
    key = None
    for line in request.split("\r\n"):
        if ":" not in line:
            continue
        name, value = line.split(":", 1)
        if name.strip().lower() == "sec-websocket-key":
            key = value.strip()
            break
    if not key:
        raise ValueError("missing websocket key")
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {websocket_accept_value(key)}\r\n"
        "\r\n"
    )
    conn.sock.sendall(response.encode("ascii"))
    conn.handshake_done = True
    if data:
        conn.ws_rx.extend(data)
        data.clear()
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="127.0.0.1")
    ap.add_argument("port", nargs="?", type=int, default=24681)
    ns = ap.parse_args()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((ns.host, ns.port))
    server.listen(64)
    print(f"WebSocket relay server listening on {ns.host}:{ns.port}", flush=True)

    selector = selectors.DefaultSelector()
    selector.register(server, selectors.EVENT_READ)

    conns: Dict[socket.socket, Conn] = {}
    sessions: Dict[int, Session] = {}

    while True:
        for key, _mask in selector.select(timeout=0.05):
            if key.fileobj is server:
                sock, addr = server.accept()
                print(f"ws relay: accepted {addr}", flush=True)
                conn = Conn(sock=sock, addr=addr)
                conns[sock] = conn
                selector.register(sock, selectors.EVENT_READ)
                continue

            sock = key.fileobj
            conn = conns.get(sock)
            if conn is None:
                continue
            try:
                chunk = sock.recv(4096)
            except OSError:
                close_conn(selector, conns, sessions, conn)
                continue
            if not chunk:
                close_conn(selector, conns, sessions, conn)
                continue

            if not conn.handshake_done:
                conn.http_rx.extend(chunk)
                try:
                    if not perform_handshake(conn):
                        continue
                except Exception:
                    close_conn(selector, conns, sessions, conn)
                    continue
            else:
                conn.ws_rx.extend(chunk)

            while conn.handshake_done:
                frame = try_extract_ws_frame(conn.ws_rx)
                if frame is None:
                    break
                fin, opcode, payload = frame
                process_ws_frame(selector, conns, sessions, conn, fin, opcode, payload)
                if conn.sock not in conns:
                    break


if __name__ == "__main__":
    main()
