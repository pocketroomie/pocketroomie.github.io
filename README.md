# Roomie Beacon

Firmware that turns any ESP32 into a Bluetooth room beacon for the **Roomie** iOS app,
plus a one-click web installer.

**Install:** https://roomiehome.github.io/roomiehome/ (Chrome or Edge on a computer).

## How it works

1. Flash the firmware from the install page. The chip type is detected automatically
   (ESP32, ESP32-C3, ESP32-S3, ESP32-C6, ESP32-H2).
2. The board starts **unconfigured**: it advertises a setup signal named `Roomie-XXXX`
   (the last 4 hex digits of its Bluetooth address).
3. In Roomie, pick the beacon and the room. Roomie writes its settings over Bluetooth; the beacon
   saves them and restarts as a room beacon.
4. To change a beacon later, power-cycle it: for **3 minutes** after power-on it accepts changes
   (alternating between the beacon signal and the setup signal), then it's a beacon only.

There is no PIN: being able to unplug the beacon is the protection. Re-flashing with "erase" (or
Roomie's "Reset beacon") returns it to unconfigured.

## Power

Bluetooth LE only (Wi-Fi is never started), NimBLE stack, CPU at its lowest Bluetooth speed,
no logging. The chip stays awake (no light sleep): beacons are meant for USB power, and an awake
chip can always be found again for re-flashing. Roughly 20–25 mA on an ESP32-C3 (~0.1 W).

## Setup protocol (for Roomie)

| | UUID |
|---|---|
| Setup service | `09d317ba-23f0-4778-afc3-d819146ab127` |
| Config characteristic (read / write) | `9194e05c-5fdc-4b80-98f5-d9c4655a1d40` |

Setup advertisement: the service UUID above, the name `Roomie-XXXX` (scan response), and
manufacturer data (company `0x02E5`): `[configured, major_hi, major_lo, minor_hi, minor_lo]`.
(iOS hides proximity-beacon packets from Core Bluetooth scans, so this is how the app tells beacons apart.)

Config record (26 bytes, same layout for read and write):

| Bytes | Read | Write |
|---|---|---|
| 0 | firmware version | command: `0x01` configure, `0x02` reset (reset needs only this byte) |
| 1 | configured (1 = yes) | ignored |
| 2–17 | proximity UUID (big-endian) | same |
| 18–19 | major (big-endian) | same |
| 20–21 | minor (big-endian) | same |
| 22 | measured power at 1 m (int8 dBm) | same |
| 23 | transmit power (int8 dBm, −12…9) | same |
| 24–25 | advertising interval (uint16 little-endian, ms, 100…10000) | same |

After a write the beacon restarts with the new settings.

Defaults before setup: measured power −59 dBm, transmit power −3 dBm, interval 300 ms.

## Building

GitHub Actions builds every chip on each push to `main` (ESP-IDF 5.4) and publishes `web/` plus
the firmware files to GitHub Pages. To build locally: `cd firmware && idf.py set-target esp32c3 && idf.py build`.

## License

MIT
