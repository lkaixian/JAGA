# Adding a site to the JAGA hub

A site is one set of sensor nodes plus one ESP32 master, with a small PC or mini server next to it.
The site pushes its readings to the hub over outbound HTTPS. The hub never connects back in, so the site
needs no public address, no tunnel and no router changes.

## On the hub (admin, once per site)

```
docker compose exec hub python admin.py add uthm-batu-pahat --name "UTHM Batu Pahat" --lat 1.8650 --lng 103.0860 --logo /uthm.png
```

The slug (`uthm-batu-pahat`) becomes the address `https://jaga.larmkx.com/server/uthm-batu-pahat`.
Lowercase letters, digits and hyphens, 3 to 40 characters. The command prints the site token **once**.
Other commands: `list`, `approve <slug>`, `revoke <slug>`, `rotate <slug>` (new token), `set <slug> --name ...`, `remove <slug> --yes`.

## On the site PC

1. Install Docker. Copy this repository (or just the `server/`, `dashboard/`, `nginx/` and `site-template/` folders).
2. In `site-template/`: `copy .env.example .env`, then fill in `MASTER_URL`, `SITE_NAME`, `HUB_URL`, `SITE_SLUG`, `SITE_TOKEN`.
3. `docker compose up -d`
4. Check: `http://localhost:8088/api/health` shows `"hub": {"ok": true, ...}`, and
   `https://jaga.larmkx.com/server/<slug>/api/status` answers on the hub within a minute.

## What happens when something is down

| Down | What you see |
|---|---|
| The site (power, internet, PC) | The hub keeps its stored data and shows the site as offline with the time it was last seen. Other sites are not affected. |
| The hub | The site keeps polling and storing locally. When the hub returns, the site sends everything the hub missed. |
| The master node | The site's own dashboard says so; the hub shows the site online with the master unreachable. |

A leaked token only lets someone write fake data for that one site. Revoke it with `revoke <slug>` and issue a new one with `rotate <slug>`.
