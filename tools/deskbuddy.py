#!/usr/bin/env python3
"""Remote control + screenshots for the DeskBuddy board (stdlib only).

  py tools/deskbuddy.py shot [name]            save the current screen to tools/shots/<name>.png
  py tools/deskbuddy.py screen face|clock|bands
  py tools/deskbuddy.py mood idle|attention|happy|laugh|surprised|sleepy
  py tools/deskbuddy.py tap X Y                tap at screen coords (240x320)
  py tools/deskbuddy.py swipe left|right
  py tools/deskbuddy.py hold X Y [ms]
  py tools/deskbuddy.py time H M               set today's local time
  py tools/deskbuddy.py weather CODE [TEMP] [DAY0/1] [D1 D2 D3]
  py tools/deskbuddy.py status
  py tools/deskbuddy.py suite                  screenshot every mood and page

Set DESKBUDDY_HOST to override the board's address.
"""
import os
import pathlib
import struct
import sys
import time
import urllib.parse
import urllib.request
import zlib

HOST = os.environ.get("DESKBUDDY_HOST", "deskbuddy.local")
SHOTS = pathlib.Path(__file__).parent / "shots"


def get(path, **params):
    qs = urllib.parse.urlencode({k: v for k, v in params.items() if v is not None})
    url = f"http://{HOST}{path}" + (f"?{qs}" if qs else "")
    with urllib.request.urlopen(url, timeout=20) as r:
        return r.read()


def bmp_to_png(bmp: bytes) -> bytes:
    off = struct.unpack_from("<I", bmp, 10)[0]
    w, h = struct.unpack_from("<ii", bmp, 18)
    bpp = struct.unpack_from("<H", bmp, 28)[0]
    assert bpp == 24, f"expected 24-bit BMP, got {bpp}"
    stride = (w * 3 + 3) & ~3
    raw = bytearray()
    for y in range(h):  # BMP is stored bottom-up
        row = bmp[off + (h - 1 - y) * stride: off + (h - 1 - y) * stride + w * 3]
        raw.append(0)  # PNG filter: none
        for i in range(0, w * 3, 3):
            raw += bytes((row[i + 2], row[i + 1], row[i]))  # BGR -> RGB

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def shot(name="screen", wait=0.45):
    time.sleep(wait)  # let the next animation frames draw
    png = bmp_to_png(get("/screenshot.bmp"))
    SHOTS.mkdir(exist_ok=True)
    path = SHOTS / f"{name}.png"
    path.write_bytes(png)
    print(path)
    return path


def swipe(direction):
    a, b = (190, 40) if direction == "left" else (40, 190)
    get("/touch", x=a, y=160, x2=b, y2=160, ms=260)


def main(argv):
    if not argv:
        print(__doc__)
        return
    cmd, args = argv[0], argv[1:]
    if cmd == "shot":
        shot(args[0] if args else "screen", wait=0.1)
    elif cmd == "screen":
        get("/screen", show=args[0])
        shot(f"screen_{args[0]}")
    elif cmd == "mood":
        get("/mood", m=args[0])
        shot(f"mood_{args[0]}")
    elif cmd == "tap":
        get("/touch", x=args[0], y=args[1], ms=120)
        shot("after_tap", wait=0.5)
    elif cmd == "hold":
        get("/touch", x=args[0], y=args[1], ms=args[2] if len(args) > 2 else 1500)
        shot("during_hold", wait=0.7)
    elif cmd == "swipe":
        swipe(args[0])
        shot(f"after_swipe_{args[0]}", wait=0.7)
    elif cmd == "time":
        get("/settime", h=args[0], m=args[1])
        print("time set")
    elif cmd == "weather":
        a = args + [None] * 7
        get("/weather", code=a[0], temp=a[1], day=a[2], d1=a[3], d2=a[4], d3=a[5])
        get("/screen", show="clock")
        shot("weather_" + str(a[0]))
    elif cmd == "scene":  # scene CODE [TEMP] [DAY0/1] [D1 D2 D3]: fake weather, open the live scene
        a = args + [None] * 6
        get("/weather", code=a[0], temp=a[1], day=a[2], d1=a[3], d2=a[4], d3=a[5])
        get("/scene", on=1)
        shot("scene_" + str(a[0]) + ("_night" if a[2] == "0" else ""), wait=0.6)
    elif cmd == "scene_at":  # scene_at CODE T: pin the animation clock (e.g. 95 4.05 catches lightning)
        get("/weather", code=args[0], temp=60, day=1)
        get("/scene", on=1, t=args[1])
        shot(f"scene_{args[0]}_at_{args[1]}", wait=0.5)
        get("/scene", on=1)  # release the freeze
    elif cmd == "refresh":  # re-fetch real weather and band data now (drops injected fake weather)
        print(get("/refresh").decode().strip())
    elif cmd == "bright":
        print(get("/bright", **({"auto": 1} if args[0] == "auto" else {"v": args[0]})).decode().strip())
    elif cmd == "status":
        print(get("/status").decode().strip())
    elif cmd == "suite":
        for m in ("idle", "attention", "happy", "laugh", "surprised", "sleepy"):
            get("/mood", m=m)
            shot(f"mood_{m}", wait=0.9)
        get("/mood", m="idle")
        for s in ("clock", "bands"):
            get("/screen", show=s)
            shot(f"screen_{s}", wait=0.9)
        get("/screen", show="face")
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
