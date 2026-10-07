"""Record a human playing, open-ended, streaming to disk.

    python -m tools.retail.record_session --name freeplay
    python -m tools.retail.record_session --name freeplay --map escape_p
    python -m tools.retail.record_session --name freeplay --no-boot   # already running

Physical input is NEVER touched here - the human playing IS the input source,
so nothing is blocked, no keys are injected and focus is not stolen after the
boot. (tesseract's trials.run_route, by contrast, blocks input for the duration.)

Two differences from tesseract's trials.record_human, both because a person does not know
in advance how long they will play:

  * it runs until the game exits or Ctrl-C, rather than for a fixed duration;
  * it appends and flushes as it goes, so a session that ends unexpectedly is
    still on disk up to the last second.

Draining matters. The hook's ring holds 8192 frames - about 2 min 16 s at 60
fps, half that at 120 - and then overwrites. This polls several times a second
and reports `dropped` if it ever falls behind, so a gap is visible in the trace
rather than silently absent.

What lands in the file is one meta line then one JSON record per frame, in
tracefile.sample_json format: the camera, and - new - the pawn's own move state,
velocity and position straight out of the game's object table, plus every key
event the game received.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.retail import drive, paths, telemetry, tracefile  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="freeplay")
    ap.add_argument("--map", default="edge_p",
                    help="edge_p gives free movement; the Training Area gates "
                         "input and is a poor place to record movement")
    ap.add_argument("--no-boot", action="store_true",
                    help="attach to a game that is already running")
    ap.add_argument("--max-minutes", type=float, default=90.0)
    a = ap.parse_args(argv)

    if not a.no_boot:
        r = drive.boot_to_map(a.map, timeout=300)
        print("boot: %s in %.1f s" % (r["state"], r["seconds"]), flush=True)
        time.sleep(1.5)

    reader = telemetry.TelemetryReader()
    reader.wait_ready()
    reader.drain()                       # discard the boot
    reader.drain_sounds()

    path = tracefile.trace_path(a.name)
    paths.ensure_dir(os.path.dirname(path))
    n_s = n_e = n_snd = dropped = 0
    t0 = time.time()
    deadline = t0 + a.max_minutes*60.0
    last_report = 0.0
    moves_seen = {}

    print("\nRECORDING -> %s" % path)
    print("play as long as you like; Ctrl-C here when you are done "
          "(or just quit the game)\n", flush=True)

    with open(path, "w", buffering=1) as f:
        f.write(json.dumps({
            "type": "meta", "trial": "human_session", "map": a.map,
            "started": time.strftime("%Y-%m-%d %H:%M:%S")}) + "\n")
        try:
            while time.time() < deadline:
                samples, events, drop = reader.drain()
                dropped += drop
                for s in samples:
                    f.write(json.dumps(tracefile.sample_json(s)) + "\n")
                    if s.pawn_valid:
                        moves_seen[s.move_name] = moves_seen.get(s.move_name, 0) + 1
                for e in events:
                    f.write(json.dumps({
                        "type": "key", "frame": e.frame, "t": round(e.t, 6),
                        "vk": e.vk, "name": e.name, "down": e.down,
                        "repeat": e.repeat, "blocked": e.blocked}) + "\n")
                sounds, sdrop = reader.drain_sounds()
                dropped += sdrop
                for e in sounds:
                    f.write(json.dumps(tracefile.sound_json(e)) + "\n")
                n_s += len(samples)
                n_e += len(events)
                n_snd += len(sounds)

                el = time.time() - t0
                if el - last_report >= 15.0:
                    last_report = el
                    live = samples[-1] if samples else None
                    where = ""
                    if live is not None and live.pawn_valid:
                        where = "  %-22s at (%.0f %.0f %.0f) %.0f uu/s" % (
                            live.move_name, live.px, live.py, live.pz,
                            live.speed2d)
                    anim = ""
                    if live is not None and getattr(live, "anim_valid", False) and live.anim1p:
                        anim = "  %s" % live.anim1p[0][0]
                    print("  %5.0f s  %6d samples, %4d keys, %4d sounds%s%s%s"
                          % (el, n_s, n_e, n_snd, where, anim,
                             "  DROPPED %d" % dropped if dropped else ""),
                          flush=True)
                    # The game going away is the normal way this ends.
                    if not drive.is_running():
                        print("  game exited", flush=True)
                        break
                time.sleep(0.2)
        except KeyboardInterrupt:
            print("\n  stopped by hand", flush=True)

    print("\nsaved %d samples, %d key events, %d sound events%s"
          % (n_s, n_e, n_snd, ", %d DROPPED" % dropped if dropped else ""))
    if moves_seen:
        top = sorted(moves_seen.items(), key=lambda kv: -kv[1])
        print("moves recorded: " + ", ".join("%s x%d" % kv for kv in top[:12]))
    print("-> %s" % path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
