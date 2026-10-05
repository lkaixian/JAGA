"""Run: python server/test_poll.py. Fake master + poll_once, no pytest."""
import json, os, sys, tempfile, threading
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
mode = {"status": 200, "nodes": 200, "latest": 200, "n": 0}
ST = {"now_s": 1000, "uptime_s": 500, "rtc_ok": True, "sd_ok": True}


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        k = self.path.split("/")[-1]
        code = mode[k]
        body = {"status": ST, "nodes": [{"node_id": 1}],
                "latest": [{"log_timestamp_s": mode["n"], "node_id": 1, "sender_timestamp_s": mode["n"],
                            "temperature_c": 25.0, "humidity_pct": 50.0, "distance_mm": 100,
                            "distance_stddev_mm": 1, "water_level_mm": 400}]}[k] if code == 200 else {}
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
assert c.get("/api/health").json() == {"ok": True}

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
print("poll ok")
