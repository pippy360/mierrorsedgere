"""Check the port's start-of-level intros against the retail recordings of them.

    python -m tools.retail.intro_check play <mirrorsedge_windows.exe> [map ...]
    python -m tools.retail.intro_check compare [map ...] [--json]

`play` (Windows) starts the port once per chapter the way a player starts it, with --trace, and
keeps the trace in build/retail/intros_port/<map>.jsonl. Keys are posted to the game's own window,
never typed: Space to skip the chapter's movie, Escape twice to quit once the intro is over.

`compare` lays each of those over the retail recording of the same chapter
(tools/retail/intro_capture.py, build/retail/intros/<map>.jsonl) and prints, per chapter:

  * the camera: how far the port's eye is from retail's through the intro, and how far apart the
    view directions and the roll are;
  * the hand-over: where the port's camera sits against retail's a few seconds after the intro;
  * the sounds: every cue retail started, paired with the port's start of the same cue.

How the two are lined up, and what the recording cannot tell:

  * Clocks. Retail's intro is taken to start at its first valid camera frame, the port's at its first
    intro frame. Retail's first frame is followed by a hitch of a few tenths of a second during which
    its Matinee runs on, so the port's clock is shifted by the one offset (searched in 10 ms steps)
    that brings the two camera paths closest; sounds get an offset of their own, fitted on the cues.
  * Near plane. The hook solves the camera position out of the view-projection's first three columns,
    and the third is the depth column, so what it reports is the eye pushed one near plane (10 uu)
    along the view direction. That is taken back out here.
  * Blind frames. For about six seconds at the start of a chapter, while its title is on screen, the
    hook reads its angles off another pass's matrix and reports yaw 0, pitch 90. Those frames count
    for position (corrected along the port's own view direction) and not for angles.
  * Frame times. The recordings were made while saving a frame every second, which stalls retail for
    65 ms or so each time. Angles, and the second position figure, are therefore taken as the best
    match within 50 ms.
  * The hook logs no sound for the first second or so of a chapter. Port sounds before retail's first
    logged one are counted apart, not as extras.
"""
import argparse
import bisect
import json
import math
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from tools.retail import paths  # noqa: E402

# retail map -> how the port is asked for it, the intro Matinee's length (s), where in it the animation starts
LEVELS = {
    "edge_p": (["--chapter", "1"], 63.134, 3.0),
    "escape_p": (["--level", "Maps/SP01/Escape_p.me1"], 17.0, 0.0),
    "stormdrain_p": (["--chapter", "2"], 7.567, 0.0),
    "cranes_p": (["--chapter", "3"], 17.1, 0.0),
    "subway_p": (["--chapter", "4"], 22.702, 0.0),
    "mall_p": (["--chapter", "5"], 16.667, 0.0),
    "factory_p": (["--chapter", "6"], 10.0, 0.0),
    "boat_p": (["--chapter", "7"], 18.05, 0.0),
    "convoy_p": (["--chapter", "8"], 3.269, 0.0),
    "scraper_p": (["--chapter", "9"], 12.7, 0.0),
}
NEAR_PLANE = 10.0       # uu: what the hook's camera position is ahead of the eye by
SLACK = 0.05            # s: retail's stalled frames
SOUND_TOLERANCE = 0.15  # s: a retail and a port start of one cue are the same event
# level ambience the hook also logs, which is not the intro's (two-digit names are the cues of
# A_Ambience_Volumes, started by walking into an ambience volume)
AMBIENT = ("Coo", "Pigeons", "ambience", "Engine", "JetPack", "Breath_", "Sparks", "Helicopter")


def ambient(cue):
    return cue.startswith(AMBIENT) or (len(cue) == 2 and cue.isdigit())


def retail_dir():
    return paths.build_dir("intros")


def port_dir():
    return paths.ensure_dir(paths.build_dir("intros_port"))


# ---- play -------------------------------------------------------------------------------------

def play(exe, names):
    import ctypes
    import ctypes.wintypes as wt

    user32 = ctypes.windll.user32
    keys = {"space": (0x20, 0x39), "escape": (0x1B, 0x01)}

    def window_of(pid):
        found = []

        @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        def each(hwnd, _):
            p = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
            if p.value == pid and user32.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True

        user32.EnumWindows(each, 0)
        return found[0] if found else None

    def post_key(hwnd, name):
        vk, scan = keys[name]
        down = 1 | (scan << 16)
        user32.PostMessageW(hwnd, 0x0100, vk, down)
        time.sleep(0.12)
        user32.PostMessageW(hwnd, 0x0101, vk, down | 0xC0000000)

    def last_sample(path):
        try:
            with open(path, "rb") as f:
                f.seek(0, os.SEEK_END)
                f.seek(max(0, f.tell() - 4096))
                lines = f.read().decode("utf-8", "replace").strip().split("\n")
        except OSError:
            return None
        for line in reversed(lines):
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if r.get("type") == "sample":
                return r
        return None

    out = port_dir()
    for name in names:
        args, length, _ = LEVELS[name]
        trace = os.path.join(out, name + ".jsonl")
        if os.path.exists(trace):
            os.remove(trace)
        with open(os.path.join(out, name + ".log"), "w") as log:
            p = subprocess.Popen([exe] + args + ["--trace", trace], stdout=log, stderr=subprocess.STDOUT, cwd=out)
        hwnd, skipped, seen_intro, over_at = None, False, False, None
        deadline = time.time() + 120 + length
        while time.time() < deadline and p.poll() is None:
            time.sleep(0.2)
            hwnd = hwnd or window_of(p.pid)
            s = last_sample(trace)
            if not s or not hwnd:
                continue
            state = s.get("cutscene")
            if state == "movie" and not skipped and s["t"] > 1.0:
                post_key(hwnd, "space")
                skipped = True
            elif state == "intro":
                seen_intro = True
            elif state == "none" and (seen_intro or s["t"] > 4.0):
                over_at = over_at or time.time()
                if time.time() - over_at >= 5.0:   # the hand-over is part of the picture
                    break
        if p.poll() is None and hwnd:
            post_key(hwnd, "escape")   # opens the menu
            time.sleep(0.8)
            post_key(hwnd, "escape")   # quits
            try:
                p.wait(timeout=15)
            except subprocess.TimeoutExpired:
                p.kill()
        elif p.poll() is None:
            p.kill()
        print("%-13s intro played: %-5s exit code %s" % (name, seen_intro, p.returncode))


# ---- compare ----------------------------------------------------------------------------------

def load(path):
    samples, sounds = [], []
    with open(path, encoding="utf-8") as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if r.get("type") == "sample":
                samples.append(r)
            elif r.get("type") == "sound" and r.get("start"):
                sounds.append(r)
    return samples, sounds


def wrap(a):
    return (a + 180.0) % 360.0 - 180.0


def angles_read(s):
    return not (abs(s["pitch"] - 90.0) < 0.01 and abs(s["yaw"]) < 0.01)


def forward(s):
    cp = math.cos(math.radians(s["pitch"]))
    return (cp * math.cos(math.radians(s["yaw"])), cp * math.sin(math.radians(s["yaw"])), math.sin(math.radians(s["pitch"])))


class Path:
    """Camera samples on a clock that starts at t0, read at any time between them."""

    def __init__(self, samples, t0):
        self.t = [s["t"] - t0 for s in samples]
        self.s = samples

    def at(self, t):
        i = bisect.bisect_left(self.t, t)
        if i <= 0 or i >= len(self.t):
            s = self.s[0 if i <= 0 else -1]
            return dict(s, read=angles_read(s))
        a, b = self.s[i - 1], self.s[i]
        span = self.t[i] - self.t[i - 1]
        u = (t - self.t[i - 1]) / span if span > 1e-9 else 0.0
        out = {k: a[k] + (b[k] - a[k]) * u for k in ("x", "y", "z")}
        for k in ("yaw", "pitch", "roll"):
            out[k] = a[k] + wrap(b[k] - a[k]) * u
        out["read"] = angles_read(a) and angles_read(b)
        return out


def retail_eye(r, p):
    """Retail's eye: the reported position less the near plane along its view direction (the port's
    in the frames where retail's own cannot be read)."""
    f = forward(r if r["read"] else p)
    return (r["x"] - NEAR_PLANE * f[0], r["y"] - NEAR_PLANE * f[1], r["z"] - NEAR_PLANE * f[2])


def pct(values, p):
    v = sorted(values)
    return v[min(len(v) - 1, int(p * len(v)))] if v else float("nan")


def bare(cue):
    return cue.rsplit(".", 1)[-1]


def compare_one(name):
    _, length, _ = LEVELS[name]
    out = {"map": name, "matinee_s": length}
    rp = os.path.join(retail_dir(), name + ".jsonl")
    pp = os.path.join(port_dir(), name + ".jsonl")
    if not os.path.exists(rp) or not os.path.exists(pp):
        out["missing"] = "no retail recording" if not os.path.exists(rp) else "no port trace"
        return out
    rs, rsnd = load(rp)
    ps, psnd = load(pp)
    valid = [s for s in rs if s.get("valid")]
    intro = [s for s in ps if s.get("cutscene") == "intro"]
    if not valid or not intro:
        out["missing"] = "no valid retail camera" if not valid else "the port played no intro"
        return out
    r0, p0, p1 = valid[0]["t"], intro[0]["t"], intro[-1]["t"]
    retail = Path(valid, r0)
    port = Path([s for s in ps if s["t"] >= p0], p0)
    out["port_intro_s"] = round(p1 - p0, 2)

    def eye_distance(t, shift):
        r, p = retail.at(t), port.at(t + shift)
        return math.dist(retail_eye(r, p), (p["x"], p["y"], p["z"]))

    # the port's clock against retail's
    best = None
    for k in range(-400, 401):
        shift = k * 0.01
        total, n, t = 0.0, 0, 0.6
        while t <= length - 0.5:
            total += eye_distance(t, shift)
            n += 1
            t += 0.1
        if n and (best is None or total / n < best[0]):
            best = (total / n, shift)
    shift = best[1]
    out["clock_shift_s"] = round(shift, 2)

    pos, pos_slack, view, roll, over = [], [], [], [], 0
    t = 0.6  # past retail's start-up hitch
    while t <= length - 0.5:
        pos.append(eye_distance(t, shift))
        pos_slack.append(min(eye_distance(t, shift + k * 0.01) for k in range(-5, 6)))
        r = retail.at(t)
        if r["read"]:
            best_angle = None
            for k in range(-5, 6):
                p = port.at(t + shift + k * 0.01)
                dot = sum(a * b for a, b in zip(forward(r), forward(p)))
                angle = math.degrees(math.acos(max(-1.0, min(1.0, dot))))
                droll = abs(wrap(r["roll"] - p["roll"]))
                if best_angle is None or angle + droll < best_angle[0] + best_angle[1]:
                    best_angle = (angle, droll)
            view.append(best_angle[0])
            roll.append(best_angle[1])
            over += 1 if best_angle[0] > 3.0 or best_angle[1] > 4.0 else 0
        t += 1.0 / 30.0
    out["eye_uu"] = {"median": round(pct(pos, 0.5), 1), "p95": round(pct(pos, 0.95), 1), "p95_within_50ms": round(pct(pos_slack, 0.95), 1)}
    out["angles_read_pct"] = round(100.0 * len(view) / max(1, len(pos)))
    if view:
        out["view_deg"] = {"median": round(pct(view, 0.5), 2), "p95": round(pct(view, 0.95), 2), "max": round(max(view), 1)}
        out["roll_deg"] = {"median": round(pct(roll, 0.5), 2), "p95": round(pct(roll, 0.95), 2), "max": round(max(roll), 1)}
        out["apart_s"] = round(over / 30.0, 1)  # seconds with the view over 3 degrees or the roll over 4 apart

    # the hand-over: three seconds after the Matinee, both standing still
    r, p = retail.at(length + 3.0), port.at(length + 3.0 + shift)
    e = retail_eye(r, p)
    yaw = math.radians(p["yaw"])
    out["after"] = {"apart_uu": round(math.dist(e, (p["x"], p["y"], p["z"])), 1),
                    "port_ahead_uu": round((p["x"] - e[0]) * math.cos(yaw) + (p["y"] - e[1]) * math.sin(yaw), 1),
                    "port_above_uu": round(p["z"] - e[2], 1)}

    # the sounds
    r_cues = [(e["t"] - r0, bare(e["cue"])) for e in rsnd if not ambient(e["cue"])]
    p_cues = [(e["t"] - p0, bare(e["cue"])) for e in psnd if p0 - 0.05 <= e["t"] <= p1 + 0.05]

    def pair(offset):
        used, pairs = set(), []
        for i, (rt, rc) in enumerate(r_cues):
            found = None
            for j, (pt, pc) in enumerate(p_cues):
                gap = abs(pt - offset - rt)
                if j not in used and pc == rc and gap <= SOUND_TOLERANCE and (found is None or gap < found[0]):
                    found = (gap, j)
            if found:
                used.add(found[1])
                pairs.append((i, found[1], found[0]))
        return pairs, used

    best = None
    for k in range(0, 101):
        pairs, _ = pair(k * 0.01)
        score = (len(pairs), -sum(g for _, _, g in pairs))
        if best is None or score > best[0]:
            best = (score, k * 0.01)
    offset = best[1]
    pairs, used = pair(offset)
    paired_r = {i for i, _, _ in pairs}
    end = length - offset   # the Matinee's end on retail's clock
    first_logged = min((t for t, _ in r_cues), default=0.0)
    gaps = sorted(g for _, _, g in pairs)
    out["sounds"] = {
        "retail_logged": len([1 for t, _ in r_cues if t <= end + 0.02]),
        "paired": len(pairs),
        "apart_ms": {"median": round(1000 * pct(gaps, 0.5)), "max": round(1000 * gaps[-1])} if gaps else None,
        "retail_only": ["%s %.2f" % (c, t) for i, (t, c) in enumerate(r_cues) if t <= end + 0.02 and i not in paired_r],
        "port_only": ["%s %.2f" % (c, t - offset) for j, (t, c) in enumerate(p_cues) if j not in used and t - offset >= first_logged - 0.02],
        "port_before_retail_logs": len([1 for j, (t, _) in enumerate(p_cues) if j not in used and t - offset < first_logged - 0.02]),
        "retail_just_after": ["%s +%.2f" % (c, t - end) for t, c in r_cues if end + 0.02 < t <= end + 1.0],
    }
    return out


def compare(names, as_json):
    for name in names:
        r = compare_one(name)
        if as_json:
            print(json.dumps(r))
            continue
        if "missing" in r:
            print("%-13s %s" % (name, r["missing"]))
            continue
        e, a, s = r["eye_uu"], r["after"], r["sounds"]
        print("%-13s %5.1f s Matinee, port clock %+.2f s" % (name, r["matinee_s"], r["clock_shift_s"]))
        print("    eye       median %.1f uu, 95%% within %.1f uu (%.1f uu allowing 50 ms)" % (e["median"], e["p95"], e["p95_within_50ms"]))
        if "view_deg" in r:
            v, ro = r["view_deg"], r["roll_deg"]
            print("    view      median %.2f deg, 95%% within %.2f, worst %.1f; roll median %.2f, worst %.1f; %.1f s apart"
                  " (angles readable in %d%% of the intro)" % (v["median"], v["p95"], v["max"], ro["median"], ro["max"], r["apart_s"], r["angles_read_pct"]))
        else:
            print("    view      retail's angles are not readable anywhere in this intro")
        print("    hand-over port camera %.1f uu from retail's: %+.1f along the view, %+.1f up" % (a["apart_uu"], a["port_ahead_uu"], a["port_above_uu"]))
        gap = s["apart_ms"]
        print("    sounds    %d of retail's %d paired%s; %d port sounds fall before retail's log starts" % (
            s["paired"], s["retail_logged"], (" (median %d ms apart, worst %d)" % (gap["median"], gap["max"])) if gap else "", s["port_before_retail_logs"]))
        for label, key in (("retail only", "retail_only"), ("port only", "port_only"), ("retail, just after the Matinee", "retail_just_after")):
            if s[key]:
                print("              %s: %s" % (label, ", ".join(s[key])))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    pl = sub.add_parser("play", help="run the port's intros and keep their traces")
    pl.add_argument("exe")
    pl.add_argument("maps", nargs="*")
    cm = sub.add_parser("compare", help="compare those traces with the retail recordings")
    cm.add_argument("maps", nargs="*")
    cm.add_argument("--json", action="store_true")
    args = ap.parse_args()
    names = [m.lower() for m in args.maps] or list(LEVELS)
    unknown = [n for n in names if n not in LEVELS]
    if unknown:
        ap.error("no intro for: %s (known: %s)" % (", ".join(unknown), ", ".join(LEVELS)))
    if args.cmd == "play":
        play(os.path.abspath(args.exe), names)
    else:
        compare(names, args.json)


if __name__ == "__main__":
    main()
