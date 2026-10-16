# 4G setup: Wi-Fi, token, wiring

The master joins the 4G pocket router's Wi-Fi and pushes readings to `https://jaga.larmkx.com/api/ingest`. Not tested on hardware yet.

## 1. Wi-Fi name, password and token

1. Put an activated data SIM in the dongle. Open its admin page (address is on the sticker). Set Wi-Fi to 2.4 GHz and WPA2-PSK. Set the carrier APN if it does not connect.
2. Make a token on the server machine and put it in that machine's `.env` (never commit it):
   ```
   MASTER_URL=
   INGEST_TOKEN=<48 random hex characters>
   ```
   Make one with `openssl rand -hex 24`. Then `docker compose --profile tunnel --profile hub up -d`.
3. Edit `master/main/node_config.h` (gitignored; copy it from `node_config.h.example` on a fresh clone):
   ```
   #define WIFI_SSID     "<dongle Wi-Fi name>"
   #define WIFI_PASS     "<dongle Wi-Fi password>"
   #define INGEST_URL    "https://jaga.larmkx.com/api/ingest"
   #define INGEST_TOKEN  "<same token as .env>"
   ```
4. Build and flash `master/` (see the `firmware-build-recipe` memory), then open the serial log.
5. Check:
   - "Got IP" means the Wi-Fi works.
   - "Ingest rejected: HTTP 401" means the two tokens differ.
   - `curl https://jaga.larmkx.com/api/latest` shows a reading.

To change the token later, change it in both places and re-flash.

## 2. Wiring

```
Solar panel -> charge controller (PV) 
12 V battery -> charge controller (BAT)
charge controller (LOAD) -> 3 A fuse -> two buck converters
    buck A, set to 5.0 V, 2 A or more -> dongle USB (red 5 V, black GND)
    buck B, set to 5.0 V               -> ESP32 board 5V/VIN pin and GND
```

Rules:
- Set each buck to 5.0 V with a multimeter before connecting anything to it.
- Connect the battery to the controller first, the panel second. Disconnect in the reverse order.
- The controller must have a low-voltage cutoff on its LOAD output. Take all loads from LOAD, not from the battery.
- The dongle has its own supply (buck A). Do not power it from the ESP32. A transmit spike on a shared rail resets the ESP32.
- Add a 470 to 1000 uF capacitor across buck A's output, close to the dongle plug.
- Join all grounds (battery negative, both buck outputs, ESP32 GND).
- Leave the 18650 out of the dongle if you later add the GPIO power switch.
- The ESP32 and the dongle are not wired together. They talk over Wi-Fi, so place them within a few metres.
- Fit a 2 A fuse close to each battery positive terminal if you add a second 7.2 Ah battery in parallel.

Sensor pins on the master, from `master/main/main.c`:

| Part | ESP32 pins |
|---|---|
| nRF24 SCK / MISO / MOSI / CSN / CE | 18 / 19 / 23 / 22 / 5 |
| SD card CS | 13 |
| DHT22 data | 4 |
| JSN-SR40T TX / RX | 14 / 26 |
| RTC SDA / SCL (DS1307, I2C) | 21 / 27 |
| LEDs blue / red / green | 32 / 33 / 25 |

The nRF24 runs on 3.3 V only. A 5 V feed destroys it.

Free pins for later: 16, 17.

## 3. Before going live

- Run `cloudflared` on one machine only. Two machines with the same tunnel token share the traffic, so about half the pushes land on the wrong server. Stop the PC's stack before starting the Pi's: `docker compose --profile tunnel down`.
- On the Pi, run `chmod 600 .env` and check `timedatectl` says the clock is synchronized.
- The Pi stores data on its SD card with no backup. Copy `data/jaga.db` and `hub-data/hub.db` somewhere else regularly.
- `master/main/node_config.h` is now gitignored. Copy `node_config.h.example` to `node_config.h` on a fresh clone and fill it in.
