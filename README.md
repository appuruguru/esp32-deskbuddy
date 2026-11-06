# DeskBuddy

An animated desk gadget for the ESP32 "Cheap Yellow Display" (CYD, ESP32-2432S028R, 2.8in touchscreen).
Swipe between pages: an animated face that reacts to touch, a clock with weather (tap it for a live
animated weather scene), and live HF ham band conditions. The light sensor dims the screen at night.

Built with PlatformIO (Arduino framework, C++) and TFT_eSPI.

## Quick start
You need an ESP32-2432S028R CYD, a USB **data** cable, a 2.4 GHz WiFi network (the ESP32 can't use 5 GHz),
Python 3.8+, and the CH340 USB driver if your computer doesn't see the board as a serial/COM port.

```
python setup.py
```

(On Windows you can use `py setup.py`.) The setup script:
1. checks Python and PlatformIO, and offers to install PlatformIO with pip if it is missing;
2. asks for your WiFi name and password (the password is hidden as you type), your ZIP code or
   latitude/longitude, your timezone, Fahrenheit/Celsius and 12/24-hour clock;
3. asks for a password to protect wireless updates (or generates one);
4. writes `include/secrets.h` and `secrets.ini` for you, then offers to build and flash the board.

The first build downloads the compiler and libraries automatically and takes a few minutes.
Some boards need the BOOT button held while the upload connects: if you see "Wrong boot mode detected",
hold BOOT, keep holding it until the upload starts writing, then release.

`secrets.h` and `secrets.ini` are git-ignored: never commit or share them.

### Manual setup (if you prefer)
Copy `include/secrets.h.example` to `include/secrets.h` and `secrets.ini.example` to `secrets.ini`, fill them in,
then `pio run -e cyd -t upload`.

### Updating later (no cable, no BOOT button)
Once the board is on your WiFi: `pio run -e cyd_ota -t upload --upload-port <the board's IP address>`.
The IP is printed on the serial monitor (`pio device monitor`) at boot and shown in your router's device list.

## Controls
- Swipe left/right: change page. Tap the weather page: live weather scene. Tap the face: it reacts.
- A tap on a dimmed screen only wakes it.

## Dev tools (optional)
The firmware serves `/screenshot.bmp` and some remote-control endpoints (touch, page, mood, brightness,
status) on your local network, only when built with `DEV_TOOLS 1` in `src/main.cpp`. `tools/deskbuddy.py`
wraps them (stdlib only): `DESKBUDDY_HOST=<board IP> py tools/deskbuddy.py shot`. These endpoints have no
login, so anyone on your WiFi can use them; set `DEV_TOOLS` to 0 if you don't want that.

## Notes
- The platform version is pinned in `platformio.ini`: newer ones ship Arduino core 3, which this code doesn't support yet.
- Clones/variants of the board can differ (display colour inversion, touch orientation, light sensor).
  See `CLAUDE.md` for the hardware notes and calibration tips.
