# JAGA project notes

Student project for IUIC UTHM 2026. Last updated 2026-10-06. Repo: github.com/lkaixian/JAGA (private), branch `master`.

## What it is

Ultrasonic water-level sensors in campus drains send readings by nRF24 radio to an ESP32 master node. A small server next to the master stores them and serves a dashboard, an API and a phone app. A central hub (jaga.larmkx.com) can serve many sites.

## Layout

| Folder | What |
|---|---|
| `/` (root) | Slave node firmware (ESP-IDF 4.3.6). Samples DHT22 + JSN-SR40T, sends over nRF24, deep sleeps. Unsent packets queue in RTC memory. |
| `master/` | Master node firmware. Same sensors, receives slaves (ids 1 to 8), logs to SD, serves `/api/*` over Wi-Fi. |
| `server/` | One site's Python service (FastAPI + SQLite). Polls the master, serves the API, optionally pushes to the hub. |
| `hub/` | Central service. Sites push to it; it serves each under `/server/<slug>` and builds `/servers.json`. |
| `dashboard/` | Static pages: `index.html` (home), `dashboard.html` (server picker), `site.html` (one site's live dashboard), `classic.html` (older), `servers.json` (fallback list), logos, `river.js`. |
| `nginx/`, `docker-compose.yml` | Front door. Profiles: default = api + nginx, `tunnel` = Cloudflare Tunnel, `hub` = hub service. |
| `app/` | Flutter app (Android arm64 built and tried; iPhone never built). |
| `site-template/` | Copy-and-fill compose for onboarding another site. |
| `docs/ERROR_HANDLING.md` | The failure-handling design (all phases built). |

## How it fits together

- Site to hub is one way: the site pushes over outbound HTTPS, the hub never connects in. Heartbeat every 30 s, readings in batches, resume is stateless.
- URLs: `/` home, `/dashboard` picker with nearest servers, `/server/<slug>` live dashboard (a bare `/<slug>` redirects), `/servers.json` directory, `/server/<slug>/api/...` data, `/downloads/jaga.apk`.
- This PC is registered as site `uthm-batu-pahat` (display name "UTHM Parit Raja"). Its `.env` points the site at the hub inside Docker (`HUB_URL=http://nginx:80`).
- Link states everywhere: server unreachable > site offline > master unreachable. Node states: live, sensor fault, no signal (180 s), no data.
- Water levels (warning, danger, chart top) are published by each server at `/api/config`. In the app they are the defaults; "Use my own levels" overrides them per server, and "Overwrite protection" (on by default) stops the server replacing custom values.
- Writes (moving a map pin) are blocked for traffic arriving through the tunnel.

## Running it

```
docker compose --profile tunnel --profile hub up -d     # api, nginx, cloudflared, hub
docker compose exec hub python admin.py list             # registered sites
```
`.env` (gitignored, never commit) holds `TUNNEL_TOKEN`, `MASTER_URL`, `HUB_PUBLIC_URL`, `HUB_URL`, `SITE_SLUG`, `SITE_TOKEN`. Optional: `SITE_NAME`, `SITE_LOGO`, `WARN_MM`, `DANGER_MM`, `DEPTH_MM`.
Tests: `python server/test_poll.py`, `server/test_push.py`, `hub/test_hub.py`, `server/trend.py`; `cd app && flutter analyze && flutter test`.
Android APK: `cd app && flutter build apk --release --target-platform android-arm64`, then copy to `downloads/jaga.apk` (gitignored) to offer it on the homepage.

## Not done or not verified

- The master node has never been connected to the server, so no real readings have flowed. Flash `master/` with real `WIFI_SSID`/`WIFI_PASS` in `master/main/node_config.h` (currently `CHANGE_ME`) and set `MASTER_URL` to its IP.
- Firmware compiles (both projects, no warnings) but has not been flashed. Hardware tests still to do: sensor unplugged, SD card pulled, two slaves powered on together, OLED text fit.
- Sonar: the drain graphic assumes the sensor is 1.45 m above the floor, so set `SONAR_MOUNT_HEIGHT_MM` to 1450. The sensor cannot read closer than about 25 cm, so mount it with the danger level at least 30 cm below it.
- The app has been run on a phone only by the owner, informally. Not covered: notifications, the 8-line chart, dark map tiles.
- iPhone: workflow `.github/workflows/ios.yml` builds an unsigned .ipa on a GitHub Mac when run by hand. Never run, never installed.
- `dashboard/uthm.png` is the wide UTHM banner on white. Replace it with the square crest if one is approved for use. Check usage rights with the university.
- Optional, not built: mirroring readings to Supabase for the Netlify demo copy; editable node names and thresholds on the web dashboard; request-and-approve flow for new sites (the hub already stores a `pending` status).
- Public access is read-only without a login. Cloudflare Access can add one, but the phone app cannot pass it.

## Decisions worth remembering

- SQLite, not Postgres or Mongo: small fixed-shape rows, one file, no extra container.
- Server timestamps (`received_at`) drive staleness and trends. The master's own timestamps can be seconds since boot and restart at zero, so they are never compared with real time.
- Duplicate check is a 10-minute window on (log time, sender time, level), not a unique key, because the master's timestamps restart after a reboot.
- Map package pinned to flutter_map 7 and latlong2 0.9: newer versions pull in `path_provider_foundation`, whose build step fails when the Flutter SDK path contains a space (`D:\New folder\flutter`).

## Gotchas on this machine

- Git Bash rewrites arguments that start with `/` into Windows paths (it once stored a logo as `C:/Program Files/Git/uthm.png`). Prefix commands with `MSYS_NO_PATHCONV=1`.
- `idf.py` does not run from the Claude shell; use the stub-`mconf` recipe saved in the assistant's memory (`firmware-build-recipe`).
- `docker compose` needs `--profile tunnel --profile hub` for every command that should keep those services.
