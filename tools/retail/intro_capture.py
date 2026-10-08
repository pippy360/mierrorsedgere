"""Record the retail start-of-level intros: the seconds in which Faith moves into place by herself.

    python -m tools.retail.intro_capture                  # every chapter that has one
    python -m tools.retail.intro_capture --only stormdrain_p,convoy_p
    python -m tools.retail.intro_capture --frames 1.0     # also save the back buffer every second

Each chapter is booted at its first checkpoint, which is what runs the intro Matinee, and the
telemetry hook (tools/retail/hook) is drained from the first frame until the intro animation has
ended. Nothing is typed into the game. A trace lands in build/retail/intros/<map>.jsonl in the
format of tools/retail/tracefile.py: per frame the camera, the pawn and the animations playing on
her first-person body (`anim1p`, with the clip time), and a record for every sound that starts or
stops.

The game is restarted once per chapter. [URL] Map / LocalMap of the user's TdEngine.ini point at
the chapter for each launch and are put back at the end. The boot URL leaves out AllowCPSaving, so
the game is not asked to save a checkpoint; the save file is still copied first and compared
afterwards.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from tools.retail import drive, menu_capture, paths, telemetry, tracefile  # noqa: E402

# map (as DefaultGame.ini names it), its first checkpoint, the intro AnimSequence, its length (s).
# The lengths are the Matinee lengths in the cooked level (build/re/intro_scan.py).
LEVELS = [
    ("edge_p", "Edge_Start", "sp01_intro", 63.2),
    ("escape_p", "Start", "sp01_intro_b", 17.0),
    ("Stormdrain_p", "Canals", "sp02_intro", 7.6),
    ("cranes_p", "SP03_Start", "sp03_intro", 17.1),
    ("Subway_p", "Start_Point_subway", "sp04_intro", 22.7),
    ("mall_p", "LevelStart", "sp05_intro", 16.7),
    ("factory_p", "Start_point", "sp06_intro", 10.0),
    ("boat_p", "Start", "sp07_intro", 18.1),
    ("convoy_p", "Kates_convoy", "sp08_intro", 3.3),
    ("Scraper_p", "Scraper_Start", "sp09_intro", 12.7),
]


def out_dir():
    return paths.ensure_dir(paths.build_dir("intros"))


def save_file():
    d = paths.medge_user_config_dir(required=True)
    return os.path.join(os.path.dirname(d), "Savefiles")


def digest_dir(d):
    h = hashlib.sha1()
    for name in sorted(os.listdir(d)):
        p = os.path.join(d, name)
        if os.path.isfile(p) and ".bak" not in name:
            h.update(name.encode())
            with open(p, "rb") as f:
                h.update(f.read())
    return h.hexdigest()


def intro_playing(sample, anim):
    return any(anim == n.lower() for n, _t, _w in (sample.anim1p or []))


def record(map_name, checkpoint, anim, length, after, frames_every, margin):
    """Boot one chapter and record until `after` seconds past the end of its intro."""
    if drive.is_running():
        drive.kill()
        time.sleep(2)
    url = "%s?LoadCheckpoint=%s" % (map_name, checkpoint)
    menu_capture._set_startup_map((url, url))

    reader = telemetry.TelemetryReader()
    stale = reader.header()
    stale_session = stale["session_qpc"] if stale else None

    path = os.path.join(out_dir(), "%s.jsonl" % map_name.lower())
    frame_dir = os.path.join(out_dir(), "%s_frames" % map_name.lower())
    t_launch = time.time()
    drive.launch()

    deadline = time.time() + 120
    while time.time() < deadline:
        h = reader.header()
        if h and h["session_qpc"] != stale_session:
            break
        time.sleep(0.2)
    else:
        raise RuntimeError("the hook never started a new telemetry session for %s" % map_name)

    n_samples = n_sounds = dropped = 0
    first_pawn = first_intro = last_intro = None      # hook-clock times
    intro_clip_max = 0.0
    next_frame_at = None
    n_frames = 0
    reason = "timeout"
    hard_stop = time.time() + margin + length + after + 180
    with open(path, "w", buffering=1) as f:
        f.write(json.dumps({"type": "meta", "trial": "intro", "map": map_name, "url": url, "anim": anim,
                            "started": time.strftime("%Y-%m-%d %H:%M:%S")}) + "\n")
        while time.time() < hard_stop:
            samples, keys, drop = reader.drain()
            sounds, sdrop = reader.drain_sounds()
            dropped += drop + sdrop
            for s in samples:
                f.write(json.dumps(tracefile.sample_json(s)) + "\n")
                if s.pawn_valid and first_pawn is None:
                    first_pawn = s.t
                if intro_playing(s, anim):
                    if first_intro is None:
                        first_intro = s.t
                    last_intro = s.t
                    intro_clip_max = max([intro_clip_max] + [t for n, t, _w in s.anim1p if n.lower() == anim])
            for e in keys:
                f.write(json.dumps({"type": "key", "frame": e.frame, "t": round(e.t, 6), "vk": e.vk, "name": e.name,
                                    "down": e.down, "repeat": e.repeat, "blocked": e.blocked}) + "\n")
            for e in sounds:
                f.write(json.dumps(tracefile.sound_json(e)) + "\n")
            n_samples += len(samples)
            n_sounds += len(sounds)
            now = samples[-1].t if samples else None

            if frames_every > 0 and first_pawn is not None and now is not None:
                if next_frame_at is None or now >= next_frame_at:
                    next_frame_at = now + frames_every
                    if drive.grab_frame(os.path.join(frame_dir, "f_%03d_t%07.2f.png" % (n_frames, now)), timeout=2.0):
                        n_frames += 1

            # The hook's animation record does not list an animation played on the Custom_Canned slot,
            # which is how the intros play, so the end is normally told by the clock: the Matinee's
            # length after the pawn appears, plus a margin for a slow first second.
            if now is not None and first_intro is not None and now - last_intro >= after:
                reason = "intro animation ended"
                break
            if now is not None and first_pawn is not None and first_intro is None and \
                    now - first_pawn >= length + after + margin:
                reason = "%.0f s after the pawn appeared" % (length + after + margin)
                break
            if not drive.is_running():
                reason = "game exited"
                break
            time.sleep(0.1)
    drive.kill()
    reader.close()
    return {"map": map_name, "url": url, "trace": path, "samples": n_samples, "sounds": n_sounds, "dropped": dropped,
            "boot_s": None if first_pawn is None else round(first_pawn, 1),
            "intro_from_pawn_s": None if first_intro is None or first_pawn is None else round(first_intro - first_pawn, 2),
            "intro_wall_s": None if first_intro is None else round(last_intro - first_intro, 2),
            "intro_clip_s": round(intro_clip_max, 2), "frames": n_frames, "stopped": reason,
            "wall_s": round(time.time() - t_launch, 1)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", default="", help="comma-separated map names (default: all)")
    ap.add_argument("--after", type=float, default=5.0, help="seconds to keep recording after the intro ends")
    ap.add_argument("--frames", type=float, default=0.0, help="save the back buffer every this many seconds (0 = never)")
    ap.add_argument("--margin", type=float, default=6.0,
                    help="extra seconds on top of the Matinee's length before a recording stops")
    a = ap.parse_args(argv)
    only = {m.strip().lower() for m in a.only.split(",") if m.strip()}
    levels = [l for l in LEVELS if not only or l[0].lower() in only]

    saves = save_file()
    backup = os.path.join(out_dir(), "savefiles_before")
    if os.path.isdir(saves):
        if os.path.isdir(backup):
            shutil.rmtree(backup)
        shutil.copytree(saves, backup)
        before = digest_dir(saves)

    was_running = drive.is_running()
    previous = menu_capture._set_startup_map(("TdMainMenu", "TdMainMenu"))
    print("startup map was %r%s" % (previous, "; a running game will be closed" if was_running else ""), flush=True)
    results = []
    try:
        for map_name, checkpoint, anim, length in levels:
            print("%s ..." % map_name, flush=True)
            try:
                r = record(map_name, checkpoint, anim, length, a.after, a.frames, a.margin)
            except Exception as e:  # keep going: one chapter failing should not lose the others
                drive.kill()
                r = {"map": map_name, "error": str(e)}
            results.append(r)
            print("  " + json.dumps(r), flush=True)
    finally:
        drive.kill()
        menu_capture._set_startup_map(previous)
        print("startup map restored", flush=True)
        if os.path.isdir(saves):
            after = digest_dir(saves)
            print("save files %s" % ("unchanged" if after == before else "CHANGED: the copy from before is in %s" % backup),
                  flush=True)
    with open(os.path.join(out_dir(), "summary.json"), "w") as f:
        json.dump(results, f, indent=1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
