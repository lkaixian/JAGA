"""JAGA API: polls the ESP32 master into SQLite and serves the same /api/* shape, plus /api/trend."""
import json
import os
import sqlite3
import threading
import time
import traceback
import urllib.request
from contextlib import asynccontextmanager, closing

from fastapi import FastAPI, HTTPException
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field
from starlette.exceptions import HTTPException as StarletteHTTPException

from trend import analyze

MASTER = os.environ.get("MASTER_URL", "http://jaga-master.local").rstrip("/")  # use the IP in Docker: no mDNS
DB = os.environ.get("DB_PATH", "/data/jaga.db")
POLL_S = 10
RETAIN_DAYS = 180  # readings older than this are deleted, once a day
API_VERSION = 1  # bump on a breaking change so old apps can say so
COLS = ("log_timestamp_s", "node_id", "sender_timestamp_s", "temperature_c", "humidity_pct",
        "distance_mm", "distance_stddev_mm", "water_level_mm")

master = {"status": None, "status_at": 0.0, "nodes": None}
health = {"master_ok": False, "last_ok": 0.0, "fails": 0, "db_ok": True}
rejected = 0  # master rows dropped as malformed since start
last_prune = 0.0


def db():
    c = sqlite3.connect(DB, timeout=10)
    c.row_factory = sqlite3.Row
    c.execute("PRAGMA journal_mode=WAL")  # readers don't block the poll writer
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


def valid(r):
    if not isinstance(r, dict) or not all(k in r for k in COLS):
        return False
    num = lambda v, t: isinstance(v, t) and not isinstance(v, bool)
    return (num(r["node_id"], int) and r["node_id"] >= 1
            and all(num(r[k], int) for k in COLS if k.endswith(("_s", "_mm")))
            and all(num(r[k], (int, float)) for k in ("temperature_c", "humidity_pct")))


def store(rows):
    global rejected, last_prune
    good = [r for r in rows if valid(r)]
    rejected += len(rows) - len(good)
    with closing(db()) as c, c:
        if time.time() - last_prune >= 86400:
            n = c.execute("DELETE FROM readings WHERE received_at < ?", (time.time() - RETAIN_DAYS * 86400,)).rowcount
            last_prune = time.time()
            if n:
                print(f"pruned {n} readings older than {RETAIN_DAYS} days", flush=True)
        for r in good:
            last = c.execute("SELECT log_timestamp_s, sender_timestamp_s FROM readings "
                             "WHERE node_id=? ORDER BY id DESC LIMIT 1", (r["node_id"],)).fetchone()
            # master re-serves the same newest reading until a new one lands; store each once
            if last and (last[0], last[1]) == (r["log_timestamp_s"], r["sender_timestamp_s"]):
                continue
            c.execute(f"INSERT INTO readings (received_at, {','.join(COLS)}) VALUES (?{',?' * len(COLS)})",
                      (time.time(), *[r[k] for k in COLS]))


def poll_once():
    ok = 0
    for key, path in (("status", "/api/status"), ("nodes", "/api/nodes")):
        try:
            master[key] = fetch(path)
            ok += 1
            if key == "status":
                master["status_at"] = time.time()
        except Exception:  # one endpoint failing must not block the others
            pass
    try:
        rows = fetch("/api/latest")
        if not isinstance(rows, list):
            raise ValueError("latest is not a list")
        ok += 1
    except Exception:
        rows = []
    try:
        store(rows)
        if not health["db_ok"]:
            health["db_ok"] = True
            print("database writable again", flush=True)
    except sqlite3.Error as e:  # the master answered fine, so this is not a failed poll
        if health["db_ok"]:
            health["db_ok"] = False
            print("database error: readings are not being stored:", repr(e), flush=True)
    was = health["master_ok"]
    if ok:
        health.update(master_ok=True, last_ok=time.time(), fails=0)
    else:
        health["fails"] += 1
        if health["fails"] >= 3:  # about 30 s at the normal poll rate
            health["master_ok"] = False
    if was and not health["master_ok"]:
        print("master unreachable: readings have stopped", flush=True)
    elif not was and health["master_ok"]:
        print("master reachable again", flush=True)


def poll_loop():
    while True:
        poll_once()
        time.sleep(min(30, POLL_S * max(1, health["fails"])))  # back off 10, 20, 30 s while polls fail


@asynccontextmanager
async def lifespan(_):
    init_db()
    threading.Thread(target=poll_loop, daemon=True).start()
    yield


app = FastAPI(lifespan=lifespan)


def err(status, code, message):
    return JSONResponse({"error": {"code": code, "message": message}}, status)


@app.exception_handler(StarletteHTTPException)
async def on_http(_, e):
    if e.status_code < 500:
        return err(e.status_code, "bad_request", str(e.detail).rstrip(".") + ".")
    return err(e.status_code, "internal", "The server hit an unexpected problem.")


@app.exception_handler(RequestValidationError)
async def on_invalid(_, e):
    return err(422, "bad_request", "The request has a missing or invalid value.")


@app.exception_handler(sqlite3.Error)
async def on_db(_, e):
    print("db error:", repr(e), flush=True)
    return err(503, "db_unavailable", "The server can't open its database.")


@app.exception_handler(Exception)
async def on_unhandled(_, e):
    traceback.print_exception(e)
    return err(500, "internal", "The server hit an unexpected problem.")


def newest(node, limit):
    with closing(db()) as c:
        return c.execute("SELECT * FROM readings WHERE node_id=? ORDER BY id DESC LIMIT ?",
                         (node, limit)).fetchall()


def as_reading(row):
    return {**{k: row[k] for k in COLS}, "received_at": row["received_at"]}  # server clock, unix seconds


def node_ids():
    with closing(db()) as c:
        return [r[0] for r in c.execute("SELECT DISTINCT node_id FROM readings ORDER BY node_id")]


def check_node(node):
    if node < 1:
        raise HTTPException(400, "The node number must be 1 or higher")


@app.get("/api/health")
def healthz():
    try:
        with closing(db()) as c:
            c.execute("SELECT 1 FROM readings LIMIT 1")
    except sqlite3.Error:
        return err(503, "db_unavailable", "The server can't open its database.")
    if not health["db_ok"]:
        return err(503, "db_unavailable", "The server can't open its database.")
    return {"ok": True, "rejected": rejected}


@app.get("/api/status")
def status():
    st = master["status"]
    if not st:
        return err(503, "master_unreachable", "The server hasn't reached the master node yet.")
    elapsed = time.time() - master["status_at"]  # keep now_s ticking while the master is unreachable
    return {**st, "now_s": st["now_s"] + int(elapsed), "uptime_s": st["uptime_s"] + int(elapsed),
            "master_age_s": int(elapsed), "master_ok": health["master_ok"],
            "api_version": API_VERSION}


@app.get("/api/nodes")
def nodes():
    with closing(db()) as c:  # positions set from the dashboard override the master's placeholders
        pos = {r["node_id"]: r for r in c.execute("SELECT * FROM node_pos")}
    # master offline: still serve saved positions (ids only) so the map is right before the master is up
    base = master["nodes"] or [{"node_id": i} for i in pos]
    return [{**n, "lat": pos[n["node_id"]]["lat"], "lng": pos[n["node_id"]]["lng"]} if n["node_id"] in pos else n
            for n in base]


@app.get("/api/site")
def site():
    """Name + centre point, so the app can show this server in its lobby."""
    pts = [(n["lat"], n["lng"]) for n in nodes() if "lat" in n]
    return {"name": os.environ.get("SITE_NAME", "JAGA"),
            "lat": sum(p[0] for p in pts) / len(pts) if pts else None,
            "lng": sum(p[1] for p in pts) / len(pts) if pts else None}


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
    return [as_reading(r) for n in node_ids() for r in newest(n, 1)]


@app.get("/api/history")
def history(node: int, limit: int = 90):
    check_node(node)
    return [as_reading(r) for r in newest(node, max(1, min(limit, 5000)))]


@app.get("/api/trend")
def trend(warn: float = 700, danger: float = 900):
    out = []
    for n in node_ids():
        rows = newest(n, 200)[::-1]
        out.append({"node_id": n, **analyze([(r["received_at"], r["water_level_mm"]) for r in rows], warn, danger)})
    return out
