# Known technical debt

Shortcuts taken on purpose, and gaps not yet closed. Each is marked in code with a `ponytail:` comment where one exists.

| Area | Debt | Fix when |
|---|---|---|
| Server ingest (`server/app.py`) | Duplicate readings are filtered by a 10-minute window, not a unique key. | Replays older than 10 minutes cause double rows. |
| Master API (`master/main/api.c`) | The master's own rows get the newest SIM signal, not the signal at sampling time. | Signal history per reading is needed. |
| RTC (`ds1307.c`) | The clock is set from build time, so it is off by the build-to-flash gap. | Real timestamps matter, so set it from the server or NTP. |
| Trend (`server/trend.py`) | Fixed 1 mm/min steady dead-band for every site. | Per-site tuning is needed. |
| Dashboard (`dashboard/site.html`) | Ages use the browser clock against the server clock, so they skew if the two differ. | Use the server's `now_s` for all ages. |
| App (`app/lib/api.dart`) | Connection refusals are matched by message text because OS error codes differ per platform. | A platform changes the wording. |
| Node config (`node_config.h`) | Sonar mount height and angle are placeholders, not measured values. | Before any real deployment. |
| Android release | Still the template application ID and debug signing config (`build.gradle.kts`). | Before publishing to the Play Store. |
| Hardware | The master has never been connected to real sensors, and PPP push over the SIM900A is untested. | First field test. |
| iOS | Build workflow exists but has never been run or tested on a device. | Before offering an iPhone app. |
| Alerts | The app alerts only while open; no background push. | Add FCM and an internet-facing server. |
