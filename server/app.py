"""JAGA API: polls the ESP32 master into SQLite and serves the same /api/* shape, plus /api/trend."""
import json
import os
import sqlite3
import threading
import time
import urllib.request
from contextlib import asynccontextmanager, closing

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

from trend import analyze

MASTER = os.environ.get("MASTER_URL", "http://jaga-master.local").rstrip("/")  # use the IP in Docker: no mDNS
DB = os.environ.get("DB_PATH", "/data/jaga.db")
POLL_S = 10
COLS = ("log_timestamp_s", "node_id", "sender_timestamp_s", "temperature_c", "humidity_pct",
        "distance_mm", "distance_stddev_mm", "water_level_mm")

master = {"status": None, "status_at": 0.0, "nodes": None}


def db():
    c = sqlite3.connect(DB, timeout=10)
    c.row_factory = sqlite3.Row
    return c


def init_db():
    with closing(db()) as c, c:
        c.execute("""CREATE TABLE IF NOT EXISTS readings (
            id INTEGER PRIMARY KEY, received_at REAL NOT NULL, log_timestamp_s INT, node_id INT,
            sender_timestamp_s INT, temperature_c REAL, humidity_pct REAL, distance_mm INT,
            distance_stddev_mm INT, water_level_mm INT)""")
        c.execute("CREATE TABLE IF NOT EXISTS node_pos (node_id INTEGER PRIMARY KEY, lat REAL NOT NULL, lng REAL NOT NULL)")
        c.execute("CREATE INDEX IF NOT EXISTS readings_node ON readings (node_id, id)")


def fetch(path):
    with urllib.request.urlopen(MASTER + path, timeout=5) as r:
        return json.load(r)


def poll_once():
    master["status"], master["status_at"] = fetch("/api/status"), time.time()
    master["nodes"] = fetch("/api/nodes")
    rows = fetch("/api/latest")
    with closing(db()) as c, c:
        for r in rows:
            last = c.execute("SELECT log_timestamp_s, sender_timestamp_s FROM readings "
                             "WHERE node_id=? ORDER BY id DESC LIMIT 1", (r["node_id"],)).fetchone()
            # master re-serves the same newest reading until a new one lands; store each once
            if last and (last[0], last[1]) == (r["log_timestamp_s"], r["sender_timestamp_s"]):
                continue
            c.execute(f"INSERT INTO readings (received_at, {','.join(COLS)}) VALUES (?{',?' * len(COLS)})",
                      (time.time(), *[r[k] for k in COLS]))


def poll_loop():
    while True:
        try:
            poll_once()
        except Exception as e:  # master offline: keep serving stored data
            print("poll failed:", e, flush=True)
        time.sleep(POLL_S)


@asynccontextmanager
async def lifespan(_):
    init_db()
    threading.Thread(target=poll_loop, daemon=True).start()
    yield


app = FastAPI(lifespan=lifespan)


def newest(node, limit):
    with closing(db()) as c:
        return c.execute("SELECT * FROM readings WHERE node_id=? ORDER BY id DESC LIMIT ?",
                         (node, limit)).fetchall()


def as_reading(row):
    return {k: row[k] for k in COLS}


def check_node(node):
    if node not in (1, 2, 3):
        raise HTTPException(400, "node must be 1, 2 or 3")


@app.get("/api/status")
def status():
    st = master["status"]
    if not st:
        raise HTTPException(503, "master not reached yet")
    elapsed = time.time() - master["status_at"]  # keep now_s ticking while the master is unreachable
    return {**st, "now_s": st["now_s"] + int(elapsed), "uptime_s": st["uptime_s"] + int(elapsed),
            "master_age_s": int(elapsed)}


@app.get("/api/nodes")
def nodes():
    with closing(db()) as c:  # positions set from the dashboard override the master's placeholders
        pos = {r["node_id"]: r for r in c.execute("SELECT * FROM node_pos")}
    # master offline: still serve saved positions (ids only) so the map is right before the master is up
    base = master["nodes"] or [{"node_id": i} for i in pos]
    return [{**n, "lat": pos[n["node_id"]]["lat"], "lng": pos[n["node_id"]]["lng"]} if n["node_id"] in pos else n
            for n in base]


class Pos(BaseModel):
    lat: float = Field(ge=-90, le=90)
    lng: float = Field(ge=-180, le=180)


@app.put("/api/nodes/{node_id}")
def set_node_pos(node_id: int, pos: Pos):
    check_node(node_id)
    with closing(db()) as c, c:
        c.execute("INSERT INTO node_pos VALUES (?,?,?) ON CONFLICT(node_id) DO UPDATE SET lat=excluded.lat, lng=excluded.lng",
                  (node_id, pos.lat, pos.lng))
    return {"node_id": node_id, "lat": pos.lat, "lng": pos.lng}


@app.get("/api/latest")
def latest():
    return [as_reading(r) for n in (1, 2, 3) for r in newest(n, 1)]


@app.get("/api/history")
def history(node: int, limit: int = 90):
    check_node(node)
    return [as_reading(r) for r in newest(node, max(1, min(limit, 5000)))]


@app.get("/api/trend")
def trend(warn: float = 700, danger: float = 900):
    out = []
    for n in (1, 2, 3):
        rows = newest(n, 200)[::-1]
        out.append({"node_id": n, **analyze([(r["received_at"], r["water_level_mm"]) for r in rows], warn, danger)})
    return out
