# SIM900A setup: SIM, token, wiring

The master dials a mini SIM900A module (GPRS over PPP) and pushes readings to `https://jaga.larmkx.com/api/ingest`. Not tested on hardware yet.

SIM900A is 2G only. It works only where your carrier still serves 2G data (GPRS). If the log says "No PPP link" with a good signal, 2G data is not available on that SIM and you need a 4G module (SIM7600 or A7670) instead.

## 1. SIM, APN and token

1. Use a data SIM with PIN off (put it in a phone and disable the PIN). Set `MODEM_APN` in `node_config.h` to the carrier APN.
2. Make a token on the server machine and put it in that machine's `.env` (never commit it):
   ```
   MASTER_URL=
   INGEST_TOKEN=<48 random hex characters>
   ```
   Make one with `openssl rand -hex 24`. Then `docker compose --profile tunnel --profile hub up -d`.
3. Edit `master/main/node_config.h` (gitignored; copy it from `node_config.h.example` on a fresh clone):
   ```
   #define MODEM_APN     "<carrier APN>"
   #define INGEST_URL    "https://jaga.larmkx.com/api/ingest"
   #define INGEST_TOKEN  "<same token as .env>"
   ```
4. Build and flash `master/` (see the `firmware-build-recipe` memory; delete `master/sdkconfig` once so the PPP settings in `sdkconfig.defaults` apply), then open the serial log.
5. Check:
   - "Modem ..., operator ..., rssi N" means AT works and the SIM registered (rssi 99 = no signal).
   - "PPP up, IP ..." means the data link works.
   - "Ingest rejected: HTTP 401" means the two tokens differ.
   - `curl https://jaga.larmkx.com/api/latest` shows a reading.

To change the token later, change it in both places and re-flash.

## 2. Wiring

Prototype: one protected 18650 (1S1P), HW-736 charger, no 12 V parts.

```
5 to 6 V panel (or 18 V panel -> LM2596 set to 5.5 V) -> 1 A fuse -> HW-736 IN
HW-736 BAT -> protected 18650 (or 1S protection board)
18650 -> 5 V boost, 3 A or more -> ESP32 board 5V/VIN pin and GND
                                 -> SIM900A board 5V
SIM900A board GND -> IRFZ44N/IRLZ44N drain; source -> common GND
GPIO16 -> 100 to 220 ohm -> gate; gate -> 100 k -> GND
GPIO17 (ESP32 TX) -> SIM900A RXD
GPIO27 (ESP32 RX) <- SIM900A TXD (3VT)
```

Rules:
- The SIM900A draws up to 2 A in bursts while transmitting. Put a 1000 uF capacitor across its 5 V and GND, close to the board, not after the MOSFET. If the protection board cuts out, use a stronger cell.
- Check the board's UART level. Most minis are 3.3 V/5 V tolerant on TXD/RXD; if yours outputs 5 V on TXD, divide it down before GPIO27.
- The board must power up on its own when supplied (PWRKEY not needed). If it stays silent, bridge PWRKEY per the board's silkscreen.
- Fit an antenna. The module will not register without one.
- Add a Schottky diode (1N5819 or similar) in series with the panel + wire, stripe toward the LM2596 or HW-736. Do not use a HER208 here, it drops over 1 V.
- Check the panel's open-circuit voltage in sun. The HW-736 input is 4.4 to 6 V; above that, use the LM2596 or a Schottky diode.
- Set the LM2596 (if used) to 5.5 V with a multimeter, panel connected and output unloaded, before wiring it to the HW-736.
- Connect the battery to the HW-736 first, the panel second. Disconnect in the reverse order.
- Join all grounds (battery negative, boost output, ESP32 GND).
- Fit a 2 A fuse close to each battery positive terminal if you add a second 7.2 Ah battery in parallel.

Sensor pins on the master, from `master/main/main.c`:

| Part | ESP32 pins |
|---|---|
| nRF24 SCK / MISO / MOSI / CSN / CE | 19 / 21 / 23 / 22 / 18 |
| SD card CS | 13 |
| DHT22 data | 4 |
| JSN-SR40T TX / RX | 14 / 26 |
| RTC SDA / SCL (DS1307, I2C) | 21 / 27 |
| LEDs blue / red / green | 32 / 33 / 25 |

The nRF24 runs on 3.3 V only. A 5 V feed destroys it.

Used by the modem: 16 (power gate), 17 (TX), 27 (RX). The RTC, SD card and LEDs are no longer fitted on the master.

## 3. Before going live

- Run `cloudflared` on one machine only. Two machines with the same tunnel token share the traffic, so about half the pushes land on the wrong server. Stop the PC's stack before starting the Pi's: `docker compose --profile tunnel down`.
- On the Pi, run `chmod 600 .env` and check `timedatectl` says the clock is synchronized.
- The Pi stores data on its SD card with no backup. Copy `data/jaga.db` and `hub-data/hub.db` somewhere else regularly.
- `master/main/node_config.h` is now gitignored. Copy `node_config.h.example` to `node_config.h` on a fresh clone and fill it in.
