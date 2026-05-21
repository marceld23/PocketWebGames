# Third-party notices

PocketWebGames itself is licensed under the MIT License (see `LICENSE`). It
depends on the third-party components listed below. These are **not** bundled
in this repository's source (except Three.js, see below); PlatformIO fetches
the firmware libraries at build time. Their licenses and copyright notices are
reproduced/located as noted.

## Bundled in this repository

### Three.js (r155)
- File: `data/shared/three.min.js.gz`
- License: **MIT** — Copyright © 2010-2023 Three.js Authors
- https://github.com/mrdoob/three.js/blob/dev/LICENSE

## Firmware libraries (fetched by PlatformIO, see `platformio.ini`)

| Library | License | Upstream |
|---------|---------|----------|
| ArduinoJson | **MIT** | https://github.com/bblanchon/ArduinoJson |
| M5Core2 | **MIT** | https://github.com/m5stack/M5Core2 |
| AsyncTCP (ESP32Async) | **LGPL-3.0** | https://github.com/ESP32Async/AsyncTCP |
| ESPAsyncWebServer (ESP32Async) | **LGPL-3.0** | https://github.com/ESP32Async/ESPAsyncWebServer |

## Platform / toolchain (Espressif Arduino core)

| Component | License |
|-----------|---------|
| arduino-esp32 core | LGPL-2.1 |
| ESP-IDF components | Apache-2.0 (mixed; see ESP-IDF) |

## LGPL note

`AsyncTCP` and `ESPAsyncWebServer` are licensed under **LGPL-3.0**, and the
Arduino-ESP32 core is **LGPL-2.1**. The LGPL permits this project's own code to
be released under the MIT License, but requires that end users be able to
relink/replace the LGPL components.

This requirement is satisfied by distributing PocketWebGames as **open source**:
the full source is published and the LGPL libraries are pulled by PlatformIO,
so anyone can substitute a different version of those libraries and rebuild the
firmware. If you instead distribute a **closed-source binary** that statically
links these libraries, you must provide the means to relink (e.g. object files)
as required by the LGPL, or replace the libraries.

No GPL-/AGPL-licensed (strong copyleft) components are used, so this project's
own source is not required to be open-sourced — but keeping it open source is
the simplest way to stay LGPL-compliant.

## Game names

The games are original implementations; game mechanics are not copyrightable
and all in-game art is generated procedurally by this project's own code. Game
titles were chosen to avoid third-party trademarks.
