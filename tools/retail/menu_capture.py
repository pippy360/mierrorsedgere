"""Capture the retail front end: the start screen and the main menu's four columns.

    python -m tools.retail.menu_capture columns          # a game already on its main menu
    python -m tools.retail.menu_capture boot             # restart the game and record the start screen

Frames are the game's own back buffer, taken through the d3d9 hook (tools/retail/hook), so they
are right even when the window is covered. They land in build/retail/menu_cap/<name>/ with a
times.json giving each frame's wall-clock offset.

Only these keys are ever sent: Left and Right (change column) and, on the start screen, Space.
Enter is never sent: NEW GAME sits under PLAY CHAPTER and its confirmation erases the save.

`boot` kills a running game, points [URL] Map / LocalMap of the user's TdEngine.ini at TdMainMenu
for the launch and puts back whatever was there afterwards.
"""

import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.retail import desktop, drive, paths  # noqa: E402

COLUMNS = ("story", "race", "options", "extras")


def out_dir(name):
    return paths.ensure_dir(paths.build_dir("menu_cap", name))


def burst(name, seconds, step=0.0, t0=None):
    """Grab frames for `seconds`, as fast as the hook allows unless `step` spaces them out."""
    d = out_dir(name)
    start = time.time()
    origin = t0 if t0 is not None else start
    log = []
    n = 0
    while time.time() - start < seconds:
        before = time.time()
        info = drive.grab_frame(os.path.join(d, "f_%04d.png" % n), timeout=3.0)
        if info:
            log.append({"i": n, "t": round(before - origin, 3), "t_done": round(time.time() - origin, 3)})
            n += 1
        if step:
            time.sleep(max(0.0, step - (time.time() - before)))
    with open(os.path.join(d, "times.json"), "w") as f:
        json.dump(log, f)
    return log


def game_window():
    hwnd, _title = desktop.find_window_by_process(drive.EXE)
    if not hwnd:
        raise SystemExit("Mirror's Edge is not running")
    return hwnd


def press(hwnd, key):
    desktop.ensure_focus(hwnd)
    desktop.key(key)


def columns(seconds):
    """From STORY, step right through the four columns, recording each."""
    hwnd = game_window()
    for i, name in enumerate(COLUMNS):
        if i:
            t0 = time.time()
            press(hwnd, "right")
            # the 0.3 s open animation and the 0.35 s camera move, as fast as frames come
            burst("%s_open" % name, 1.5, t0=t0)
        log = burst(name, seconds, step=0.5)
        print("%-8s %d frames" % (name, len(log)))
    press(hwnd, "right")  # back to STORY, as found
    time.sleep(1.0)


def _set_startup_map(value):
    """Set [URL] Map and LocalMap in the user's TdEngine.ini; returns the previous (Map, LocalMap)."""
    d = paths.medge_user_config_dir(required=True)
    p = os.path.join(d, "TdEngine.ini")
    with open(p, "r", encoding="latin-1", newline="") as f:
        text = f.read()
    m = re.search(r"(?m)^\[URL\]\s*$", text)
    if not m:
        raise SystemExit("[URL] not found in %s" % p)
    start = m.end()
    nxt = re.search(r"(?m)^\[[^\]]+\]\s*$", text[start:])
    end = start + nxt.start() if nxt else len(text)
    body = text[start:end]
    previous = []
    for key, val in (("Map", value[0]), ("LocalMap", value[1])):
        found = re.search(r"(?m)^%s=(.*?)\r?$" % key, body)
        previous.append(found.group(1) if found else "")
        body = re.sub(r"(?m)^%s=.*?(\r?)$" % key, lambda mm: "%s=%s%s" % (key, val, mm.group(1)), body, count=1)
    with open(p, "w", encoding="latin-1", newline="") as f:
        f.write(text[:start] + body + text[end:])
    return tuple(previous)


def boot(seconds_before_key, seconds_after_key):
    """Restart the game on its start screen and record it, then the move into the main menu."""
    if drive.is_running():
        drive.kill()
        time.sleep(2)
    previous = _set_startup_map(("TdMainMenu", "TdMainMenu"))
    print("startup map was %r" % (previous,))
    try:
        t0 = time.time()
        hwnd, _title, rect = drive.launch()
        print("window after %.1f s: %s" % (time.time() - t0, rect))
        log = burst("start", seconds_before_key, t0=t0)
        print("start    %d frames" % len(log))
        tk = time.time()
        press(hwnd, "space")
        log = burst("start_to_menu", seconds_after_key, t0=tk)
        print("to menu  %d frames" % len(log))
    finally:
        _set_startup_map(previous)
        print("startup map restored")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("what", choices=("columns", "boot"))
    ap.add_argument("--seconds", type=float, default=8.0, help="columns: how long to record each column")
    ap.add_argument("--before", type=float, default=40.0, help="boot: seconds to record before pressing a key")
    ap.add_argument("--after", type=float, default=10.0, help="boot: seconds to record after it")
    a = ap.parse_args(argv)
    if a.what == "columns":
        columns(a.seconds)
    else:
        boot(a.before, a.after)


if __name__ == "__main__":
    main()
