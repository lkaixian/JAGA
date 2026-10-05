"""Run: python server/test_poll.py. Fake master + poll_once, no pytest."""
import json, os, sys, tempfile, threading
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
mode = {"status": 200, "nodes": 200, "latest": 200, "history": 200, "n": 0, "raw": None, "hist": []}
ST = {"now_s": 1000, "uptime_s": 500, "rtc_ok": True, "sd_ok": True}


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        k = self.path.split("/")[-1].split("?")[0]
        code = mode[k]
        body = {"status": ST, "nodes": [{"node_id": 1}],
                "latest": [{"log_timestamp_s": mode["n"], "node_id": 1, "sender_timestamp_s": mode["n"],
                            "temperature_c": 25.0, "humidity_pct": 50.0, "distance_mm": 100,
                            "distance_stddev_mm": 1, "water_level_mm": 400}], "history": mode["hist"]}[k] if code == 200 else {}
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
assert c.get("/api/health").json() == {"ok": True, "rejected": 0, "hub": None}

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


def row(node, log, snd, lvl):
    return {"log_timestamp_s": log, "node_id": node, "sender_timestamp_s": snd, "temperature_c": 25.0,
            "humidity_pct": 50.0, "distance_mm": 100, "distance_stddev_mm": 1, "water_level_mm": lvl}


def logs(node):
    with A.closing(A.db()) as d:
        return [r[0] for r in d.execute("SELECT log_timestamp_s FROM readings WHERE node_id=? ORDER BY id", (node,))]


# item 1: oversize int and NaN rows are rejected, a crash inside poll_once does not end the loop
rej = A.rejected
poll(raw=[row(4, 1, 1, 2**70), {**row(4, 2, 2, 300), "temperature_c": float("nan")}, row(4, 3, 3, 300)])
assert A.rejected == rej + 2 and logs(4) == [3]
real_once, f = A.poll_once, A.health["fails"]
A.poll_once = lambda: 1 / 0
A.poll_step()
A.poll_step()
A.poll_once = real_once
assert A.health["fails"] == f + 2 and A.last_err is not None
poll(raw=[row(4, 4, 4, 300)])
A.poll_step()
assert logs(4) == [3, 4] and A.last_err is None

# item 2: unopenable DB at startup still serves, then recovers once the path works
good_db, bad_dir = A.DB, tempfile.mkdtemp()
A.DB = os.path.join(bad_dir, "sub", "x.db")
real_loop, started = A.poll_loop, []
A.poll_loop = lambda: started.append(1)
with TestClient(A.app) as c2:
    r = c2.get("/api/health")
    assert r.status_code == 503 and r.json()["error"]["code"] == "db_unavailable" and A.health["db_ok"] is False
    assert c2.get("/api/latest").json()["error"]["code"] == "db_unavailable"
A.poll_loop = real_loop
os.makedirs(os.path.join(bad_dir, "sub"))
poll(raw=[row(20, 1, 1, 300)])
assert A.health["db_ok"] is True and logs(20) == [1] and c.get("/api/health").status_code == 200
A.DB = good_db

# item 3: dedupe against every recent row, not only the last
poll(raw=[row(5, 10, 10, 300)]); poll(raw=[row(5, 20, 20, 310)]); poll(raw=[row(5, 10, 10, 300)])
assert logs(5) == [10, 20]  # older reading re-served
for _ in range(2):
    poll(raw=[row(6, 10, 10, 300)]); poll(raw=[row(6, 20, 20, 310)])
assert logs(6) == [10, 20]  # alternating sources
poll(raw=[row(7, 1, 1, 300)]); poll(raw=[row(7, 1, 1, 350)])
assert logs(7) == [1, 1]  # same timestamps, new level
with A.closing(A.db()) as d, d:
    d.execute("UPDATE readings SET received_at = received_at - 700 WHERE node_id=7")
poll(raw=[row(7, 1, 1, 300)])
assert logs(7) == [1, 1, 1]  # outside the 10-minute window it counts as new

# item 4: backfill burst, latest shows only the newest
mode["hist"] = [row(8, i, i, 300 + i) for i in (5, 4, 3, 2, 1)]
poll(raw=[row(8, 5, 5, 305)])
assert logs(8) == [1, 2, 3, 4, 5]
poll(raw=[row(8, 5, 5, 305)])
assert logs(8) == [1, 2, 3, 4, 5]
# an older backfilled row is stamped by how far behind the newest it is, not with "now"
mode["hist"] = [row(12, 1000, 1000, 300), row(12, 880, 880, 290), row(12, 5000, 5000, 280)]
poll(raw=[row(12, 1000, 1000, 300)])
assert logs(12) == [880, 1000]
with A.closing(A.db()) as d:
    ages = {l: A.time.time() - t for l, t in d.execute("SELECT log_timestamp_s, received_at FROM readings WHERE node_id=12")}
assert 119 <= ages[880] <= 125 and ages[1000] < 5, ages
mode.update(history=500, hist=[])
poll(raw=[row(10, 1, 1, 300)])
assert logs(10) == [1] and A.health["fails"] == 0  # history failed: latest only, poll not failed
mode.update(history=200, hist={"not": "a list"})
poll(raw=[row(11, 1, 1, 300)])
assert logs(11) == [1]
mode.update(hist=[], raw=None)

# item 5: names in the master-never-reached fallback
assert c.put("/api/nodes/1", json={"lat": 1.5, "lng": 2.5}).status_code == 200
old_nodes, A.master["nodes"] = A.master["nodes"], None
n1 = [n for n in c.get("/api/nodes").json() if n["node_id"] == 1][0]
assert n1["name"] == "Tasik G3" and n1["lat"] == 1.5  # saved position wins, team name stays
assert [n["node_id"] for n in c.get("/api/nodes").json()][:5] == [1, 2, 3, 4, 5]  # all five even with no master
assert c.get("/api/site").json()["logo"] == "/logo.png"
A.master["nodes"] = old_nodes

# item 6: status is one tuple, age and now_s come from the same assignment
poll()
assert isinstance(A.master["status"], tuple) and "status_at" not in A.master
A.master["status"] = (ST, A.time.time() - 100)
s = c.get("/api/status").json()
assert s["master_age_s"] in (100, 101) and s["now_s"] in (1100, 1101)


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
