# Roomie Beacon

Firmware that turns any ESP32 into a Bluetooth room beacon for the **Roomie** iOS app,
plus a one-click web installer.

**Install:** https://roomiehome.github.io/roomiehome/beacons.html (Chrome or Edge on a computer). About Roomie: https://roomiehome.github.io/roomiehome/

## How it works

1. Flash the firmware from the install page. The chip type is detected automatically
   (ESP32, ESP32-C3, ESP32-S3, ESP32-C6, ESP32-H2).
2. The board starts **unconfigured**: it advertises a setup signal named `Roomie-XXXX`
   (the last 4 hex digits of its Bluetooth address).
3. In Roomie, pick the beacon and the room. Roomie writes its settings over Bluetooth; the beacon
   saves them and restarts as a room beacon.
4. To change a beacon later, power-cycle it: for **3 minutes** after it's plugged in (or after its reset button, or after Roomie changed it) it accepts changes
   (alternating between the beacon signal and the setup signal), then it's a beacon only.

Unexpected restarts (a power dip at high transmit power, a crash) don't open the setup window; the board counts them and reports them in its setup signal the next time it's plugged in, so Roomie can warn about a weak charger.

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
manufacturer data (company `0x02E5`): `[status, major_hi, major_lo, minor_hi, minor_lo]`, where status bit 0 = configured, bits 1–4 = reason of the last unexpected restart since the previous power-on (`esp_reset_reason_t`, capped at 15; 9 = brownout), bits 5–7 = how many (capped at 7).
(iOS hides proximity-beacon packets from Core Bluetooth scans, so this is how the app tells beacons apart.)

Config record (26 bytes written; 28 read, with a restart report at the end):

| Bytes | Read | Write |
|---|---|---|
| 0 | firmware version | command: `0x01` configure, `0x02` reset (reset needs only this byte) |
| 1 | configured (1 = yes) | ignored |
| 2–17 | proximity UUID (big-endian) | same |
| 18–19 | major (big-endian) | same |
| 20–21 | minor (big-endian) | same |
| 22 | measured power at 1 m (int8 dBm) | same |
| 23 | transmit power (int8 dBm, −12…21; rounded to the nearest level the chip supports: up to +21 on C3/S3, +20 on C6/H2, +9 on ESP32) | same |
| 24–25 | advertising interval (uint16 little-endian, ms, 100…10000) | same |
| 26 | last unexpected restart reason since the previous power-on (0 = none) | — |
| 27 | number of unexpected restarts | — |

After a write the beacon restarts with the new settings.

Defaults before setup: measured power −59 dBm, transmit power +9 dBm, interval 300 ms. The setup signal always uses the chip's maximum power, so boards with weak antennas are still easy to find.

## Building

GitHub Actions builds every chip on each push to `main` (ESP-IDF 5.4) and publishes `web/` plus
the firmware files to GitHub Pages. To build locally: `cd firmware && idf.py set-target esp32c3 && idf.py build`.

## License

MIT
