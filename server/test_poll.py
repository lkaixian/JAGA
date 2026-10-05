"""Run: python server/test_poll.py. Fake master + poll_once, no pytest."""
import json, os, sys, tempfile, threading
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
mode = {"status": 200, "nodes": 200, "latest": 200, "n": 0, "raw": None}
ST = {"now_s": 1000, "uptime_s": 500, "rtc_ok": True, "sd_ok": True}


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        k = self.path.split("/")[-1]
        code = mode[k]
        body = {"status": ST, "nodes": [{"node_id": 1}],
                "latest": [{"log_timestamp_s": mode["n"], "node_id": 1, "sender_timestamp_s": mode["n"],
                            "temperature_c": 25.0, "humidity_pct": 50.0, "distance_mm": 100,
                            "distance_stddev_mm": 1, "water_level_mm": 400}]}[k] if code == 200 else {}
        if k == "latest" and mode["raw"] is not None:
            body = mode["raw"]
        self.send_response(code)
        self.end_headers()
        self.wfile.write(json.dumps(body).encode())

    def log_message(self, *a):
        pass


srv = HTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
os.environ["MASTER_URL"] = f"http://127.0.0.1:{srv.server_port}"
os.environ["DB_PATH"] = os.path.join(tempfile.mkdtemp(), "t.db")

import app as A
from fastapi.testclient import TestClient

A.POLL_S = 0.01
A.init_db()
c = TestClient(A.app)  # no `with`: keeps the real poll thread out of the test


def poll(**kw):
    mode.update(kw)
    A.poll_once()


assert c.get("/api/status").status_code == 503
assert c.get("/api/status").json()["error"]["code"] == "master_unreachable"
assert c.get("/api/health").json() == {"ok": True, "rejected": 0}

poll(n=1)  # (a)
s = c.get("/api/status").json()
assert s["master_ok"] is True and s["rtc_ok"] is True and "master_age_s" in s
assert len(c.get("/api/history?node=1").json()) == 1
poll()  # same reading again: deduped
assert len(c.get("/api/history?node=1").json()) == 1

poll(status=500, n=2)  # (b) status down, latest still stored
assert len(c.get("/api/history?node=1").json()) == 2
assert c.get("/api/status").json()["master_ok"] is True

poll(status=500, nodes=500, latest=500)  # (c)
poll()
assert c.get("/api/status").json()["master_ok"] is True  # only 2 failed polls
poll()
assert c.get("/api/status").json()["master_ok"] is False
assert len(c.get("/api/latest").json()) == 1 and len(c.get("/api/history?node=1").json()) == 2
poll(status=200, nodes=200, latest=200, n=3)
assert c.get("/api/status").json()["master_ok"] is True and A.health["fails"] == 0
assert len(c.get("/api/history?node=1").json()) == 3

GOOD = {"log_timestamp_s": 50, "node_id": 2, "sender_timestamp_s": 50, "temperature_c": 25.5,
        "humidity_pct": 50, "distance_mm": 100, "distance_stddev_mm": 1, "water_level_mm": 400}
n_before = len(c.get("/api/latest").json())
poll(raw=[{**GOOD, "node_id": 0}, {"node_id": 3}, "x", {**GOOD, "water_level_mm": "9"}, GOOD])  # S3
assert A.rejected == 4 and len(c.get("/api/latest").json()) == n_before + 1
assert c.get("/api/health").json()["rejected"] == 4
f = A.health["fails"]
poll(status=500, nodes=500, raw={"not": "a list"})  # object instead of list: failed fetch, no crash
assert A.health["fails"] == f + 1
poll(raw=None)

real_db = A.db  # S4


def boom():
    raise A.sqlite3.OperationalError("database is locked")


A.db = boom
poll()
assert A.health["db_ok"] is False and A.health["fails"] == 0  # master answered: not a failed poll
A.db = real_db
r = c.get("/api/health")
assert r.status_code == 503 and r.json()["error"]["code"] == "db_unavailable"
poll()
assert A.health["db_ok"] is True and c.get("/api/health").status_code == 200

now = A.time.time()  # S8
with A.closing(A.db()) as d, d:
    for age in (181, 1):
        d.execute("INSERT INTO readings (received_at, node_id) VALUES (?, 9)", (now - age * 86400,))
A.last_prune = 0.0
poll()
with A.closing(A.db()) as d:
    assert d.execute("SELECT COUNT(*) FROM readings WHERE node_id=9").fetchone()[0] == 1
assert c.get("/api/status").json()["api_version"] == 1


def shape(r, status, code):
    assert r.status_code == status and list(r.json()) == ["error"], r.text
    assert r.json()["error"]["code"] == code and r.json()["error"]["message"], r.text


shape(c.get("/api/history?node=0"), 400, "bad_request")  # unknown node
shape(c.get("/api/history?node=abc"), 422, "bad_request")  # bad query value
shape(c.get("/api/nope"), 404, "bad_request")
shape(c.put("/api/nodes/1", json={"lat": 999, "lng": 0}), 422, "bad_request")
A.master["status"] = None  # as before the master was ever reached
shape(c.get("/api/status"), 503, "master_unreachable")

A.db = boom
shape(c.get("/api/latest"), 503, "db_unavailable")
shape(c.get("/api/health"), 503, "db_unavailable")
A.db = real_db
A.node_ids = lambda: 1 / 0  # unhandled error: no text leaks
r = TestClient(A.app, raise_server_exceptions=False).get("/api/latest")
shape(r, 500, "internal")
assert "division" not in r.text
print("poll ok")
