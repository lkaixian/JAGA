# Where things stand (2026-10-08) and how to retry

## Done, not yet tried on real hardware
- `pi-server/` + `pi-server.tar.gz`: whole stack for a Raspberry Pi 4 (both gitignored; rebuild with the copy command below after changing server/, hub/, dashboard/ or nginx/).
- Push mode: ESP32 master POSTs readings to `/api/ingest` (commit c55234b). Compiles; never flashed. Server test: `python server/test_ingest.py`.

## Retry on the Pi
1. `scp C:\JAGA\pi-server.tar.gz <user>@<pi>:~/` then `tar xzf pi-server.tar.gz && cd pi-server && ./install.sh`
2. Edit `.env`: `TUNNEL_TOKEN`, `HUB_PUBLIC_URL=https://jaga.larmkx.com`, `MASTER_URL=` (empty), `INGEST_TOKEN=$(openssl rand -hex 24)`.
3. Stop cloudflared on the PC first (`docker compose stop cloudflared`), then `./install.sh hub tunnel` on the Pi.
4. Register the site on the Pi hub and put the printed token in `.env` as `SITE_TOKEN`, with `HUB_URL=http://nginx:80`, `SITE_SLUG=uthm-batu-pahat`:
   `docker compose --profile hub --profile tunnel exec hub python admin.py add uthm-batu-pahat --name "UTHM Parit Raja" --lat 1.8650 --lng 103.0860 --logo /uthm.png`
   then `docker compose --profile hub --profile tunnel up -d`.
5. Firmware: in `master/main/node_config.h` set `WIFI_SSID`, `WIFI_PASS`, `INGEST_URL "https://jaga.larmkx.com/api/ingest"`, `INGEST_TOKEN` (same as the Pi). Never commit them. Build and flash (see memory `firmware-build-recipe`).
6. Check: ESP32 serial log shows "Got IP" and no "Ingest rejected"; `curl https://jaga.larmkx.com/api/latest` shows a reading; `/api/status` says `master_ok: true`.

## If it fails
- `Ingest rejected: HTTP 401`: token differs between firmware and Pi `.env`.
- HTTP 403 or no POST arrives: nginx config not reloaded (`docker compose restart nginx`).
- TLS error: board clock or Wi-Fi not up yet; it retries on its own (5 s, doubling to 60 s).
- Dashboard says master unreachable: no POST for 120 s.

## Repack after changes
`cd C:\JAGA && rm -rf pi-server/{server,hub,dashboard,nginx,docker-compose.yml} && cp -r server hub dashboard nginx docker-compose.yml pi-server/ && tar czf pi-server.tar.gz pi-server` (keeps the hand-written `.env.example`, `install.sh`, `README.md` inside `pi-server/`; do not `rm -rf pi-server` itself).
