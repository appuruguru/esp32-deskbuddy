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
2. asks for your WiFi name and password (the password is hidden as you type; or press Enter to set WiFi up
   from your phone instead, see below), your ZIP code or latitude/longitude, your timezone,
   Fahrenheit/Celsius and 12/24-hour clock;
3. asks for a password to protect wireless updates (or generates one);
4. writes `include/secrets.h` and `secrets.ini` for you, then offers to build and flash the board.

The first build downloads the compiler and libraries automatically and takes a few minutes.
Some boards need the BOOT button held while the upload connects: if you see "Wrong boot mode detected",
hold BOOT, keep holding it until the upload starts writing, then release.

`secrets.h` and `secrets.ini` are git-ignored: never commit or share them.

### Manual setup (if you prefer)
Copy `include/secrets.h.example` to `include/secrets.h` and `secrets.ini.example` to `secrets.ini`, fill them in,
then `pio run -e cyd -t upload`.

### Changing WiFi (new router, moving house, taking it to a friend's)
No cable or rebuild needed. When the board can't join its WiFi for 40 seconds after power-up (or has no WiFi
saved), the screen switches to **WIFI SETUP**:
1. On your phone, join the WiFi shown on the screen (`DeskBuddy-XXXX`, with the password shown there).
2. A setup page opens by itself (or browse to `192.168.4.1`). Choose your network, enter its password, Save.
3. The board restarts and joins it. If it still can't connect, the setup screen comes back.

To change the network while the board is still working (for example before you move it), **hold the middle
of the HF bands page for 4 seconds** to open the same setup screen; "Cancel" on the screen closes it.
A network saved this way overrides the WiFi in `secrets.h`. The hotspot only serves the setup page to
devices that joined it, and its password is random each time.

If the board's saved network is only temporarily down (power cut, router still booting), the hotspot appears
but the board keeps retrying quietly and rejoins by itself when the network returns.

### Updating later (no cable, no BOOT button)
Once the board is on your WiFi: `pio run -e cyd_ota -t upload --upload-port <the board's IP address>`.
The IP is printed on the serial monitor (`pio device monitor`) at boot and shown in your router's device list.

## Controls
- Swipe left/right: change page. Tap the weather page: live weather scene. Tap the face: it reacts.
- A tap on a dimmed screen only wakes it.
- Hold any corner for 2 seconds: flip the screen 180 degrees.
- Hold the middle of the HF bands page for 4 seconds: WiFi setup hotspot.

## Dev tools (optional)
The firmware serves `/screenshot.bmp` and some remote-control endpoints (touch, page, mood, brightness,
status) on your local network, only when built with `DEV_TOOLS 1` in `src/main.cpp`. `tools/deskbuddy.py`
wraps them (stdlib only): `DESKBUDDY_HOST=<board IP> py tools/deskbuddy.py shot`. These endpoints have no
login, so anyone on your WiFi can use them; set `DEV_TOOLS` to 0 if you don't want that.

## Notes
- The platform version is pinned in `platformio.ini`: newer ones ship Arduino core 3, which this code doesn't support yet.
- Clones/variants of the board can differ (display colour inversion, touch orientation, light sensor).
  See `CLAUDE.md` for the hardware notes and calibration tips.
