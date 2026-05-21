#!/usr/bin/env python3
"""
Capture a screenshot of each game from the PC mock, for the README.

Starts mock_server.py for one game at a time, joins it in headless Chromium,
lets the bots play for a few seconds, and saves docs/screenshots/<slug>.png.

Run:  uv run --with playwright python tools/screenshot_games.py
(Chromium must be installed once: uv run --with playwright playwright install chromium)
"""

import pathlib
import subprocess
import sys
import time

from playwright.sync_api import sync_playwright

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "screenshots"
OUT.mkdir(parents=True, exist_ok=True)
MOCK = ROOT / "mock_server.py"

# slug, keys to hold while capturing (to create motion for that game)
GAMES = [
    ("dogfight", []),
    ("blaster", []),
    ("trails", []),
    ("racing", ["ArrowUp"]),   # hold the gas so the car is out on the track
    ("snake", []),
    ("bounce", []),
]

VIEW = {"width": 760, "height": 420}
# SwiftShader so WebGL (Three.js) renders in headless.
ARGS = ["--use-gl=angle", "--use-angle=swiftshader", "--ignore-gpu-blocklist",
        "--enable-unsafe-swiftshader"]


def capture(slug, hold):
    proc = subprocess.Popen([sys.executable, str(MOCK), slug],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.8)
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True, args=ARGS)
            page = browser.new_page(viewport=VIEW, device_scale_factor=2)
            page.goto("http://localhost:8080/%s/" % slug, wait_until="load")
            page.fill("#name", "DEMO")
            page.click("#join")
            time.sleep(2.2)                       # past "GET READY"
            for k in hold:
                page.keyboard.down(k)
            time.sleep(3.2)                        # let bots move around
            for k in hold:
                page.keyboard.up(k)
            time.sleep(0.3)
            out = OUT / ("%s.png" % slug)
            page.screenshot(path=str(out))
            browser.close()
        print("captured", out.relative_to(ROOT))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        time.sleep(0.6)


def main():
    for slug, hold in GAMES:
        capture(slug, hold)


if __name__ == "__main__":
    main()
