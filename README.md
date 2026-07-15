# ESP32 Home Dashboard

Firmware for the [Waveshare ESP32-S3-Touch-LCD-7](https://docs.waveshare.com/ESP32-S3-Touch-LCD-7) (rev 1.2), turning it into a
wall-mounted dashboard for weather, aviation tracking, smart home control,
and ISS pass alerts — all over WiFi, no backend server required.

## Hardware
- Waveshare ESP32-S3-Touch-LCD-7, rev 1.2 (800×480, 8MB PSRAM, 16MB Flash)

## Status
Early scaffold. Services and screens are stubbed out with working HTTP/JSON
plumbing where the API is simple (weather, ISS, aviation, Hubitat/HA), and
TODO markers where the logic still needs to be filled in (LVGL radar
canvas, per-tab widget wiring beyond the dashboard).

Note: the "Porsche" tab is currently an empty placeholder — it depended
entirely on the OBD-II integration, which has been dropped from this build.
Remove the tab in `src/screens/screen_manager.cpp` if you don't plan to
revisit it, or leave it as a placeholder for later.

## Getting started
1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
2. Copy `include/secrets.h.example` to `include/secrets.h` and fill in your
   WiFi credentials and API keys. `secrets.h` is gitignored — it will never
   be committed.
3. `pio run -t upload` to build and flash.
4. Confirm the display panel pin mapping in `src/config/display_config.h`
   against Waveshare's official demo for your board revision — pin
   assignments have shifted slightly between hardware revisions.

## Architecture
Everything runs directly on the ESP32 — no backend aggregator server:
- **Weather / ISS / airport METAR-TAF**: direct HTTPS + JSON, polled on a
  timer (see intervals in `main.cpp`).
- **Aviation radar**: ADS-B Exchange bounding-box query; aircraft are
  plotted as bearing/distance points on a plain LVGL canvas rather than
  rendering map tiles (too heavy for this chip).
- **Smart home**: Hubitat Maker API and Home Assistant REST API, both
  local-LAN calls.
- **Notifications**: MQTT subscription (`dashboard/notify`) so HA/Hubitat
  automations can push alerts directly instead of being polled.

## Repo layout
```
src/
  main.cpp                 — setup/loop, wires services to screens
  config/                  — display/panel init
  services/                — one file pair per data source (.h/.cpp)
  screens/                 — LVGL tabview + per-tab screen builders
include/
  secrets.h.example        — copy to secrets.h, fill in real values
```

## License
MIT — see `LICENSE`.
