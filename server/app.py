"""JAGA API: polls the ESP32 master into SQLite and serves the same /api/* shape, plus /api/trend."""
import hashlib
import hmac
import json
import math
import os
import sqlite3
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
from contextlib import asynccontextmanager, closing

from fastapi import FastAPI, Header, HTTPException
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field
from starlette.exceptions import HTTPException as StarletteHTTPException

from trend import analyze

MASTER = os.environ.get("MASTER_URL", "http://jaga-master.local").rstrip("/")  # use the IP in Docker: no mDNS
# Set MASTER_URL empty and INGEST_TOKEN to let a master on another network POST its readings to /api/ingest instead.
INGEST_TOKEN = os.environ.get("INGEST_TOKEN", "")
INGEST_STALE_S = 1500  # no POST for this long = master unreachable
DB = os.environ.get("DB_PATH", "/data/jaga.db")
POLL_S = 10
RETAIN_DAYS = 180  # readings older than this are deleted, once a day
API_VERSION = 1  # bump on a breaking change so old apps can say so
COLS = ("log_timestamp_s", "node_id", "sender_timestamp_s", "temperature_c", "humidity_pct",
        "distance_mm", "distance_stddev_mm", "water_level_mm")

def read_config():
    try:
        w, d, p = (int(os.environ.get(k, v)) for k, v in (("WARN_MM", 700), ("DANGER_MM", 900), ("DEPTH_MM", 1200)))
        if 0 < w < d <= p:
            return w, d, p
    except ValueError:
        pass
    print("WARN_MM, DANGER_MM and DEPTH_MM must be positive whole numbers with warn below danger and danger "
          "at most depth. Using 700, 900 and 1200.", flush=True)
    return 700, 900, 1200


WARN_MM, DANGER_MM, DEPTH_MM = read_config()


def cfg_rev(warn, danger, depth):  # same 3 lines in hub/app.py (it cannot import server/): keep identical
    return hashlib.sha1(f"{warn}:{danger}:{depth}".encode()).hexdigest()[:8]


master = {"status": None, "nodes": None}  # status is one (dict, fetched_at) tuple: a single assignment, no torn read
health = {"master_ok": False, "last_ok": 0.0, "fails": 0, "db_ok": True}
rejected = 0  # master rows dropped as malformed since start
last_prune = 0.0
last_err = None  # last poll_once crash text, so a repeating error prints once

# Hub push client: off unless all three are set. Outbound only, the hub never connects in.
HUB_URL = os.environ.get("HUB_URL", "").rstrip("/")
SITE_SLUG = os.environ.get("SITE_SLUG", "")
SITE_TOKEN = os.environ.get("SITE_TOKEN", "")
PUSH_OK_S = 30
PUSH_BACKOFF_S = (30, 60, 120)
PUSH_TIMEOUT_S = 10
BATCH = 500
MAX_BATCHES = 20  # per iteration, so a huge first backfill can't hold up heartbeats
push = {"fails": 0, "err": None, "last_ok": None, "pending": 0, "latest": None, "cursor": None}


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
    # sqlite3 raises OverflowError (not sqlite3.Error) above 2**63, and NaN/inf are not readings
    num = lambda v, t: (isinstance(v, t) and not isinstance(v, bool)
                        and (abs(v) < 2**63 if isinstance(v, int) else math.isfinite(v)))
    return (num(r["node_id"], int) and r["node_id"] >= 1
            and all(num(r[k], int) for k in COLS if k.endswith(("_s", "_mm")))
            and all(num(r[k], (int, float)) for k in ("temperature_c", "humidity_pct")))


def dup(c, r):
    # master re-serves readings; store each once. ponytail: only a 10-minute window, not a unique key,
    # because master timestamps restart from small numbers after a reboot without RTC.
    return c.execute("SELECT 1 FROM readings WHERE node_id=? AND received_at>=? AND log_timestamp_s=? "
                     "AND sender_timestamp_s=? AND water_level_mm=? LIMIT 1",
                     (r["node_id"], time.time() - 600, r["log_timestamp_s"], r["sender_timestamp_s"],
                      r["water_level_mm"])).fetchone() is not None


def with_backfill(rows):
    # /api/latest serves one row per node; a slave flushing a burst needs the master's history too
    out = []
    with closing(db()) as c:
        for r in rows:
            if valid(r) and not dup(c, r):
                try:
                    h = fetch(f"/api/history?node={r['node_id']}&limit=8")
                    for x in reversed(h):  # oldest first
                        # arrival time of an older row = now minus how far behind the newest it is, on the master's
                        # own clock; a larger log_timestamp_s means the master rebooted, so the clocks don't compare
                        if valid(x) and x["node_id"] == r["node_id"] and x["log_timestamp_s"] <= r["log_timestamp_s"]:
                            x["_age"] = r["log_timestamp_s"] - x["log_timestamp_s"]
                            out.append(x)
                except Exception:  # latest alone is still stored
                    pass
            out.append(r)
    return out


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
            if dup(c, r):
                continue
            c.execute(f"INSERT INTO readings (received_at, {','.join(COLS)}) VALUES (?{',?' * len(COLS)})",
                      (time.time() - r.get("_age", 0), *[r[k] for k in COLS]))


def poll_once():
    ok = 0
    for key, path in (("status", "/api/status"), ("nodes", "/api/nodes")):
        try:
            v = fetch(path)
            master[key] = (v, time.time()) if key == "status" else v
            ok += 1
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
        init_db()  # retried here when startup could not open the database
        store(with_backfill(rows))
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


def poll_step():
    global last_err
    try:
        poll_once()
        last_err = None
    except Exception as e:  # a bad poll must never end the thread
        health["fails"] += 1
        if repr(e) != last_err:
            last_err = repr(e)
            print("poll failed:", last_err, flush=True)


def poll_loop():
    while True:
        poll_step()
        time.sleep(min(30, POLL_S * max(1, health["fails"])))  # back off 10, 20, 30 s while polls fail


def ingest_watch():
    while True:
        time.sleep(10)
        if health["master_ok"] and time.time() - health["last_ok"] > INGEST_STALE_S:
            health["master_ok"] = False
            print("master unreachable: readings have stopped", flush=True)


def hub_on():
    return bool(HUB_URL and SITE_SLUG and SITE_TOKEN)


def hub_post(path, body):
    req = urllib.request.Request(
        f"{HUB_URL}/hub/v1/sites/{urllib.parse.quote(SITE_SLUG, safe='')}/{path}", json.dumps(body).encode(),
        {"Authorization": f"Bearer {SITE_TOKEN}", "Content-Type": "application/json", "User-Agent": "jaga-site/1"})
    with urllib.request.urlopen(req, timeout=PUSH_TIMEOUT_S) as r:
        return json.load(r)


def push_rows(latest):
    # Tie-safe resume: the hub only says how far it got in received_at. Within and across iterations we keep our own
    # (received_at, id) cursor, so rows sharing one received_at that straddle a batch or iteration boundary are not
    # lost, and rejected rows are skipped for good. The cursor is dropped when the hub reports less than it did
    # before (hub data lost), and ignored when it is behind the hub.
    cur = push["cursor"]
    if not (cur and cur[0] >= latest and (push["latest"] is None or latest >= push["latest"])):
        cur = (latest, 2**62)
    push["latest"] = latest
    cols = ("received_at",) + COLS
    for _ in range(MAX_BATCHES):
        with closing(db()) as c:
            rows = c.execute(f"SELECT id, {','.join(cols)} FROM readings WHERE (received_at, id) > (?, ?) "
                             "ORDER BY received_at, id LIMIT ?", (*cur, BATCH)).fetchall()
        if not rows:
            break
        hub_post("readings", {"rows": [{k: r[k] for k in cols} for r in rows]})
        cur = (rows[-1]["received_at"], rows[-1]["id"])
        push["cursor"] = cur
    with closing(db()) as c:
        push["pending"] = c.execute("SELECT COUNT(*) FROM readings WHERE (received_at, id) > (?, ?)", cur).fetchone()[0]


def push_once():
    s = site()
    ans = hub_post("heartbeat", {
        "name": s["name"], "lat": s["lat"], "lng": s["lng"], "api_version": API_VERSION, "site_time": time.time(),
        "master_ok": health["master_ok"], "status": status() if master["status"] else None, "nodes": nodes(),
        "config": {"warn_mm": WARN_MM, "danger_mm": DANGER_MM, "depth_mm": DEPTH_MM}})
    push["last_ok"] = time.time()  # the hub answered; a row failure below still backs off
    push_rows(float(ans["latest_received_at"]))


def push_step():
    """One iteration. Returns the seconds to wait before the next."""
    try:
        push_once()
        msg = None
    except urllib.error.HTTPError as e:
        msg = {401: "The hub rejected this site's token.", 403: "This site isn't approved on the hub."}.get(
            e.code, f"The hub answered with an error ({e.code}). Retrying.")
    except Exception as e:  # a bad iteration must never end the thread
        msg = f"The hub can't be reached ({e!r}). Retrying."
    if msg is None:
        if push["err"]:
            print("hub reachable again", flush=True)
        push.update(fails=0, err=None)
        return PUSH_OK_S
    if msg != push["err"]:  # once per transition, not every retry
        push["err"] = msg
        print(msg, flush=True)
    push["fails"] += 1
    return PUSH_BACKOFF_S[min(push["fails"], len(PUSH_BACKOFF_S)) - 1]


def push_loop():
    while True:
        time.sleep(push_step())


@asynccontextmanager
async def lifespan(_):
    try:
        init_db()
    except Exception as e:  # keep serving: /api/health reports it and the poll retries init_db
        health["db_ok"] = False
        print("database unavailable at startup:", repr(e), flush=True)
    threading.Thread(target=poll_loop if MASTER else ingest_watch, daemon=True).start()
    if hub_on():
        threading.Thread(target=push_loop, daemon=True, name="push").start()
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
    hub = None if not hub_on() else {
        "ok": push["err"] is None and push["last_ok"] is not None,
        "last_push_s": None if push["last_ok"] is None else time.time() - push["last_ok"],
        "pending_rows": push["pending"]}
    return {"ok": True, "rejected": rejected, "hub": hub}


@app.post("/api/ingest")
def ingest(body: dict, authorization: str = Header("")):
    """The master pushes {"rows": [...], "status": {...}} here when the server can't poll it."""
    if not INGEST_TOKEN or not hmac.compare_digest(authorization.encode(), f"Bearer {INGEST_TOKEN}".encode()):
        return err(401, "unauthorized", "The ingest token is wrong or not set.")
    rows, st = body.get("rows"), body.get("status")
    if not isinstance(rows, list):
        return err(400, "bad_request", "rows must be a list.")
    # The master queues samples (30 s apart) and pushes them together, so date each row by how far it is behind
    # the newest row of its node in this batch, on the master's own clock. Without this a whole batch shares one
    # arrival time and every trend (water rate, temperature, humidity) piles up at one instant.
    newest_log = {}
    for r in rows:
        if valid(r):
            newest_log[r["node_id"]] = max(newest_log.get(r["node_id"], 0), r["log_timestamp_s"])
    for r in rows:
        if valid(r):
            r["_age"] = max(0, min(newest_log[r["node_id"]] - r["log_timestamp_s"], 86400))
    try:
        store(rows)
    except sqlite3.Error:
        return err(503, "db_unavailable", "The server can't open its database.")
    if isinstance(st, dict) and all(isinstance(st.get(k), int) for k in ("now_s", "uptime_s")):
        master["status"] = (st, time.time())
    health.update(master_ok=True, last_ok=time.time(), fails=0)
    return {"ok": True}


@app.get("/api/status")
def status():
    if not master["status"]:
        return err(503, "master_unreachable", "The server hasn't reached the master node yet.")
    st, at = master["status"]
    elapsed = time.time() - at  # keep now_s ticking while the master is unreachable
    return {**st, "now_s": st["now_s"] + int(elapsed), "uptime_s": st["uptime_s"] + int(elapsed),
            "master_age_s": int(elapsed), "master_ok": health["master_ok"],
            "api_version": API_VERSION}


# The team's five drain stations. The master's own names and positions are placeholders, so these win.
DEFAULT_NODES = {1: ("Tasik G3", 1.8630, 103.0840), 2: ("Infront FKAAB", 1.8640, 103.0850),
                 3: ("Infront FSKTM", 1.8650, 103.0860), 4: ("Infront FPTP", 1.8660, 103.0870),
                 5: ("Infront FKEE", 1.8670, 103.0880)}


@app.get("/api/nodes")
def nodes():
    """Every node this server knows about, whether or not the master is reachable right now."""
    out = {i: {"node_id": i, "name": n, "lat": la, "lng": lo} for i, (n, la, lo) in DEFAULT_NODES.items()}
    for n in master["nodes"] or []:  # nodes the defaults don't cover
        if isinstance(n, dict) and isinstance(n.get("node_id"), int) and n["node_id"] >= 1:
            out.setdefault(n["node_id"], dict(n))
    with closing(db()) as c:
        for (i,) in c.execute("SELECT DISTINCT node_id FROM readings WHERE node_id >= 1"):
            out.setdefault(i, {"node_id": i, "name": f"Node {i}"})
        for r in c.execute("SELECT * FROM node_pos"):  # positions saved from the dashboard
            out.setdefault(r["node_id"], {"node_id": r["node_id"], "name": f"Node {r['node_id']}"}).update(
                lat=r["lat"], lng=r["lng"])
    return [out[i] for i in sorted(out)]


@app.get("/api/site")
def site():
    """Name, logo and centre point, so the app can show this server in its lobby."""
    pts = [(n["lat"], n["lng"]) for n in nodes() if n.get("lat") is not None and n.get("lng") is not None]
    return {"name": os.environ.get("SITE_NAME", "JAGA"),
            "logo": os.environ.get("SITE_LOGO", "/logo.png"),  # path on this server, or a full URL
            "lat": sum(p[0] for p in pts) / len(pts) if pts else None,
            "lng": sum(p[1] for p in pts) / len(pts) if pts else None}


@app.get("/api/config")
def config():
    """Water level thresholds and drain depth, so the app draws the same lines the server alerts on."""
    return {"warn_mm": WARN_MM, "danger_mm": DANGER_MM, "depth_mm": DEPTH_MM,
            "rev": cfg_rev(WARN_MM, DANGER_MM, DEPTH_MM)}


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
