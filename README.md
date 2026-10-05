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
| `NODE_ID` | Unique per physical node (slaves currently 1 and 2, master is 3) |
| `SONAR_MOUNT_HEIGHT_MM` / `SONAR_MOUNT_ANGLE_DEG` | This node's sensor mount geometry, used to convert raw distance into water level |
| `SONAR_MAX_RANGE_MM` | Sanity ceiling for sonar readings (defaults to the sensor's own spec range) |
| `WIFI_SSID` / `WIFI_PASS` (master only) | Wi-Fi the master joins to serve the HTTP API |
| `OLED_ENABLED` (master only) | Set to `0` for real deployment - skips the I2C probe and all display writes to save power |

A second slave node is just a copy of the root project with `NODE_ID` set to `2`.

## Hardware notes

- **JSN-SR40T** runs in trigger/echo mode (not UART auto-stream). Its ECHO
  output is 5V logic and needs a voltage divider down to 3.3V before the
  ESP32's echo GPIO.
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

## HTTP API (master)

The master joins Wi-Fi and serves JSON on port 80 at `http://jaga-master.local`
(or the IP it logs as "Got IP" on the serial console; Android often can't
resolve `.local`). CORS is open. The API comes up whenever Wi-Fi does, without a reboot.

| Endpoint | Returns |
|---|---|
| `GET /api/status` | `{now_s, uptime_s, rtc_ok, sd_ok}` - `now_s` is the master's clock, use it to age readings |
| `GET /api/nodes` | `[{node_id, name, lat, lng}]` - coordinates live only in `NODE_META` in `master/main/api.c` |
| `GET /api/latest` | each node's newest reading |
| `GET /api/history?node=1-3&limit=N` | that node's last N readings, newest first (max 90, RAM only) |

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

### Public HTTPS via Cloudflare Tunnel (optional)

Lets the dashboard and the phone app reach the server from anywhere over HTTPS, with no port forwarding.
Only nginx is exposed; the master stays on your LAN and only the Python container talks to it.

1. Domain on Cloudflare DNS (e.g. `larmkx.com`). Zero Trust > Networks > Tunnels > Create tunnel > Cloudflared.
2. Copy the token. Add a public hostname, e.g. `jaga.larmkx.com`, service `http://nginx:80`.
3. `TUNNEL_TOKEN=<token> docker compose --profile tunnel up -d`, then open `https://jaga.larmkx.com`.
4. In the Flutter app settings enter `https://jaga.larmkx.com`.

Writes (moving a pin) are rejected for tunnel traffic; reads are public. To require a login for reads too, add a
Cloudflare Access policy (email one-time PIN) on that hostname. Moving pins works from `http://localhost:8088` on the LAN.

## App: server lobby

Readings are public, so the Flutter app has no login. It works like a game lobby:

- First launch with nothing saved: asks for location once, reads the directory (`dashboard/servers.json`, served at
  `<your domain>/servers.json`, URL set as `directoryUrl` in `app/lib/lobby.dart`), and auto-joins the nearest server
  within 50 km. Denied GPS or nothing close: shows the list instead.
- Lobby list: name, distance, live/offline dot; or "Join by address" for anything not listed (LAN IP, `.local`, a domain).
- Joined servers are remembered (last 5); Settings > Switch server returns to the lobby, and it won't auto-join again.
- To list a new deployment: add `{name, url, lat, lng}` to `servers.json`. Each server also reports its own centre at
  `GET /api/site` (name from the `SITE_NAME` env var).
