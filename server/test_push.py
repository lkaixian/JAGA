"""Run: python server/test_push.py. Fake hub + push_step, no pytest."""
import contextlib, io, json, os, sys, tempfile, threading
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
hub = {"hb": 200, "rows": 200, "calls": [], "stored": {}, "latest": None}


class H(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        kind = self.path.rsplit("/", 1)[1]
        hub["calls"].append((self.path, dict(self.headers), kind, body))
        code = hub["hb" if kind == "heartbeat" else "rows"]
        if code == 200 and kind == "readings":
            for r in body["rows"]:
                hub["stored"][(r["received_at"], r["node_id"], r["log_timestamp_s"])] = r
        top = max([k[0] for k in hub["stored"]], default=0) if hub["latest"] is None else hub["latest"]
        out = {"ok": True, "hub_time": 1.0, "latest_received_at": top}
        self.send_response(code)
        self.end_headers()
        self.wfile.write(json.dumps(out).encode())

    def log_message(self, *a):
        pass


srv = HTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
os.environ["DB_PATH"] = os.path.join(tempfile.mkdtemp(), "t.db")
for k in ("HUB_URL", "SITE_SLUG", "SITE_TOKEN"):
    os.environ.pop(k, None)

import app as A
from fastapi.testclient import TestClient

A.init_db()
c = TestClient(A.app)

# disabled: no thread, hub is null
A.poll_loop = lambda: None
started = []
A.push_loop = lambda: started.append(1)
with TestClient(A.app) as c0:
    assert c0.get("/api/health").json()["hub"] is None
assert not started and not A.hub_on()

A.HUB_URL, A.SITE_SLUG, A.SITE_TOKEN = f"http://127.0.0.1:{srv.server_port}", "my-site", "tok"
with TestClient(A.app):
    pass
import time; time.sleep(0.1)
assert started == [1]


def reset(**kw):
    hub.update(hb=200, rows=200, calls=[], stored={}, latest=None)
    hub.update(kw)
    A.push.update(fails=0, err=None, last_ok=None, pending=0, latest=None, cursor=None)


def add(n, start=1000, step=1):
    with A.closing(A.db()) as d, d:
        for i in range(n):
            d.execute("INSERT INTO readings (received_at, log_timestamp_s, node_id, sender_timestamp_s, temperature_c, "
                      "humidity_pct, distance_mm, distance_stddev_mm, water_level_mm) VALUES (?,?,1,?,25,50,100,1,300)",
                      (start + i * step, i, i))


def posts(kind):
    return [x for x in hub["calls"] if x[2] == kind]


def run(f):
    b = io.StringIO()
    with contextlib.redirect_stdout(b):
        f()
    return b.getvalue().splitlines()


# heartbeat fields and headers
reset()
add(3)
assert A.push_step() == 30
path, hd, _, body = posts("heartbeat")[0]
assert path == "/hub/v1/sites/my-site/heartbeat" and hd["Authorization"] == "Bearer tok"
assert hd["Content-Type"] == "application/json" and hd["User-Agent"] == "jaga-site/1"
assert set(body) == {"name", "lat", "lng", "api_version", "site_time", "master_ok", "status", "nodes"}
assert body["name"] == "JAGA" and body["api_version"] == 1 and body["status"] is None and len(body["nodes"]) >= 5
h = c.get("/api/health").json()["hub"]
assert h["ok"] is True and h["last_push_s"] < 5 and h["pending_rows"] == 0
assert len(hub["stored"]) == 3

# resume: 1200 rows, hub has the first 100 -> 1100 rows in 500/500/100
reset(latest=1099.0)
with A.closing(A.db()) as d, d:
    d.execute("DELETE FROM readings")
add(1200)
A.push_step()
sizes = [len(x[3]["rows"]) for x in posts("readings")]
assert sizes == [500, 500, 100], sizes
sent = [r["received_at"] for x in posts("readings") for r in x[3]["rows"]]
assert sent == sorted(sent) and sent[0] == 1100 and sent[-1] == 2199
# hub at the end: nothing sent
reset(latest=2199.0)
A.push_step()
assert not posts("readings") and len(posts("heartbeat")) == 1

# 500 then recovery: back-off advances, resets, nothing lost
reset()
hub["hb"] = 500
assert [A.push_step() for _ in range(4)] == [30, 60, 120, 120]
hub.update(hb=200, rows=500)
hub["rows"] = 200
assert A.push_step() == 30 and A.push["fails"] == 0
assert len(hub["stored"]) == 1200
assert c.get("/api/health").json()["hub"]["ok"] is True

# 401 logs once and keeps retrying; 403 is a new message; recovery logs once
reset(hb=401)
out = run(lambda: [A.push_step() for _ in range(3)])
assert out == ["The hub rejected this site's token."] and A.push["fails"] == 3
assert c.get("/api/health").json()["hub"]["ok"] is False
hub["hb"] = 403
assert run(A.push_step) == ["This site isn't approved on the hub."]
hub["hb"] = 200
assert run(A.push_step) == ["hub reachable again"] and A.push_step() == 30

# ties: equal received_at straddling batch and iteration boundaries
reset()
with A.closing(A.db()) as d, d:
    d.execute("DELETE FROM readings")
add(5, start=5000, step=0)
A.BATCH, A.MAX_BATCHES = 2, 2
A.push_step()
assert len(hub["stored"]) == 4
A.push_step()
assert len(hub["stored"]) == 5 and A.push["pending"] == 0
reset()
A.push_step()  # hub lost everything: cursor dropped? latest 0 < earlier
assert len(hub["stored"]) == 4
print("push ok")
