# JAGA - Water Level Monitoring Network

ESP32 + nRF24L01+ sensor network: battery-powered slave nodes report temperature,
humidity, and water level (via ultrasonic distance) to an always-on master node,
which aggregates and logs everything to a microSD card.

## Structure

Two independent ESP-IDF projects live in this repo:

- **`/` (project `jaga`)** - slave sensor node firmware. Samples DHT22 + JSN-SR40T
  once per cycle, transmits over nRF24, deep sleeps for the rest of the cycle.
  Flash this onto each battery-powered sensor node.
- **`/master` (project `jaga_master`)** - master node firmware. Runs the same
  sensors as a slave, *and* continuously listens on nRF24 for the slaves,
  logs every reading (its own + received) to `/sdcard/telem.csv`, and can
  drive an optional DS1307 RTC (for real timestamps) and SSD1306 OLED (status
  display). Stays awake between cycles (no sleep) since it must keep listening
  for slave packets and serving the HTTP API; Wi-Fi modem sleep handles power.

Each is built/flashed independently from its own directory:

```sh
cd master   # or the repo root for the slave firmware
idf.py set-target esp32
idf.py build flash monitor
```

## Per-node configuration

`main/node_config.h` in each project holds values that differ per physical
unit - **edit these before flashing a new node**:

| Setting | Meaning |
|---|---|
| `NODE_ID` | Unique per physical node (1-8; the master stores up to 8 nodes, currently slaves 1 and 2, master is 3) |
| `SONAR_MOUNT_HEIGHT_MM` / `SONAR_MOUNT_ANGLE_DEG` | This node's sensor mount geometry, used to convert raw distance into water level |
| `SONAR_MAX_RANGE_MM` | Sanity ceiling for sonar readings (defaults to the sensor's own spec range) |
| `MODEM_APN` / `INGEST_URL` / `INGEST_TOKEN` (master only) | Carrier APN for the SIM900A, and where the master pushes readings (see `docs/4G_SETUP.md`) |
| `OLED_ENABLED` (master only) | Set to `0` for real deployment - skips the I2C probe and all display writes to save power |

A second slave node is just a copy of the root project with `NODE_ID` set to `2`.

## Hardware notes

- **JSN-SR40T** runs in trigger/echo mode (not UART auto-stream). Its ECHO
  output is 5V logic and needs a voltage divider down to 3.3V before the
  ESP32's echo GPIO.
- **JSN-SR40T blind zone:** it cannot read closer than about 25 cm. Mount it so the danger level
  is at least 30 cm below the sensor, or the reading turns to garbage exactly when the water is highest.
- **nRF24L01+** shares one SPI bus; on the master, the microSD card shares
  that same bus (separate CS line).
- **DS1307** (optional) and the **SSD1306 OLED** (master only, optional)
  share one I2C bus.
- The `telemetry_packet_t` struct layout in `main.c` must stay byte-identical
  between the slave and master projects - it's the over-the-air wire format.

## Status LEDs

Blue pulses per sonar sampling attempt, green pulses on each successful
sample and lights solid on a fully successful cycle, red pulses for 5s on
any read/log/TX error that cycle.

## HTTP API (master, removed)

The master no longer has Wi-Fi: it pushes readings through a SIM900A modem to `/api/ingest` on the server (`docs/4G_SETUP.md`). The section below describes the old Wi-Fi API and no longer applies. The master used to join Wi-Fi and serve JSON on port 80 at `http://jaga-master.local`
(or the IP it logs as "Got IP" on the serial console; Android often can't
resolve `.local`). CORS is open. The API comes up whenever Wi-Fi does, without a reboot.

| Endpoint | Returns |
|---|---|
| `GET /api/status` | `{now_s, uptime_s, rtc_ok, sd_ok, radio_ok}` - `now_s` is the master's clock, use it to age readings |
| `GET /api/nodes` | `[{node_id, name, lat, lng}]` - coordinates live only in `NODE_META` in `master/main/api.c` |
| `GET /api/latest` | each node's newest reading |
| `GET /api/history?node=1-8&limit=N` | that node's last N readings, newest first (max 90, RAM only) |

Sentinels: `temperature_c`/`humidity_pct == -999` = DHT22 failed;
`water_level_mm == -2147483648` = sonar failed. Without an RTC, timestamps are
seconds since boot; with one they are local wall time stored as UTC epoch.

Clients: `dashboard/index.html` (open from disk, set the API URL in Settings;
must be `file://` or plain `http://`) and the Flutter app in `app/`. Alerts in
the app only fire while it is running; background push would need FCM and an
internet-facing server.

## Dashboard server (nginx + Python + SQLite)

```sh
MASTER_URL=http://<master-ip> docker compose up --build   # then open http://localhost:8088
```

- `nginx` serves `dashboard/` and proxies `/api/` to the `api` container.
- `server/app.py` (FastAPI) polls the master every 10s into SQLite (`data/jaga.db`), so history
  outlasts the master's 90-reading RAM buffer and survives its reboots. It serves the same
  `/api/status|nodes|latest|history` shape, plus `GET /api/trend?warn=&danger=`.
- `server/trend.py` fits the last 30 min per node: rate (mm/min), rising/steady/falling, and time to the
  warning/danger levels. Self-check: `python server/trend.py`.
- `MASTER_URL` needs the master's IP: containers can't resolve `.local`.
- `dashboard/index.html` is the team's J.A.G.A page (5 nodes `JAGA-01..05` = node ids 1..5), reading this API.
  Add `?sim` for its simulated storm. Rain, battery and RSSI show a dash: no such sensors yet.
  Its drain graphic assumes the sensor is 1.45 m above the floor, so set `SONAR_MOUNT_HEIGHT_MM` to 1450 in `node_config.h`.
- `dashboard/classic.html` is the earlier dashboard (settings dialog, trend table).

### Putting it on the internet: jaga.larmkx.com (Cloudflare Tunnel)

One-time setup, about 15 minutes. No router ports are opened and the master node is never exposed: only nginx is.

1. **Domain on Cloudflare.** Add `larmkx.com` to a free Cloudflare account and change the nameservers at your registrar to the two Cloudflare gives you. Wait until Cloudflare shows the domain as Active.
2. **Create the tunnel.** Cloudflare dashboard > Zero Trust > Networks > Tunnels > Create a tunnel > Cloudflared. Name it `jaga`. On the next screen copy the long token (the text after `--token`). Skip the install step: Docker runs it for you.
3. **Public hostname.** In the tunnel's Public Hostname tab add: subdomain `jaga`, domain `larmkx.com`, service `http://nginx:80` (if the form has a separate type dropdown, pick HTTP and enter `nginx:80`; if it has one box, type the whole `http://nginx:80`). Cloudflare creates the DNS record itself.
4. **Give the token to Docker.** In `C:\JAGA` create a file named `.env` (it is gitignored, never commit it) containing:
   ```
   TUNNEL_TOKEN=<the token>
   MASTER_URL=http://<master IP on your Wi-Fi>
   ```
5. **Start it.** `docker compose --profile tunnel up -d`
6. **Check it.** Open `https://jaga.larmkx.com` (homepage), `https://jaga.larmkx.com/dashboard`, and `https://jaga.larmkx.com/servers.json` (the list the app reads).

What you get: `/` homepage (pick a server, app download), `/dashboard` the server picker with the nearest servers, `/server/<slug>` one site's live dashboard (a bare `/<slug>` redirects there), `/servers.json` the directory for the app, `/server/<slug>/api/...` that site's data.
The PC running Docker must stay on and on the same network as the master node. Visitors can read; moving a pin is blocked for tunnel traffic. To require a login for reading too, add a Cloudflare Access policy (email one-time PIN) on `jaga.larmkx.com`, but note the phone app cannot pass that login.
To offer the Android app on the homepage, copy the APK to `downloads/jaga.apk` (the folder is gitignored).
Server list for the app: add one entry per deployment to `dashboard/servers.json` (`name`, `url`, `lat`, `lng`, optional `logo` path or URL). This server's icon comes from `SITE_LOGO` (default `/uthm.png`).

## Hub and sites (many deployments)

One **hub** (`hub/`, at jaga.larmkx.com) serves many **sites** (`server/` + a master node each, e.g. UTHM Batu Pahat, UTHM Pagoh).

- A site pushes to the hub over outbound HTTPS only (heartbeat every 30 s, readings in batches). The hub never connects in, so a site needs no tunnel, public IP or open port.
- The hub stores each site under its own slug and serves it at `https://jaga.larmkx.com/server/<slug>` (dashboard) and `.../server/<slug>/api/...` (same JSON as a site server, plus `site_ok` and `site_age_s`).
- If a site goes down, the hub keeps its stored data and marks only that site offline. Other sites are not affected. If the hub goes down, sites keep working locally and catch up when it returns.
- `/servers.json` (the list the app reads) is generated by the hub from the registered sites, with `online` and `last_seen_s`.
- Each site has its own secret token; a site is added by the hub admin with `docker compose exec hub python admin.py add <slug> --name "..." ...` (`list`, `approve`, `revoke`, `rotate`, `set`, `remove` also exist). Sites can be created as `--pending` and approved later.
- Start the hub: `docker compose --profile hub up -d` (data in `hub-data/`, set `HUB_PUBLIC_URL` in `.env`). Add a site: see `site-template/README.md`.
- This PC's own stack (`docker compose up -d`) is itself a site; to publish it on the hub, set `HUB_URL`, `SITE_SLUG` and `SITE_TOKEN` in `.env`.
- Tests: `python hub/test_hub.py`, `python server/test_push.py`, `python server/test_poll.py`.

## App: server lobby

Readings are public, so the Flutter app has no login. It works like a game lobby:

- First launch with nothing saved: asks for location once, reads the directory (`dashboard/servers.json`, served at
  `<your domain>/servers.json`, URL set as `directoryUrl` in `app/lib/lobby.dart`), and auto-joins the nearest server
  within 50 km. Denied GPS or nothing close: shows the list instead.
- Lobby list: name, distance, live/offline dot; or "Join by address" for anything not listed (LAN IP, `.local`, a domain).
- Joined servers are remembered (last 5); Settings > Switch server returns to the lobby, and it won't auto-join again.
- To list a new deployment: add `{name, url, lat, lng}` to `servers.json`. Each server also reports its own centre at
  `GET /api/site` (name from the `SITE_NAME` env var).

## iPhone

The Flutter code is shared with Android, but an iPhone build needs a Mac and Xcode, so it cannot be built or tested on a Windows PC. **The iOS build has not been tested by anyone yet.**

1. Build: GitHub > Actions > "iOS build (unsigned)" > Run workflow. It builds on a GitHub Mac (manual trigger only) and gives you `jaga-unsigned.ipa` as a downloadable artifact.
2. Install on a real iPhone, which needs signing:
   - **Free:** sign with your own Apple ID using Sideloadly or AltStore on a PC. The install expires after 7 days and must be redone; up to 3 apps.
   - **Paid ($99 a year, Apple Developer Program):** TestFlight or ad hoc builds. This needs signing certificates added to the workflow as secrets; not set up.
3. First things to check on a phone: the location prompt, the notification prompt (alerts show while the app is open), the map, and joining a server.
