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
burst = [{**row, "node_id": 2, "log_timestamp_s": t, "sender_timestamp_s": t, "temperature_c": 20 + i}
         for i, t in enumerate((1000, 1030, 1060))]  # three samples 30 s apart, pushed together
assert c.post("/api/ingest", json={"rows": burst}, headers=ok).status_code == 200
got = sorted(c.get("/api/history?node=2").json(), key=lambda r: r["log_timestamp_s"])
gaps = [round(b["received_at"] - a["received_at"]) for a, b in zip(got, got[1:])]
assert gaps == [30, 30], gaps  # dated by their own spacing, not all by the arrival time
full = {**row, "node_id": 4, "battery_v": 3.9, "link_retries": 2, "rssi_dbm": -83}
assert c.post("/api/ingest", json={"rows": [full]}, headers=ok).status_code == 200
g = c.get("/api/history?node=4").json()[0]
assert (g["battery_v"], g["link_retries"], g["rssi_dbm"]) == (3.9, 2, -83), g
assert c.get("/api/history?node=1").json()[0]["battery_v"] is None  # old firmware: no field
s = c.get("/api/status").json()
assert s["master_ok"] is True and s["rtc_ok"] is True
print("ingest ok")
