# Third-party components and licenses

Fridge Tracker itself is licensed under the **GNU General Public License v3.0 or later** (see [LICENSE](LICENSE)).
The firmware links against GxEPD2, which is GPL-3.0, so the firmware as a whole — including the
prebuilt `.bin` files in Releases — is distributed under GPL-3.0.

## Firmware libraries (linked into the `.bin`)

| Component | Version tested | License |
|---|---|---|
| [GxEPD2](https://github.com/ZinggJM/GxEPD2) — e-paper drivers | 1.6.9 | GPL-3.0 |
| [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) | 1.13.0 | BSD-2-Clause |
| [Adafruit BusIO](https://github.com/adafruit/Adafruit_BusIO) (dependency of GFX) | 1.17.4 | MIT |
| [ArduinoJson](https://arduinojson.org/) | 7.4.3 | MIT |
| [Arduino core for ESP32](https://github.com/espressif/arduino-esp32) (incl. BLE, Preferences, LittleFS) | 3.3.12 | LGPL-2.1, components under Apache-2.0 |

## Fonts

`firmware/libraries/FridgeCommon/src/fonts.h` contains bitmap glyphs rasterised by `firmware/tools/gen_fonts.py`
from the TTF files in `firmware/tools/fonts/`:

| Font | Copyright | License |
|---|---|---|
| Roboto Condensed (`RobotoCondensed.ttf`) — Latin and Cyrillic | Copyright 2011 Google Inc. | Apache-2.0, see [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt) |
| Noto Sans Hebrew (`NotoSansHebrew.ttf`) — Hebrew | Copyright 2024 The Noto Project Authors (https://github.com/notofonts/hebrew) | SIL Open Font License 1.1, see [licenses/OFL-1.1.txt](licenses/OFL-1.1.txt) |

The bitmap data derived from Noto Sans Hebrew remains under the SIL Open Font License 1.1.
The license headers are taken from the font files' own `name` table.

## Web app

| Component | How it is used | License |
|---|---|---|
| [ZXing for JS](https://github.com/zxing-js/library) (`@zxing/library` 0.23.0) | Barcode scanning, loaded from unpkg.com at runtime, not bundled | Apache-2.0 |
| [Open Food Facts](https://world.openfoodfacts.org/) API | Product names by barcode, queried by `site/proxy.php` | Database: ODbL 1.0; contents: Database Contents License |

Product names come from Open Food Facts. If you run a public instance, keep the attribution
("Найдено в Open Food Facts" is shown next to such names) and follow their
[API usage rules](https://openfoodfacts.github.io/openfoodfacts-server/api/) — in particular, send a
User-Agent that identifies your app.

## Hardware documentation

Pin maps for the Zectrix NOTE4C come from the vendor wiki:
https://wiki.zectrix.com/en/hardware/note4c/quick-start
