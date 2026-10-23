"""Hub checks: python hub/test_hub.py"""
import contextlib
import hashlib
import io
import json
import os
import shutil
import sqlite3
import sys
import tempfile
import time

tmp = tempfile.mkdtemp()
os.environ["HUB_DB"] = os.path.join(tmp, "hub.db")
os.environ["HUB_PUBLIC_URL"] = "https://hub.test/"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import admin
    from fastapi.testclient import TestClient

    import app as hub

    def run(*argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            admin.main(list(argv))
        return out.getvalue()

    def token(out):
        return [l for l in out.splitlines() if l.startswith("SITE_TOKEN=")][0].split("=", 1)[1]

    def bearer(t):
        return {"Authorization": f"Bearer {t}"}

    out = run("add", "site-a", "--name", "Alpha", "--lat", "1.5", "--lng", "103.5")
    assert "SITE_SLUG=site-a" in out and "HUB_URL=" in out
    ta = token(out)
    tb = token(run("add", "site-b", "--name", "Bravo", "--logo", "/b.png"))
    tp = token(run("add", "site-p", "--name", "Pending", "--pending"))
    tr = token(run("add", "site-r", "--name", "Revoked"))
    run("revoke", "site-r")
    for bad in ("api", "x", "Bad_Slug", "-ab"):
        try:
            run("add", bad, "--name", "x")
            raise AssertionError(bad)
        except SystemExit:
            pass
    with sqlite3.connect(os.environ["HUB_DB"]) as c:  # only the hash is stored
        assert ta not in str(c.execute("SELECT * FROM sites").fetchall())

    now = time.time()
    hb = lambda name="Site Says": {"name": name, "lat": 9.0, "lng": 9.0, "api_version": 1, "site_time": now,
                                    "master_ok": True, "status": {"now_s": 100, "uptime_s": 50, "rtc_ok": True},
                                    "nodes": [{"node_id": 1, "name": "Drain 1", "lat": 1.0, "lng": 2.0},
                                              {"node_id": 2, "name": "Drain 2", "lat": None, "lng": None}]}

    def row(node, at, level=500, **kw):
        return {"received_at": at, "node_id": node, "log_timestamp_s": int(at), "sender_timestamp_s": 1,
                "temperature_c": 30.5, "humidity_pct": 70, "distance_mm": 1000, "distance_stddev_mm": 2,
                "water_level_mm": level, **kw}  # kw may carry battery_v, link_retries, rssi_dbm

    with TestClient(hub.app) as cl:
        # nothing reported yet
        r = cl.get("/server/site-a/api/status")
        assert r.status_code == 503 and r.json()["error"]["code"] == "site_unreachable", r.text
        assert r.json()["error"]["message"] == "This site hasn't reported yet."
        for p in ("nodes", "latest", "history?node=1"):
            assert cl.get(f"/server/site-a/api/{p}").json() == []

        rv = lambda w, d, p: hashlib.sha1(f"{w}:{d}:{p}".encode()).hexdigest()[:8]
        dflt = {"warn_mm": 700, "danger_mm": 900, "depth_mm": 1200, "rev": rv(700, 900, 1200)}
        assert cl.get("/server/site-a/api/config").json() == dflt  # before any heartbeat
        assert cl.get("/server/nope/api/config").json()["error"]["code"] == "unknown_site"
        assert cl.get("/server/site-p/api/config").status_code == 404

        # auth
        u ="/hub/v1/sites/site-a/heartbeat"
        w1 = cl.post(u, json=hb(), headers=bearer("wrong"))
        w2 = cl.post("/hub/v1/sites/nope/heartbeat", json=hb(), headers=bearer(ta))
        assert w1.status_code == w2.status_code == 401 and w1.json() == w2.json()
        assert w1.json()["error"]["code"] == "unauthorized"
        assert cl.post(u, json=hb()).status_code == 401
        for s, t in (("site-p", tp), ("site-r", tr)):
            r = cl.post(f"/hub/v1/sites/{s}/heartbeat", json=hb(), headers=bearer(t))
            assert r.status_code == 403 and r.json()["error"]["code"] == "site_not_approved"
            assert cl.post(f"/hub/v1/sites/{s}/readings", json={"rows": []}, headers=bearer(t)).status_code == 403
            assert cl.get(f"/server/{s}/api/latest").status_code == 404

        # heartbeat; admin-set name and position survive, the logo default shows
        r = cl.post(u, json=hb(), headers=bearer(ta))
        assert r.status_code == 200 and r.json()["ok"] and r.json()["latest_received_at"] == 0.0, r.text
        s = cl.get("/server/site-a/api/site").json()
        assert s == {"name": "Alpha", "logo": "https://hub.test/logo.png", "lat": 1.5, "lng": 103.5}, s  # absolute, not relative to the slug
        assert cl.post(u, json={"name": "x"}, headers=bearer(ta)).status_code == 422
        st = cl.get("/server/site-a/api/status").json()
        assert st["site_ok"] is True and st["master_ok"] is True and st["api_version"] == 1
        assert st["now_s"] >= 100 and st["site_age_s"] == 0 and st["rtc_ok"] is True and "master_age_s" in st
        nd = cl.get("/server/site-a/api/nodes").json()
        assert nd == [{"node_id": 1, "name": "Drain 1", "lat": 1.0, "lng": 2.0}, {"node_id": 2, "name": "Drain 2"}], nd
        cl.post("/hub/v1/sites/site-b/heartbeat", json=hb(), headers=bearer(tb))

        # config: no config in the heartbeat so far -> defaults; then stored, served, and bad ones ignored
        assert cl.get("/server/site-a/api/config").json() == dflt
        c1 = {"warn_mm": 500, "danger_mm": 800, "depth_mm": 1000}
        want = {**c1, "rev": rv(500, 800, 1000)}
        assert cl.post(u, json={**hb(), "config": c1}, headers=bearer(ta)).status_code == 200
        assert cl.get("/server/site-a/api/config").json() == want
        assert cl.get("/server/site-b/api/config").json() == dflt  # per site
        for bad in ({"warn_mm": 900, "danger_mm": 800, "depth_mm": 1000}, {"warn_mm": 1, "danger_mm": 2, "depth_mm": 1},
                    {"warn_mm": 0, "danger_mm": 2, "depth_mm": 3}, {"warn_mm": "1", "danger_mm": 2, "depth_mm": 3},
                    {"warn_mm": True, "danger_mm": 2, "depth_mm": 3}, {"warn_mm": 1.5, "danger_mm": 2, "depth_mm": 3},
                    {"warn_mm": 1}, {}, None):
            assert cl.post(u, json={**hb(), "config": bad}, headers=bearer(ta)).status_code == 200, bad
            assert cl.get("/server/site-a/api/config").json() == want, bad
        assert cl.post(u, json=hb(), headers=bearer(ta)).status_code == 200  # absent
        assert cl.get("/server/site-a/api/config").json() == want

        # readings: idempotent, bad rows dropped, limits
        ru = "/hub/v1/sites/site-a/readings"
        good = [row(1, now - 20 + i) for i in range(5)] + [row(3, now - 5, level=900)]
        bads = [row(1, now, node_id=0), row(1, now, water_level_mm=2**63), row(1, now, temperature_c=1e999),
                row(1, now + 2 * 86400), row(1, now, node_id=True), {"received_at": now}, "x"]
        r = cl.post(ru, content=json.dumps({"rows": good + bads}), headers={**bearer(ta), "Content-Type": "application/json"})
        assert r.status_code == 200 and r.json()["stored"] == 6 and r.json()["rejected"] == len(bads), r.text
        assert abs(r.json()["latest_received_at"] - (now - 5)) < 1e-6
        r = cl.post(ru, json={"rows": good}, headers=bearer(ta))
        assert r.json()["stored"] == 0 and r.json()["rejected"] == 0
        r = cl.post(ru, json={"rows": [row(1, now - 1000 + i) for i in range(501)]}, headers=bearer(ta))
        assert r.status_code == 413 and r.json()["error"]["code"] == "too_many_rows"
        r = cl.post(ru, content=b"x" * 1_100_000, headers=bearer(ta))
        assert r.status_code == 413
        assert cl.post(ru, content=b"not json", headers=bearer(ta)).status_code == 422
        assert cl.post(u, json=hb(), headers=bearer(ta)).json()["latest_received_at"] > 0

        # shapes
        lat = cl.get("/server/site-a/api/latest").json()
        assert [x["node_id"] for x in lat] == [1, 3] and "received_at" in lat[0]
        h = cl.get("/server/site-a/api/history?node=1&limit=3").json()
        assert len(h) == 3 and h[0]["received_at"] > h[2]["received_at"], h
        assert len(cl.get("/server/site-a/api/history?node=1").json()) == 5
        assert cl.get("/server/site-a/api/history?node=0").status_code == 400
        assert cl.get("/server/site-a/api/history").status_code == 422
        tr_ = cl.get("/server/site-a/api/trend?warn=700&danger=1000").json()
        assert [x["node_id"] for x in tr_] == [1, 3] and tr_[0]["level_mm"] == 500 and "direction" in tr_[0], tr_
        assert [n["node_id"] for n in cl.get("/server/site-a/api/nodes").json()] == [1, 2, 3]
        assert cl.get("/server/site-a/api/nodes").json()[2]["name"] == "Node 3"

        # isolation
        assert cl.get("/server/site-b/api/latest").json() == []
        assert cl.get("/server/site-b/api/history?node=1").json() == []
        assert cl.get("/server/site-b/api/trend").json() == []
        assert cl.post("/hub/v1/sites/site-b/readings", json={"rows": good}, headers=bearer(ta)).status_code == 401
        with sqlite3.connect(os.environ["HUB_DB"]) as c:  # A goes quiet 5 minutes ago
            c.execute("UPDATE sites SET last_seen=? WHERE slug='site-a'", (time.time() - 300,))
        a, b = (cl.get(f"/server/{x}/api/status").json() for x in ("site-a", "site-b"))
        assert a["site_ok"] is False and a["site_age_s"] >= 300 and b["site_ok"] is True

        # directory
        d = cl.get("/servers.json").json()
        assert [x["slug"] for x in d] == ["site-a", "site-b"], d
        assert d[0] == {"name": "Alpha", "url": "https://hub.test/server/site-a", "lat": 1.5, "lng": 103.5,
                        "logo": "https://hub.test/logo.png", "slug": "site-a", "online": False,
                        "last_seen_s": d[0]["last_seen_s"]} and d[0]["last_seen_s"] >= 300
        assert d[1]["logo"] == "https://hub.test/b.png" and d[1]["online"] is True
        run("set", "site-b", "--logo", "https://cdn.test/b.png", "--name", "Zulu")
        assert cl.post("/hub/v1/sites/site-b/heartbeat", json=hb("Other"), headers=bearer(tb)).status_code == 200
        d = cl.get("/servers.json").json()
        assert d[-1]["name"] == "Zulu" and d[-1]["logo"] == "https://cdn.test/b.png"

        # misc
        assert cl.get("/server/nope/api/latest").json()["error"]["code"] == "unknown_site"
        assert cl.get("/server/nope/api/latest").status_code == 404
        r = cl.put("/server/site-a/api/nodes/1", json={"lat": 1, "lng": 2})
        assert r.status_code == 403 and r.json() == {"error": {"code": "read_only", "message": "Positions are set by the site."}}
        assert cl.get("/hub/v1/health").json() == {"ok": True, "sites": 4}

        # rotate kills the old token; revoke blocks; remove deletes readings
        tn = token(run("rotate", "site-a"))
        assert cl.post(u, json=hb(), headers=bearer(ta)).status_code == 401
        assert cl.post(u, json=hb(), headers=bearer(tn)).status_code == 200
        assert "site-a" in run("list") and "approved" in run("list")
        try:
            run("remove", "site-a")
            raise AssertionError("remove without --yes")
        except SystemExit:
            pass
        run("remove", "site-a", "--yes")
        with sqlite3.connect(os.environ["HUB_DB"]) as c:
            assert c.execute("SELECT COUNT(*) FROM readings").fetchone()[0] == 0
        assert cl.get("/server/site-a/api/latest").status_code == 404

    # an old database without config_json is migrated, and a second start is harmless
    old = os.path.join(tmp, "old.db")
    with sqlite3.connect(old) as c:
        c.executescript(admin.SCHEMA)
        c.execute("INSERT INTO sites (slug, token_hash) VALUES ('old-one', 'x')")
        assert "config_json" not in [r[1] for r in c.execute("PRAGMA table_info(sites)")]
        hub.init_schema(c)
        hub.init_schema(c)
        assert c.execute("SELECT config_json FROM sites").fetchall() == [(None,)]

    # a broken database answers JSON 503, not a stack trace
    os.environ["HUB_DB"] = hub.DB = os.path.join(tmp, "missing", "x.db")
    with TestClient(hub.app) as cl:
        r = cl.get("/hub/v1/health")
        assert r.status_code == 503 and r.json()["error"]["code"] == "db_unavailable", r.text
    print("hub ok")
finally:
    shutil.rmtree(tmp, ignore_errors=True)
