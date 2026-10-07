"""Frame-rate camera telemetry and input events from the d3d9 proxy.

The hook publishes two ring buffers in a named shared-memory section (see
"telemetry ring buffers" in tools/retail/hook/d3d9_proxy.cpp):

  * one camera record per PRESENTED frame - frame number, QPC timestamp,
    position (uu), yaw/pitch (degrees), validity flags;
  * one input record per key or mouse-button message the game window received -
    injected and human input both, stamped with the frame it landed on.

This is the measurement channel the state-file protocol cannot be: the .state
file is rewritten every 2nd frame with no timestamp and no history, so polling
it cannot tell a missed frame from a duplicate. Reading the ring gives every
frame exactly once, with real timestamps, and a reader that falls behind
DETECTS the gap from the published count instead of silently losing samples.

The section is created by whichever side maps it first. Mapping it here before
the game runs is fine - the header is zero, ``wait_ready`` blocks until the
hook initialises it (magic is written last), and a game restart is visible as
a changed ``sessionQpc``, which resets the read cursors.

Nothing here needs the game focused, and reading is passive: it never injects
input or writes game state.
"""

import math
import mmap
import struct
import time
from dataclasses import dataclass

MAP_NAME = "medge_telemetry_v6"
MAGIC = 0x4D45544C
VERSION = 6
TELEM_CAPACITY = 65536          # 18 min at 60 fps; see the hook for why
INPUT_CAPACITY = 2048
SOUND_CAPACITY = 4096           # v6: sound start/stop events
ANIM_TOP = 3                    # v6: sequences per mesh in a record

# magic, version, caps, qpcFreq, sessionQpc, telem/input counts, sound cap
# and count (v6, in what were reserved words), 16 reserved.
_HEADER_FMT = "<4I2Q2lIl16x"
_HEADER_SIZE = struct.calcsize(_HEADER_FMT)
# frame, flags, qpc, x y z yaw pitch roll, 4 move/walk bytes, vel, pawn pos,
# gravityZ, the look (controller yaw/pitch, pawn yaw; v5), physics + 3 pad,
# then (v6) six AnimSlots: name[28], time, weight - three for the
# first-person mesh, three for the third-person one, heaviest first.
# v2 added everything from the move byte onward;
# v3 added roll.
_ANIM_FMT = "28sff"
_TELEM_FMT = "<2IQ6f4B10f4B" + _ANIM_FMT * (2 * ANIM_TOP)
_TELEM_SIZE = struct.calcsize(_TELEM_FMT)
_INPUT_FMT = "<4IQ2I"               # frame, vk, down, flags, qpc, scan, pad
_INPUT_SIZE = struct.calcsize(_INPUT_FMT)
_SOUND_FMT = "<2IQ40s4f"            # frame, flags, qpc, cue, x y z, dur
_SOUND_SIZE = struct.calcsize(_SOUND_FMT)

assert _HEADER_SIZE == 64 and _TELEM_SIZE == 304 and _INPUT_SIZE == 32 \
    and _SOUND_SIZE == 72, "layout drifted from d3d9_proxy.cpp"

TOTAL_SIZE = (_HEADER_SIZE + TELEM_CAPACITY * _TELEM_SIZE
              + INPUT_CAPACITY * _INPUT_SIZE + SOUND_CAPACITY * _SOUND_SIZE)
_INPUT_BASE = _HEADER_SIZE + TELEM_CAPACITY * _TELEM_SIZE
_SOUND_BASE = _INPUT_BASE + INPUT_CAPACITY * _INPUT_SIZE

# Virtual-key names for the handful of keys traces actually contain.
VK_NAMES = {
    0x01: "lmb", 0x02: "rmb", 0x04: "mmb", 0x05: "xmb1",
    0x08: "backspace", 0x09: "tab", 0x0D: "enter", 0x10: "shift",
    0x11: "ctrl", 0x12: "alt", 0x1B: "escape", 0x20: "space",
    0x25: "left", 0x26: "up", 0x27: "right", 0x28: "down",
    0xA0: "lshift", 0xA1: "rshift", 0xA2: "lctrl", 0xA3: "rctrl",
}


@dataclass
class Sample:
    frame: int
    t: float          # seconds since the hook session started
    x: float
    y: float
    z: float
    yaw: float        # degrees, same convention as drive.camera_position()
    pitch: float
    # Roll (v3). Positive tips the camera's up axis toward its own right, which
    # is how the port signs its wall-run lean - see rollFromBasis in the hook.
    # Absent from every trace recorded before 2026-08-16, where it reads 0.0.
    roll: float
    cam_valid: bool
    freecam: bool
    noclip: bool
    # From the game's own object table (v2). `pawn_valid` false during menus,
    # loads and cutscenes, when there is no pawn at all - the move fields read
    # None then, so a gap cannot be mistaken for MOVE_None.
    pawn_valid: bool = False
    move: int = None          # EMovement; .move_name for the string
    old_move: int = None
    pending_move: int = None
    walking: int = None       # EWalkingState: idle/sneak/walk/jog/run/sprint
    physics: int = None       # EPhysics; 12 and 13 are the custom wall modes
    vx: float = None          # pawn velocity, uu/s - free of the camera spring
    vy: float = None
    vz: float = None
    px: float = None          # pawn Location, uu - the feet, not the eye
    py: float = None
    pz: float = None
    gravity_z: float = None   # WorldInfo::WorldGravityZ, live
    # The look (v5), from the object table: the controller's Rotation is what
    # the mouse writes, the pawn's yaw is the body's. None when the hook could
    # not read them, and absent from every trace before 2026-09-10.
    rot_valid: bool = False
    cyaw: float = None        # controller yaw, degrees, UE heading
    cpitch: float = None      # controller pitch, degrees, up positive
    pyaw: float = None        # pawn yaw, degrees
    # Which animation (v6), read off the pawn's two SkeletalMeshComponents'
    # AnimTrees: the up-to-three heaviest AnimNodeSequence leaves of each,
    # as (sequence name, clip time in s, blend weight 0..1), heaviest first.
    # The first-person list is the body the camera is attached to and the one
    # the port's medgebodyanim stands in for. Empty before v6 and whenever
    # the hook could not walk the tree (flag 64 clear).
    anim_valid: bool = False
    anim1p: list = None
    anim3p: list = None

    @property
    def move_name(self):
        from tools.retail.movenames import name
        return name(self.move)

    @property
    def speed2d(self):
        """Horizontal pawn speed, uu/s. None without a pawn."""
        if self.vx is None:
            return None
        return math.hypot(self.vx, self.vy)


@dataclass
class SoundEvent:
    """A sound starting or stopping (v6): an AudioComponent appearing in or
    leaving the AudioDevice's active list. `cue` is the SoundCue's object
    name; `pawn` says the component's Owner is the player's pawn (her own
    footsteps, foley, breathing); x/y/z the component's Location in uu, and
    `dur` how long it had played, on a stop."""
    frame: int
    t: float
    cue: str
    start: bool
    pawn: bool
    x: float
    y: float
    z: float
    dur: float


@dataclass
class KeyEvent:
    frame: int
    t: float
    vk: int
    down: bool
    repeat: bool      # auto-repeat press of a key already held
    blocked: bool     # swallowed by the noclip input filter
    extended: bool
    scan: int

    @property
    def name(self):
        n = VK_NAMES.get(self.vk)
        if n:
            return n
        if 0x30 <= self.vk <= 0x5A:          # digits and letters
            return chr(self.vk).lower()
        return "vk%02X" % self.vk


class TelemetryReader:
    """Drain-style reader over the hook's shared-memory rings."""

    def __init__(self):
        self._mm = mmap.mmap(-1, TOTAL_SIZE, tagname=MAP_NAME)
        self._session = None
        self._telem_read = 0
        self._input_read = 0
        self._sound_read = 0
        self._freq = None

    def close(self):
        self._mm.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def header(self):
        """Parsed header dict, or None while the hook has not initialised it."""
        (magic, version, tcap, icap, freq, session,
         tcount, icount, scap, scount) = struct.unpack_from(_HEADER_FMT, self._mm, 0)
        if magic != MAGIC or version != VERSION:
            return None
        if tcap != TELEM_CAPACITY or icap != INPUT_CAPACITY or freq == 0:
            return None
        if scap != SOUND_CAPACITY:
            return None
        return {"qpc_freq": freq, "session_qpc": session,
                "telem_count": tcount & 0xFFFFFFFF,
                "input_count": icount & 0xFFFFFFFF,
                "sound_count": scount & 0xFFFFFFFF}

    def wait_ready(self, timeout=10.0):
        """Block until the hook has initialised the section; raises on timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            h = self.header()
            if h:
                return h
            time.sleep(0.1)
        raise RuntimeError(
            "telemetry section %r never became valid - is the game running "
            "with the current hook installed?" % MAP_NAME)

    def _resync(self, h):
        """A new hook session restarts the counts; drop our cursors with it."""
        if self._session != h["session_qpc"]:
            self._session = h["session_qpc"]
            self._freq = h["qpc_freq"]
            self._telem_read = 0
            self._input_read = 0
            self._sound_read = 0

    def _t(self, qpc):
        return (qpc - self._session) / float(self._freq)

    def drain(self):
        """All records published since the last drain.

        Returns (samples, key_events, dropped) where dropped counts records
        that were overwritten before we read them - zero unless the poll
        interval exceeded the ring (~1 minute of frames).
        """
        h = self.header()
        if not h:
            return [], [], 0
        self._resync(h)

        samples, dropped = self._drain_ring(
            h["telem_count"], self._telem_read, TELEM_CAPACITY,
            _HEADER_SIZE, _TELEM_FMT, _TELEM_SIZE, self._parse_telem)
        self._telem_read = h["telem_count"]

        events, edropped = self._drain_ring(
            h["input_count"], self._input_read, INPUT_CAPACITY,
            _INPUT_BASE, _INPUT_FMT, _INPUT_SIZE, self._parse_input)
        self._input_read = h["input_count"]

        return samples, events, dropped + edropped

    def drain_sounds(self):
        """All sound events published since the last call: (events, dropped).

        Separate from drain() so its callers keep their three-tuple; a
        recorder calls both.
        """
        h = self.header()
        if not h:
            return [], 0
        self._resync(h)
        sounds, dropped = self._drain_ring(
            h["sound_count"], self._sound_read, SOUND_CAPACITY,
            _SOUND_BASE, _SOUND_FMT, _SOUND_SIZE, self._parse_sound)
        self._sound_read = h["sound_count"]
        return sounds, dropped

    def _drain_ring(self, count, read, capacity, base, fmt, recsize, parse):
        new = count - read
        if new <= 0:
            return [], 0
        dropped = 0
        if new > capacity:
            # The writer lapped us; the oldest records are gone. Also leave one
            # slot of headroom: the slot at count % capacity may be mid-write
            # by the time we finish reading.
            dropped = new - (capacity - 1)
            read = count - (capacity - 1)
        out = []
        for n in range(read, count):
            off = base + (n % capacity) * recsize
            out.append(parse(struct.unpack_from(fmt, self._mm, off)))
        return out, dropped

    @staticmethod
    def _anims(fields):
        out = []
        for i in range(0, len(fields), 3):
            name = fields[i].split(b"\0", 1)[0].decode("ascii", "replace")
            if name:
                out.append((name, round(fields[i + 1], 4), round(fields[i + 2], 4)))
        return out

    def _parse_sound(self, rec):
        frame, flags, qpc, cue, x, y, z, dur = rec
        return SoundEvent(frame=frame, t=self._t(qpc),
                          cue=cue.split(b"\0", 1)[0].decode("ascii", "replace"),
                          start=bool(flags & 1), pawn=bool(flags & 4),
                          x=x, y=y, z=z, dur=dur)

    def _parse_telem(self, rec):
        (frame, flags, qpc, x, y, z, yaw, pitch, roll,
         mv, oldmv, pendmv, walk,
         vx, vy, vz, px, py, pz, grav, cyaw, cpitch, pyaw,
         phys, _p0, _p1, _p2) = rec[:27]
        anims = rec[27:]
        pawn = bool(flags & 8)
        world = bool(flags & 16)
        rot = bool(flags & 32)
        anim = bool(flags & 64)
        return Sample(frame=frame, t=self._t(qpc), x=x, y=y, z=z,
                      rot_valid=rot,
                      anim_valid=anim,
                      anim1p=self._anims(anims[:3 * ANIM_TOP]) if anim else [],
                      anim3p=self._anims(anims[3 * ANIM_TOP:]) if anim else [],
                      cyaw=cyaw if rot else None, cpitch=cpitch if rot else None,
                      pyaw=pyaw if rot else None,
                      yaw=yaw, pitch=pitch, roll=roll,
                      cam_valid=bool(flags & 1), freecam=bool(flags & 2),
                      noclip=bool(flags & 4),
                      pawn_valid=pawn,
                      move=mv if pawn else None,
                      old_move=oldmv if pawn else None,
                      pending_move=pendmv if pawn else None,
                      walking=walk if pawn else None,
                      physics=phys if pawn else None,
                      vx=vx if pawn else None, vy=vy if pawn else None,
                      vz=vz if pawn else None,
                      px=px if pawn else None, py=py if pawn else None,
                      pz=pz if pawn else None,
                      gravity_z=grav if world else None)

    def _parse_input(self, rec):
        frame, vk, down, flags, qpc, scan, _pad = rec
        return KeyEvent(frame=frame, t=self._t(qpc), vk=vk, down=bool(down),
                        repeat=bool(flags & 1), blocked=bool(flags & 2),
                        extended=bool(flags & 4), scan=scan)

    def record(self, seconds, poll=0.25, stop=None):
        """Collect for a duration (or until ``stop(samples)`` returns True)."""
        samples, events, dropped = [], [], 0
        deadline = time.time() + seconds
        while time.time() < deadline:
            time.sleep(poll)
            s, e, d = self.drain()
            samples += s
            events += e
            dropped += d
            if stop and stop(samples):
                break
        return samples, events, dropped

    def fps(self, window=2.0):
        """Measured present rate over a short window."""
        h = self.wait_ready()
        self._resync(h)
        n0, t0 = h["telem_count"], time.time()
        time.sleep(window)
        h = self.header()
        if not h:
            return None
        return (h["telem_count"] - n0) / (time.time() - t0)


# --- analysis ------------------------------------------------------------------

def _median(vals):
    s = sorted(vals)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def jump_metrics(samples, rise_eps=3.0):
    """Gravity and launch speed from one standing jump's camera-height series.

    Uses the assumption-free estimator from doc/medge_movement.md section 2:
    only the apex height above standing and the total airtime are read off the
    series, then g = 8*apex/airtime^2 and v0 = g*airtime/2. The camera spring
    adds ~7 uu at the apex (the camera lags the pawn), which is why measured g
    lands a few percent below the true value; four-trial averages were what
    established -1600.

    ``samples`` must span quiet standing, the jump, and the landing. Returns a
    dict, or None with a reason when the series does not contain a clean jump.
    """
    good = [s for s in samples if s.cam_valid]
    if len(good) < 20:
        return {"ok": False, "reason": "too few valid samples (%d)" % len(good)}

    # Baseline from the pre-jump stretch: everything before the first rise.
    z0 = _median([s.z for s in good[:10]])
    above = [s.z > z0 + rise_eps for s in good]

    takeoff = None
    for i in range(len(good) - 3):
        if above[i] and above[i + 1] and above[i + 2]:
            takeoff = i
            break
    if takeoff is None:
        return {"ok": False, "reason": "no rise above baseline %.1f" % z0}
    if takeoff < 5:
        return {"ok": False, "reason": "jump began before a baseline settled"}

    apex_i = max(range(takeoff, len(good)), key=lambda i: good[i].z)
    apex = good[apex_i].z - z0

    landing = None
    for i in range(apex_i, len(good) - 3):
        if not above[i] and not above[i + 1] and not above[i + 2]:
            landing = i
            break
    if landing is None:
        # The sampler missing the landing was exactly what inflated two of the
        # six original gravity trials to g ~ 2500; refuse rather than skew.
        return {"ok": False, "reason": "landing never observed"}

    airtime = good[landing].t - good[takeoff].t
    if airtime < 0.3 or airtime > 2.0:
        return {"ok": False, "reason": "implausible airtime %.3f s" % airtime}

    g = 8.0 * apex / (airtime * airtime)
    v0 = g * airtime / 2.0
    dts = [good[i + 1].t - good[i].t for i in range(takeoff, landing)]
    return {"ok": True, "baseline_z": z0, "apex_uu": apex,
            "airtime_s": airtime, "g": g, "v0": v0,
            "takeoff_t": good[takeoff].t, "landing_t": good[landing].t,
            "samples_airborne": landing - takeoff,
            "mean_dt_ms": 1000.0 * sum(dts) / max(1, len(dts))}


def horizontal_speed(samples, window=5):
    """Per-frame horizontal speed (uu/s) from differentiated camera positions.

    The camera bobs and springs around the pawn, so raw frame-to-frame deltas
    are noisy; a short boxcar over ``window`` frames settles them without
    hiding the momentum curve, which evolves over tens of frames.

    Returns [(t, speed), ...] aligned to the trailing sample of each window.
    """
    good = [s for s in samples if s.cam_valid]
    out = []
    for i in range(window, len(good)):
        a, b = good[i - window], good[i]
        dt = b.t - a.t
        if dt <= 0:
            continue
        dx = b.x - a.x
        dy = b.y - a.y
        out.append((b.t, ((dx * dx + dy * dy) ** 0.5) / dt))
    return out


if __name__ == "__main__":
    r = TelemetryReader()
    h = r.wait_ready()
    print("session qpc=%d freq=%d telem=%d input=%d"
          % (h["session_qpc"], h["qpc_freq"],
             h["telem_count"], h["input_count"]))
    print("present rate: %.1f fps" % r.fps())
    s, e, d = r.drain()
    if s:
        last = s[-1]
        print("camera: (%.1f, %.1f, %.1f) yaw=%.1f pitch=%.1f roll=%.1f valid=%s"
              % (last.x, last.y, last.z, last.yaw, last.pitch, last.roll,
                 last.cam_valid))
    print("drained %d samples, %d key events, %d dropped" % (len(s), len(e), d))
