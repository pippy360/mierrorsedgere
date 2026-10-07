"""Where a retail recording goes, and what one record looks like.

Copied from tesseract's tools/medge/trials.py (trace_dir, trace_path,
sample_json, sound_json), which the rest of that module - scripted trials
that drive the game with injected input - is not needed for.
"""

import os
import time

from . import paths


def trace_dir():
    return paths.ensure_dir(paths.build_dir("trials"))


def trace_path(name):
    stamp = time.strftime("%Y%m%d_%H%M%S", time.gmtime())
    return os.path.join(trace_dir(), "%s_%s.jsonl" % (stamp, name))


def sample_json(s):
    """One telemetry sample as a trace record.

    x/y/z is the CAMERA, kept first and unchanged so every existing miner
    still reads these traces. Everything from `move` on is the pawn, read out
    of the game's own object table (doc/medge_sdk.md), and is absent from
    traces recorded before that existed - so read it with .get(), never [].
    It is worth having: `move` is the game's own answer to which move Faith is
    in, where every earlier trace left that to be reconstructed from z-curves,
    and px/py/pz and vx/vy/vz carry no camera spring, which is what made the
    old camera-derived gravity read 4.6% high.
    """
    rec = {
        "type": "sample", "frame": s.frame, "t": round(s.t, 6),
        "x": round(s.x, 3), "y": round(s.y, 3), "z": round(s.z, 3),
        "yaw": round(s.yaw, 3), "pitch": round(s.pitch, 3),
        "roll": round(s.roll, 3),
        "valid": s.cam_valid, "freecam": s.freecam, "noclip": s.noclip,
    }
    if s.pawn_valid:
        rec.update({
            "move": s.move, "move_name": s.move_name,
            "old_move": s.old_move, "walking": s.walking,
            "physics": s.physics,
            "px": round(s.px, 3), "py": round(s.py, 3), "pz": round(s.pz, 3),
            "vx": round(s.vx, 3), "vy": round(s.vy, 3), "vz": round(s.vz, 3),
        })
    if s.gravity_z is not None:
        rec["gravity_z"] = round(s.gravity_z, 2)
    if getattr(s, "rot_valid", False):
        rec["cyaw"] = round(s.cyaw, 3)
        rec["cpitch"] = round(s.cpitch, 3)
        rec["pyaw"] = round(s.pyaw, 3)
    # Which animation (telemetry v6): the heaviest AnimNodeSequence leaves of
    # the first- and third-person bodies, [name, clip time, weight] each,
    # heaviest first. Absent from traces before 2026-09-26.
    if getattr(s, "anim_valid", False):
        rec["anim1p"] = [[n, t, w] for n, t, w in s.anim1p]
        rec["anim3p"] = [[n, t, w] for n, t, w in s.anim3p]
    return rec


def sound_json(e):
    """One sound event as a trace record (telemetry v6): a SoundCue starting
    or stopping. `pawn` is the game's own answer to "hers?" - the component's
    Owner is the player's pawn - which the breathing and foley cues carry and
    the material footstep cues (played through the sound groups) do not."""
    return {"type": "sound", "frame": e.frame, "t": round(e.t, 6),
            "cue": e.cue, "start": e.start, "pawn": e.pawn,
            "x": round(e.x, 1), "y": round(e.y, 1), "z": round(e.z, 1),
            "dur": round(e.dur, 3)}
