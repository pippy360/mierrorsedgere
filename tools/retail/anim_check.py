"""Does the port play the animations retail played?

Runs the port's first-person animation tree (`me_anim`) over a recorded retail run and lays what
it plays next to what retail's own tree was playing on each frame (the `anim1p` records of a
telemetry v6 recording: the three heaviest sequences, with their time and weight).

    python -m tools.retail.anim_check                       # every v6 recording in recordings/
    python -m tools.retail.anim_check --trace <file.jsonl.gz>
    python -m tools.retail.anim_check --move MOVE_Jump      # the frames of one movement state, in detail
    python -m tools.retail.anim_check --at <trace> <t0> <t1>  # every frame between two times

Scores, per movement state:
    lead     the heaviest sequence is the same one
    overlap  the weight the two have in common, sequence by sequence, over retail's weight
    time     among frames with the same lead, how far apart its playback position is (median, seconds)

A recording has the pawn but not the level, and some of what the moves play depends on what
they found in the level: which vault, which way of catching a ledge, whether a long jump is over
a gap, how far the ground is. Those come from the recording itself, as the controller would give
them in the game: the ground distance from where the fall ends, the rest from the name of the
animation retail went on to play. `--no-hints` runs without them. The hints say which of a move's
animations to play, never when or how: the slot, the rate, the blend times and the frame are the
port's.

Needs `me_anim` built (cmake --build <dir> --target me_anim); `--exe` says where.
"""

import argparse
import collections
import glob
import gzip
import json
import math
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(REPO, "build", "retail", "anim")


def find_exe(given):
    if given:
        return given
    for d in ("build-ucrt", "build-win", "build"):
        for name in ("me_anim.exe", "me_anim"):
            p = os.path.join(REPO, d, name)
            if os.path.exists(p):
                return p
    raise SystemExit("me_anim not found; build it or pass --exe")


def read_samples(path):
    opener = gzip.open if path.endswith(".gz") else open
    out = []
    with opener(path, "rt", encoding="utf-8") as f:
        for line in f:
            d = json.loads(line)
            if d.get("type") == "sample":
                out.append(d)
    return out


MOVE_FALLING, MOVE_JUMP = 2, 11
# Moves a fall ends in with the feet on the ground.
GROUND_MOVES = {1, 15, 16, 20, 26, 78, 91}


def move_anim_names():
    """The animations the director plays by name: its kMoveAnims table."""
    src = open(os.path.join(REPO, "src", "anim", "fp_director.cpp"), encoding="utf-8").read()
    return set(re.findall(r'\{"(\w+)", Slot::', src))


def level_hints(run, names):
    """What the moves read off the level, for one unbroken run of samples: per sample
    (ground distance, long jump over a gap, sideways move going left, hanging free, pushing a
    direction, animation)."""
    n = len(run)
    out = [[-1.0, 0, 0, 0, 1, "-"] for _ in range(n)]
    # Braking is the ground friction alone, 8 a second off the speed; anything gentler is the player
    # still pushing.
    speed = [math.hypot(d["vx"], d["vy"]) for d in run]
    for i in range(n):
        dt = run[i]["t"] - run[i - 1]["t"] if i else 0.0
        braking = i > 0 and dt > 0.0 and speed[i] < speed[i - 1] * (1.0 - 6.0 * dt)
        out[i][4] = 0 if (speed[i] < 1.0 or braking) else 1
    leaf_names = [[a[0].lower() for a in d["anim1p"]] for d in run]
    # Where each stretch of one movement state ends.
    end = [0] * n
    for i in range(n - 1, -1, -1):
        end[i] = i + 1 if i == n - 1 or run[i + 1]["move"] != run[i]["move"] else end[i + 1]
    for i, d in enumerate(run):
        j = end[i]
        move = d["move"]
        if move == MOVE_FALLING and j < n and run[j]["move"] in GROUND_MOVES:
            out[i][0] = max(0.0, d["pz"] - run[j]["pz"])
        seen = set()
        for k in range(i, min(j, i + 40)):
            seen.update(leaf_names[k])
        if move == MOVE_JUMP and "jumpfast" in seen:
            out[i][1] = 1
        if any(s.startswith("dodgejumpleft") for s in seen):
            out[i][2] = 1
        if any(s.startswith("hangfree") for s in seen):
            out[i][3] = 1
    # The animation a move picked: the frame retail first shows one of the director's named
    # animations (or shows it started over), moved back to the start of the move when that is no
    # more than 2 frames before.
    for i in range(n):
        recent = set()
        for k in range(max(0, i - 3), i):
            recent.update(leaf_names[k])
        before = {a[0].lower(): a[1] for a in run[i - 1]["anim1p"]} if i else {}
        fresh = [a for a in run[i]["anim1p"]
                 if (a[0].lower() in names or a[0].lower().startswith("springboard"))
                 and (a[0].lower() not in recent or a[1] < before.get(a[0].lower(), 0.0) - 0.1)]
        if not fresh:
            continue
        name = max(fresh, key=lambda a: a[2])[0].lower()
        if name.startswith("springboard"):
            name = "@reached"
        at = i
        for k in range(i, max(-1, i - 3), -1):
            if k == 0 or run[k - 1]["move"] != run[k]["move"]:
                at = k
                break
        if name == "@reached":
            at = max(0, i - 1)
        if out[at][5] == "-":
            out[at][5] = name
        elif out[i][5] == "-":
            out[i][5] = name
    return out


def write_frames(samples, path, hints=True):
    """The frames file me_anim reads. Returns the samples kept, in file order (None for a reset line)."""
    kept = []
    last_t = None
    for d in samples:
        if "anim1p" not in d or not d.get("valid", True) or d.get("freecam") or d.get("noclip"):
            last_t = None
            continue
        if last_t is None or d["t"] - last_t > 0.25 or d["t"] < last_t:
            kept.append(None)
        last_t = d["t"]
        kept.append(d)
    names = move_anim_names() if hints else set()
    with open(path, "w", encoding="utf-8") as f:
        i = 0
        while i < len(kept):
            if kept[i] is None:
                f.write("reset\n")
                i += 1
                continue
            j = i
            while j < len(kept) and kept[j] is not None:
                j += 1
            run = kept[i:j]
            extra = level_hints(run, names) if hints else None
            for k, d in enumerate(run):
                f.write("%.6f %d %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f" % (
                    d["t"], d["move"], d["px"], d["py"], d["pz"], d["vx"], d["vy"], d["vz"],
                    d.get("pyaw", d.get("yaw", 0.0)), d.get("cyaw", d.get("yaw", 0.0)), d.get("cpitch", d.get("pitch", 0.0))))
                if extra:
                    f.write(" %.2f %d %d %d %d %s" % tuple(extra[k]))
                f.write("\n")
            i = j
    return kept


def read_leaves(path):
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line == "reset":
                out.append(None)
                continue
            parts = line.split("\t")
            leaves = []
            for p in parts[1:]:
                name, time, weight = p.rsplit(":", 2)
                leaves.append((name, float(time), float(weight)))
            out.append(leaves)
    return out


HINTS = True


def run(trace, exe, game_root=None):
    os.makedirs(OUT, exist_ok=True)
    stem = os.path.basename(trace).split(".")[0]
    frames = os.path.join(OUT, stem + "_frames.txt")
    leaves = os.path.join(OUT, stem + "_port.txt")
    kept = write_frames(read_samples(trace), frames, HINTS)
    cmd = [exe, "--frames", frames, "--out", leaves]
    if game_root:
        cmd += ["--game-root", game_root]
    env = dict(os.environ)
    if os.name == "nt":
        env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("me_anim failed: " + r.stderr[-400:])
    port = read_leaves(leaves)
    if len(port) != len(kept):
        raise SystemExit("me_anim wrote %d lines for %d frames" % (len(port), len(kept)))
    return [(d, p) for d, p in zip(kept, port) if d is not None]


def score(rows):
    per = collections.defaultdict(lambda: {"n": 0, "lead": 0, "overlap": 0.0, "dt": []})
    for d, port in rows:
        retail = d["anim1p"]
        if not retail:
            continue
        s = per[d["move_name"]]
        s["n"] += 1
        rw = {a[0].lower(): a[2] for a in retail}
        pw = {a[0].lower(): a[2] for a in port}
        total = sum(rw.values())
        s["overlap"] += sum(min(w, pw.get(n, 0.0)) for n, w in rw.items()) / total if total > 0 else 0.0
        # The lead: the port's heaviest sequence is retail's, or one retail has within 0.02 of it
        # (two sequences at full weight are in no particular order).
        if port:
            name = port[0][0].lower()
            for a in retail:
                if a[0].lower() == name and a[2] >= retail[0][2] - 0.02:
                    s["lead"] += 1
                    s["dt"].append(abs(port[0][1] - a[1]))
                    break
    return per


def print_scores(per, title):
    print(title)
    print("  %-26s %7s %6s %8s %7s" % ("move", "frames", "lead", "overlap", "time"))
    tot = {"n": 0, "lead": 0, "overlap": 0.0}
    for move, s in sorted(per.items(), key=lambda kv: -kv[1]["n"]):
        med = statistics.median(s["dt"]) if s["dt"] else float("nan")
        print("  %-26s %7d %5.1f%% %7.1f%% %7.3f" % (move, s["n"], 100.0 * s["lead"] / s["n"], 100.0 * s["overlap"] / s["n"], med))
        for k in tot:
            tot[k] += s[k]
    if tot["n"]:
        print("  %-26s %7d %5.1f%% %7.1f%%" % ("all", tot["n"], 100.0 * tot["lead"] / tot["n"], 100.0 * tot["overlap"] / tot["n"]))


def fmt(leaves):
    return "  ".join("%s %.2f w%.2f" % (a[0], a[1], a[2]) for a in leaves)


def detail(rows, pick, limit):
    shown = 0
    for d, port in rows:
        if not pick(d):
            continue
        print("%9.3f %-18s sp %4.0f vz %5.0f | retail %-78s | port %s" % (
            d["t"], d["move_name"][5:], math.hypot(d["vx"], d["vy"]), d["vz"], fmt(d["anim1p"]), fmt(port)))
        shown += 1
        if shown >= limit:
            break


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--trace", action="append", help="a recording; default every v6 recording in recordings/")
    ap.add_argument("--exe", help="me_anim")
    ap.add_argument("--game-root")
    ap.add_argument("--move", help="print the frames of this movement state")
    ap.add_argument("--at", nargs=3, metavar=("TRACE", "T0", "T1"), help="print every frame of TRACE between two times")
    ap.add_argument("--limit", type=int, default=60)
    ap.add_argument("--no-hints", action="store_true", help="give the moves nothing about the level")
    args = ap.parse_args()
    global HINTS
    HINTS = not args.no_hints
    exe = find_exe(args.exe)

    if args.at:
        rows = run(args.at[0], exe, args.game_root)
        t0, t1 = float(args.at[1]), float(args.at[2])
        detail(rows, lambda d: t0 <= d["t"] <= t1, 100000)
        return
    traces = args.trace or sorted(glob.glob(os.path.join(REPO, "recordings", "*.jsonl.gz")))
    total = collections.defaultdict(lambda: {"n": 0, "lead": 0, "overlap": 0.0, "dt": []})
    for trace in traces:
        rows = run(trace, exe, args.game_root)
        if not rows:
            continue
        if args.move:
            print(os.path.basename(trace))
            detail(rows, lambda d: d["move_name"] == args.move, args.limit)
            continue
        per = score(rows)
        if args.trace:
            print_scores(per, os.path.basename(trace))
        for move, s in per.items():
            for k in ("n", "lead", "overlap"):
                total[move][k] += s[k]
            total[move]["dt"] += s["dt"]
    if not args.move and not args.trace:
        print_scores(total, "all recordings")


if __name__ == "__main__":
    main()
