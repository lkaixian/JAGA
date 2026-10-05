# Error handling design

Covers the slave and master firmware, the Python server, the dashboard and the Flutter app.
Status: phases P0 and P1 are built and committed; P2 is in progress. Firmware changes are written but
not yet compiled (no ESP-IDF toolchain in the dev shell). The build order is at the end.

## 1. Principles

1. **One broken part never takes down the rest.** A loose sensor wire, a missing SD card or a dead Wi-Fi
   network each degrade one feature. None of them reboot-loops a node.
2. **Unknown is not zero.** A failed read is reported as failed, never as 0 m or the last good value
   without a label. The firmware already does this with sentinels (below); every layer keeps doing it.
3. **Stale beats blank, but stale is always labelled.** Show the last known value with its age and a
   dimmed style. A node going silent while its level was rising is the worst case, so that value stays visible.
4. **Retry with limits.** Every retry has a cap, a back-off and a place it gives up. Writes are idempotent
   (the server already skips a reading it stored).
5. **Say what happened, what it means, what to do.** Plain words, no "Oops", no "Something went wrong".
6. **Fail loudly in one place.** Each layer reports its health in one place the next layer can read
   (`/api/status`, the OLED, a banner), not only in a log.

Sentinels, the single contract everyone parses:

| Value | Meaning | Where |
|---|---|---|
| `temperature_c`, `humidity_pct` = `-999` | DHT22 read failed | firmware, API passthrough |
| `water_level_mm` = `-2147483648` (INT32_MIN) | Sonar read failed (0 valid samples) | firmware, API passthrough |
| `rtc_ok` / `sd_ok` / `radio_ok` = `false` | That peripheral is not working right now | `/api/status` |

## 2. Shared vocabulary

Two kinds of state, derived the same way in the dashboard and the app. If they disagree, one has a bug.

**Link state** (where the break is), highest priority first:

| State | Test | Banner text |
|---|---|---|
| Server unreachable | request to the server fails or times out (5 s) | "Can't reach the server. Showing data from 14:32." |
| Master unreachable | server answers, `master_ok` is false (3 failed polls, about 30 s) | "The server can't reach the master node. Readings stopped at 14:32." |
| Connected | neither of the above | none |

**Node state:**

| State | Test | Shown as |
|---|---|---|
| Live | newest reading under 180 s old, sonar value valid | normal |
| Sensor fault | newest reading arrived, but sonar value is the sentinel | "JAGA-02's water sensor isn't reading. Check the sonar wiring." Temperature still shown if valid. |
| No signal | newest reading 180 s or older (3 missed uplinks) | dimmed, "JAGA-02 hasn't reported for 7 min. Last level 0.62 m." |
| No data | never heard from | "No data yet" |

Rule: a node that goes **No signal while at Watch or above** keeps its status chip and gets an extra
"lost signal" line. Silence near a flood is not "all clear".

## 3. Firmware

| # | Failure | Today | Design |
|---|---|---|---|
| F1 | DHT22, sonar or nRF24 init fails (`ESP_ERROR_CHECK`) | Abort, reboot, repeat forever. A slave drains its battery and sends nothing. | Log it, set a `*_ok` flag, carry on. Missing sonar sends the INT32_MIN sentinel, missing DHT sends -999. A slave with no radio goes back to deep sleep and tries again next wake. The master shows `RADIO FAIL` on the OLED and `radio_ok:false` in `/api/status`. |
| F2 | Slave packet gets no ACK | The reading is lost. | The nRF24 already retries 15 times in hardware. Add 3 software attempts, 300 ms apart. If all fail, keep the packet in an `RTC_DATA_ATTR` queue (last 5, survives deep sleep) and send the queue oldest-first on the next wake, before the new reading. The packet carries its own `timestamp_s`, so the wire format is unchanged and the master logs the original time. |
| F3 | Master main loop hangs | Task watchdog only logs (`ESP_TASK_WDT_PANIC` off). | Turn panic on, timeout 30 s. Subscribe the main task and feed it in the 1 s drain loop. A hang now reboots in 30 s. |
| F4 | SD card missing, removed or write fails | Mounted once at boot. Never retried. `sd_ok` is set once. | On a write failure, unmount and set `sd_available=false`. Every 10 cycles, if false, try to mount again. Call `api_set_health()` each cycle so `sd_ok` is live. |
| F5 | Wi-Fi drops | Reconnect immediately, no delay, forever. | Back off 1, 2, 4, 8, 16, 30 s and hold at 30 s. If `httpd_start` fails, retry in 30 s. |
| F6 | An API handler fails (bad query, out of memory) | Partial reply or none. | Reply `500` with `{"error":{"code":"internal","message":"..."}}`. Same shape as the server's. |
| F7 | Sonar reads inside the 25 cm blind zone | Garbage distance. | Not fixable in software. Design rule: mount the sensor so the danger level is at least 30 cm below it. Documented in the README, not coded. |

Not done, on purpose: battery telemetry, over-the-air updates, a boot-loop counter. Deep sleep already
paces a failing slave, and F1 removes the loop.

## 4. Server (Python + SQLite + nginx)

| # | Failure | Today | Design |
|---|---|---|---|
| S1 | One master endpoint fails during a poll | `poll_once` stops at the first exception, so `/api/latest` is skipped if `/api/status` fails. | Fetch status, nodes and latest independently. A failure in one never blocks the others. |
| S2 | Master down | Prints "poll failed" every 10 s. | Track `master_ok`, `last_ok`, consecutive failures. Back off 10, 20, 30 s. Log only on a change (up to down, down to up). Return `master_ok` and `master_age_s` in `/api/status` (the age already exists). |
| S3 | Master returns a wrong shape | KeyError, whole poll lost. | Validate each row (required keys, numeric types). Drop the bad row, count it in `rejected`, keep the rest. |
| S4 | SQLite error (locked, disk full, corrupt) | Exception in the poll thread or a 500 with a stack trace. | Enable WAL. Catch `sqlite3.Error` in the poll loop and report `db_ok:false`. A global handler turns anything else into a JSON 503. |
| S5 | Error shape | FastAPI default `{"detail": ...}` | One shape everywhere: `{"error":{"code":"master_unreachable","message":"..."}}`. Codes: `master_unreachable`, `db_unavailable`, `bad_request`, `internal`. Clients branch on `code`, never on the text. |
| S6 | `api` container is down, nginx answers 502 | HTML error page. | `error_page 502 503 504` returns the same JSON with code `server_down`. |
| S7 | Container hangs | Nothing notices. | `/api/health` plus a Docker `healthcheck`. nginx waits for the API to be healthy. `restart: unless-stopped` stays. |
| S8 | Database grows forever | Never pruned. | Optional: delete readings older than 180 days once a day. About 1.5 M rows per year for 3 nodes, so this can wait. |

## 5. Dashboard

| # | Failure | Today | Design |
|---|---|---|---|
| D1 | Server or master down | Header text switches to "Offline". | Link banner from section 2, with the time of the last good data. Poll backs off 10, 20, 30 s and polls at once when the tab regains focus. |
| D2 | Sonar fault in the newest reading | The row is filtered out silently, so the page shows an older level as if it were current. | Keep the newest raw row. If it is a sonar fault, show the Sensor fault state. Older valid points stay in the chart. |
| D3 | Node silent | Last value shown as current. | No signal state: dimmed card, row and map pin (grey ring), "last seen N min ago". Severity banner keeps working off the last value but says it is old. |
| D4 | Moving a pin fails to save | `alert()` popup, pin stays moved. | Put the pin back, show an inline notice: "Couldn't save the position. Nothing changed." |
| D5 | Leaflet, map tiles or fonts blocked | Map shows a text fallback (exists). | Keep. Everything else must still render. Add a test by blocking the CDN. |
| D6 | Unexpected response | Logged to the console only. | Any parse failure counts as a failed poll and shows in the banner details, not only the console. |

## 6. App

| # | Failure | Today | Design |
|---|---|---|---|
| A1 | Offline | One `online` boolean. | Same link and node states as the dashboard (section 2), shown as a banner at the top of the Status tab. |
| A2 | Polling | Every 10 s, even when backgrounded. | Same back-off as D1. Pause when the app is backgrounded, refresh at once on resume. |
| A3 | Server and app out of step | Parse errors are swallowed. | Add `api_version` to `/api/status`. If it is newer than the app supports, say "This server needs a newer app." A `TypeError` or `FormatException` while parsing counts as a failed poll with that message. |
| A4 | Directory unreachable | Falls back to recent servers (exists). | Also cache the last good directory in `SharedPreferences`, so the lobby still lists servers offline. |
| A5 | Joining fails | One message: "Can't reach X". | Three messages: no network, server not answering, "That address answered, but it isn't a JAGA server." (the reply has no `now_s`). |
| A6 | Notification permission denied | Alerts silently never fire. | Settings shows a warning row: "Alerts are off. Allow notifications to get flood alerts." |
| A7 | Node goes silent while high | No alert. | One local notification when a node at Watch or above hits No signal. Fires once per outage. |
| A8 | App closed | No alerts. | Known limit (needs push + an internet-facing sender). Stated in Settings and the README. |

## 7. Constants (one table, used everywhere)

| Name | Value | Used by |
|---|---|---|
| Request timeout | 5 s | server to master, dashboard, app |
| Poll interval, healthy | 10 s | server, dashboard, app |
| Poll back-off | 10, 20, 30 s, capped | server, dashboard, app |
| Node stale after | 180 s | dashboard, app |
| Master unreachable after | 3 failed polls (about 30 s) | server |
| Slave TX attempts per wake | 3, 300 ms apart | slave |
| Slave unsent queue | 5 packets | slave |
| Master task watchdog | 30 s | master |
| SD remount | every 10 cycles | master |
| Wi-Fi reconnect back-off | 1 to 30 s | master |

If a number changes, change it here first.

## 8. Message wording

Each message says what happened and what to do. No exclamation marks, no apologies, no "Oops".

| Situation | Text |
|---|---|
| Server unreachable | Can't reach the server. Showing data from 14:32. |
| Master unreachable | The server can't reach the master node. Readings stopped at 14:32. |
| No signal | JAGA-02 hasn't reported for 7 min. Last level 0.62 m. |
| Sensor fault | JAGA-02's water sensor isn't reading. Check the sonar wiring. |
| Position not saved | Couldn't save the position. Nothing changed. |
| Join: no network | No internet connection. Check your network and try again. |
| Join: no answer | That server isn't answering. |
| Join: wrong service | That address answered, but it isn't a JAGA server. |
| Nothing nearby | No server found within 50 km. Pick one below or enter an address. |
| Alerts off | Alerts are off. Allow notifications to get flood alerts. |
| Server too new | This server needs a newer version of the app. |

## 9. Build order

| Phase | Items | Why first | Size |
|---|---|---|---|
| P0: stop losing data | F1, F2, F3, S1, S2, D2, D3 | These are the cases where a reading is lost or a stale level looks current. | 1 to 2 days |
| P1: say where it broke | S5, S6, S7, D1, D4, A1, A2, F4, F5 | Link banner, one error shape, healthchecks. | 2 days |
| P2: polish | A3 to A7, D5, D6, F6, S3, S4, S8 | Version check, offline directory, extra alerts. | 1 to 2 days |

One runnable check per layer, as the rest of the repo does it:
- Firmware: build with the queue code and unplug the sonar. The node must still send, with the sentinel.
- Server: extend `smoke.py` with a master that fails one endpoint, then all of them, and assert the stored
  data survives and `master_ok` flips.
- Dashboard: a headless page load with the API returning 503, then with the CDN blocked.
- App: unit tests for the node-state function (stale, fault, no data) and the join error classifier.

## 10. Not doing

- No cloud queue and no end-to-end delivery receipt from slave to dashboard.
- No client-side offline outbox. Reads are public and writes are only pin moves.
- No automatic master reboot from the server.
- No retries inside a single HTTP request. The next poll is the retry.
