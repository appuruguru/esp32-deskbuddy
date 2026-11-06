# DeskBuddy (ESP32 CYD) - project briefing for Claude

ESP32 "Cheap Yellow Display" (ESP32-2432S028R) desk gadget. Arduino framework on PlatformIO, TFT_eSPI.
Portrait 240x320. Swipeable pages: animated face, clock + weather (tap = live animated weather scene),
HF band conditions (ham radio). Source is `src/main.cpp`; config in `platformio.ini` and `include/secrets.h`.

## Never echo secrets
`include/secrets.h` (WiFi name/password, location, timezone) and `secrets.ini` (OTA password) are private.
Do not print them in chat. Both are git-ignored and NOT included when this project is copied between
machines: run `python setup.py` (prompts for everything and writes both files), or copy the `.example`
files and let the user fill them in. Never type the user's secrets yourself. `platformio.ini` reads the OTA password from `secrets.ini` (extra_configs).
Write config files WITHOUT a BOM (PowerShell `Set-Content -Encoding utf8` adds one and breaks platformio.ini).

## Hardware facts (verified on the first board)
- ESP32-WROOM, 4 MB flash, no PSRAM. USB serial is a CH340 (needs the CH340 driver on Windows/macOS).
- Display ILI9341-family 240x320, backlight GPIO21 (PWM). Needs `tft.invertDisplay(true)` or colors are
  inverted (white background). If a NEW board looks wrong, test inversion / RGB order first.
- XPT2046 resistive touch on its own SPI bus (CLK25 MOSI32 MISO39 CS33 IRQ36), raw range mapped in
  `RAW_X_MIN/MAX`, `RAW_Y_MIN/MAX`; touch is read in the landscape frame then rotated for portrait.
- Light sensor (LDR) on GPIO34: reads LOWER when brighter and also sees the screen's own backlight.
  Auto-brightness divides by a measured ratio-vs-backlight table (`ldrRatio`) calibrated on the first
  board; a new board/case may need recalibration (use `/ldr` + `/bright?v=` sweeps in lit/dim/dark rooms).
- RGB LED (GPIO 4/16/17, active low), speaker output GPIO26, microSD slot are present but unused so far.
- The board does NOT auto-enter download mode: a USB flash needs BOOT held through the esptool connect.

## Flashing procedure
1. First flash (USB): ask the user to send "go" FIRST, then press and hold BOOT (typing with the same hand
   releases it), keep holding until the face appears. Run a retry loop (up to ~7 attempts) of
   `pio run -e cyd -t upload --upload-port <COM>`. Do not message the user mid-flash.
2. After that, update over WiFi (no BOOT): `pio run -e cyd_ota -t upload --upload-port <board IP>`.
   mDNS names do not resolve reliably; use the IP (shown by `/status`, or the router).
3. A firmware that crashes within ~16 s of boot cannot be fixed over OTA: it needs BOOT + USB again.
4. Partition scheme is `min_spiffs.csv` (two 1.9 MB OTA slots); changing partitions needs a USB flash.
5. pio may not be on PATH: use `py -m platformio ...` (Windows) or `python3 -m platformio ...`.
6. USB serial port numbers change on replug - detect, don't assume.

## Seeing and driving the real screen (use it - don't guess at UI)
`src/main.cpp` has `#define DEV_TOOLS 1`: the board serves `/screenshot.bmp` plus `/mood`, `/touch` (tap/
swipe injection), `/screen`, `/scene`, `/settime`, `/weather`, `/bright`, `/refresh`, `/status`.
`py tools/deskbuddy.py shot|suite|mood|swipe|tap|scene|weather|bright|refresh|status` saves PNGs to
`tools/shots/` - then Read the PNG. Set `DESKBUDDY_HOST=<board IP>` to point it at a board.
Always screenshot after UI changes. Opening the serial port resets the board (DTR/RTS).

## Gotchas learned the hard way
- Use `since(now, t)` for elapsed-time checks (unsigned `now - t` underflowed and caused phantom sleep).
- Network fetches (open-meteo weather, hamqsl.com band data) run in a background task (priority 0, core 0,
  watchdog disabled, 30 s boot grace). A crash/WDT reset boots into "safe mode" (orange dot, no fetching)
  until the next power-on. Keep WiFi + ArduinoOTA early in `loop()`; never run network calls on the main task.
- TLS needs several ~16 KB blocks; the heap check threshold is 26000 (a 45000 threshold silently skipped
  fetches when memory fragmented).
- The sprite is 8-bit color: very dark yellows/whites quantize to muddy olive; use orange/blue tints for glows.
- A tap on a dimmed screen (backlight < 30) only wakes it; it must not also change page.

## Setting up a SECOND board
- Give it a unique OTA hostname (e.g. `deskbuddy2` in `platformio.ini` `[common] ota_hostname`) and keep its
  own IP; the first board is `deskbuddy` and stays online.
- Copy `include/secrets.h.example` to `include/secrets.h`, fill in WiFi + location + timezone.
- Copy `secrets.ini.example` to `secrets.ini` and set an OTA password (the user's choice; any string).
- Do the BOOT-held USB flash, then verify display colors/inversion, touch direction, and brightness with
  the screenshot tool before building anything new.
