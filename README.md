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
  logs every reading (its own + received) to `/sdcard/telemetry.csv`, and can
  drive an optional DS1307 RTC (for real timestamps) and SSD1306 OLED (status
  display). Light sleeps between cycles rather than deep sleeping, since it
  must keep listening for incoming slave packets.

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
