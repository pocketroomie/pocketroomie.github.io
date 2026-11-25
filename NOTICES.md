# Notices

Pocket Roomie and the Pocket Roomie beacon firmware are independent projects. They are not made by, endorsed by or
affiliated with Apple Inc. or any company named below. Product names are trademarks of their owners:
Apple, iPhone, iPad, iOS, HomeKit, Apple Home, HomePod, Apple TV, Siri, Shortcuts, SF Symbols and App Store
(Apple Inc.); Bluetooth (Bluetooth SIG, Inc.); ESP32 and ESP-IDF (Espressif Systems (Shanghai) Co., Ltd.);
GitHub (GitHub, Inc.); Chrome (Google LLC); Microsoft Edge (Microsoft Corporation).

## This repository

The firmware (`firmware/`) and the website (`web/`) are released under the MIT License (see `LICENSE`),
provided as is, without warranty of any kind. The Pocket Roomie name and icon are not licensed for use as the
name or icon of another product.

## Third-party components

| Component | License | Where |
|---|---|---|
| ESP-IDF (Espressif Systems) | Apache License 2.0 (some parts under other permissive licenses, see the ESP-IDF sources) | Linked into the firmware binaries |
| NimBLE (Apache Mynewt) | Apache License 2.0 | Linked into the firmware binaries |
| ESP Web Tools 10.5.2 (ESPHome maintainers) | Apache License 2.0 | `web/vendor/esp-web-tools/`, unmodified |
| Lucide icons | ISC License (portions MIT, Feather, Cole Bemis) | Inline SVG in `web/` |
| Tabler Icons | MIT License, (c) Pawel Kuna | Inline SVG in `web/` |

The Apache License 2.0 text is in `licenses/Apache-2.0.txt`. The ISC and MIT notices for the icons are
reproduced on the website's Legal & Credits page (`web/legal.html`).

## Safety and use

The firmware turns an ESP32 board into a Bluetooth beacon. Flashing a board erases it and is done at your own
risk. Use a suitable, certified USB power supply and follow the instructions of your board's maker.
