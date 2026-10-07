"""Replay a recorded retail run in this engine's ParkourController, and diff it.

    python -m tools.retail.replay                          # newest recording
    python -m tools.retail.replay --trace <file.jsonl>     # a given one
    python -m tools.retail.replay --segment 4              # re-anchor every 4 s
    python -m tools.retail.replay --window 7               # one window, with its detail
    python -m tools.retail.replay --dry                    # write the script, no run

tools/retail/record_session.py records a person playing the retail game: the
pawn's own position, velocity and move every frame, the view, and every key
the game received. This turns that recording into a script for me_replay
(src/tools/replay_main.cpp), which loads the same level from the retail
packages and drives the ParkourController with it - ONE STEP PER RECORDED
RETAIL FRAME, at that frame's own dt, with the keys that frame took
(trace.frame_start_keys) and the view it turned to. So port frame k and
retail sample i0 + k are the same moment, and the comparison is per frame.

Two kinds of window, as in tesseract's replay (whose trace handling
tools/retail/trace.py is):

  * the FULL run, open loop from the first key: how far the controller
    follows the human before the paths part, and where;
  * SEGMENTS, each re-anchored on a retail frame - its feet, velocity and
    view, and the frame state retail's PlayerMove carries
    (trace.retail_ground_state) - then open loop for --segment seconds. A
    segment that ends within DIVERGE_UU of retail reproduces; one that does
    not says where it parted, during which retail move, and how.

There is no coordinate conversion: this engine works in UE units in the
level's own space, as retail does. Retail's pawn Location is the capsule
centre and this engine's position is the feet, so the anchor is
pz - trace.centre_above_feet(sample), lifted ANCHOR_LIFT_UU to settle.
"""
import argparse
import bisect
import glob
import json
import math
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.retail import paths, trace  # noqa: E402
from tools.retail.trace import (FRAME_EPS, KEYS, PULSES, UE_PHYS_WALKING,  # noqa: E402
                                centre_above_feet, held_from_start)

ROOT = paths.repo_root()
ANCHOR_LIFT_UU = 3.0
DIVERGE_UU = 50.0           # the gap that counts as "parted"
FELL_UU = 300.0             # port below retail by this much = fell through

# me_replay's key bits (replay_main.cpp KeyBit)
BITS = {"w": 1, "s": 2, "a": 4, "d": 8, "space": 16, "lshift": 32, "q": 64, "lmb": 128}
LATCHED = ("space",)        # a press inside one frame still jumps (trace.py, LATCHED)
assert set(BITS) == set(KEYS.values())


# ---------------------------------------------------------------------------
# Where things are

def trace_stem(path):
    """'20260930_213855_escape_overlay_session' for .jsonl and .jsonl.gz alike."""
    name = os.path.basename(path)
    for ext in (".gz", ".jsonl"):
        if name.endswith(ext):
            name = name[:-len(ext)]
    return name


def newest_trace():
    """The newest recording: new ones in build/retail/trials, the committed
    ones in recordings/. By the stamp in the name, which a checkout keeps and
    a file's mtime does not."""
    files = (glob.glob(os.path.join(paths.build_dir("trials"), "*.jsonl"))
             + glob.glob(os.path.join(ROOT, "recordings", "*.jsonl.gz")))
    if not files:
        raise SystemExit("no recordings in %s or recordings/ - record one with "
                         "python -m tools.retail.record_session" % paths.build_dir("trials"))
    return max(files, key=trace_stem)


def find_exe(given=None):
    if given:
        return given
    for rel in ("build/me_replay", "build/me_replay.exe", "build-win/me_replay.exe",
                "build/Release/me_replay.exe"):
        p = os.path.join(ROOT, rel)
        if os.path.isfile(p):
            return p
    raise SystemExit("no me_replay binary - build it:  cmake -B build && cmake --build build "
                     "--target me_replay  (or pass --exe)")


def find_level(map_name):
    """'escape_p' -> 'Maps/SP01/Escape_p.me1', by searching the install."""
    maps = paths.medge_path("TdGame", "CookedPC", "Maps")
    want = map_name.lower()
    if not want.endswith(".me1"):
        want += ".me1"
    for dirpath, _dirs, files in os.walk(maps):
        for f in files:
            if f.lower() == want:
                rel = os.path.relpath(os.path.join(dirpath, f), paths.medge_path("TdGame", "CookedPC"))
                return rel.replace("\\", "/")
    raise SystemExit("no %s under %s - pass --level Maps/<dir>/<name>.me1" % (want, maps))


def move_names():
    """EMovement number -> name, read off src/math/types.hpp."""
    path = os.path.join(ROOT, "src", "math", "types.hpp")
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    body = text[text.index("enum class EMovement"):]
    body = body[:body.index("};")]
    for m in re.finditer(r"MOVE_(\w+)\s*=\s*(\d+)", body):
        out[int(m.group(2))] = m.group(1)
    return out


# ---------------------------------------------------------------------------
# Frames and windows

def frame_inputs(samples, fk):
    """Per recorded frame i (the one that ENDS at sample i, i >= 1): the key
    bits it took, and the keys held going into frame i + 1.

    frame_start_keys puts every key event on the start of the frame that took
    it (t[i-1] + FRAME_EPS), so frame i takes the events in
    (t[i-2] + FRAME_EPS, t[i-1] + FRAME_EPS]. A latched key pressed and
    released inside one frame is still a press there; Q and the left button
    are presses, never holds."""
    n = len(samples)
    bits = [0]*n
    held_after = [None]*n
    held = set(k for k in held_from_start(fk) if k not in PULSES)
    ki = 0
    # events before the second sample belong to nothing replayable: fold them in
    edge0 = samples[0]["t"] + FRAME_EPS*1.5
    while ki < len(fk) and fk[ki][0] <= edge0:
        _t, k, down = fk[ki]
        if k not in PULSES:
            (held.add if down else held.discard)(k)
        ki += 1
    held_after[0] = set(held)
    for i in range(1, n):
        edge = samples[i - 1]["t"] + FRAME_EPS*1.5
        pressed = set()
        while ki < len(fk) and fk[ki][0] <= edge:
            _t, k, down = fk[ki]
            if down:
                pressed.add(k)
            if k not in PULSES:
                (held.add if down else held.discard)(k)
            ki += 1
        b = 0
        for k in held:
            b |= BITS[k]
        for k in pressed:
            if k in PULSES or k in LATCHED:
                b |= BITS[k]
        bits[i] = b
        held_after[i] = set(held)
    return bits, held_after


def plan_windows(samples, fk, seconds, full=True):
    """[(name, i0, i1)]: windows anchored on sample i0, replayed over frames
    i0+1 .. i1."""
    ts = [s["t"] for s in samples]
    t_first = trace.first_key_time(fk)
    i_first = max(0, bisect.bisect_left(ts, t_first) - 1)
    out = []
    if full:
        out.append(("full", i_first, len(samples) - 1))
    if seconds > 0:
        tb = ts[i_first]
        n = 1
        while tb < ts[-1] - 0.5:
            # anchor on a frame where retail walks: a wall run or a grab has state
            # an anchor cannot hand over
            lo = bisect.bisect_left(ts, tb - 1.0)
            hi = bisect.bisect_right(ts, tb + 1.0)
            cands = [i for i in range(lo, hi) if i < len(samples) - 1
                     and samples[i].get("physics") == UE_PHYS_WALKING
                     and samples[i].get("move_name") == "MOVE_Walking"]
            i0 = (min(cands, key=lambda i: abs(ts[i] - tb)) if cands
                  else min(max(bisect.bisect_left(ts, tb), 0), len(samples) - 2))
            i1 = min(bisect.bisect_right(ts, ts[i0] + seconds) - 1, len(samples) - 1)
            if i1 > i0:
                out.append(("seg%02d" % n, i0, i1))
            tb += seconds
            n += 1
    return out


def anchor_line(wid, s, fk, samples, held):
    st = trace.retail_ground_state(samples, fk, s["t"])
    grounded = s.get("physics") == UE_PHYS_WALKING
    feet_z = s["pz"] - centre_above_feet(s) + ANCHOR_LIFT_UU
    stop_left = (trace.STOP_TIME_S - st["stop"]) if st["stop"] >= 0.0 else 0.0
    hb = 0
    for k in held:
        hb |= BITS[k]
    return ("seg %d %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %d %d %.3f %.4f %.4f %.3f %d"
            % (wid, s["px"], s["py"], feet_z, s["vx"], s["vy"], s["vz"],
               s["look_yaw"], s["look_pitch"], 1 if grounded else 0,
               st["jump"] if not grounded else 0, st["energy"], st["acctime"],
               stop_left, st["prejump"], hb))


def write_script(path, samples, fk, windows):
    bits, held_after = frame_inputs(samples, fk)
    with open(path, "w", newline="\n") as f:
        f.write("# generated by tools/retail/replay.py - do not edit\n")
        for wid, (name, i0, i1) in enumerate(windows):
            f.write("# %s: samples %d..%d\n" % (name, i0, i1))
            f.write(anchor_line(wid, samples[i0], fk, samples, held_after[i0]) + "\n")
            for i in range(i0 + 1, i1 + 1):
                s, p = samples[i], samples[i - 1]
                f.write("f %.6f %d %.3f %.3f\n" % (s["t"] - p["t"], bits[i],
                                                    s["look_yaw"], s["look_pitch"]))


# ---------------------------------------------------------------------------
# The run

def run(exe, level, script, out, log):
    cmd = [exe, "--game-root", paths.find_medge_install(), "--level", level,
           "--script", script, "--out", out]
    with open(log, "w") as lf:
        r = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT)
    if r.returncode != 0 or not os.path.isfile(out):
        tail = open(log, errors="replace").read().splitlines()[-15:]
        raise SystemExit("me_replay failed (exit %d):\n%s" % (r.returncode, "\n".join(tail)))


def parse(path):
    starts, frames = {}, {}
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == "S":
                starts[int(p[1])] = {"feet": tuple(map(float, p[2:5])), "grounded": int(p[5])}
            elif p[0] == "T":
                frames.setdefault(int(p[1]), []).append({
                    "k": int(p[2]), "t": float(p[3]),
                    "x": float(p[4]), "y": float(p[5]), "z": float(p[6]),
                    "vx": float(p[7]), "vy": float(p[8]), "vz": float(p[9]),
                    "yaw": float(p[10]), "pitch": float(p[11]), "move": int(p[12]),
                    "grounded": int(p[13]), "health": float(p[14])})
    return starts, frames


# ---------------------------------------------------------------------------
# The comparison

def retail_feet(s):
    return (s["px"], s["py"], s["pz"] - centre_above_feet(s))


def score(samples, i0, rows):
    out = []
    for r in rows:
        s = samples[i0 + r["k"]]
        rp = retail_feet(s)
        dx, dy, dz = r["x"] - rp[0], r["y"] - rp[1], r["z"] - rp[2]
        out.append({"t": r["t"], "gap": math.sqrt(dx*dx + dy*dy + dz*dz),
                    "gap_xy": math.hypot(dx, dy), "dz": dz, "port": (r["x"], r["y"], r["z"]),
                    "retail": rp, "retail_move": (s.get("move_name") or "?").replace("MOVE_", ""),
                    "port_move": r["move"], "grounded": r["grounded"], "health": r["health"]})
    if not out:
        return {"status": "no output"}
    end = out[-1]
    worst = max(out, key=lambda r: r["gap"])
    parted = next((r for r in out if r["gap"] > DIVERGE_UU), None)
    fell = next((r for r in out if r["dz"] < -FELL_UU), None)
    died = next((r for r in out if r["health"] <= 0.0), None)
    if fell:
        status = "FELL"
    elif end["gap"] <= DIVERGE_UU:
        status = "reproduces"
    elif end["gap_xy"] <= DIVERGE_UU:
        status = "height differs"
    else:
        r0 = out[0]["retail"]
        travel = (end["retail"][0] - r0[0], end["retail"][1] - r0[1])
        tl = math.hypot(*travel)
        along = 0.0
        if tl > 1.0:
            along = ((end["port"][0] - end["retail"][0])*travel[0]
                     + (end["port"][1] - end["retail"][1])*travel[1])/tl
        status = ("short %.0f uu" % -along if along < -DIVERGE_UU else
                  "long %.0f uu" % along if along > DIVERGE_UU else "off line")
    return {"status": status, "end_gap": end["gap"], "end_dz": end["dz"],
            "max_gap": worst["gap"], "max_gap_t": worst["t"],
            "parted_t": parted["t"] if parted else None,
            "parted_at": parted["retail"] if parted else None,
            "parted_move": parted["retail_move"] if parted else None,
            "fell_t": fell["t"] if fell else None, "died_t": died["t"] if died else None,
            "port_grounded": sum(r["grounded"] for r in out)/len(out),
            "frames": len(out), "rows": out}


def moves_text(samples, i0, i1, limit=5):
    mv = trace.moves_in(samples, samples[i0]["t"], samples[i1]["t"])
    s = ", ".join("%s %.1f" % (n, d) for (n, d) in mv[:limit])
    return s + (", +%d" % (len(mv) - limit) if len(mv) > limit else "")


def write_detail(path, name, samples, i0, i1, keys, sc, names):
    s0 = samples[i0]
    with open(path, "w") as f:
        f.write("%s: retail t %.2f..%.2f s, anchor %s at UE (%.0f, %.0f, %.0f) view %.1f/%.1f "
                "vel (%.0f, %.0f, %.0f) %s\n"
                % (name, s0["t"], samples[i1]["t"], s0.get("move_name"), s0["px"], s0["py"],
                   s0["pz"], s0["look_yaw"], s0["look_pitch"], s0["vx"], s0["vy"], s0["vz"],
                   "grounded" if s0.get("physics") == UE_PHYS_WALKING else "airborne"))
        f.write("keys (t rel): %s\n" % ", ".join(
            "%s%s@%.2f" % ("+" if down else "-", k, kt - s0["t"])
            for (kt, k, down) in keys if s0["t"] <= kt <= samples[i1]["t"]))
        f.write("   t   | retail feet x y z         move           | port feet x y z           "
                "move           gnd | gap   dz\n")
        for n, r in enumerate(sc.get("rows", ())):
            if n % 4:
                continue
            f.write("%6.2f | %7.0f %7.0f %7.0f  %-14s | %7.0f %7.0f %7.0f  %-14s %d   | %5.0f %5.0f\n"
                    % (r["t"], r["retail"][0], r["retail"][1], r["retail"][2], r["retail_move"][:14],
                       r["port"][0], r["port"][1], r["port"][2],
                       names.get(r["port_move"], str(r["port_move"]))[:14], r["grounded"],
                       r["gap"], r["dz"]))


def report(tracepath, level, samples, windows, frames, out, keys, names):
    lines = ["replay of %s on %s" % (os.path.basename(tracepath), level), ""]
    hdr = ("window  start(s) len(s)  retail moves in window                        "
           "status            end gap  max gap  parted at (s)")
    lines += [hdr, "-"*len(hdr)]
    t0 = samples[windows[0][1]]["t"] if windows else 0.0
    results = []
    for wid, (name, i0, i1) in enumerate(windows):
        sc = score(samples, i0, frames.get(wid, []))
        write_detail("%s_%s.txt" % (out, name), name, samples, i0, i1, keys, sc, names)
        parted = "%.2f" % sc["parted_t"] if sc.get("parted_t") is not None else "-"
        lines.append("%-7s %8.2f %6.2f  %-46s %-17s %7s %8s  %s"
                     % (name, samples[i0]["t"] - t0, samples[i1]["t"] - samples[i0]["t"],
                        moves_text(samples, i0, i1)[:46], sc["status"],
                        "%.0f" % sc["end_gap"] if "end_gap" in sc else "-",
                        "%.0f" % sc["max_gap"] if "max_gap" in sc else "-", parted))
        sc = dict(sc)
        sc.pop("rows", None)
        results.append({"window": name, "i0": i0, "i1": i1, "t_start": samples[i0]["t"],
                        "t_end": samples[i1]["t"], "score": sc})
    segs = [r for r in results if r["window"] != "full"]
    ok = sum(1 for r in segs if r["score"].get("status") == "reproduces")
    lines.append("")
    full = next((r for r in results if r["window"] == "full"), None)
    if full and full["score"].get("parted_t") is not None:
        p = full["score"]
        lines.append("full run: parts from retail after %.2f s at UE (%.0f, %.0f, %.0f) during %s; %s"
                     % (p["parted_t"], p["parted_at"][0], p["parted_at"][1], p["parted_at"][2],
                        p["parted_move"], p["status"]))
    elif full:
        lines.append("full run: %s, end gap %.0f uu" % (full["score"].get("status"),
                                                       full["score"].get("end_gap", -1)))
    if segs:
        lines.append("segments reproducing (end gap <= %.0f uu): %d of %d" % (DIVERGE_UU, ok, len(segs)))
    text = "\n".join(lines)
    print(text)
    with open(out + "_report.txt", "w") as f:
        f.write(text + "\n")
    with open(out + "_windows.json", "w") as f:
        json.dump({"trace": tracepath, "level": level, "results": results}, f, indent=1)
    return results


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--trace", help="recording .jsonl or .jsonl.gz (default: the newest)")
    ap.add_argument("--level", help="map package relative to CookedPC, e.g. Maps/SP01/Escape_p.me1 "
                                    "(default: the recording's own map)")
    ap.add_argument("--segment", type=float, default=4.0,
                    help="re-anchor window length in seconds (0: the full run only)")
    ap.add_argument("--no-full", action="store_true", help="skip the open-loop full run")
    ap.add_argument("--window", type=int, help="replay only this segment number")
    ap.add_argument("--look", default="auto", choices=("auto", "controller", "camera", "velocity"),
                    help="where the view comes from (trace.stamp_look)")
    ap.add_argument("--exe", help="the me_replay binary (default: found under build/)")
    ap.add_argument("--name", help="output stem (default: the recording's)")
    ap.add_argument("--dry", action="store_true", help="write the script and stop")
    a = ap.parse_args(argv)

    tracepath = a.trace or newest_trace()
    meta, samples, keys = trace.load_trace(tracepath)
    source = trace.stamp_look(samples, keys, a.look)
    fk = trace.frame_start_keys(keys, samples)
    level = a.level or find_level((meta or {}).get("map") or "edge_p")
    windows = plan_windows(samples, fk, a.segment, full=not a.no_full and a.window is None)
    if a.window is not None:
        windows = [w for w in windows if w[0] == "seg%02d" % a.window]
        if not windows:
            raise SystemExit("no segment %d" % a.window)
    print("%s: %d pawn samples over %.1f s, %d key events; view from the %s; level %s"
          % (os.path.basename(tracepath), len(samples), samples[-1]["t"] - samples[0]["t"],
             len(keys), source, level))
    for p in meta.get("pauses") or ():
        print("retail was PAUSED %.2f s at t %.2f: cut" % (p["seconds"], p["t"]))

    outdir = paths.ensure_dir(paths.build_dir("replay"))
    stem = a.name or trace_stem(tracepath)
    out = os.path.join(outdir, stem)
    script = out + "_script.txt"
    write_script(script, samples, fk, windows)
    print("%d windows -> %s" % (len(windows), script))
    if a.dry:
        return 0
    run(find_exe(a.exe), level, script, out + "_port.txt", out + "_me_replay.log")
    _starts, frames = parse(out + "_port.txt")
    report(tracepath, level, samples, windows, frames, out, keys, move_names())
    print("-> %s_report.txt, %s_<window>.txt" % (out, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
