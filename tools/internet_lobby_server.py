#!/usr/bin/env python3
import argparse
import random
import socket
import struct
import time

MAGIC = 0x52494E45  # "ENIR" little-endian
VERSION = 1

INET_CREATE = 1
INET_JOIN = 2
INET_LEAVE = 3
INET_START = 4
INET_POLL = 5
INET_CREATE_OK = 101
INET_JOIN_OK = 102
INET_POLL_OK = 103
INET_START_OK = 104
INET_ERROR = 105


def read_u8(data, offset):
    if offset + 1 > len(data):
        raise ValueError("short buffer")
    return data[offset], offset + 1


def read_u16(data, offset):
    if offset + 2 > len(data):
        raise ValueError("short buffer")
    return struct.unpack_from("<H", data, offset)[0], offset + 2


def read_u32(data, offset):
    if offset + 4 > len(data):
        raise ValueError("short buffer")
    return struct.unpack_from("<I", data, offset)[0], offset + 4


def read_str(data, offset):
    length, offset = read_u16(data, offset)
    if offset + length > len(data):
        raise ValueError("short buffer")
    value = data[offset:offset + length].decode("utf-8", "ignore")
    return value, offset + length


def write_u8(value):
    return struct.pack("<B", value)


def write_u16(value):
    return struct.pack("<H", value)


def write_u32(value):
    return struct.pack("<I", value)


def write_str(value):
    raw = value.encode("utf-8")
    return write_u16(len(raw)) + raw


def make_code():
    alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
    return "".join(random.choice(alphabet) for _ in range(4))


class Room:
    def __init__(self, code, host_ip, host_port, start, host_id, host_name):
        self.code = code
        self.host_ip = host_ip
        self.host_port = host_port
        self.start = start
        self.created_at = time.time()
        self.last_seen = self.created_at
        self.members = {host_id: host_name}
        self.started = False

    def expired(self, ttl):
        return (time.time() - self.last_seen) > ttl


def parse_start(payload):
    # Layout A: session_id, seed, expected, host_port, filter, level_id, host_id
    offset = 0
    try:
        session_id, offset = read_u32(payload, offset)
        seed, offset = read_u32(payload, offset)
        expected_players, offset = read_u8(payload, offset)
        host_port, offset = read_u16(payload, offset)
        filter_optimized, offset = read_u8(payload, offset)
        level_id, offset = read_str(payload, offset)
        host_id, offset = read_str(payload, offset)
        pack_name = ""
        if offset < len(payload):
            pack_name, offset = read_str(payload, offset)
        host_name = None
        if offset < len(payload):
            host_name, offset = read_str(payload, offset)
        return {
            "session_id": session_id,
            "seed": seed,
            "expected_players": expected_players,
            "host_port": host_port,
            "filter_optimized": filter_optimized,
            "level_id": level_id,
            "host_id": host_id,
            "pack_name": pack_name,
            "host_name": host_name,
        }
    except Exception:
        pass

    # Layout B: optional lobby magic + session_id, level_id, seed, expected, host_port, host_id, filter
    offset = 0
    magic, offset = read_u32(payload, offset)
    if magic == 0x454E4C42:  # "ENLB"
        _version, offset = read_u8(payload, offset)
        _type, offset = read_u8(payload, offset)
    else:
        offset = 0
        magic = None

    session_id, offset = read_u32(payload, offset)
    level_id, offset = read_str(payload, offset)
    seed, offset = read_u32(payload, offset)
    expected_players, offset = read_u8(payload, offset)
    host_port, offset = read_u16(payload, offset)
    host_id, offset = read_str(payload, offset)
    filter_optimized = 1
    if offset < len(payload):
        filter_optimized, offset = read_u8(payload, offset)
    pack_name = ""
    if offset < len(payload):
        pack_name, offset = read_str(payload, offset)
    host_name = None
    if offset < len(payload):
        host_name, offset = read_str(payload, offset)
    return {
        "session_id": session_id,
        "seed": seed,
        "expected_players": expected_players,
        "host_port": host_port,
        "filter_optimized": filter_optimized,
        "level_id": level_id,
        "host_id": host_id,
        "pack_name": pack_name,
        "host_name": host_name,
    }


def build_start(start):
    return (
        write_u32(start["session_id"])
        + write_str(start["level_id"])
        + write_u32(start["seed"])
        + write_u8(start["expected_players"])
        + write_u16(start["host_port"])
        + write_str(start["host_id"])
        + write_u8(start["filter_optimized"])
    )


def normalize_member_name(name):
    if not name:
        return "Player"
    return name


def ordered_members(room):
    members = []
    host_id = room.start.get("host_id", "")
    if host_id in room.members:
        members.append((host_id, normalize_member_name(room.members[host_id])))
    for member_id in sorted(room.members.keys()):
        if member_id == host_id:
            continue
        members.append((member_id, normalize_member_name(room.members[member_id])))
    return members


def encode_members(room):
    members = ordered_members(room)
    if len(members) > 255:
        members = members[:255]
    payload = write_u8(len(members))
    for member_id, name in members:
        payload += write_str(member_id)
        payload += write_str(name)
    return payload


def handle_request(data, addr, rooms, ttl):
    offset = 0
    magic, offset = read_u32(data, offset)
    version, offset = read_u8(data, offset)
    msg_type, offset = read_u8(data, offset)
    if magic != MAGIC or version != VERSION:
        return None

    if msg_type == INET_CREATE:
        code, offset = read_str(data, offset)
        if not code:
            return (
                write_u32(MAGIC)
                + write_u8(VERSION)
                + write_u8(INET_ERROR)
                + write_str("please choose room code")
            )
        if code in rooms and not rooms[code].expired(ttl):
            return (
                write_u32(MAGIC)
                + write_u8(VERSION)
                + write_u8(INET_ERROR)
                + write_str("room code is already used")
            )
        start = parse_start(data[offset:])
        host_name = normalize_member_name(start.get("host_name"))
        rooms[code] = Room(code, addr[0], start["host_port"], start, start["host_id"], host_name)
        resp = (
            write_u32(MAGIC)
            + write_u8(VERSION)
            + write_u8(INET_CREATE_OK)
            + write_str(code)
            + write_str(addr[0])
        )
        return resp

    if msg_type == INET_JOIN:
        code, offset = read_str(data, offset)
        _client_id, offset = read_str(data, offset)
        client_name = "Player"
        if offset < len(data):
            client_name, offset = read_str(data, offset)
        room = rooms.get(code)
        if not room or room.expired(ttl):
            rooms.pop(code, None)
            return (
                write_u32(MAGIC)
                + write_u8(VERSION)
                + write_u8(INET_ERROR)
                + write_str("Room not found.")
            )
        room.members[_client_id] = normalize_member_name(client_name)
        room.last_seen = time.time()
        resp = (
            write_u32(MAGIC)
            + write_u8(VERSION)
            + write_u8(INET_JOIN_OK)
            + build_start(room.start)
            + write_str(room.host_ip)
            + write_u8(len(room.members))
            + write_str(room.start.get("pack_name", ""))
            + encode_members(room)
        )
        return resp

    if msg_type == INET_START:
        code, offset = read_str(data, offset)
        room = rooms.get(code)
        if not room or room.expired(ttl):
            rooms.pop(code, None)
            return (
                write_u32(MAGIC)
                + write_u8(VERSION)
                + write_u8(INET_ERROR)
                + write_str("Room not found.")
            )
        start = parse_start(data[offset:])
        room.start = start
        host_name = start.get("host_name")
        if host_name is not None:
            room.members[start["host_id"]] = normalize_member_name(host_name)
        elif start["host_id"] not in room.members:
            room.members[start["host_id"]] = "Player"
        room.host_ip = addr[0]
        room.host_port = start["host_port"]
        room.started = True
        room.last_seen = time.time()
        return (
            write_u32(MAGIC)
            + write_u8(VERSION)
            + write_u8(INET_START_OK)
        )

    if msg_type == INET_POLL:
        code, offset = read_str(data, offset)
        room = rooms.get(code)
        if not room or room.expired(ttl):
            rooms.pop(code, None)
            return (
                write_u32(MAGIC)
                + write_u8(VERSION)
                + write_u8(INET_ERROR)
                + write_str("Room not found.")
            )
        room.last_seen = time.time()
        if not room.started:
            return (
                write_u32(MAGIC)
                + write_u8(VERSION)
                + write_u8(INET_POLL_OK)
                + write_u8(0)
                + write_u8(len(room.members))
                + encode_members(room)
            )
        return (
            write_u32(MAGIC)
            + write_u8(VERSION)
            + write_u8(INET_POLL_OK)
            + write_u8(1)
            + write_u8(len(room.members))
            + build_start(room.start)
            + write_str(room.host_ip)
            + write_str(room.start.get("pack_name", ""))
            + encode_members(room)
        )

    if msg_type == INET_LEAVE:
        code, offset = read_str(data, offset)
        client_id, offset = read_str(data, offset)
        room = rooms.get(code)
        if room:
            host_id = room.start.get("host_id", "")
            if client_id == host_id:
                # Host owns the room. If host leaves, close the room for everyone
                # so the code can be reused immediately and no one remains in a
                # stale, unstartable room.
                rooms.pop(code, None)
            else:
                room.members.pop(client_id, None)
                room.last_seen = time.time()
                if not room.members:
                    rooms.pop(code, None)
        return (
            write_u32(MAGIC)
            + write_u8(VERSION)
            + write_u8(INET_CREATE_OK)
            + write_str(code)
            + write_str("")
        )

    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=12347)
    parser.add_argument("--ttl", type=int, default=1800, help="room TTL in seconds")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.host, args.port))
    sock.settimeout(1.0)

    rooms = {}
    print(f"Lobby server listening on {args.host}:{args.port}")
    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            # cleanup
            expired = [code for code, room in rooms.items() if room.expired(args.ttl)]
            for code in expired:
                rooms.pop(code, None)
            continue
        try:
            response = handle_request(data, addr, rooms, args.ttl)
        except Exception:
            response = (
                write_u32(MAGIC)
                + write_u8(VERSION)
                + write_u8(INET_ERROR)
                + write_str("Bad request.")
            )
        if response:
            sock.sendto(response, addr)


if __name__ == "__main__":
    main()
