"""Run: python server/test_ntfy.py. ntfy fires once per rise into warning or danger; no pytest."""
import os, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.update(MASTER_URL="", INGEST_TOKEN="s", DB_PATH=os.path.join(tempfile.mkdtemp(), "t.db"),
                  NTFY_URL="http://ntfy.invalid/topic", WARN_MM="700", DANGER_MM="900")
import app as A
from fastapi.testclient import TestClient

A.init_db()
sent = []
A.ntfy = lambda title, body, priority, tags: sent.append((title, priority))
c = TestClient(A.app)
n = [0]


def push(level, node=1):
    n[0] += 1
    r = {"log_timestamp_s": n[0] * 60, "node_id": node, "sender_timestamp_s": n[0], "temperature_c": 25.0,
         "humidity_pct": 50.0, "distance_mm": 100, "distance_stddev_mm": 1, "water_level_mm": level}
    assert c.post("/api/ingest", json={"rows": [r]}, headers={"Authorization": "Bearer s"}).status_code == 200


push(300)                       # first sight: quiet
push(750); assert len(sent) == 1 and sent[-1][1] == 4          # into warning
push(760); assert len(sent) == 1                               # still warning: no repeat
push(950); assert len(sent) == 2 and sent[-1][1] == 5          # into danger
push(960); assert len(sent) == 2
push(400); assert len(sent) == 2                               # falling: quiet
push(-2147483648); assert len(sent) == 2                       # sonar error ignored
push(1000); assert len(sent) == 3 and sent[-1][1] == 5         # straight from normal to danger
push(300, node=2); push(800, node=2); assert len(sent) == 4    # nodes tracked separately
A.NTFY_URL = ""; push(100); push(950); assert len(sent) == 4   # disabled: nothing
print("ntfy ok")
