"""JAGA hub admin: manage sites straight in the hub database. Stdlib only.
   docker compose exec hub python admin.py add uthm-pagoh --name "UTHM Pagoh" --lat 2.15 --lng 102.7"""
import argparse
import hashlib
import json
import os
import re
import secrets
import sqlite3
import sys
import time
from contextlib import closing

DB = os.environ.get("HUB_DB", "/data/hub.db")
SLUG = re.compile(r"^[a-z0-9][a-z0-9-]{1,38}[a-z0-9]$")
RESERVED = {"api", "hub", "server", "servers", "admin", "classic", "dashboard", "downloads", "static"}
SCHEMA = """
CREATE TABLE IF NOT EXISTS sites (
    slug TEXT PRIMARY KEY, name TEXT, lat REAL, lng REAL, logo TEXT, token_hash TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'approved' CHECK (status IN ('approved','pending','revoked')),
    created_at REAL, last_seen REAL, status_json TEXT, nodes_json TEXT, master_ok INTEGER,
    api_version INTEGER, admin_set TEXT);
CREATE TABLE IF NOT EXISTS readings (
    id INTEGER PRIMARY KEY, slug TEXT NOT NULL, received_at REAL NOT NULL, node_id INT,
    log_timestamp_s INT, sender_timestamp_s INT, temperature_c REAL, humidity_pct REAL,
    distance_mm INT, distance_stddev_mm INT, water_level_mm INT,
    UNIQUE (slug, node_id, received_at, log_timestamp_s, sender_timestamp_s));
CREATE INDEX IF NOT EXISTS readings_site_node ON readings (slug, node_id, id);
"""


def connect():
    c = sqlite3.connect(DB, timeout=10)
    c.row_factory = sqlite3.Row
    c.execute("PRAGMA journal_mode=WAL")
    c.executescript(SCHEMA)
    return c


def new_token(c, slug):
    tok = secrets.token_urlsafe(32)
    c.execute("UPDATE sites SET token_hash=? WHERE slug=?", (hashlib.sha256(tok.encode()).hexdigest(), slug))
    return tok


def show(slug, tok):  # the token is never stored, so this is the only time it can be seen
    print(f"Token for {slug} (shown once, store it now). Put these in the site's environment:\n"
          f"SITE_SLUG={slug}\nHUB_URL=<the hub address, e.g. https://jaga.larmkx.com>\nSITE_TOKEN={tok}")


def need(c, slug):
    if not c.execute("SELECT 1 FROM sites WHERE slug=?", (slug,)).fetchone():
        sys.exit(f"No site called {slug}.")


def fixed(c, slug, fields):
    """Record fields the admin set, so heartbeats can't overwrite them, and write the values."""
    row = c.execute("SELECT admin_set FROM sites WHERE slug=?", (slug,)).fetchone()
    locked = set(json.loads(row["admin_set"] or "[]")) | set(fields)
    for k, v in fields.items():
        c.execute(f"UPDATE sites SET {k}=? WHERE slug=?", (v, slug))  # k comes from the fixed list below
    c.execute("UPDATE sites SET admin_set=? WHERE slug=?", (json.dumps(sorted(locked)), slug))


def given(a):
    return {k: getattr(a, k) for k in ("name", "lat", "lng", "logo") if getattr(a, k) is not None}


def main(argv=None):
    p = argparse.ArgumentParser(prog="admin.py")
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("add", "set"):
        s = sub.add_parser(name)
        s.add_argument("slug")
        s.add_argument("--name", required=name == "add")
        s.add_argument("--lat", type=float)
        s.add_argument("--lng", type=float)
        s.add_argument("--logo", help="a path on the site such as /logo.png, or a full URL")
        if name == "add":
            s.add_argument("--pending", action="store_true")
    sub.add_parser("list")
    for name in ("approve", "revoke", "rotate", "remove"):
        s = sub.add_parser(name)
        s.add_argument("slug")
        if name == "remove":
            s.add_argument("--yes", action="store_true")
    a = p.parse_args(argv)

    if a.cmd == "add" and (not SLUG.match(a.slug) or a.slug in RESERVED):
        sys.exit("The slug must be 3 to 40 characters of a-z, 0-9 and '-', not start or end with '-', "
                 "and not be a reserved word.")
    if a.cmd == "remove" and not a.yes:
        sys.exit("This deletes the site and all its readings. Run again with --yes to confirm.")
    for k in ("lat", "lng"):
        v = getattr(a, k, None)
        if v is not None and not abs(v) <= (90 if k == "lat" else 180):
            sys.exit(f"{k} is out of range.")

    with closing(connect()) as c, c:
        if a.cmd == "add":
            if c.execute("SELECT 1 FROM sites WHERE slug=?", (a.slug,)).fetchone():
                sys.exit(f"{a.slug} already exists.")
            c.execute("INSERT INTO sites (slug, token_hash, status, created_at) VALUES (?,?,?,?)",
                      (a.slug, "", "pending" if a.pending else "approved", time.time()))
            fixed(c, a.slug, given(a))
            show(a.slug, new_token(c, a.slug))
        elif a.cmd == "list":
            for r in c.execute("SELECT s.*, (SELECT COUNT(*) FROM readings WHERE slug=s.slug) n FROM sites s ORDER BY slug"):
                seen = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(r["last_seen"])) if r["last_seen"] else "never"
                print(f"{r['slug']:<40} {r['status']:<9} last seen {seen}  readings {r['n']}")
        else:
            need(c, a.slug)
            if a.cmd == "set":
                if not given(a):
                    sys.exit("Give at least one of --name, --lat, --lng, --logo.")
                fixed(c, a.slug, given(a))
            elif a.cmd in ("approve", "revoke"):
                c.execute("UPDATE sites SET status=? WHERE slug=?", ("approved" if a.cmd == "approve" else "revoked", a.slug))
            elif a.cmd == "rotate":
                show(a.slug, new_token(c, a.slug))
            else:
                c.execute("DELETE FROM readings WHERE slug=?", (a.slug,))
                c.execute("DELETE FROM sites WHERE slug=?", (a.slug,))
            if a.cmd != "rotate":
                print("Done.")


if __name__ == "__main__":
    main()
