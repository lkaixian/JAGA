"""JAGA hub: sites push heartbeats and readings here over HTTPS; the hub serves each site's data
at /server/<slug>/api/... in the same shape as server/app.py. The hub never connects to a site."""
import hashlib
import hmac
import json
import math
import os
import sqlite3
import time
import traceback
from contextlib import asynccontextmanager, closing

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field, ValidationError
from starlette.concurrency import run_in_threadpool
from starlette.exceptions import HTTPException as StarletteHTTPException

from admin import SCHEMA  # one schema, shared with the admin tool
from trend import analyze

DB = os.environ.get("HUB_DB", "/data/hub.db")
PUBLIC = os.environ.get("HUB_PUBLIC_URL", "http://localhost:8088").rstrip("/")
RETAIN_DAYS = 180
ONLINE_S = 90  # a site is online if it was heard from within this long
MAX_BODY = 1_000_000
MAX_ROWS = 500
COLS = ("log_timestamp_s", "node_id", "sender_timestamp_s", "temperature_c", "humidity_pct",
        "distance_mm", "distance_stddev_mm", "water_level_mm")
last_prune = 0.0


def db():
    c = sqlite3.connect(DB, timeout=10)
    c.row_factory = sqlite3.Row
    c.execute("PRAGMA journal_mode=WAL")
    return c


@asynccontextmanager
async def lifespan(_):
    try:
        with closing(db()) as c:
            c.executescript(SCHEMA)
    except Exception as e:  # keep serving: requests answer db_unavailable until it recovers
        print("database unavailable at startup:", repr(e), flush=True)
    yield


app = FastAPI(lifespan=lifespan)


class Err(Exception):
    def __init__(self, status, code, message):
        self.status, self.code, self.message = status, code, message


def err(status, code, message):
    return JSONResponse({"error": {"code": code, "message": message}}, status)


@app.exception_handler(Err)
async def on_err(_, e):
    return err(e.status, e.code, e.message)


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


# ---- site side: authenticated writes ----

def auth(slug, header):
    row = site_row(slug)
    tok = header[7:] if header and header[:7].lower() == "bearer " else ""
    given = hashlib.sha256(tok.encode()).hexdigest()
    ok = hmac.compare_digest(given, row["token_hash"] if row else "0" * 64)  # compare even for an unknown slug
    if not row or not ok:  # same answer for both: no way to list valid slugs
        raise Err(401, "unauthorized", "The site name or token isn't recognised.")
    if row["status"] != "approved":
        raise Err(403, "site_not_approved", "This site hasn't been approved on the hub yet.")
    return row


async def body_of(request):
    if int(request.headers.get("content-length") or 0) > MAX_BODY:
        raise Err(413, "too_large", "The request body is over 1 MB.")
    buf = b""
    async for chunk in request.stream():  # a chunked body has no length header, so count as it arrives
        buf += chunk
        if len(buf) > MAX_BODY:
            raise Err(413, "too_large", "The request body is over 1 MB.")
    return buf


class NodeIn(BaseModel):
    node_id: int = Field(ge=1, lt=2**63)
    name: str = Field(max_length=100)
    lat: float | None = Field(None, ge=-90, le=90)
    lng: float | None = Field(None, ge=-180, le=180)


class Heartbeat(BaseModel):
    name: str = Field(min_length=1, max_length=100)
    lat: float | None = Field(None, ge=-90, le=90)
    lng: float | None = Field(None, ge=-180, le=180)
    api_version: int = Field(ge=0, lt=2**31)
    site_time: float = Field(allow_inf_nan=False)
    master_ok: bool
    status: dict | None = None
    nodes: list[NodeIn] = Field(max_length=200)


def newest_received(c, slug):
    return c.execute("SELECT MAX(received_at) FROM readings WHERE slug=?", (slug,)).fetchone()[0] or 0.0


@app.post("/hub/v1/sites/{slug}/heartbeat")
async def heartbeat(slug: str, request: Request):
    row = await run_in_threadpool(auth, slug, request.headers.get("authorization"))
    try:
        hb = Heartbeat.model_validate_json(await body_of(request))
    except ValidationError:
        raise Err(422, "bad_request", "The request has a missing or invalid value.")
    return await run_in_threadpool(save_heartbeat, row, hb)


def save_heartbeat(row, hb):
    now = time.time()
    locked = set(json.loads(row["admin_set"] or "[]"))
    sets = {"last_seen": now, "master_ok": int(hb.master_ok), "api_version": hb.api_version,
            "nodes_json": json.dumps([n.model_dump() for n in hb.nodes])}
    if hb.status is not None:  # a null status keeps the last known one, which the status endpoint labels as old
        sets["status_json"] = json.dumps({"at": now, "st": hb.status})
    for k in ("name", "lat", "lng"):  # admin-set values win; the site fills the rest
        if k not in locked and getattr(hb, k) is not None:
            sets[k] = getattr(hb, k)
    with closing(db()) as c, c:
        c.execute(f"UPDATE sites SET {','.join(k + '=?' for k in sets)} WHERE slug=?", (*sets.values(), row["slug"]))
        return {"ok": True, "hub_time": now, "latest_received_at": newest_received(c, row["slug"])}


def num(v, t):
    # sqlite3 raises OverflowError (not sqlite3.Error) above 2**63, and NaN/inf are not readings
    return (isinstance(v, t) and not isinstance(v, bool)
            and (abs(v) < 2**63 if isinstance(v, int) else math.isfinite(v)))


def valid(r, now):
    return (isinstance(r, dict) and all(k in r for k in COLS + ("received_at",))
            and num(r["received_at"], (int, float)) and 0 < r["received_at"] <= now + 86400
            and num(r["node_id"], int) and r["node_id"] >= 1
            and all(num(r[k], int) for k in COLS if k.endswith(("_s", "_mm")))
            and all(num(r[k], (int, float)) for k in ("temperature_c", "humidity_pct")))


@app.post("/hub/v1/sites/{slug}/readings")
async def readings(slug: str, request: Request):
    row = await run_in_threadpool(auth, slug, request.headers.get("authorization"))
    try:
        data = json.loads(await body_of(request))
    except ValueError:
        data = None
    rows = data.get("rows") if isinstance(data, dict) else None
    if not isinstance(rows, list):
        raise Err(422, "bad_request", "The request has a missing or invalid value.")
    if len(rows) > MAX_ROWS:
        raise Err(413, "too_many_rows", f"Send at most {MAX_ROWS} readings per request.")
    return await run_in_threadpool(save_readings, row["slug"], rows)


def save_readings(slug, rows):
    global last_prune
    now = time.time()
    good = [r for r in rows if valid(r, now)]
    stored = 0
    with closing(db()) as c, c:
        if now - last_prune >= 86400:
            c.execute("DELETE FROM readings WHERE received_at < ?", (now - RETAIN_DAYS * 86400,))
            last_prune = now
        for r in good:  # the UNIQUE key makes a retried batch store nothing new
            stored += c.execute(f"INSERT OR IGNORE INTO readings (slug, received_at, {','.join(COLS)}) "
                                f"VALUES (?,?{',?' * len(COLS)})", (slug, r["received_at"], *[r[k] for k in COLS])).rowcount
        c.execute("UPDATE sites SET last_seen=? WHERE slug=?", (now, slug))
        return {"stored": stored, "rejected": len(rows) - len(good), "latest_received_at": newest_received(c, slug)}


# ---- public reads ----

def site_row(slug):
    with closing(db()) as c:
        return c.execute("SELECT * FROM sites WHERE slug=?", (slug,)).fetchone()


def approved(slug):
    row = site_row(slug)
    if not row or row["status"] != "approved":
        raise Err(404, "unknown_site", "There is no site with that name on this hub.")
    return row


def as_reading(r):
    return {**{k: r[k] for k in COLS}, "received_at": r["received_at"]}


def newest(slug, node, limit):
    with closing(db()) as c:
        return c.execute("SELECT * FROM readings WHERE slug=? AND node_id=? ORDER BY id DESC LIMIT ?",
                         (slug, node, limit)).fetchall()


def node_ids(slug):
    with closing(db()) as c:
        return [r[0] for r in c.execute("SELECT DISTINCT node_id FROM readings WHERE slug=? AND node_id >= 1 "
                                        "ORDER BY node_id", (slug,))]


@app.get("/hub/v1/health")
def hub_health():
    with closing(db()) as c:
        return {"ok": True, "sites": c.execute("SELECT COUNT(*) FROM sites").fetchone()[0]}


@app.get("/servers.json")
def servers():
    now = time.time()
    with closing(db()) as c:
        rows = c.execute("SELECT * FROM sites WHERE status='approved' ORDER BY name, slug").fetchall()
    out = []
    for r in rows:
        logo = r["logo"] or "/logo.png"
        out.append({"name": r["name"] or r["slug"], "url": f"{PUBLIC}/server/{r['slug']}", "lat": r["lat"],
                    "lng": r["lng"], "logo": PUBLIC + logo if logo.startswith("/") else logo, "slug": r["slug"],
                    "online": r["last_seen"] is not None and now - r["last_seen"] <= ONLINE_S,
                    "last_seen_s": None if r["last_seen"] is None else int(now - r["last_seen"])})
    return out


API = "/server/{slug}/api"


@app.get(API + "/health")
def site_health(slug: str):
    approved(slug)
    return {"ok": True}


@app.get(API + "/status")
def status(slug: str):
    row = approved(slug)
    if row["master_ok"] is None:  # never sent a heartbeat
        return err(503, "site_unreachable", "This site hasn't reported yet.")
    if not row["status_json"]:
        return err(503, "master_unreachable", "The server hasn't reached the master node yet.")
    now = time.time()
    saved = json.loads(row["status_json"])
    st, elapsed = saved["st"], now - saved["at"]  # keep now_s ticking while the site is silent
    adv = {k: st[k] + int(elapsed) for k in ("now_s", "uptime_s") if num(st.get(k), (int, float))}
    return {**st, **adv, "master_age_s": int(elapsed), "master_ok": bool(row["master_ok"]),
            "api_version": row["api_version"], "site_ok": now - row["last_seen"] <= ONLINE_S,
            "site_age_s": int(now - row["last_seen"])}


@app.get(API + "/nodes")
def nodes(slug: str):
    row = approved(slug)
    out = {}
    for n in json.loads(row["nodes_json"] or "[]"):
        out[n["node_id"]] = {k: v for k, v in n.items() if v is not None}  # omit null lat/lng
    for i in node_ids(slug):
        out.setdefault(i, {"node_id": i, "name": f"Node {i}"})
    return [out[i] for i in sorted(out)]


@app.put(API + "/nodes/{node_id}")
def set_node_pos(slug: str, node_id: int):
    raise Err(403, "read_only", "Positions are set by the site.")


@app.get(API + "/site")
def site(slug: str):
    row = approved(slug)
    return {"name": row["name"] or slug, "logo": row["logo"] or "/logo.png", "lat": row["lat"], "lng": row["lng"]}


@app.get(API + "/latest")
def latest(slug: str):
    approved(slug)
    with closing(db()) as c:
        rows = c.execute("SELECT * FROM readings WHERE id IN (SELECT MAX(id) FROM readings WHERE slug=? "
                         "GROUP BY node_id) ORDER BY node_id", (slug,)).fetchall()
    return [as_reading(r) for r in rows]


@app.get(API + "/history")
def history(slug: str, node: int, limit: int = 90):
    approved(slug)
    if node < 1:
        raise Err(400, "bad_request", "The node number must be 1 or higher.")
    return [as_reading(r) for r in newest(slug, node, max(1, min(limit, 5000)))]


@app.get(API + "/trend")
def trend(slug: str, warn: float = 700, danger: float = 900):
    approved(slug)
    return [{"node_id": n, **analyze([(r["received_at"], r["water_level_mm"]) for r in newest(slug, n, 200)[::-1]],
                                     warn, danger)} for n in node_ids(slug)]
