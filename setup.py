#!/usr/bin/env python3
"""DeskBuddy setup: checks your tools, asks for your settings and writes the private config files.

    python setup.py          (on Windows you can also use: py setup.py)

Creates include/secrets.h (WiFi, location, timezone) and secrets.ini (OTA password).
Both are git-ignored and stay on your computer. Standard library only.
"""
import getpass
import json
import pathlib
import re
import secrets
import shutil
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent
SECRETS_H = ROOT / "include" / "secrets.h"
SECRETS_INI = ROOT / "secrets.ini"

TIMEZONES = [
    ("US Eastern", "EST5EDT,M3.2.0,M11.1.0"),
    ("US Central", "CST6CDT,M3.2.0,M11.1.0"),
    ("US Mountain", "MST7MDT,M3.2.0,M11.1.0"),
    ("US Arizona (no daylight saving)", "MST7"),
    ("US Pacific", "PST8PDT,M3.2.0,M11.1.0"),
    ("US Alaska", "AKST9AKDT,M3.2.0,M11.1.0"),
    ("US Hawaii", "HST10"),
    ("UK / Ireland", "GMT0BST,M3.5.0/1,M10.5.0"),
    ("Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("UTC", "UTC0"),
]


def ask(prompt, default=None, secret=False):
    shown = f"{prompt} [{default}]: " if default else f"{prompt}: "
    while True:
        if secret and sys.stdin.isatty():
            value = getpass.getpass(shown)
        else:
            value = input(shown)
        value = value.strip("\r\n")
        if value == "" and default is not None:
            return default
        if value != "":
            return value
        print("  Please enter a value.")


def yes_no(prompt, default=True):
    hint = "Y/n" if default else "y/N"
    value = input(f"{prompt} [{hint}]: ").strip().lower()
    return default if value == "" else value.startswith("y")


def c_string(value):
    return value.replace("\\", "\\\\").replace('"', '\\"')


def write_private(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        backup = path.with_name(path.name + ".bak")
        shutil.copyfile(path, backup)
        print(f"  (previous {path.name} saved as {backup.name})")
    path.write_text(text, encoding="utf-8", newline="\n")  # no BOM: a BOM breaks PlatformIO


def pio_command():
    exe = shutil.which("pio") or shutil.which("platformio")
    if exe:
        return [exe]
    probe = subprocess.run([sys.executable, "-m", "platformio", "--version"],
                           capture_output=True, text=True)
    return [sys.executable, "-m", "platformio"] if probe.returncode == 0 else None


def step_tools():
    print("\n== 1. Tools ==")
    if sys.version_info < (3, 8):
        sys.exit(f"Python 3.8 or newer is required (you have {sys.version.split()[0]}).")
    print(f"Python {sys.version.split()[0]}: ok")
    pio = pio_command()
    if pio is None:
        print("PlatformIO is not installed (it builds and uploads the firmware).")
        if not yes_no("Install it now with pip?"):
            sys.exit("Install PlatformIO ('pip install platformio') and run setup again.")
        done = subprocess.run([sys.executable, "-m", "pip", "install", "--user", "platformio"])
        if done.returncode != 0:
            sys.exit("pip could not install PlatformIO. Install it manually and run setup again.")
        pio = pio_command()
        if pio is None:
            sys.exit("PlatformIO installed but cannot be started. Open a new terminal and run setup again.")
    print("PlatformIO: ok")
    print("(The compiler and libraries download automatically on the first build.)")
    return pio


def step_wifi():
    print("\n== 2. WiFi ==")
    print("The board only supports 2.4 GHz networks (not 5 GHz).")
    ssid = ask("WiFi network name")
    password = ask("WiFi password (hidden while typing)", secret=True)
    return ssid, password


def lookup_zip(zip_code):
    try:
        with urllib.request.urlopen(f"https://api.zippopotam.us/us/{zip_code}", timeout=10) as r:
            place = json.load(r)["places"][0]
        return place["latitude"], place["longitude"], f"{place['place name']}, {place['state abbreviation']}"
    except Exception:
        return None


def step_location():
    print("\n== 3. Location (for the weather) ==")
    print("Enter a US ZIP code, or type latitude,longitude (for example 51.5,-0.12).")
    while True:
        entry = ask("ZIP code or lat,lon")
        if re.fullmatch(r"\d{5}", entry):
            found = lookup_zip(entry)
            if found:
                lat, lon, name = found
                print(f"  Found {name}  ({lat}, {lon})")
                if yes_no("  Use this location?"):
                    return f"{float(lat):.2f}", f"{float(lon):.2f}"
            else:
                print("  ZIP lookup failed (offline or unknown ZIP). Enter lat,lon instead.")
        else:
            parts = [p for p in re.split(r"[,\s]+", entry) if p]
            try:
                lat, lon = float(parts[0]), float(parts[1])
                if abs(lat) <= 90 and abs(lon) <= 180 and len(parts) == 2:
                    return f"{lat:.2f}", f"{lon:.2f}"
            except (ValueError, IndexError):
                pass
            print("  Could not read that. Use a 5-digit ZIP or 'lat,lon'.")


def step_timezone():
    print("\n== 4. Timezone ==")
    for i, (name, _) in enumerate(TIMEZONES, 1):
        print(f"  {i:2}. {name}")
    print(f"  {len(TIMEZONES) + 1:2}. Other (type a POSIX timezone string)")
    while True:
        choice = ask("Choose a number", "1")
        if choice.isdigit() and 1 <= int(choice) <= len(TIMEZONES):
            return TIMEZONES[int(choice) - 1][1]
        if choice.isdigit() and int(choice) == len(TIMEZONES) + 1:
            return ask("POSIX timezone string (e.g. EST5EDT,M3.2.0,M11.1.0)")
        print("  Pick one of the numbers above.")


def step_preferences():
    print("\n== 5. Display ==")
    fahrenheit = yes_no("Show temperatures in Fahrenheit? (No = Celsius)", True)
    clock24 = yes_no("Use a 24-hour clock? (No = 12-hour AM/PM)", False)
    return fahrenheit, clock24


def step_ota():
    print("\n== 6. Wireless update password ==")
    print("After the first USB flash, you update the board over WiFi. This password protects that.")
    print("Press Enter to generate a random one (it is saved in secrets.ini).")
    while True:
        entry = input("OTA password (letters, digits, - _ . only) [random]: ").strip()
        if entry == "":
            return secrets.token_urlsafe(12).replace("~", "-")
        if re.fullmatch(r"[A-Za-z0-9_.-]{6,64}", entry):
            return entry
        print("  Use 6-64 characters: letters, digits, '-', '_' or '.'.")


def main():
    print("DeskBuddy setup")
    pio = step_tools()
    for path in (SECRETS_H, SECRETS_INI):
        if path.exists() and not yes_no(f"\n{path.name} already exists. Replace it?", False):
            sys.exit("Left your existing files alone. Run setup again whenever you like.")
    ssid, wifi_pass = step_wifi()
    lat, lon = step_location()
    tz = step_timezone()
    fahrenheit, clock24 = step_preferences()
    ota = step_ota()

    header = (
        "#pragma once\n\n"
        "// Written by setup.py. Private and git-ignored: never share this file.\n"
        f'#define WIFI_SSID "{c_string(ssid)}"\n'
        f'#define WIFI_PASS "{c_string(wifi_pass)}"\n'
        f'#define WEATHER_LAT "{lat}"\n'
        f'#define WEATHER_LON "{lon}"\n'
        f'#define TZ_POSIX "{c_string(tz)}"\n'
        f"#define USE_FAHRENHEIT {1 if fahrenheit else 0}\n"
        f"#define CLOCK_24H {1 if clock24 else 0}\n"
    )
    ini = f"; Written by setup.py. Private and git-ignored: never share this file.\n[secrets]\nota_password = {ota}\n"
    print("\n== Writing files ==")
    write_private(SECRETS_H, header)
    write_private(SECRETS_INI, ini)
    print(f"Saved {SECRETS_H.relative_to(ROOT)} and {SECRETS_INI.relative_to(ROOT)}.")

    print("\n== Next: flash the board ==")
    print("Plug the CYD into USB. Some boards need the BOOT button held while the upload connects")
    print("(hold it if you see 'Wrong boot mode detected', and release once writing starts).")
    if yes_no("Build and flash it now?", True):
        print("(The first build downloads the toolchain and libraries and takes a few minutes.)")
        code = subprocess.run(pio + ["run", "-e", "cyd", "-t", "upload"], cwd=ROOT).returncode
        if code == 0:
            print("\nDone! The board restarts and joins your WiFi. Later updates, with no cable:")
            print("  pio run -e cyd_ota -t upload --upload-port <the board's IP address>")
        else:
            print("\nThe upload did not finish. Check the cable and BOOT button, then run:")
            print("  pio run -e cyd -t upload")
    else:
        print("When you are ready:  pio run -e cyd -t upload")


if __name__ == "__main__":
    try:
        main()
    except (KeyboardInterrupt, EOFError):
        sys.exit("\nSetup cancelled. Nothing was written.")
