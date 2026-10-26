# JAGA - Water Level Monitoring Network

JAGA is a flood-monitoring system. Battery-powered sensor nodes measure temperature, humidity and
water level (ultrasonic distance) and report over nRF24 radio to an always-on master node. The master
logs every reading to a microSD card and pushes it to a server, which feeds a web dashboard and a
Flutter phone app.

```
slave nodes --nRF24--> master node --4G (SIM900A)--> site server --HTTPS--> hub --> dashboard / app
```

## Repository layout

| Path | Purpose |
|---|---|
| `/` (project `jaga`) | Slave node firmware (ESP-IDF). Samples DHT22 + JSN-SR40T, transmits, then deep sleeps. |
| `master/` (project `jaga_master`) | Master firmware (ESP-IDF). Runs the same sensors, listens for slaves, logs to `/sdcard/telem.csv`, pushes readings to the server. Optional DS1307 RTC and SSD1306 OLED. |
| `server/` | Site server (FastAPI + SQLite): stores readings, serves the API, computes trends. |
| `hub/` | Hub that aggregates many sites and serves the public site. |
| `dashboard/` | Web dashboard, server picker and `servers.json` directory. |
| `site-template/` | Starter kit for adding a new site. |
| `app/` | Flutter phone app. |

## Firmware

Each firmware project builds from its own directory:

```sh
cd master          # or stay in the repo root for the slave firmware
idf.py set-target esp32
idf.py build flash monitor
```

### Per-node configuration

Edit `main/node_config.h` in each project before flashing a new node:

| Setting | Meaning |
|---|---|
| `NODE_ID` | Unique per node, 1-8. Currently slaves are 1 and 2, the master is 3. A new slave is a copy of the root project with a new ID. |
| `SONAR_MOUNT_HEIGHT_MM`, `SONAR_MOUNT_ANGLE_DEG` | Sensor mounting geometry, used to convert distance into water level. |
| `SONAR_MAX_RANGE_MM` | Ceiling for valid sonar readings (defaults to the sensor's spec range). |
| `MODEM_APN`, `INGEST_URL`, `INGEST_TOKEN` | Master only. Carrier APN and where the master pushes readings (see `docs/4G_SETUP.md`). |
| `OLED_ENABLED` | Master only. Set to `0` for real deployments to save power. |

### Hardware notes

- **JSN-SR40T** runs in trigger/echo mode. Its ECHO pin is 5 V logic, so use a voltage divider down to 3.3 V.
- **Blind zone:** the JSN-SR40T cannot read closer than about 25 cm. Mount it so the danger level is at least
  30 cm below the sensor, otherwise readings become unreliable exactly when the water is highest.
- **SPI:** the nRF24L01+ and, on the master, the microSD card share one SPI bus (separate CS lines).
- **I2C:** the optional DS1307 RTC and SSD1306 OLED share one I2C bus.
- **Wire format:** `telemetry_packet_t` lives in one shared header, `common/telemetry.h`, included by both the slave and master projects. `test/host/packet_layout.c` pins the byte offsets in CI, so the two sides cannot drift apart.

### Status LEDs

| LED | Meaning |
|---|---|
| Blue | Pulses on each sonar sampling attempt. |
| Green | Pulses on each successful sample; solid after a fully successful cycle. |
| Red | Pulses for 5 s after any read, log or transmit error. |

## Server and API

Start the local stack and open <http://localhost:8088>:

```sh
docker compose up --build
```

- `nginx` serves `dashboard/` and proxies `/api/` to the server.
- `server/app.py` stores readings in SQLite (`data/jaga.db`), so history survives master reboots.
- `server/trend.py` fits the last 30 minutes per node and reports the rate (mm/min), the direction
  (rising, steady, falling) and the time to the warning and danger levels. Self-check: `python server/trend.py`.
- `dashboard/index.html` is the main dashboard (nodes `JAGA-01` to `JAGA-05` map to node IDs 1-5). Add `?sim` for a
  simulated storm. Rain, battery and RSSI show a dash because those sensors do not exist yet. Its drain graphic
  assumes the sensor is 1.45 m above the floor, so set `SONAR_MOUNT_HEIGHT_MM` to `1450`.
- `dashboard/classic.html` is the earlier dashboard with a settings dialog and trend table.

Endpoints: `/api/status`, `/api/nodes`, `/api/latest`, `/api/history?node=1-8&limit=N`,
`/api/trend?warn=&danger=`, `/api/site`.

Sentinel values: `temperature_c` or `humidity_pct` of `-999` means the DHT22 failed; `water_level_mm` of
`-2147483648` means the sonar failed. Without an RTC, timestamps are seconds since boot.

> The master no longer has Wi-Fi or its own HTTP API. It pushes readings to the server's `/api/ingest`
> through a SIM900A modem (`docs/4G_SETUP.md`).

## Hub and sites

One **hub** (`hub/`, at jaga.larmkx.com) serves many **sites** (a `server/` plus a master node each, for example
UTHM Batu Pahat and UTHM Pagoh).

- Sites push to the hub over outbound HTTPS only (heartbeat every 30 s, readings in batches). A site needs no
  tunnel, public IP or open port.
- The hub serves each site at `/server/<slug>` (dashboard) and `/server/<slug>/api/...` (same JSON as a site,
  plus `site_ok` and `site_age_s`).
- If a site goes down, the hub keeps its data and marks only that site offline. If the hub goes down, sites keep
  working locally and catch up when it returns.
- `/servers.json`, the list the app reads, is generated from the registered sites with `online` and `last_seen_s`.
- Each site has its own secret token. The hub admin manages sites with
  `docker compose exec hub python admin.py <command>` (`add`, `list`, `approve`, `revoke`, `rotate`, `set`, `remove`).
  Sites can be added as `--pending` and approved later.

```sh
docker compose --profile hub up -d     # start the hub (data in hub-data/, set HUB_PUBLIC_URL in .env)
```

To add a site, see `site-template/README.md`. To publish this PC's own stack as a site, set `HUB_URL`,
`SITE_SLUG` and `SITE_TOKEN` in `.env`.

Tests: `python hub/test_hub.py`, `python server/test_push.py`, `python server/test_poll.py`.

### Publishing with Cloudflare Tunnel (jaga.larmkx.com)

About 15 minutes of one-time setup. No router ports are opened, and the master node is never exposed.

1. **Add the domain.** Add `larmkx.com` to a free Cloudflare account and switch the registrar's nameservers to
   the two Cloudflare gives you. Wait until the domain shows as Active.
2. **Create the tunnel.** Zero Trust > Networks > Tunnels > Create a tunnel > Cloudflared, named `jaga`. Copy the
   token (the text after `--token`) and skip the install step; Docker runs it.
3. **Add a public hostname.** Subdomain `jaga`, domain `larmkx.com`, service `http://nginx:80`
   (or type `HTTP` and `nginx:80` if the form splits them).
4. **Set the token.** Create `.env` in the repo root (gitignored, never commit it):
   ```
   TUNNEL_TOKEN=<the token>
   MASTER_URL=http://<master IP on your Wi-Fi>
   ```
5. **Start.** `docker compose --profile tunnel up -d`
6. **Check.** Open `https://jaga.larmkx.com`, `/dashboard` and `/servers.json`.

Public routes: `/` homepage, `/dashboard` server picker, `/server/<slug>` a site's live dashboard,
`/servers.json` the directory for the app.

Notes:
- The PC running Docker must stay on and share a network with the master node.
- Visitors can read; moving a map pin is blocked for tunnel traffic.
- To require login for reading, add a Cloudflare Access policy. The phone app cannot pass that login.
- To offer the Android app on the homepage, copy the APK to `downloads/jaga.apk` (gitignored).
- To list a deployment in the app, add `name`, `url`, `lat`, `lng` (and optionally `logo`) to
  `dashboard/servers.json`. This server's icon comes from `SITE_LOGO` (default `/uthm.png`).

## Phone app

The Flutter app has no login because readings are public. It works like a game lobby:

- On first launch it asks for location once, reads the server directory (`directoryUrl` in `app/lib/lobby.dart`)
  and joins the nearest server within 50 km. If location is denied or nothing is close, it shows the list.
- The lobby lists each server with name, distance and a live/offline dot. "Join by address" accepts anything
  not listed (LAN IP, `.local` name or domain).
- The last 5 joined servers are remembered. Settings > Switch server returns to the lobby without auto-joining.
- Each server reports its own centre at `GET /api/site` (name from the `SITE_NAME` env var).
- Alerts fire only while the app is open. Background push would need FCM and an internet-facing server.

### iPhone

The iOS build has **not been tested**. It needs a Mac and Xcode, so it cannot be built on Windows locally.

1. **Build:** GitHub > Actions > "iOS build (unsigned)" > Run workflow. A GitHub Mac produces `jaga-unsigned.ipa`
   as a downloadable artifact.
2. **Install** (signing is required):
   - Free: sign with your Apple ID using Sideloadly or AltStore. It expires after 7 days; maximum 3 apps.
   - Paid ($99/year, Apple Developer Program): TestFlight or ad hoc builds. Needs signing certificates added to
     the workflow as secrets, which are not set up yet.
3. **First checks:** location prompt, notification prompt, the map, and joining a server.

## Known technical debt

Shortcuts and open gaps are tracked in [docs/TECHNICAL_DEBT.md](docs/TECHNICAL_DEBT.md).
