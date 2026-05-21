#!/usr/bin/env python3
"""
PocketWebGames - PC mock server (Python standard library only).

Stands in for the M5Stack Core2 so the browser games in data/ can be tested
on a PC without hardware. Serves the static assets and speaks the same
WebSocket protocol as the firmware. Bot opponents fly/run around so there is
something to play against.

The Core2 runs a launcher and hosts one game at a time; this mock hosts ONE
game per process, chosen on the command line:

    uv run mock_server.py              # dogfight (default)
    uv run mock_server.py bomberman    # bomberman

Then open http://localhost:8080 in a browser (it redirects to the active game).

Controls:
  Dogfight  - arrow keys steer, Space fires, left Shift boosts.
  Bomberman - arrow keys / WASD move, Space drops a bomb.

This is a TEST mock, not a second source of truth: it implements just enough
of each game's rules to exercise the client. The authoritative logic lives in
the C++ firmware (src/DogfightGame.cpp, src/BombermanGame.cpp).
"""

import base64
import hashlib
import json
import math
import os
import random
import socket
import struct
import sys
import threading
import time

# ----------------------------------------------------------------------------
# Shared config
# ----------------------------------------------------------------------------
HOST = "0.0.0.0"
PORT = 8080
DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".json": "application/json",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".gz": "application/gzip",
}


# ============================================================================
# Shared math helpers (dogfight)
# ============================================================================
def mulberry32(seed):
    a = seed & 0xFFFFFFFF

    def nxt():
        nonlocal a
        a = (a + 0x6D2B79F5) & 0xFFFFFFFF
        t = a
        t = ((t ^ (t >> 15)) * (t | 1)) & 0xFFFFFFFF
        t = (t ^ (t + (((t ^ (t >> 7)) * (t | 61)) & 0xFFFFFFFF))) & 0xFFFFFFFF
        return ((t ^ (t >> 14)) & 0xFFFFFFFF) / 4294967296.0

    return nxt


def quat_look(vx, vy, vz):
    n = math.sqrt(vx * vx + vy * vy + vz * vz)
    if n < 1e-6:
        return (0.0, 0.0, 0.0, 1.0)
    vx, vy, vz = vx / n, vy / n, vz / n
    d = vz
    if d > 0.9999:
        return (0.0, 0.0, 0.0, 1.0)
    if d < -0.9999:
        return (0.0, 1.0, 0.0, 0.0)
    ax, ay, az = -vy, vx, 0.0
    s = math.sqrt((1 + d) * 2)
    invs = 1.0 / s
    qx, qy, qz, qw = ax * invs, ay * invs, az * invs, s * 0.5
    ln = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw)
    return (qx / ln, qy / ln, qz / ln, qw / ln)


def closest_on_ray(ox, oy, oz, dx, dy, dz, px, py, pz):
    vx, vy, vz = px - ox, py - oy, pz - oz
    t = vx * dx + vy * dy + vz * dz
    if t < 0:
        t = 0.0
    cx = ox + dx * t - px
    cy = oy + dy * t - py
    cz = oz + dz * t - pz
    return (cx * cx + cy * cy + cz * cz, t)


# ============================================================================
# WebSocket client + frame codec (shared)
# ============================================================================
class WSClient:
    def __init__(self, sock, addr):
        self.sock = sock
        self.addr = addr
        self.send_lock = threading.Lock()
        self.joined = False
        self.open = True

    def send_json(self, obj):
        self.send_text(json.dumps(obj, separators=(",", ":")))

    def send_text(self, text):
        if not self.open:
            return
        frame = encode_ws_frame(text.encode("utf-8"))
        try:
            with self.send_lock:
                self.sock.sendall(frame)
        except OSError:
            self.open = False

    def close(self):
        self.open = False
        try:
            self.sock.close()
        except OSError:
            pass


def encode_ws_frame(payload, opcode=0x1):
    header = bytearray()
    header.append(0x80 | opcode)
    n = len(payload)
    if n < 126:
        header.append(n)
    elif n < 65536:
        header.append(126)
        header.extend(struct.pack(">H", n))
    else:
        header.append(127)
        header.extend(struct.pack(">Q", n))
    return bytes(header) + payload


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def read_ws_frame(sock):
    head = recv_exact(sock, 2)
    if not head:
        return None
    b0, b1 = head[0], head[1]
    opcode = b0 & 0x0F
    masked = (b1 & 0x80) != 0
    length = b1 & 0x7F
    if length == 126:
        ext = recv_exact(sock, 2)
        if not ext:
            return None
        length = struct.unpack(">H", ext)[0]
    elif length == 127:
        ext = recv_exact(sock, 8)
        if not ext:
            return None
        length = struct.unpack(">Q", ext)[0]
    mask = b"\x00\x00\x00\x00"
    if masked:
        mask = recv_exact(sock, 4)
        if mask is None:
            return None
    payload = recv_exact(sock, length) if length else b""
    if payload is None:
        return None
    if masked:
        payload = bytes(payload[i] ^ mask[i % 4] for i in range(len(payload)))
    return (opcode, payload)


# ============================================================================
# Dogfight simulation
# ============================================================================
DF_MAX_PLAYERS = 6
DF_START_HP = 100
DF_DAMAGE_PER_HIT = 25
DF_ARENA_SIZE = 500.0
DF_HIT_RADIUS = 6.0
DF_LASER_RANGE = 220.0
DF_ROUND_MS = 3 * 60 * 1000
DF_RESPAWN_MS = 2000
DF_ASTEROID_HP_PER_R = 5
DF_NUM_ASTEROIDS = 22
DF_NUM_BOTS = 2
DF_BOT_FIRE_INTERVAL = 1.6
DF_BOT_ACCURACY = 0.45


def gen_asteroids(seed):
    rnd = mulberry32(seed)
    out = []
    for _ in range(DF_NUM_ASTEROIDS):
        r = 6.0 + rnd() * 10.0
        x = (rnd() * 2 - 1) * DF_ARENA_SIZE * 0.45
        y = (rnd() * 2 - 1) * DF_ARENA_SIZE * 0.15
        z = (rnd() * 2 - 1) * DF_ARENA_SIZE * 0.45
        max_hp = int(r * DF_ASTEROID_HP_PER_R)
        out.append({"r": r, "x": x, "y": y, "z": z, "hp": max_hp,
                    "max_hp": max_hp, "alive": True})
    return out


class DogfightSim:
    slug = "dogfight"
    name = "Space Dogfight"

    def __init__(self):
        self.lock = threading.RLock()
        self.seed = random.randint(1, 0x7FFFFFFF)
        self.asteroids = gen_asteroids(self.seed)
        self.round_start = time.time()
        self.clients = {}     # WSClient -> player_id
        self.players = {}     # id -> dict
        self.next_id = 1
        self.bots = []
        self._spawn_bots()
        self._last = time.time()
        self._last_sb = self._last_score = self._last_round = 0.0

    # ---- helpers ----
    def remaining_sec(self):
        return max(0, int(DF_ROUND_MS / 1000.0 - (time.time() - self.round_start)))

    def restart_round(self):
        self.seed = random.randint(1, 0x7FFFFFFF)
        self.asteroids = gen_asteroids(self.seed)
        self.round_start = time.time()
        for b in self.bots:
            b["score"] = 0
        for p in self.players.values():
            p["score"] = 0

    def _spawn_bots(self):
        for i in range(DF_NUM_BOTS):
            self.bots.append(self._new_bot(DF_MAX_PLAYERS - i))

    def _new_bot(self, bot_id):
        return {
            "id": bot_id, "name": "BOT-%d" % bot_id,
            "x": random.uniform(-80, 80), "y": random.uniform(-20, 20),
            "z": random.uniform(-80, 80),
            "qx": 0.0, "qy": 0.0, "qz": 0.0, "qw": 1.0,
            "hp": DF_START_HP, "score": 0, "dead": False, "respawn_at": 0.0,
            "heading": random.uniform(0, math.tau), "pitch": random.uniform(-0.3, 0.3),
            "speed": random.uniform(18, 30),
            "next_turn": time.time() + random.uniform(1, 3),
            "next_fire": time.time() + random.uniform(1, 4),
        }

    # ---- net ----
    def on_text(self, client, m):
        t = m.get("t")
        if t == "join":
            self._join(client, m.get("name", "PILOT"))
            return
        if t == "ping":
            client.send_json({"t": "pong"})
            return
        pid = self.clients.get(client)
        if pid is None:
            return
        if t == "s":
            p = self.players.get(pid)
            if p:
                p["x"] = float(m.get("x", 0.0)); p["y"] = float(m.get("y", 0.0))
                p["z"] = float(m.get("z", 0.0))
                p["qx"] = float(m.get("qx", 0.0)); p["qy"] = float(m.get("qy", 0.0))
                p["qz"] = float(m.get("qz", 0.0)); p["qw"] = float(m.get("qw", 1.0))
        elif t == "f":
            self._fire(pid, float(m.get("x", 0.0)), float(m.get("y", 0.0)),
                       float(m.get("z", 0.0)), float(m.get("dx", 0.0)),
                       float(m.get("dy", 0.0)), float(m.get("dz", 1.0)))

    def _join(self, client, name):
        if client.joined:
            return
        if len(self.players) >= (DF_MAX_PLAYERS - DF_NUM_BOTS):
            client.send_json({"t": "err", "m": "full"})
            client.close()
            return
        pid = self.next_id
        self.next_id += 1
        self.players[pid] = {
            "id": pid, "name": str(name)[:16] or "PILOT",
            "x": random.uniform(-20, 20), "y": random.uniform(-5, 5),
            "z": random.uniform(-20, 20),
            "qx": 0.0, "qy": 0.0, "qz": 0.0, "qw": 1.0,
            "hp": DF_START_HP, "score": 0, "dead": False, "respawn_at": 0.0,
        }
        self.clients[client] = pid
        client.joined = True
        self._send_init(client, pid)
        print("[df] join id=%d name=%s" % (pid, self.players[pid]["name"]))
        self.broadcast_round(); self.broadcast_scores()

    def on_disconnect(self, client):
        pid = self.clients.pop(client, None)
        if pid is not None:
            self.players.pop(pid, None)
            self.broadcast({"t": "leave", "id": pid})

    def _send_init(self, client, pid):
        dead = [i for i, a in enumerate(self.asteroids) if not a["alive"]]
        adhp = [[i, a["hp"]] for i, a in enumerate(self.asteroids)
                if a["alive"] and a["hp"] < a["max_hp"]]
        client.send_json({"t": "init", "id": pid, "seed": self.seed,
                          "arena": int(DF_ARENA_SIZE), "hp": DF_START_HP,
                          "acount": len(self.asteroids), "dead": dead,
                          "adhp": adhp, "pu": []})

    def _fire(self, shooter_id, ox, oy, oz, dx, dy, dz):
        length = math.sqrt(dx * dx + dy * dy + dz * dz)
        if length < 1e-6:
            return
        dx, dy, dz = dx / length, dy / length, dz / length
        shooter = self.players.get(shooter_id)
        if not shooter or shooter["dead"]:
            return
        self.broadcast({"t": "fb", "id": shooter_id, "x": ox, "y": oy, "z": oz,
                        "dx": dx, "dy": dy, "dz": dz}, exclude_id=shooter_id)
        best_t = DF_LASER_RANGE
        hit_bot = None
        hit_ast = -1
        for b in self.bots:
            if b["dead"]:
                continue
            d2, t = closest_on_ray(ox, oy, oz, dx, dy, dz, b["x"], b["y"], b["z"])
            if t > DF_LASER_RANGE:
                continue
            if d2 <= DF_HIT_RADIUS * DF_HIT_RADIUS and t < best_t:
                best_t = t; hit_bot = b; hit_ast = -1
        for i, a in enumerate(self.asteroids):
            if not a["alive"]:
                continue
            d2, t = closest_on_ray(ox, oy, oz, dx, dy, dz, a["x"], a["y"], a["z"])
            if t > DF_LASER_RANGE:
                continue
            if d2 <= a["r"] * a["r"] and t < best_t:
                best_t = t; hit_ast = i; hit_bot = None
        if hit_bot is not None:
            self._damage_bot(hit_bot, shooter_id)
        elif hit_ast >= 0:
            a = self.asteroids[hit_ast]
            a["hp"] -= DF_DAMAGE_PER_HIT
            if a["hp"] <= 0:
                a["alive"] = False
                self.broadcast({"t": "adestroy", "id": hit_ast})
            else:
                self.broadcast({"t": "adamage", "id": hit_ast, "hp": a["hp"]})

    def _damage_bot(self, bot, shooter_id):
        bot["hp"] = max(0, bot["hp"] - DF_DAMAGE_PER_HIT)
        self.broadcast({"t": "hit", "a": shooter_id, "target": bot["id"], "hp": bot["hp"]})
        if bot["hp"] == 0:
            s = self.players.get(shooter_id)
            if s:
                s["score"] += 1
            bot["dead"] = True
            bot["respawn_at"] = time.time() + DF_RESPAWN_MS / 1000.0
            self.broadcast({"t": "kill", "a": shooter_id, "target": bot["id"]})
            self.broadcast_scores()

    def _damage_player(self, p, shooter_id):
        p["hp"] = max(0, p["hp"] - DF_DAMAGE_PER_HIT)
        self.broadcast({"t": "hit", "a": shooter_id, "target": p["id"], "hp": p["hp"]})
        if p["hp"] == 0:
            for b in self.bots:
                if b["id"] == shooter_id:
                    b["score"] += 1
            p["dead"] = True
            p["respawn_at"] = time.time() + DF_RESPAWN_MS / 1000.0
            self.broadcast({"t": "kill", "a": shooter_id, "target": p["id"]})
            self.broadcast_scores()

    # ---- broadcasts ----
    def broadcast(self, obj, exclude_id=None):
        text = json.dumps(obj, separators=(",", ":"))
        for client, pid in list(self.clients.items()):
            if exclude_id is not None and pid == exclude_id:
                continue
            client.send_text(text)

    def broadcast_states(self):
        arr = []
        for p in self.players.values():
            arr.append({"id": p["id"], "x": p["x"], "y": p["y"], "z": p["z"],
                        "qx": p["qx"], "qy": p["qy"], "qz": p["qz"], "qw": p["qw"],
                        "hp": p["hp"], "r": 1 if p["dead"] else 0})
        for b in self.bots:
            arr.append({"id": b["id"], "x": b["x"], "y": b["y"], "z": b["z"],
                        "qx": b["qx"], "qy": b["qy"], "qz": b["qz"], "qw": b["qw"],
                        "hp": b["hp"], "r": 1 if b["dead"] else 0})
        self.broadcast({"t": "sb", "p": arr})

    def broadcast_scores(self):
        rows = [[p["id"], p["score"], p["name"]] for p in self.players.values()]
        rows += [[b["id"], b["score"], b["name"]] for b in self.bots]
        self.broadcast({"t": "score", "p": rows})

    def broadcast_round(self):
        self.broadcast({"t": "round", "state": "running",
                        "remaining": self.remaining_sec(), "winner": 0,
                        "nA": sum(1 for a in self.asteroids if a["alive"]),
                        "nP": 0, "nC": len(self.players) + len(self.bots),
                        "fw": "PC-MOCK"})

    # ---- tick ----
    def tick(self, now):
        dt = now - self._last
        self._last = now
        if self.remaining_sec() <= 0:
            self.restart_round()
            for client, pid in list(self.clients.items()):
                self._send_init(client, pid)
            self.broadcast_round(); self.broadcast_scores()
        for p in self.players.values():
            if p["dead"] and p["respawn_at"] and now >= p["respawn_at"]:
                p["hp"] = DF_START_HP; p["dead"] = False; p["respawn_at"] = 0.0
                p["x"] = random.uniform(-30, 30); p["y"] = random.uniform(-8, 8)
                p["z"] = random.uniform(-30, 30)
                self.broadcast({"t": "respawn", "id": p["id"],
                                "x": p["x"], "y": p["y"], "z": p["z"]})
        self._update_bots(now, dt)
        if now - self._last_sb >= 0.1:
            self._last_sb = now; self.broadcast_states()
        if now - self._last_score >= 0.5:
            self._last_score = now; self.broadcast_scores()
        if now - self._last_round >= 1.0:
            self._last_round = now; self.broadcast_round()

    def _update_bots(self, now, dt):
        half = DF_ARENA_SIZE * 0.5
        targets = list(self.players.values())
        for b in self.bots:
            if b["dead"]:
                if b["respawn_at"] and now >= b["respawn_at"]:
                    fresh = self._new_bot(b["id"]); fresh["score"] = b["score"]
                    b.update(fresh)
                    self.broadcast({"t": "respawn", "id": b["id"],
                                    "x": b["x"], "y": b["y"], "z": b["z"]})
                continue
            if now >= b["next_turn"]:
                b["heading"] += random.uniform(-1.2, 1.2)
                b["pitch"] = max(-0.5, min(0.5, b["pitch"] + random.uniform(-0.3, 0.3)))
                b["speed"] = random.uniform(18, 32)
                b["next_turn"] = now + random.uniform(1.0, 3.0)
            ch, sh = math.cos(b["heading"]), math.sin(b["heading"])
            cp, sp = math.cos(b["pitch"]), math.sin(b["pitch"])
            dx, dy, dz = sh * cp, sp, ch * cp
            b["x"] += dx * b["speed"] * dt
            b["y"] += dy * b["speed"] * dt
            b["z"] += dz * b["speed"] * dt
            if abs(b["x"]) > half or abs(b["z"]) > half or abs(b["y"]) > half * 0.4:
                b["x"] = max(-half, min(half, b["x"]))
                b["y"] = max(-half * 0.4, min(half * 0.4, b["y"]))
                b["z"] = max(-half, min(half, b["z"]))
                b["heading"] += math.pi
                b["next_turn"] = now + random.uniform(0.5, 1.5)
            b["qx"], b["qy"], b["qz"], b["qw"] = quat_look(dx, dy, dz)
            if targets and now >= b["next_fire"]:
                b["next_fire"] = now + random.uniform(DF_BOT_FIRE_INTERVAL * 0.6,
                                                      DF_BOT_FIRE_INTERVAL * 1.6)
                tgt = min(targets, key=lambda p: (p["x"] - b["x"]) ** 2 +
                          (p["y"] - b["y"]) ** 2 + (p["z"] - b["z"]) ** 2)
                if tgt["dead"]:
                    continue
                tx, ty, tz = tgt["x"] - b["x"], tgt["y"] - b["y"], tgt["z"] - b["z"]
                dist = math.sqrt(tx * tx + ty * ty + tz * tz)
                if dist > DF_LASER_RANGE or dist < 1e-3:
                    continue
                tx, ty, tz = tx / dist, ty / dist, tz / dist
                self.broadcast({"t": "fb", "id": b["id"], "x": b["x"], "y": b["y"],
                                "z": b["z"], "dx": tx, "dy": ty, "dz": tz})
                if random.random() < DF_BOT_ACCURACY:
                    self._damage_player(tgt, b["id"])


# ============================================================================
# Bomberman simulation
# ============================================================================
BM_W = 13
BM_H = 11
BM_FUSE = 2.5
BM_FLAME = 0.6
BM_BASE_BOMBS = 1
BM_MAX_BOMBS = 6
BM_BASE_FLAME = 2
BM_MAX_FLAME = 6
BM_BRICK_PROB = 0.72
BM_PU_PROB = 0.35
BM_ROUND = 180
BM_RESPAWN = 2.0
BM_PU_TYPES = 3
BM_MAX_PLAYERS = 6
BM_NUM_BOTS = 2

BM_SPAWNS = [(1, 1), (BM_W - 2, 1), (1, BM_H - 2), (BM_W - 2, BM_H - 2),
             (BM_W // 2, 1), (BM_W // 2, BM_H - 2)]


class BombermanSim:
    slug = "blaster"
    name = "Blaster"

    def __init__(self):
        self.lock = threading.RLock()
        self.clients = {}     # WSClient -> id
        self.players = {}     # id -> dict
        self.bots = []
        self.next_id = 1
        self.bombs = {}       # id -> dict
        self.flames = []      # {x,y,until,owner}
        self.powerups = {}    # id -> dict
        self.next_bomb = 1
        self.next_pu = 1
        self.cells = []
        self.round_start = time.time()
        self._gen_level()
        for i in range(BM_NUM_BOTS):
            self.bots.append(self._new_bot(BM_MAX_PLAYERS - i))
        self._last = time.time()
        self._last_sb = self._last_score = self._last_round = 0.0

    # ---- grid ----
    def _idx(self, x, y):
        return y * BM_W + x

    def _gen_level(self):
        self.cells = ["."] * (BM_W * BM_H)
        for y in range(BM_H):
            for x in range(BM_W):
                border = (x == 0 or y == 0 or x == BM_W - 1 or y == BM_H - 1)
                if border or (x % 2 == 0 and y % 2 == 0):
                    self.cells[self._idx(x, y)] = "#"
                elif random.random() < BM_BRICK_PROB:
                    self.cells[self._idx(x, y)] = "+"
        for (sx, sy) in BM_SPAWNS:
            for ox, oy in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
                x, y = sx + ox, sy + oy
                if 0 < x < BM_W - 1 and 0 < y < BM_H - 1 and self.cells[self._idx(x, y)] == "+":
                    self.cells[self._idx(x, y)] = "."

    def cells_str(self):
        return "".join(self.cells)

    def _solid(self, x, y, ignore=None):
        if x < 0 or y < 0 or x >= BM_W or y >= BM_H:
            return True
        if self.cells[self._idx(x, y)] in ("#", "+"):
            return True
        for b in self.bombs.values():
            if b["x"] == x and b["y"] == y and (x, y) != ignore:
                return True
        return False

    # ---- fighters ----
    def _all(self):
        return list(self.players.values()) + self.bots

    def _find(self, fid):
        p = self.players.get(fid)
        if p:
            return p
        for b in self.bots:
            if b["id"] == fid:
                return b
        return None

    def remaining_sec(self):
        return max(0, int(BM_ROUND - (time.time() - self.round_start)))

    def _new_bot(self, bot_id):
        sx, sy = BM_SPAWNS[(bot_id - 1) % len(BM_SPAWNS)]
        return {"id": bot_id, "name": "BOT-%d" % bot_id, "x": float(sx), "y": float(sy),
                "tx": sx, "ty": sy, "dead": False, "respawn_at": 0.0, "score": 0,
                "max_bombs": BM_BASE_BOMBS, "active_bombs": 0, "flame": BM_BASE_FLAME,
                "speed": 3.0, "next_bomb": time.time() + random.uniform(2, 5),
                "is_bot": True}

    # ---- net ----
    def on_text(self, client, m):
        t = m.get("t")
        if t == "join":
            self._join(client, m.get("name", "PLAYER"))
            return
        if t == "ping":
            client.send_json({"t": "pong"})
            return
        pid = self.clients.get(client)
        if pid is None:
            return
        if t == "s":
            p = self.players.get(pid)
            if p:
                p["x"] = float(m.get("x", 1.0)); p["y"] = float(m.get("y", 1.0))
        elif t == "b":
            p = self.players.get(pid)
            if p:
                self._place_bomb(p)

    def _join(self, client, name):
        if client.joined:
            return
        if len(self.players) >= (BM_MAX_PLAYERS - BM_NUM_BOTS):
            client.send_json({"t": "err", "m": "full"})
            client.close()
            return
        pid = self.next_id
        self.next_id += 1
        sx, sy = BM_SPAWNS[(pid - 1) % len(BM_SPAWNS)]
        self.players[pid] = {"id": pid, "name": str(name)[:16] or "PLAYER",
                             "x": float(sx), "y": float(sy), "dead": False,
                             "respawn_at": 0.0, "score": 0,
                             "max_bombs": BM_BASE_BOMBS, "active_bombs": 0,
                             "flame": BM_BASE_FLAME, "is_bot": False}
        self.clients[client] = pid
        client.joined = True
        self._send_init(client, self.players[pid])
        print("[bm] join id=%d name=%s" % (pid, self.players[pid]["name"]))
        self.broadcast_round(); self.broadcast_scores()

    def on_disconnect(self, client):
        pid = self.clients.pop(client, None)
        if pid is not None:
            self.players.pop(pid, None)
            self.broadcast({"t": "leave", "id": pid})

    def _send_init(self, client, p):
        client.send_json({
            "t": "init", "id": p["id"], "w": BM_W, "h": BM_H,
            "sx": p["x"], "sy": p["y"], "mb": p["max_bombs"], "fl": p["flame"],
            "cells": self.cells_str(),
            "bombs": [{"id": b["id"], "x": b["x"], "y": b["y"], "range": b["range"]}
                      for b in self.bombs.values()],
            "pu": [{"id": pu["id"], "type": pu["type"], "x": pu["x"], "y": pu["y"]}
                   for pu in self.powerups.values()],
        })

    # ---- bombs / explosions ----
    def _place_bomb(self, f):
        if f["dead"] or f["active_bombs"] >= f["max_bombs"]:
            return
        tx, ty = round(f["x"]), round(f["y"])
        if not (0 <= tx < BM_W and 0 <= ty < BM_H):
            return
        if self.cells[self._idx(tx, ty)] != ".":
            return
        for b in self.bombs.values():
            if b["x"] == tx and b["y"] == ty:
                return
        bid = self.next_bomb
        self.next_bomb += 1
        self.bombs[bid] = {"id": bid, "x": tx, "y": ty, "owner": f["id"],
                           "range": f["flame"], "explode_at": time.time() + BM_FUSE}
        f["active_bombs"] += 1
        self.broadcast({"t": "bomb", "id": bid, "x": tx, "y": ty,
                        "range": f["flame"], "owner": f["id"]})

    def _process_bombs(self, now):
        again = True
        while again:
            again = False
            for bid in list(self.bombs.keys()):
                b = self.bombs.get(bid)
                if b and now >= b["explode_at"]:
                    self._explode(b, now)
                    again = True

    def _explode(self, b, now):
        self.bombs.pop(b["id"], None)
        owner = self._find(b["owner"])
        if owner and owner["active_bombs"] > 0:
            owner["active_bombs"] -= 1
        cells = [(b["x"], b["y"])]
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            for r in range(1, b["range"] + 1):
                nx, ny = b["x"] + dx * r, b["y"] + dy * r
                if nx < 0 or ny < 0 or nx >= BM_W or ny >= BM_H:
                    break
                c = self.cells[self._idx(nx, ny)]
                if c == "#":
                    break
                cells.append((nx, ny))
                if c == "+":
                    self.cells[self._idx(nx, ny)] = "."
                    self.broadcast({"t": "brick", "x": nx, "y": ny})
                    self._maybe_drop_pu(nx, ny)
                    break
                for ob in self.bombs.values():
                    if ob["x"] == nx and ob["y"] == ny:
                        ob["explode_at"] = now
        for (x, y) in cells:
            self.flames.append({"x": x, "y": y, "until": now + BM_FLAME, "owner": b["owner"]})
        self.broadcast({"t": "boom", "c": [[x, y] for (x, y) in cells]})

    def _maybe_drop_pu(self, x, y):
        if random.random() >= BM_PU_PROB:
            return
        pid = self.next_pu
        self.next_pu += 1
        pu = {"id": pid, "type": random.randrange(BM_PU_TYPES), "x": x, "y": y}
        self.powerups[pid] = pu
        self.broadcast({"t": "pspawn", "id": pid, "type": pu["type"], "x": x, "y": y})

    def _expire_flames(self, now):
        self.flames = [f for f in self.flames if now < f["until"]]

    def _kill_on_flames(self, now):
        for f in self._all():
            if f["dead"]:
                continue
            tx, ty = round(f["x"]), round(f["y"])
            for fl in self.flames:
                if fl["x"] == tx and fl["y"] == ty:
                    self._kill(f, fl["owner"])
                    break

    def _check_pickups(self):
        for f in self._all():
            if f["dead"]:
                continue
            tx, ty = round(f["x"]), round(f["y"])
            for pid in list(self.powerups.keys()):
                pu = self.powerups[pid]
                if pu["x"] == tx and pu["y"] == ty:
                    self._apply_pu(f, pu["type"])
                    self.broadcast({"t": "ppickup", "id": pid, "target": f["id"],
                                    "type": pu["type"]})
                    self.powerups.pop(pid, None)

    def _apply_pu(self, f, t):
        if t == 0 and f["max_bombs"] < BM_MAX_BOMBS:
            f["max_bombs"] += 1
        elif t == 1 and f["flame"] < BM_MAX_FLAME:
            f["flame"] += 1
        elif t == 2:
            f["speed"] = min(6.0, f.get("speed", 3.0) + 0.6)

    def _kill(self, target, owner):
        if target["dead"]:
            return
        target["dead"] = True
        target["respawn_at"] = time.time() + BM_RESPAWN
        target["active_bombs"] = 0
        if owner and owner != target["id"]:
            o = self._find(owner)
            if o:
                o["score"] += 1
        self.broadcast({"t": "kill", "a": owner, "target": target["id"]})
        self.broadcast_scores()

    def _respawn(self, f):
        sx, sy = BM_SPAWNS[(f["id"] - 1) % len(BM_SPAWNS)]
        f["x"] = float(sx); f["y"] = float(sy)
        f["dead"] = False; f["respawn_at"] = 0.0
        f["max_bombs"] = BM_BASE_BOMBS; f["flame"] = BM_BASE_FLAME
        f["active_bombs"] = 0
        if f.get("is_bot"):
            f["tx"] = sx; f["ty"] = sy; f["speed"] = 3.0

    def restart_round(self):
        self._gen_level()
        self.bombs.clear(); self.flames.clear(); self.powerups.clear()
        self.round_start = time.time()
        for f in self._all():
            f["score"] = 0
            self._respawn(f)

    # ---- broadcasts ----
    def broadcast(self, obj, exclude_id=None):
        text = json.dumps(obj, separators=(",", ":"))
        for client, pid in list(self.clients.items()):
            if exclude_id is not None and pid == exclude_id:
                continue
            client.send_text(text)

    def broadcast_states(self):
        arr = [{"id": f["id"], "x": f["x"], "y": f["y"], "d": 1 if f["dead"] else 0,
                "mb": f["max_bombs"], "fl": f["flame"]} for f in self._all()]
        self.broadcast({"t": "sb", "p": arr})

    def broadcast_scores(self):
        self.broadcast({"t": "score", "p": [[f["id"], f["score"], f["name"]]
                                            for f in self._all()]})

    def broadcast_round(self):
        self.broadcast({"t": "round", "state": "running",
                        "remaining": self.remaining_sec(), "winner": 0,
                        "nC": len(self._all()), "fw": "PC-MOCK"})

    # ---- tick ----
    def tick(self, now):
        dt = now - self._last
        self._last = now
        if self.remaining_sec() <= 0:
            self.restart_round()
            for client, pid in list(self.clients.items()):
                p = self.players.get(pid)
                if p:
                    self._send_init(client, p)
            self.broadcast_round(); self.broadcast_scores()
        self._process_bombs(now)
        self._expire_flames(now)
        self._kill_on_flames(now)
        self._check_pickups()
        for p in self.players.values():
            if p["dead"] and p["respawn_at"] and now >= p["respawn_at"]:
                self._respawn(p)
                self.broadcast({"t": "respawn", "id": p["id"], "x": p["x"], "y": p["y"]})
        self._update_bots(now, dt)
        if now - self._last_sb >= 0.1:
            self._last_sb = now; self.broadcast_states()
        if now - self._last_score >= 0.5:
            self._last_score = now; self.broadcast_scores()
        if now - self._last_round >= 1.0:
            self._last_round = now; self.broadcast_round()

    def _update_bots(self, now, dt):
        for b in self.bots:
            if b["dead"]:
                if b["respawn_at"] and now >= b["respawn_at"]:
                    self._respawn(b)
                    self.broadcast({"t": "respawn", "id": b["id"], "x": b["x"], "y": b["y"]})
                continue
            # Reached current target tile?
            if abs(b["x"] - b["tx"]) < 0.06 and abs(b["y"] - b["ty"]) < 0.06:
                b["x"], b["y"] = float(b["tx"]), float(b["ty"])
                # Maybe drop a bomb (to break bricks / threaten players).
                if now >= b["next_bomb"] and b["active_bombs"] < b["max_bombs"] \
                        and self._brick_or_foe_adjacent(b):
                    self._place_bomb(b)
                    b["next_bomb"] = now + random.uniform(2.5, 5.0)
                # Pick a new adjacent passable tile.
                opts = []
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = b["tx"] + dx, b["ty"] + dy
                    if not self._solid(nx, ny, ignore=(b["tx"], b["ty"])):
                        opts.append((nx, ny))
                if opts:
                    b["tx"], b["ty"] = random.choice(opts)
            else:
                step = b["speed"] * dt
                if b["x"] != b["tx"]:
                    d = b["tx"] - b["x"]
                    b["x"] += max(-step, min(step, d))
                if b["y"] != b["ty"]:
                    d = b["ty"] - b["y"]
                    b["y"] += max(-step, min(step, d))

    def _brick_or_foe_adjacent(self, b):
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = b["tx"] + dx, b["ty"] + dy
            if 0 <= nx < BM_W and 0 <= ny < BM_H and self.cells[self._idx(nx, ny)] == "+":
                return True
        for p in self.players.values():
            if not p["dead"] and abs(round(p["x"]) - b["tx"]) + abs(round(p["y"]) - b["ty"]) <= 2:
                return True
        return False


# ============================================================================
# Curve Fever simulation
# ============================================================================
CF_GW = 120
CF_GH = 80
CF_SPEED = 20.0
CF_TURN = 3.4
CF_STATE = 0.033
CF_GAP_INTERVAL = 2.6
CF_GAP_LEN = 0.18
CF_GRACE = 0.5
CF_MINI_GAP = 1.6
CF_ROUND = 180
CF_MAX_PLAYERS = 6
CF_NUM_BOTS = 2
CF_FX = [0.22, 0.78, 0.50, 0.22, 0.78, 0.50]
CF_FY = [0.25, 0.25, 0.50, 0.75, 0.75, 0.50]


class CurveFeverSim:
    slug = "trails"
    name = "Trails"

    def __init__(self):
        self.lock = threading.RLock()
        self.clients = {}
        self.players = {}
        self.bots = []
        self.next_id = 1
        self.grid = bytearray(CF_GW * CF_GH)
        self.state_running = True   # mock is always "running"
        self.mini_active = False
        self.mini_restart_at = time.time()
        self.round_start = time.time()
        for i in range(CF_NUM_BOTS):
            self.bots.append(self._new_fighter(CF_MAX_PLAYERS - i, "BOT-%d" % (CF_MAX_PLAYERS - i), True))
        self._last = time.time()
        self._last_sb = self._last_score = self._last_round = 0.0

    # ---- helpers ----
    def _all(self):
        return list(self.players.values()) + self.bots

    def _find(self, fid):
        p = self.players.get(fid)
        if p:
            return p
        for b in self.bots:
            if b["id"] == fid:
                return b
        return None

    def remaining_sec(self):
        return max(0, int(CF_ROUND - (time.time() - self.round_start)))

    def _spawn_xy(self, fid):
        i = (fid - 1) % 6
        x = CF_FX[i] * CF_GW
        y = CF_FY[i] * CF_GH
        h = math.atan2(CF_GH * 0.5 - y, CF_GW * 0.5 - x)
        return x, y, h

    def _new_fighter(self, fid, name, is_bot):
        x, y, h = self._spawn_xy(fid)
        return {"id": fid, "name": name, "x": x, "y": y, "heading": h, "turn": 0,
                "alive": False, "score": 0, "gap_until": 0.0, "next_gap": 0.0,
                "grace_until": 0.0, "is_bot": is_bot, "cx": -1, "cy": -1}

    # ---- net ----
    def on_text(self, client, m):
        t = m.get("t")
        if t == "join":
            self._join(client, m.get("name", "PLAYER"))
            return
        if t == "ping":
            client.send_json({"t": "pong"})
            return
        pid = self.clients.get(client)
        if pid is None:
            return
        if t == "i":
            p = self.players.get(pid)
            if p:
                d = int(m.get("d", 0))
                p["turn"] = -1 if d < 0 else (1 if d > 0 else 0)

    def _join(self, client, name):
        if client.joined:
            return
        if len(self.players) >= (CF_MAX_PLAYERS - CF_NUM_BOTS):
            client.send_json({"t": "err", "m": "full"})
            client.close()
            return
        pid = self.next_id
        self.next_id += 1
        f = self._new_fighter(pid, str(name)[:16] or "PLAYER", False)
        if self.mini_active:
            now = time.time()
            f["alive"] = True
            f["grace_until"] = now + CF_GRACE
            f["next_gap"] = now + CF_GAP_INTERVAL
            f["cx"], f["cy"] = int(f["x"]), int(f["y"])
            self.grid[int(f["y"]) * CF_GW + int(f["x"])] = f["id"]
        self.players[pid] = f
        self.clients[client] = pid
        client.joined = True
        client.send_json({"t": "init", "id": pid, "w": CF_GW, "h": CF_GH})
        print("[cf] join id=%d name=%s" % (pid, f["name"]))
        self.broadcast_round(); self.broadcast_scores()

    def on_disconnect(self, client):
        pid = self.clients.pop(client, None)
        if pid is not None:
            self.players.pop(pid, None)
            self.broadcast({"t": "leave", "id": pid})

    # ---- sim ----
    def _occupied(self, cx, cy):
        return 0 <= cx < CF_GW and 0 <= cy < CF_GH and self.grid[cy * CF_GW + cx]

    def _free_dist(self, x, y, h, maxd=18):
        cs, sn = math.cos(h), math.sin(h)
        for d in range(1, maxd + 1):
            cx = int(x + cs * d)
            cy = int(y + sn * d)
            if cx < 0 or cy < 0 or cx >= CF_GW or cy >= CF_GH or self.grid[cy * CF_GW + cx]:
                return d
        return maxd

    def _integrate(self, f, now, dt):
        f["heading"] += f["turn"] * CF_TURN * dt
        f["x"] += math.cos(f["heading"]) * CF_SPEED * dt
        f["y"] += math.sin(f["heading"]) * CF_SPEED * dt
        cx, cy = int(f["x"]), int(f["y"])
        if cx < 0 or cy < 0 or cx >= CF_GW or cy >= CF_GH:
            self._kill(f); return
        if now >= f["next_gap"]:
            f["gap_until"] = now + CF_GAP_LEN
            f["next_gap"] = now + CF_GAP_INTERVAL
        # Only react on entering a NEW cell (never collide with our own cell).
        if cx == f.get("cx") and cy == f.get("cy"):
            return
        in_gap = now < f["gap_until"]
        grace = now < f["grace_until"]
        if not grace and self.grid[cy * CF_GW + cx]:
            self._kill(f); return
        if not in_gap:
            self.grid[cy * CF_GW + cx] = f["id"]
        f["cx"], f["cy"] = cx, cy

    def _kill(self, f):
        if not f["alive"]:
            return
        f["alive"] = False
        self.broadcast({"t": "dead", "id": f["id"]})

    def _alive_count(self):
        return sum(1 for f in self._all() if f["alive"])

    def _start_mini(self, now):
        self.grid = bytearray(CF_GW * CF_GH)
        for f in self._all():
            x, y, h = self._spawn_xy(f["id"])
            f["x"], f["y"], f["heading"] = x, y, h
            f["turn"] = 0
            f["alive"] = True
            f["gap_until"] = 0.0
            f["next_gap"] = now + CF_GAP_INTERVAL
            f["grace_until"] = now + CF_GRACE
            f["cx"], f["cy"] = int(x), int(y)
            self.grid[int(y) * CF_GW + int(x)] = f["id"]
        self.mini_active = True
        self.mini_restart_at = 0.0
        self.broadcast({"t": "clear"})
        self.broadcast_states()

    def restart_round(self):
        self.grid = bytearray(CF_GW * CF_GH)
        for f in self._all():
            f["score"] = 0
            f["alive"] = False
        self.round_start = time.time()
        self.mini_active = False
        self.mini_restart_at = time.time()

    def _update_bots(self, now):
        for b in self.bots:
            if not b["alive"]:
                continue
            fd = self._free_dist(b["x"], b["y"], b["heading"])
            if fd < 9:
                dl = self._free_dist(b["x"], b["y"], b["heading"] - 0.5)
                dr = self._free_dist(b["x"], b["y"], b["heading"] + 0.5)
                b["turn"] = -1 if dl > dr else 1
            else:
                r = random.random()
                if r < 0.02:
                    b["turn"] = random.choice([-1, 1])
                elif r < 0.12:
                    b["turn"] = 0

    # ---- broadcasts ----
    def broadcast(self, obj, exclude_id=None):
        text = json.dumps(obj, separators=(",", ":"))
        for client, pid in list(self.clients.items()):
            if exclude_id is not None and pid == exclude_id:
                continue
            client.send_text(text)

    def broadcast_states(self):
        if not self.clients:
            return
        now = time.time()
        arr = [{"id": f["id"], "x": f["x"], "y": f["y"],
                "a": 1 if f["alive"] else 0,
                "g": 1 if now < f["gap_until"] else 0} for f in self._all()]
        self.broadcast({"t": "sb", "p": arr})

    def broadcast_scores(self):
        self.broadcast({"t": "score", "p": [[f["id"], f["score"], f["name"]]
                                            for f in self._all()]})

    def broadcast_round(self):
        self.broadcast({"t": "round", "state": "running",
                        "remaining": self.remaining_sec(), "winner": 0,
                        "nC": len(self._all()), "fw": "PC-MOCK"})

    # ---- tick ----
    def tick(self, now):
        dt = now - self._last
        self._last = now
        if dt > 0.05:
            dt = 0.05

        if self.remaining_sec() <= 0:
            self.restart_round()
            for client, pid in list(self.clients.items()):
                client.send_json({"t": "init", "id": pid, "w": CF_GW, "h": CF_GH})
            self.broadcast({"t": "clear"})
            self.broadcast_round(); self.broadcast_scores()

        if self.mini_active:
            self._update_bots(now)
            for f in self._all():
                if f["alive"]:
                    self._integrate(f, now, dt)
            na = len(self._all())
            al = self._alive_count()
            if (na >= 2 and al <= 1) or (na <= 1 and al == 0):
                if al == 1:
                    for f in self._all():
                        if f["alive"]:
                            f["score"] += 1
                            break
                    self.broadcast_scores()
                self.mini_active = False
                self.mini_restart_at = now + CF_MINI_GAP
        elif now >= self.mini_restart_at:
            self._start_mini(now)

        if now - self._last_sb >= CF_STATE:
            self._last_sb = now; self.broadcast_states()
        if now - self._last_score >= 0.5:
            self._last_score = now; self.broadcast_scores()
        if now - self._last_round >= 1.0:
            self._last_round = now; self.broadcast_round()


# ============================================================================
# 3D Racing simulation
# ============================================================================
RACE_N = 24
RACE_RX = 160.0
RACE_RZ = 110.0
RACE_WIDTH = 18.0
RACE_CP = 26.0
RACE_LAPS = 3
RACE_ROUND = 180
RACE_MAX_PLAYERS = 6
RACE_NUM_BOTS = 2


def race_track():
    pts = []
    for i in range(RACE_N):
        a = i / RACE_N * 2 * math.pi
        rx = 1.0 + 0.18 * math.sin(3 * a)
        rz = 1.0 + 0.10 * math.cos(2 * a)
        pts.append((math.cos(a) * RACE_RX * rx, math.sin(a) * RACE_RZ * rz))
    return pts


class RacingSim:
    slug = "racing"
    name = "3D Racing"

    def __init__(self):
        self.lock = threading.RLock()
        self.clients = {}
        self.players = {}
        self.bots = []
        self.next_id = 1
        self.track = race_track()
        self.round_start = time.time()
        self.ended_until = 0.0
        self.winner = 0
        for i in range(RACE_NUM_BOTS):
            self.bots.append(self._new_bot(RACE_MAX_PLAYERS - i))
        self._last = time.time()
        self._last_sb = self._last_score = self._last_round = 0.0

    def _all(self):
        return list(self.players.values()) + self.bots

    def _find(self, fid):
        p = self.players.get(fid)
        if p:
            return p
        for b in self.bots:
            if b["id"] == fid:
                return b
        return None

    def remaining_sec(self):
        return max(0, int(RACE_ROUND - (time.time() - self.round_start)))

    def _start_pose(self, fid):
        t = self.track
        fx, fz = t[1][0] - t[0][0], t[1][1] - t[0][1]
        fl = math.hypot(fx, fz) or 1
        fx, fz = fx / fl, fz / fl
        rx, rz = fz, -fx
        row = (fid - 1) // 2
        lane = ((fid - 1) % 2) * 8.0 - 4.0
        x = t[0][0] - fx * (8 + row * 7) + rx * lane
        z = t[0][1] - fz * (8 + row * 7) + rz * lane
        return x, z, math.atan2(fx, fz)

    def _new_bot(self, fid):
        x, z, h = self._start_pose(fid)
        return {"id": fid, "name": "BOT-%d" % fid, "x": x, "z": z, "h": h,
                "lap": 0, "next_cp": 1, "speed": random.uniform(52, 62), "is_bot": True}

    # ---- net ----
    def on_text(self, client, m):
        t = m.get("t")
        if t == "join":
            self._join(client, m.get("name", "RACER"))
            return
        if t == "ping":
            client.send_json({"t": "pong"})
            return
        pid = self.clients.get(client)
        if pid is None:
            return
        if t == "s":
            p = self.players.get(pid)
            if p:
                p["x"] = float(m.get("x", 0.0)); p["z"] = float(m.get("z", 0.0))
                p["h"] = float(m.get("h", 0.0))
                self._checkpoints(p)

    def _join(self, client, name):
        if client.joined:
            return
        if len(self.players) >= (RACE_MAX_PLAYERS - RACE_NUM_BOTS):
            client.send_json({"t": "err", "m": "full"})
            client.close()
            return
        pid = self.next_id
        self.next_id += 1
        x, z, h = self._start_pose(pid)
        self.players[pid] = {"id": pid, "name": str(name)[:16] or "RACER",
                             "x": x, "z": z, "h": h, "lap": 0, "next_cp": 1, "is_bot": False}
        self.clients[client] = pid
        client.joined = True
        self._send_init(client, self.players[pid])
        print("[race] join id=%d name=%s" % (pid, self.players[pid]["name"]))
        self.broadcast_round(); self.broadcast_scores()

    def on_disconnect(self, client):
        pid = self.clients.pop(client, None)
        if pid is not None:
            self.players.pop(pid, None)
            self.broadcast({"t": "leave", "id": pid})

    def _send_init(self, client, p):
        client.send_json({"t": "init", "id": p["id"], "width": RACE_WIDTH,
                          "laps": RACE_LAPS, "sx": p["x"], "sz": p["z"], "sh": p["h"],
                          "track": [[round(x, 2), round(z, 2)] for (x, z) in self.track]})

    # ---- sim ----
    def _checkpoints(self, f):
        if self.ended_until:
            return
        cx, cz = self.track[f["next_cp"]]
        if (f["x"] - cx) ** 2 + (f["z"] - cz) ** 2 < RACE_CP * RACE_CP:
            if f["next_cp"] == 0:
                f["lap"] += 1
                f["next_cp"] = 1
                self.broadcast({"t": "lap", "id": f["id"], "lap": f["lap"]})
                self.broadcast_scores()
                if f["lap"] >= RACE_LAPS:
                    self.winner = f["id"]
                    self.ended_until = time.time() + 4.0
                    self.broadcast_round()
            else:
                f["next_cp"] = (f["next_cp"] + 1) % RACE_N

    def _update_bots(self, now, dt):
        for b in self.bots:
            tx, tz = self.track[b["next_cp"]]
            desired = math.atan2(tx - b["x"], tz - b["z"])
            dh = (desired - b["h"] + math.pi) % (2 * math.pi) - math.pi
            maxdh = 2.5 * dt
            b["h"] += max(-maxdh, min(maxdh, dh))
            b["x"] += math.sin(b["h"]) * b["speed"] * dt
            b["z"] += math.cos(b["h"]) * b["speed"] * dt
            self._checkpoints(b)

    def restart_round(self):
        self.track = race_track()
        self.round_start = time.time()
        self.ended_until = 0.0
        self.winner = 0
        for f in self._all():
            x, z, h = self._start_pose(f["id"])
            f["x"], f["z"], f["h"] = x, z, h
            f["lap"] = 0
            f["next_cp"] = 1

    # ---- broadcasts ----
    def broadcast(self, obj, exclude_id=None):
        text = json.dumps(obj, separators=(",", ":"))
        for client, pid in list(self.clients.items()):
            if exclude_id is not None and pid == exclude_id:
                continue
            client.send_text(text)

    def broadcast_states(self):
        if not self.clients:
            return
        arr = [{"id": f["id"], "x": round(f["x"], 2), "z": round(f["z"], 2),
                "h": round(f["h"], 3), "l": f["lap"]} for f in self._all()]
        self.broadcast({"t": "sb", "p": arr})

    def broadcast_scores(self):
        self.broadcast({"t": "score", "p": [[f["id"], f["lap"], f["name"]]
                                            for f in self._all()]})

    def broadcast_round(self):
        ended = self.ended_until and time.time() < self.ended_until
        self.broadcast({"t": "round", "state": "ended" if ended else "running",
                        "remaining": self.remaining_sec(),
                        "winner": self.winner if ended else 0,
                        "nC": len(self._all()), "fw": "PC-MOCK"})

    # ---- tick ----
    def tick(self, now):
        dt = now - self._last
        self._last = now
        if dt > 0.05:
            dt = 0.05

        if self.ended_until:
            if now >= self.ended_until:
                self.restart_round()
                for client, pid in list(self.clients.items()):
                    p = self.players.get(pid)
                    if p:
                        self._send_init(client, p)
                self.broadcast_round(); self.broadcast_scores()
        else:
            if self.remaining_sec() <= 0:
                self.winner = max(self._all(), key=lambda f: f["lap"])["id"] if self._all() else 0
                self.ended_until = now + 4.0
                self.broadcast_round()
            else:
                self._update_bots(now, dt)

        if now - self._last_sb >= 0.1:
            self._last_sb = now; self.broadcast_states()
        if now - self._last_score >= 0.5:
            self._last_score = now; self.broadcast_scores()
        if now - self._last_round >= 1.0:
            self._last_round = now; self.broadcast_round()


# ============================================================================
# Snake Arena simulation
# ============================================================================
SNK_GW = 32
SNK_GH = 24
SNK_STEP = 0.12
SNK_MAXLEN = 96
SNK_GROW = 3
SNK_FOOD = 5
SNK_RESPAWN = 1.5
SNK_ROUND = 180
SNK_MAX_PLAYERS = 6
SNK_NUM_BOTS = 2
SNK_FX = [0.20, 0.80, 0.50, 0.20, 0.80, 0.50]
SNK_FY = [0.25, 0.25, 0.50, 0.75, 0.75, 0.50]
SNK_DIRS = [(1, 0), (-1, 0), (0, 1), (0, -1)]


class SnakeSim:
    slug = "snake"
    name = "Snake Arena"

    def __init__(self):
        self.lock = threading.RLock()
        self.clients = {}
        self.players = {}
        self.bots = []
        self.next_id = 1
        self.occ = bytearray(SNK_GW * SNK_GH)
        self.food = {}      # id -> (x,y)
        self.next_food = 1
        self.round_start = time.time()
        self.accum = 0.0
        for i in range(SNK_NUM_BOTS):
            self.bots.append(self._new_fighter(SNK_MAX_PLAYERS - i, "BOT-%d" % (SNK_MAX_PLAYERS - i), True))
        self._reset_all()
        self._last = time.time()
        self._last_score = self._last_round = 0.0

    def _all(self):
        return list(self.players.values()) + self.bots

    def _find(self, fid):
        p = self.players.get(fid)
        if p:
            return p
        for b in self.bots:
            if b["id"] == fid:
                return b
        return None

    def remaining_sec(self):
        return max(0, int(SNK_ROUND - (time.time() - self.round_start)))

    def _new_fighter(self, fid, name, is_bot):
        return {"id": fid, "name": name, "cells": [], "dx": 1, "dy": 0,
                "pdx": 1, "pdy": 0, "alive": False, "score": 0, "grow": 0,
                "respawn_at": 0.0, "is_bot": is_bot}

    def _spawn(self, f):
        k = (f["id"] - 1) % 6
        x = int(SNK_FX[k] * SNK_GW)
        y = int(SNK_FY[k] * SNK_GH)
        if self.occ[y * SNK_GW + x]:
            x, y = self._free_cell()
        f["cells"] = [(x, y)]
        f["dx"] = f["pdx"] = 1 if x < SNK_GW // 2 else -1
        f["dy"] = f["pdy"] = 0
        f["alive"] = True
        f["grow"] = 0
        self.occ[y * SNK_GW + x] = f["id"]

    def _free_cell(self):
        for _ in range(300):
            x, y = random.randrange(SNK_GW), random.randrange(SNK_GH)
            if not self.occ[y * SNK_GW + x] and (x, y) not in self.food.values():
                return x, y
        return 1, 1

    def _spawn_food(self):
        x, y = self._free_cell()
        fid = self.next_food
        self.next_food += 1
        self.food[fid] = (x, y)
        self.broadcast({"t": "food", "x": x, "y": y})

    def _reset_all(self):
        self.occ = bytearray(SNK_GW * SNK_GH)
        self.food = {}
        for f in self._all():
            f["score"] = 0
            f["cells"] = []
            f["respawn_at"] = 0.0
            self._spawn(f)
        for _ in range(SNK_FOOD):
            self._spawn_food()
        self.round_start = time.time()

    # ---- net ----
    def on_text(self, client, m):
        t = m.get("t")
        if t == "join":
            self._join(client, m.get("name", "PLAYER"))
            return
        if t == "ping":
            client.send_json({"t": "pong"})
            return
        pid = self.clients.get(client)
        if pid is None:
            return
        if t == "d":
            f = self.players.get(pid)
            if f:
                dx, dy = int(m.get("x", 0)), int(m.get("y", 0))
                if dx:
                    dx, dy = (1 if dx > 0 else -1), 0
                elif dy:
                    dx, dy = 0, (1 if dy > 0 else -1)
                else:
                    return
                if len(f["cells"]) > 1 and dx == -f["dx"] and dy == -f["dy"]:
                    return
                f["pdx"], f["pdy"] = dx, dy

    def _join(self, client, name):
        if client.joined:
            return
        if len(self.players) >= (SNK_MAX_PLAYERS - SNK_NUM_BOTS):
            client.send_json({"t": "err", "m": "full"})
            client.close()
            return
        pid = self.next_id
        self.next_id += 1
        f = self._new_fighter(pid, str(name)[:16] or "PLAYER", False)
        self.players[pid] = f
        self._spawn(f)
        self.clients[client] = pid
        client.joined = True
        self._send_init(client, f)
        print("[snk] join id=%d name=%s" % (pid, f["name"]))
        self.broadcast_round(); self.broadcast_scores()

    def on_disconnect(self, client):
        pid = self.clients.pop(client, None)
        if pid is not None:
            f = self.players.pop(pid, None)
            if f:
                for (x, y) in f["cells"]:
                    if self.occ[y * SNK_GW + x] == pid:
                        self.occ[y * SNK_GW + x] = 0
            self.broadcast({"t": "leave", "id": pid})

    def _send_init(self, client, p):
        client.send_json({
            "t": "init", "id": p["id"], "w": SNK_GW, "h": SNK_GH,
            "snakes": [{"id": f["id"], "a": 1 if f["alive"] else 0,
                        "c": [[x, y] for (x, y) in f["cells"]]} for f in self._all()],
            "food": [[x, y] for (x, y) in self.food.values()],
        })

    # ---- sim ----
    def _kill(self, f):
        for (x, y) in f["cells"]:
            if self.occ[y * SNK_GW + x] == f["id"]:
                self.occ[y * SNK_GW + x] = 0
        f["cells"] = []
        f["alive"] = False
        f["respawn_at"] = time.time() + SNK_RESPAWN
        self.broadcast({"t": "dead", "id": f["id"]})

    def _bot_choose(self, b):
        head = b["cells"][0]
        opts = []
        for dx, dy in SNK_DIRS:
            if len(b["cells"]) > 1 and dx == -b["dx"] and dy == -b["dy"]:
                continue
            nx, ny = head[0] + dx, head[1] + dy
            if nx < 0 or ny < 0 or nx >= SNK_GW or ny >= SNK_GH:
                continue
            if self.occ[ny * SNK_GW + nx]:
                continue
            opts.append((dx, dy, nx, ny))
        if not opts:
            return
        if self.food and random.random() < 0.85:
            tx, ty = min(self.food.values(),
                         key=lambda c: (c[0] - head[0]) ** 2 + (c[1] - head[1]) ** 2)
            opts.sort(key=lambda o: (o[2] - tx) ** 2 + (o[3] - ty) ** 2)
            best = opts[0]
        else:
            best = random.choice(opts)
        b["pdx"], b["pdy"] = best[0], best[1]

    def _do_step(self):
        for b in self.bots:
            if b["alive"]:
                self._bot_choose(b)
        fl = self._all()
        new = {}
        dead = set()
        for f in fl:
            if not f["alive"]:
                continue
            if not (f["pdx"] == -f["dx"] and f["pdy"] == -f["dy"]):
                f["dx"], f["dy"] = f["pdx"], f["pdy"]
            hx, hy = f["cells"][0]
            nx, ny = hx + f["dx"], hy + f["dy"]
            if nx < 0 or ny < 0 or nx >= SNK_GW or ny >= SNK_GH:
                dead.add(f["id"])
            else:
                new[f["id"]] = (nx, ny)
        ids = list(new.keys())
        for i in range(len(ids)):
            for j in range(i + 1, len(ids)):
                if new[ids[i]] == new[ids[j]]:
                    dead.add(ids[i]); dead.add(ids[j])
        for fid, (nx, ny) in new.items():
            if self.occ[ny * SNK_GW + nx]:
                dead.add(fid)
        for f in fl:
            if f["id"] in dead and f["alive"]:
                self._kill(f)
        moved = []
        ate_any = False
        for f in fl:
            if not f["alive"] or f["id"] in dead:
                continue
            nx, ny = new[f["id"]]
            ate = None
            for k, (fx, fy) in list(self.food.items()):
                if fx == nx and fy == ny:
                    ate = k; break
            f["cells"].insert(0, (nx, ny))
            self.occ[ny * SNK_GW + nx] = f["id"]
            if ate is not None:
                f["score"] += 1
                ate_any = True
                fx, fy = self.food.pop(ate)
                self.broadcast({"t": "eat", "x": fx, "y": fy})
                self._spawn_food()
                f["grow"] += SNK_GROW
            if f["grow"] > 0 and len(f["cells"]) < SNK_MAXLEN:
                f["grow"] -= 1
                kept = True
            else:
                tx, ty = f["cells"].pop()
                if self.occ[ty * SNK_GW + tx] == f["id"]:
                    self.occ[ty * SNK_GW + tx] = 0
                kept = False
            moved.append([f["id"], nx, ny, 1 if kept else 0])
        if moved:
            self.broadcast({"t": "step", "m": moved})
        if ate_any:
            self.broadcast_scores()

    # ---- broadcasts ----
    def broadcast(self, obj, exclude_id=None):
        text = json.dumps(obj, separators=(",", ":"))
        for client, pid in list(self.clients.items()):
            if exclude_id is not None and pid == exclude_id:
                continue
            client.send_text(text)

    def broadcast_scores(self):
        self.broadcast({"t": "score", "p": [[f["id"], f["score"], f["name"]]
                                            for f in self._all()]})

    def broadcast_round(self):
        self.broadcast({"t": "round", "state": "running",
                        "remaining": self.remaining_sec(), "winner": 0,
                        "nC": len(self._all()), "fw": "PC-MOCK"})

    # ---- tick ----
    def tick(self, now):
        dt = now - self._last
        self._last = now
        if dt > 0.3:
            dt = 0.3
        if self.remaining_sec() <= 0:
            self._reset_all()
            for client, pid in list(self.clients.items()):
                p = self.players.get(pid)
                if p:
                    self._send_init(client, p)
            self.broadcast_round(); self.broadcast_scores()
        self.accum += dt
        guard = 0
        while self.accum >= SNK_STEP and guard < 5:
            self.accum -= SNK_STEP
            guard += 1
            self._do_step()
        for f in self.players.values():
            if not f["alive"] and f["respawn_at"] and now >= f["respawn_at"]:
                self._spawn(f); f["respawn_at"] = 0.0
                self.broadcast({"t": "respawn", "id": f["id"],
                                "x": f["cells"][0][0], "y": f["cells"][0][1]})
        for b in self.bots:
            if not b["alive"] and b["respawn_at"] and now >= b["respawn_at"]:
                self._spawn(b); b["respawn_at"] = 0.0
                self.broadcast({"t": "respawn", "id": b["id"],
                                "x": b["cells"][0][0], "y": b["cells"][0][1]})
        if now - self._last_score >= 0.5:
            self._last_score = now; self.broadcast_scores()
        if now - self._last_round >= 1.0:
            self._last_round = now; self.broadcast_round()


# ============================================================================
# Pong simulation
# ============================================================================
PONG_PADDLE_LEN = 0.20
PONG_PADDLE_T = 0.03
PONG_BALL_R = 0.018
PONG_BALL_SPEED = 0.55
PONG_BALL_MAX = 1.5
PONG_SPEEDUP = 1.04
PONG_ROUND = 180
PONG_EDGES = 4
PONG_BOT_EDGES = [1, 2, 3]   # bots fill these; the human takes edge 0


class PongSim:
    slug = "bounce"
    name = "Bounce"

    def __init__(self):
        self.lock = threading.RLock()
        self.clients = {}
        self.players = {}    # id -> fighter (humans)
        self.bots = []
        self.next_id = 1
        self.round_start = time.time()
        self.bx = self.by = 0.5
        self.bvx = self.bvy = 0.0
        self.last_hit = 0
        self._reset_ball()
        # bots on edges 1,2,3 (ids 4,5,6)
        for i, e in enumerate(PONG_BOT_EDGES):
            self.bots.append({"id": 4 + i, "name": "BOT-%d" % (4 + i), "edge": e,
                              "v": 0.5, "score": 0, "is_bot": True})
        self._last = time.time()
        self._last_sb = self._last_score = self._last_round = 0.0

    def _all(self):
        return list(self.players.values()) + self.bots

    def _find(self, fid):
        p = self.players.get(fid)
        if p:
            return p
        for b in self.bots:
            if b["id"] == fid:
                return b
        return None

    def _on_edge(self, e):
        for f in self._all():
            if f["edge"] == e:
                return f
        return None

    def remaining_sec(self):
        return max(0, int(PONG_ROUND - (time.time() - self.round_start)))

    def _reset_ball(self):
        self.bx = self.by = 0.5
        t = random.uniform(0.45, 1.12)
        sx = random.choice([-1, 1]); sy = random.choice([-1, 1])
        self.bvx = math.cos(t) * PONG_BALL_SPEED * sx
        self.bvy = math.sin(t) * PONG_BALL_SPEED * sy
        self.last_hit = 0

    # ---- net ----
    def on_text(self, client, m):
        t = m.get("t")
        if t == "join":
            self._join(client, m.get("name", "PLAYER"))
            return
        if t == "ping":
            client.send_json({"t": "pong"})
            return
        pid = self.clients.get(client)
        if pid is None:
            return
        if t == "p":
            f = self.players.get(pid)
            if f:
                v = float(m.get("v", 0.5))
                f["v"] = max(0.0, min(1.0, v))

    def _join(self, client, name):
        if client.joined:
            return
        edge = -1
        for e in range(PONG_EDGES):
            if not self._on_edge(e):
                edge = e; break
        if edge < 0:
            client.send_json({"t": "err", "m": "full"})
            client.close()
            return
        pid = self.next_id
        self.next_id += 1
        self.players[pid] = {"id": pid, "name": str(name)[:16] or "PLAYER",
                             "edge": edge, "v": 0.5, "score": 0, "is_bot": False}
        self.clients[client] = pid
        client.joined = True
        client.send_json({"t": "init", "id": pid, "edge": edge,
                          "plen": PONG_PADDLE_LEN, "pt": PONG_PADDLE_T})
        print("[pong] join id=%d edge=%d" % (pid, edge))
        self.broadcast_round(); self.broadcast_scores()

    def on_disconnect(self, client):
        pid = self.clients.pop(client, None)
        if pid is not None:
            self.players.pop(pid, None)
            self.broadcast({"t": "leave", "id": pid})

    # ---- sim ----
    def _score_miss(self, miss):
        scorer = 0
        if self.last_hit and miss and self.last_hit != miss["id"]:
            s = self._find(self.last_hit)
            if s:
                s["score"] += 1; scorer = s["id"]
        self.broadcast({"t": "point", "scorer": scorer, "on": miss["id"] if miss else 0})
        self.broadcast_scores()
        self._reset_ball()

    def _set_speed(self, sp):
        m = math.hypot(self.bvx, self.bvy)
        if m < 1e-5:
            self.bvx = sp; self.bvy = 0; return
        self.bvx *= sp / m; self.bvy *= sp / m

    def _step_ball(self, dt):
        T, R = PONG_PADDLE_T, PONG_BALL_R
        half = PONG_PADDLE_LEN * 0.5 + R
        sp = math.hypot(self.bvx, self.bvy)
        n = max(1, min(8, int(math.ceil(sp * dt / 0.015))))
        h = dt / n
        for _ in range(n):
            self.bx += self.bvx * h
            self.by += self.bvy * h
            if self.bvx < 0 and self.bx <= T:
                pl = self._on_edge(0)
                if pl:
                    if abs(self.by - pl["v"]) <= half:
                        self.bx = T; ns = min(PONG_BALL_MAX, sp * PONG_SPEEDUP)
                        self.bvx = abs(self.bvx); self.bvy += (self.by - pl["v"]) * 2.0
                        self._set_speed(ns); self.last_hit = pl["id"]
                    elif self.bx <= R:
                        self._score_miss(pl); return
                elif self.bx <= R:
                    self.bvx = abs(self.bvx); self.bx = R
            elif self.bvx > 0 and self.bx >= 1 - T:
                pl = self._on_edge(1)
                if pl:
                    if abs(self.by - pl["v"]) <= half:
                        self.bx = 1 - T; ns = min(PONG_BALL_MAX, sp * PONG_SPEEDUP)
                        self.bvx = -abs(self.bvx); self.bvy += (self.by - pl["v"]) * 2.0
                        self._set_speed(ns); self.last_hit = pl["id"]
                    elif self.bx >= 1 - R:
                        self._score_miss(pl); return
                elif self.bx >= 1 - R:
                    self.bvx = -abs(self.bvx); self.bx = 1 - R
            if self.bvy < 0 and self.by <= T:
                pl = self._on_edge(2)
                if pl:
                    if abs(self.bx - pl["v"]) <= half:
                        self.by = T; ns = min(PONG_BALL_MAX, sp * PONG_SPEEDUP)
                        self.bvy = abs(self.bvy); self.bvx += (self.bx - pl["v"]) * 2.0
                        self._set_speed(ns); self.last_hit = pl["id"]
                    elif self.by <= R:
                        self._score_miss(pl); return
                elif self.by <= R:
                    self.bvy = abs(self.bvy); self.by = R
            elif self.bvy > 0 and self.by >= 1 - T:
                pl = self._on_edge(3)
                if pl:
                    if abs(self.bx - pl["v"]) <= half:
                        self.by = 1 - T; ns = min(PONG_BALL_MAX, sp * PONG_SPEEDUP)
                        self.bvy = -abs(self.bvy); self.bvx += (self.bx - pl["v"]) * 2.0
                        self._set_speed(ns); self.last_hit = pl["id"]
                    elif self.by >= 1 - R:
                        self._score_miss(pl); return
                elif self.by >= 1 - R:
                    self.bvy = -abs(self.bvy); self.by = 1 - R

    def _update_bots(self, dt):
        for b in self.bots:
            target = self.by if b["edge"] in (0, 1) else self.bx
            d = target - b["v"]
            step = 1.3 * dt   # bot paddle speed (fraction/sec); imperfect so beatable
            b["v"] += max(-step, min(step, d))
            b["v"] = max(0.0, min(1.0, b["v"]))

    # ---- broadcasts ----
    def broadcast(self, obj, exclude_id=None):
        text = json.dumps(obj, separators=(",", ":"))
        for client, pid in list(self.clients.items()):
            if exclude_id is not None and pid == exclude_id:
                continue
            client.send_text(text)

    def broadcast_states(self):
        if not self.clients:
            return
        self.broadcast({"t": "sb", "b": [round(self.bx, 4), round(self.by, 4)],
                        "pd": [[f["id"], f["edge"], round(f["v"], 4)] for f in self._all()]})

    def broadcast_scores(self):
        self.broadcast({"t": "score", "p": [[f["id"], f["score"], f["name"]] for f in self._all()]})

    def broadcast_round(self):
        self.broadcast({"t": "round", "state": "running",
                        "remaining": self.remaining_sec(), "winner": 0,
                        "nC": len(self._all()), "fw": "PC-MOCK"})

    # ---- tick ----
    def tick(self, now):
        dt = now - self._last
        self._last = now
        if dt > 0.05:
            dt = 0.05
        if self.remaining_sec() <= 0:
            for f in self._all():
                f["score"] = 0
            self._reset_ball()
            self.round_start = time.time()
            self.broadcast_round(); self.broadcast_scores()
        self._update_bots(dt)
        self._step_ball(dt)
        if now - self._last_sb >= 0.033:
            self._last_sb = now; self.broadcast_states()
        if now - self._last_score >= 0.5:
            self._last_score = now; self.broadcast_scores()
        if now - self._last_round >= 1.0:
            self._last_round = now; self.broadcast_round()


# ============================================================================
# HTTP / WebSocket plumbing (shared)
# ============================================================================
ACTIVE = None   # set in main()


def serve_static(sock, path):
    rel = path.split("?")[0]
    if rel == "/" or rel == "":
        sock.sendall(("HTTP/1.1 302 Found\r\nLocation: /%s/\r\n"
                      "Content-Length: 0\r\nConnection: close\r\n\r\n"
                      % ACTIVE.slug).encode("ascii"))
        return
    if rel.endswith("/"):
        rel += "index.html"
    rel = rel.lstrip("/")
    full = os.path.normpath(os.path.join(DATA_DIR, rel))
    if not full.startswith(DATA_DIR):
        sock.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
                     b"Connection: close\r\n\r\n")
        return
    extra_headers = ""
    if not os.path.isfile(full):
        if os.path.isfile(full + ".gz"):
            full += ".gz"
            extra_headers = "Content-Encoding: gzip\r\n"
        else:
            body = b"404 Not Found"
            sock.sendall((b"HTTP/1.1 404 Not Found\r\nContent-Length: %d\r\n"
                          b"Connection: close\r\n\r\n" % len(body)) + body)
            return
    logical = full[:-3] if full.endswith(".gz") else full
    ext = os.path.splitext(logical)[1].lower()
    ctype = CONTENT_TYPES.get(ext, "application/octet-stream")
    with open(full, "rb") as f:
        body = f.read()
    header = ("HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n%s"
              "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
              % (ctype, len(body), extra_headers)).encode("ascii")
    sock.sendall(header + body)


def do_ws_handshake(sock, headers):
    key = headers.get("sec-websocket-key", "")
    accept = base64.b64encode(
        hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")
    sock.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n"
                  % accept).encode("ascii"))


def handle_connection(sock, addr):
    sock.settimeout(None)
    try:
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = sock.recv(4096)
            if not chunk:
                sock.close(); return
            data += chunk
            if len(data) > 65536:
                break
        head_text = data.split(b"\r\n\r\n", 1)[0].decode("latin-1")
        lines = head_text.split("\r\n")
        parts = lines[0].split(" ")
        path = parts[1] if len(parts) > 1 else "/"
        headers = {}
        for ln in lines[1:]:
            if ":" in ln:
                k, v = ln.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        if headers.get("upgrade", "").lower() == "websocket" and path.startswith("/ws"):
            do_ws_handshake(sock, headers)
            ws_loop(WSClient(sock, addr))
        else:
            serve_static(sock, path)
            sock.close()
    except OSError:
        try:
            sock.close()
        except OSError:
            pass


def ws_loop(client):
    print("[ws] connect from %s" % (client.addr,))
    try:
        while client.open:
            frame = read_ws_frame(client.sock)
            if frame is None:
                break
            opcode, payload = frame
            if opcode == 0x8:
                break
            elif opcode == 0x9:
                with client.send_lock:
                    client.sock.sendall(encode_ws_frame(payload, opcode=0xA))
            elif opcode == 0x1:
                try:
                    msg = json.loads(payload.decode("utf-8"))
                except (ValueError, UnicodeDecodeError):
                    continue
                with ACTIVE.lock:
                    ACTIVE.on_text(client, msg)
    except OSError:
        pass
    finally:
        with ACTIVE.lock:
            ACTIVE.on_disconnect(client)
        client.close()
        print("[ws] disconnect %s" % (client.addr,))


def game_loop():
    while True:
        now = time.time()
        with ACTIVE.lock:
            ACTIVE.tick(now)
        time.sleep(0.02)


# ============================================================================
# Main
# ============================================================================
SIMS = {"dogfight": DogfightSim, "blaster": BombermanSim,
        "trails": CurveFeverSim, "racing": RacingSim, "snake": SnakeSim,
        "bounce": PongSim}


def main():
    global ACTIVE
    if not os.path.isdir(DATA_DIR):
        raise SystemExit("data/ not found: %s" % DATA_DIR)

    choice = (sys.argv[1].lower() if len(sys.argv) > 1 else "dogfight")
    if choice not in SIMS:
        raise SystemExit("unknown game '%s' - choose one of: %s"
                         % (choice, ", ".join(SIMS)))
    ACTIVE = SIMS[choice]()

    threading.Thread(target=game_loop, daemon=True).start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(16)
    print("=" * 56)
    print(" Core2 mock server - hosting: %s" % ACTIVE.name)
    print("   Browser:  http://localhost:%d" % PORT)
    print("   Switch game: uv run mock_server.py [%s]" % "|".join(SIMS))
    print("   Stop with Ctrl+C")
    print("=" * 56)

    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=handle_connection, args=(conn, addr),
                             daemon=True).start()
    except KeyboardInterrupt:
        print("\n[server] stopped")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
