"""Run: python server/test_ingest.py. Master pushes to /api/ingest; no pytest."""
import os, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.update(MASTER_URL="", INGEST_TOKEN="secret", DB_PATH=os.path.join(tempfile.mkdtemp(), "t.db"))
import app as A
from fastapi.testclient import TestClient

A.init_db()
c = TestClient(A.app)
row = {"log_timestamp_s": 5, "node_id": 1, "sender_timestamp_s": 5, "temperature_c": 25.0, "humidity_pct": 50.0,
       "distance_mm": 100, "distance_stddev_mm": 1, "water_level_mm": 400}
st = {"now_s": 1000, "uptime_s": 50, "rtc_ok": True, "sd_ok": True, "radio_ok": True}
ok = {"Authorization": "Bearer secret"}

assert c.post("/api/ingest", json={"rows": [row]}).status_code == 401
assert c.post("/api/ingest", json={"rows": [row]}, headers={"Authorization": "Bearer nope"}).status_code == 401
assert c.post("/api/ingest", json={"rows": "x"}, headers=ok).status_code == 400
assert c.post("/api/ingest", json={"rows": [row, {"node_id": 1}], "status": st}, headers=ok).json() == {"ok": True}
assert c.post("/api/ingest", json={"rows": [row]}, headers=ok).status_code == 200  # duplicate: stored once
assert len(c.get("/api/history?node=1").json()) == 1 and A.rejected == 1
s = c.get("/api/status").json()
assert s["master_ok"] is True and s["rtc_ok"] is True
print("ingest ok")
