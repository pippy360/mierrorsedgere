"""Reading a retail recording: the engine-neutral half of a replay.

Copied from tesseract's tools/medge/replay.py, where it was worked out against
eight recorded human runs; the comments are that file's and keep their
references to it and to its docs (doc/medge_port_parity.md and friends in the
tesseract repo). What is here knows nothing about either engine:

  * load_trace: the samples and key events, a key held from before the
    recorder attached, noclip's phantom releases, a paused stretch cut out;
  * stamp_look: where the view comes from (the controller's rotation, the
    camera, or the pawn's velocity when the recording froze its camera);
  * frame_start_keys: each key moved to the recorded frame that took it;
  * retail_ground_state: the frame state retail's PlayerMove carries into a
    frame, rebuilt by retail's own rules (none of it is recorded);
  * retail_at / moves_in: retail's feet at a time, and its moves in a window.

Positions are UE units throughout. Retail's pawn Location is the capsule
CENTRE, centre_above_feet() over the floor.
"""
import bisect
import gzip
import json
import math

TICK_MS = 16
# Retail's pawn Location is the capsule centre. CollisionHeight is 90 (a
# half-height, units.py) and the pawn floats a few uu above the floor: at the
# escape_p spawn the centre reads 12069.15 over a roof whose top converts to
# 11976, so 93. The anchor is lifted a little on top and settled onto OUR
# floor, so a unit either way costs nothing.
CENTRE_ABOVE_FEET = 93.0
# ...except on the SHORT capsule: a move with bUseCustomCollision runs on a
# 122 uu capsule with the feet fixed (TdMove.ShrinkCollision), so its centre
# stands 29 uu lower - every recorded slide steps pz down exactly 29.00. Read
# with 93, a slide's feet came out 29 uu under the floor, and a re-anchor
# dropped inside one started the port in the ground.
SHORT_CAPSULE_MOVES = frozenset(("MOVE_Slide", "MOVE_Crouch", "MOVE_180TurnInAir",
                                 "MOVE_LayOnGround", "MOVE_Coil", "MOVE_AirBarge",
                                 "MOVE_MeleeCrouch", "MOVE_MeleeSlide"))
SHORT_CAPSULE_DROP_UU = 29.0


def centre_above_feet(sample):
    """How far over the floor retail's pawn centre stands in this sample."""
    if sample.get("move_name") in SHORT_CAPSULE_MOVES:
        return CENTRE_ABOVE_FEET - SHORT_CAPSULE_DROP_UU
    return CENTRE_ABOVE_FEET


ANCHOR_LIFT_UU = 3.0
DIVERGE_UU = 50.0           # the gap that counts as "parted"
FELL_UU = 300.0             # port below retail by this much = fell through

# The left button is the attack - TdPlayerController.AttackPress, which
# barges a door in front of her (doc/medge_doors.md) - and, like Q, a press
# and not a hold: the engine edge-triggers it.
KEYS = {0x57: "w", 0x41: "a", 0x53: "s", 0x44: "d",
        0x20: "space", 0xA0: "lshift", 0x10: "lshift", 0x51: "q", 0x01: "lmb"}
PULSES = ("q", "lmb")
KEY_MOVE = {"w": 1, "s": -1}
KEY_STRAFE = {"a": 1, "d": -1}
UE_PHYS_WALKING = 1
UE_PHYS_FALLING = 2

# The moves whose controller state is PlayerWalking, whose PlayerMove calls
# GetWalkAcceleration / GetSprintAcceleration and so writes SpeedSprintEnergy
# and AccelerationTime (TdPlayerController.PlayerWalking; Crouch, Slide and
# Landing have no ControllerState of their own and stay in it). Every other
# move - the roll and the grabs (PlayerGrabbing), the wall moves
# (PlayerWallWalking), the uncontrolled fall (PlayerDying) - leaves both as
# they were.
WALKING_MOVES = {"MOVE_Walking", "MOVE_Jump", "MOVE_Falling", "MOVE_Landing",
                 "MOVE_Crouch", "MOVE_Slide", "MOVE_VaultOver", "MOVE_SpeedVault",
                 "MOVE_StepUp", "MOVE_AutoStepUp", "MOVE_SpringBoarding",
                 "MOVE_SoftLanding",
                 # TdMove_Barge has no ControllerState: PlayerWalking, keys live
                 "MOVE_Barge"}
VAULT_MOVES = {"MOVE_VaultOver", "MOVE_SpeedVault", "MOVE_StepUp", "MOVE_AutoStepUp"}
# ...of which these ignore the keys for the whole move (DisableMovementTime -1
# on Default__TdMove_*: TdPawn.SetIgnoreMoveInput zeroes aForward/aStrafe in
# PlayerInput), so their frames take PlayerMove's no-key path. So does a
# recorded MOVE_Landing: TdMove_Landing.LandHard @133 (and LandOnSoftObject
# @51) calls SetIgnoreMoveInput(-1), lifted only by TdMove.StopMove @20's
# StopIgnoreMoveInput when OnCustomAnimEnd @0 sets MOVE_Walking - a normal
# landing ends inside StartMove (LandNormal -> EndLanding) and never shows as
# a MOVE_Landing frame. The edge_pt1 run's first frame after its 2 s
# LandHard goes 0 -> 46.7 uu/s, AccelWalk 7 x 400 x dt: energy 0, where W
# held through the stumble had left 299.
NOKEY_MOVES = VAULT_MOVES | {"MOVE_Slide", "MOVE_SpringBoarding", "MOVE_SoftLanding", "MOVE_Landing"}
STOP_TAP_S = 0.15       # AccelerationTime under this at a release is a tap (@908)
STOP_TIME_S = 0.25      # SetTimer(0.25, 'PlayStop') (@948)
STOP_VEL_UU = 35.0      # StoppingVelocity (@967)


# ---------------------------------------------------------------------------
# The trace

def pawn_sample(d):
    """A sample record that is the pawn: live, read, and not the free camera."""
    return d.get("px") is not None and d.get("valid", True) and not d.get("freecam")


def open_trace(path):
    """A recording as text: plain .jsonl, or .jsonl.gz as recordings/ keeps them."""
    if path.endswith(".gz"):
        return gzip.open(path, "rt", errors="replace")
    return open(path, errors="replace")


def load_trace(path, game_clock=True):
    """(meta, samples, keys). The sound events a v6 trace carries are kept on
    the meta as meta["sounds"] - a list of the raw records, in time order -
    so the replay's callers keep their three-tuple.

    Everything comes back on the GAME's clock: a span where retail was paused
    is cut out (excise_pauses, "A paused recording") and meta["pauses"] says
    where. game_clock=False keeps the recorder's wall clock, pause and all."""
    meta = None
    samples = []
    keys = []
    sounds = []
    # A key held since BEFORE the recording began never has a down edge in
    # it - only the autorepeats the window receives while it is held. The run
    # of 2026-09-25 opens running backward on an `s` whose first event is a
    # repeat and whose release comes 0.83 s in; dropping repeats replayed that
    # stretch with nothing held, and the port braked where retail ran on.
    # A key's LEADING repeats (before any real event of that key) are turned
    # into a down edge just before the first real key-down, so the first
    # anchor holds it and first_key_time moves by a millisecond at most.
    #
    # And a RELEASE of a key the stream never saw pressed is a key held from
    # before it (held_from_start) - unless it comes once noclip is in play:
    # turning noclip on (V, or the `noclip` command) has the hook post a
    # WM_KEYUP for every key it blocks, which the recorder keeps though nobody
    # pressed anything. Those phantom releases are dropped here.
    leading = {}
    seen = set()
    raw = []
    noclip_t = None
    with open_trace(path) as f:
        for line in f:
            try:
                d = json.loads(line)
            except ValueError:
                continue                    # a line cut short by the recorder
            t = d.get("type")
            if t == "meta":
                meta = d
            elif t == "sample":
                if d.get("freecam") or d.get("noclip"):
                    if noclip_t is None or d["t"] < noclip_t:
                        noclip_t = d["t"]
                if not pawn_sample(d):
                    continue
                samples.append(d)
            elif t == "key":
                if d.get("blocked"):
                    if noclip_t is None or d["t"] < noclip_t:
                        noclip_t = d["t"]
                    continue
                if d.get("vk") not in KEYS:
                    continue
                raw.append(d)
            elif t == "sound":
                sounds.append(d)
    if not samples:
        raise SystemExit("%s: no pawn samples" % path)
    downed = set()
    for d in sorted(raw, key=lambda r: r["t"]):
        k = KEYS[d["vk"]]
        if d.get("repeat"):
            if d.get("down") and k not in seen and k not in leading:
                leading[k] = d["t"]
            continue
        if not d.get("down") and k not in downed and noclip_t is not None and d["t"] >= noclip_t - 1e-3:
            continue                        # noclip's releaseHeldKeys, not a key
        seen.add(k)
        if d.get("down"):
            downed.add(k)
        keys.append((d["t"], k, bool(d["down"])))
    keys.sort()
    downs = [t for (t, _k, dn) in keys if dn]
    first = downs[0] if downs else None
    # Leading repeats stand for a press only when the key's first real event
    # is not a release: that key is already held from the start
    # (held_from_start), and a press placed after its release would stick.
    firstreal = {}
    for (_t, k, dn) in keys:
        firstreal.setdefault(k, dn)
    for k, rt in leading.items():
        if firstreal.get(k) is False:
            continue
        at = rt if first is None else first - 0.001
        keys.append((at, k, True))
    keys.sort()
    sounds.sort(key=lambda d: d.get("t", 0.0))
    pauses = []
    if game_clock:
        samples, keys, sounds, pauses = excise_pauses(samples, keys, sounds)
    meta = dict(meta or {})
    meta["sounds"] = sounds
    meta["pauses"] = pauses
    return meta, samples, keys


def first_key_time(keys):
    downs = [t for (t, k, down) in keys if down]
    if not downs:
        raise SystemExit("the trace has no key-down: nothing to replay")
    return downs[0]


def held_from_start(keys):
    """The keys already down when the key stream begins: those whose FIRST
    event is a release. The recorder drains the ring once it attaches, so a
    key pressed before that - the forward a player is already running on -
    arrives only as its release. The 2026-09-26 16:40 run's W is one: she
    ran at 439 uu/s into a jump with no forward in the schedule, so the
    port could not vault (the vault wants the stick) and hung off the ledge
    retail stepped onto. Everything that rebuilds the held keys starts here;
    first_key_time does not, so a window's t0 stays the first real press."""
    first = {}
    for (_t, k, down) in keys:
        first.setdefault(k, down)
    return {k for k, down in first.items() if not down}


# ---------------------------------------------------------------------------
# A paused recording.
#
# The ring's `t` is the hook's QPC clock and `frame` counts Presents
# (telemetry.py _t, d3d9_proxy.cpp telemetryTick): both run on while the game
# is paused, and nothing in a v6 record says it is. The sixth human run
# (2026-09-26 21:40) froze for 4.065 s at 85.74 s - 244 frames whose
# position, 530 uu/s velocity, look and every clip time are bit-identical -
# and the port, replaying on the wall clock, ran on through it and stopped
# 488 uu past her. Retail's pause is a UIScene: TdHUD.PauseGame opens
# PauseSceneRef (Escape or a pad's Start, GBA_Pause in TdInput.ini; or a
# pad's disconnect, TdPlayerController.OnControllerChanged ->
# DelayedPauseGame), and Default__UIScene has bPauseGameWhileActive and
# bFlushPlayerInput True and SceneInputMode INPUTMODE_Locked - the story
# scene, TdUI_InGame's TdSPPause, overrides none of them. So:
#
#   * the world stops (WorldInfo.Pauser), read here off the state: while it
#     ticks every AnimNodeSequence leaf's time advances - Stand's too - and a
#     pawn with speed moves, so a run of samples each identical to the one
#     before in all of them is a stopped world. ONE repeated sample is not:
#     0-85 per recorded v6 run, never two in a row (whether the hook read the
#     game a frame stale or the game skipped a tick is not known);
#   * the held keys are released as the scene opens (bFlushPlayerInput): the
#     sixth run braked x(1 - 8 dt) on its last live frame with the walk-stop
#     clip 0.0168 s in, and W's WM_KEYUP arrived 0.68 s later. Measured on
#     that one pause;
#   * what is pressed while it is up is the menu's (INPUTMODE_Locked;
#     TdUIScene_Pause.HandleInputKey takes Escape and B to OnBack): two
#     clicks, each 3-4 ms ahead of the menu's A_Pos cue, the second the frame
#     before the world resumed, and no melee after. On resume she carried on
#     braking, x(1 - 8 dt) over one frame: nothing held.
#
# A key pressed during the pause and still down at the resume is taken as not
# held (the scene took its press); no recording has one and UE3's PlayerInput
# was not read for it. Before telemetry v6 there are no clips and only a pawn
# with speed can show a pause: the 2026-09-23 20:31 run's Escape (film 109.63
# s), standing, is not seen, and its menu click (111.75 s) replays as a barge.
PAUSE_MIN_FROZEN = 2        # repeated samples in a row before a run is a pause
PHYS_MAX = 13               # EPhysics' last, PHYS_WallClimbing (doc/medge_sdk.md)
_WORLD = ("px", "py", "pz", "vx", "vy", "vz", "cyaw", "cpitch", "pyaw")


def frozen(prev, s):
    """Sample s shows the same world as prev: nothing advanced between them."""
    if any(prev.get(k) != s.get(k) for k in _WORLD):
        return False
    # a freed pawn reads garbage (EPhysics 61-255 on the 2026-09-25 15:46
    # run's tail, once the level was left): not a world at all
    if (s.get("physics") or 0) > PHYS_MAX:
        return False
    a, b = prev.get("anim1p"), s.get("anim1p")
    if a or b:
        return a == b and prev.get("anim3p") == s.get("anim3p")
    # before telemetry v6 there are no clips, and only a pawn with speed says so
    return math.hypot(s.get("vx") or 0.0, s.get("vy") or 0.0) + abs(s.get("vz") or 0.0) > 1.0


def find_pauses(samples):
    """[(i_live, i_last)]: the last sample before each stopped run, and the
    run's last sample. samples[i_live + 1 .. i_last] are the stopped world."""
    out = []
    i, n = 1, len(samples)
    while i < n:
        if not frozen(samples[i - 1], samples[i]):
            i += 1
            continue
        j = i
        while j + 1 < n and frozen(samples[j], samples[j + 1]):
            j += 1
        if j - i + 1 >= PAUSE_MIN_FROZEN:
            out.append((i - 1, j))
        i = j + 1
    return out


def excise_pauses(samples, keys, sounds):
    """The recording on the GAME's clock: each stopped run cut out and
    everything after it moved back by the run's wall time, so a sample, a key
    and a sound keep the game time they happened at - the first live sample
    after a cut is one frame after the last before it.

    The keys: every held key is released at the start of the last live
    frame (FRAME_EPS after the sample before it, where frame_start_keys reads
    a key that frame took); what arrives while the world is stopped is the
    menu's, and dropped; after a cut a release of a key the game no longer
    holds is dropped too, so none reads as held from the start
    (held_from_start). The menu's own cues (not the pawn's) go with it.
    Returns (samples, keys, sounds, pauses), each pause a dict: t (the last
    live sample, game clock), wall_t, seconds (cut), frames, flushed (the keys
    released), dropped [(wall t, key, down)]. With no pause, the inputs."""
    cuts = []
    for (i0, i1) in find_pauses(samples):
        tl = samples[i0]["t"]
        cuts.append({"i0": i0, "i1": i1, "t_live": tl,
                     "t_flush": samples[i0 - 1]["t"] + FRAME_EPS if i0 > 0 else tl,
                     "t_last": samples[i1]["t"], "seconds": samples[i1]["t"] - tl,
                     "flushed": [], "dropped": [], "done": False})
    if not cuts:
        return samples, keys, sounds, []

    def shift(t):
        d = 0.0
        for c in cuts:
            if t > c["t_last"]:
                d += c["seconds"]
            elif t > c["t_live"]:
                return c["t_live"] - d          # inside: the moment it stopped
            else:
                break
        return t - d

    gone = set()
    for c in cuts:
        gone.update(range(c["i0"] + 1, c["i1"] + 1))
    out_s = [dict(s, t=shift(s["t"])) for i, s in enumerate(samples) if i not in gone]

    # the keys the GAME holds, the pulses' presses among them (Q and the left
    # button are edges to the schedule, but a release of one still says it
    # came up)
    held = held_from_start(keys)
    out_k = []

    def flush(c):
        for k in sorted(held):
            out_k.append((shift(c["t_flush"]), k, False))
            c["flushed"].append(k)
        held.clear()
        c["done"] = True

    ci = 0
    for (t, k, down) in keys:
        while ci < len(cuts) and t > cuts[ci]["t_flush"]:
            if not cuts[ci]["done"]:
                flush(cuts[ci])
            if t <= cuts[ci]["t_last"]:
                break
            ci += 1
        if ci < len(cuts) and cuts[ci]["t_flush"] < t <= cuts[ci]["t_last"]:
            cuts[ci]["dropped"].append((t, k, down))        # the menu's
            continue
        if down:
            held.add(k)
            out_k.append((shift(t), k, True))
        elif k in held:
            held.discard(k)
            out_k.append((shift(t), k, False))
        elif not cuts[0]["done"]:
            out_k.append((shift(t), k, False))              # before any cut: as recorded
    for c in cuts:
        if not c["done"]:
            flush(c)
    out_k.sort(key=lambda e: e[0])

    out_n = [dict(r, t=shift(r.get("t", 0.0))) for r in sounds or ()
             if r.get("pawn") or not any(c["t_live"] < r.get("t", 0.0) <= c["t_last"]
                                         for c in cuts)]
    pauses = [{"t": shift(c["t_live"]), "wall_t": c["t_live"], "seconds": c["seconds"],
               "frames": c["i1"] - c["i0"], "flushed": c["flushed"], "dropped": c["dropped"]}
              for c in cuts]
    return out_s, out_k, out_n, pauses


def pauses_in(pauses, t_from, t_to):
    """The pauses cut out between two times on the same clock."""
    return [p for p in pauses or () if t_from <= p["t"] <= t_to]


# ---------------------------------------------------------------------------
# Where the look came from.
#
# The ring's camera yaw/pitch is the look when it moves. In the first human
# recording (2026-09-07) it did not: yaw 170.0 on every one of 4825 play
# samples and the camera position parked at the spawn while the pawn ran
# 10 km, so the record carries no mouse at all. The fallback reads the look
# off the pawn: on the ground, with a wish direction held, the velocity
# heading is the control yaw plus the wish offset (W: 0, A: -90, D: +90,
# S: 180 - UE3 is left-handed, right is yaw + 90), so yaw is heading minus
# offset; elsewhere the last such yaw is held (airborne air control is
# 0.025, and a grab or a wall run does not turn her). It lags a mouse turn
# by the pawn's own turn response, which is the price of not having the
# mouse.

MIN_HEADING_SPEED = 150.0       # uu/s before a velocity has a heading


def camera_look_is_live(samples, t0):
    """Judged over the PLAY, not the boot: the boot's cutscene camera moves
    even when the gameplay camera record is frozen."""
    yaws = {round(s["yaw"], 1) for s in samples if s["t"] >= t0}
    return len(yaws) > 3


def _wish_offset(held):
    move = sum(v for k, v in KEY_MOVE.items() if k in held)
    strafe = sum(v for k, v in KEY_STRAFE.items() if k in held)   # engine: 1 is LEFT
    if not move and not strafe:
        return None
    return math.degrees(math.atan2(-strafe, move))


def derive_look(samples, keys):
    """Stamp every sample with look_yaw/look_pitch from the pawn velocity."""
    ki = 0
    held = set(k for k in held_from_start(keys) if k not in PULSES)
    last = None
    pending = []
    for s in samples:
        while ki < len(keys) and keys[ki][0] <= s["t"]:
            _t, k, down = keys[ki]
            if k not in PULSES:
                (held.add if down else held.discard)(k)
            ki += 1
        yaw = None
        sp = math.hypot(s["vx"], s["vy"])
        if (s.get("physics") == UE_PHYS_WALKING and s.get("move_name") == "MOVE_Walking"
                and sp >= MIN_HEADING_SPEED):
            off = _wish_offset(held)
            if off is not None:
                yaw = (math.degrees(math.atan2(s["vy"], s["vx"])) - off) % 360.0
        if yaw is None:
            pending.append(s)
            s["look_yaw"] = last
        else:
            if last is None:
                for p in pending:
                    p["look_yaw"] = yaw
            pending = []
            last = yaw
            s["look_yaw"] = yaw
        s["look_pitch"] = 0.0
    if last is None:
        raise SystemExit("no grounded travel with a wish direction: cannot derive a look")
    for p in pending:
        p["look_yaw"] = last


def controller_look_is_live(samples, t0):
    """v5 traces carry the controller's own rotation (cyaw/cpitch)."""
    ys = {round(s["cyaw"], 1) for s in samples if s.get("cyaw") is not None and s["t"] >= t0}
    return len(ys) > 3


def stamp_look(samples, keys, source="auto"):
    if source == "auto":
        t0 = first_key_time(keys)
        if controller_look_is_live(samples, t0):
            source = "controller"
        elif camera_look_is_live(samples, t0):
            source = "camera"
        else:
            source = "velocity"
    if source == "controller":
        last = None
        for s in samples:
            if s.get("cyaw") is not None:
                last = (s["cyaw"] % 360.0, s["cpitch"])
            s["look_yaw"], s["look_pitch"] = last if last else (s["yaw"] % 360.0, s["pitch"])
    elif source == "camera":
        for s in samples:
            s["look_yaw"] = s["yaw"] % 360.0
            s["look_pitch"] = s["pitch"]
    else:
        derive_look(samples, keys)
    return source


# ---------------------------------------------------------------------------
# Schedules

def held_at(keys, t):
    held = held_from_start(keys)
    for (kt, k, down) in keys:
        if kt >= t:
            break
        if down:
            held.add(k)
        else:
            held.discard(k)
    return held


# ---------------------------------------------------------------------------
# Retail's frame timing
#
# A key's stamp is when the window received it, not when the game used it.
# Retail reads its input once a frame, and the frame that ENDS at sample i
# runs from sample i-1's state: a key stamped in (t[i-1], t[i]] is that
# frame's and acts from t[i-1]. Of the jump and W-from-rest presses in the
# recorded runs, 156 of 167 show their response in exactly that frame (the
# MOVE_Jump, the first |V2D| over 1). The rest show it one frame later, a
# race with the poll - the seventh run's late ones sit 7.2-13.8 ms into their
# frames, its in-frame ones 8.7-14.6 ms - and those go to the start of the
# frame that shows them. A move key pressed or released while she runs
# loses the same race: of 617 the recording can read (on the ground, the view
# still), 564 turn the frame's acceleration along the key's axis by over 1000
# uu/s^2 in the frame their stamp is in (the median is 4600), and 53 leave it
# within 500 until the next. The frame also turns to its END sample's look:
# PlayerWalking.PlayerMove reads GetAxes(Pawn.Rotation) @0, calls
# UpdateRotation @1040 and only then ProcessMove @1117, so the frame
# (t[i-1], t[i]] turned to yaw_i. Only half of it moves on yaw_i: the wish
# and both acceleration natives (@238-@830) run in @0's axes, the facing the
# LAST frame left, which the engine keeps as medgeaxisyaw, the previous
# tick's yaw (physics.cpp medge_momentum); the frame's own turn (aTurn, the
# turn brake's, medgelookturn) and what runs after @1040 (ProcessMove,
# StartMove's JumpAddXY, the slide's entry) take yaw_i. The look at t[i-1]
# is what lines both halves up - neither "fix" the wish onto the current
# yaw nor move the look back.
#
# The "stamp" timing (the option that keeps the old schedule) put each key on
# the tick nearest its stamp - a stamp sits 11.5 ms into its frame on average
# - and each look at its own sample's stamp, a frame late, and counted every
# tick from the first key, so a re-anchor off that grid made everything after
# it late by the difference: the seventh run's window 16 asked its last
# FindWallForward 20 ms after retail's, at a wall where one frame decides
# (doc/medge_port_parity.md, "Retail's input and look timing").

TIMINGS = ("frame", "stamp")
FRAME_EPS = 1e-4        # into the frame, so a "<= t" walk puts the key in it
RESPONSE_LATE = 1       # frames a press may show late: the poll race
REST_UU = 0.01          # |V2D| she is at rest under...
MOVING_UU = 1.0         # ...and moving over
RUNNING_UU = 50.0       # running: the move keys' acceleration test (the survey's)
ACCEL_QUIET = 500.0     # uu/s^2 along the key's axis: a frame that did not take it
ACCEL_TOOK = 1000.0     # ...and one that did
# What a press visibly starts. Off the ground the jump key starts a jump or
# one of the moves it asks for (each recorded answering a press off
# MOVE_Walking); the crouch a slide or a crouch; Q a 180; the left button a
# barge or a melee; a move key from rest starts her moving, and pressed or
# released while she runs on the ground with the view still, it turns the
# frame's acceleration along its axis. A key with no such answer - a jump key
# in the air, W on a hang, whose pull-up shows in the move a frame before the
# velocity - stays in the frame its stamp is in.
JUMP_KEY_MOVES = frozenset(("MOVE_Jump", "MOVE_SpringBoarding", "MOVE_VaultOver",
                            "MOVE_WallClimbing", "MOVE_WallRunningLeft",
                            "MOVE_WallRunningRight"))
CROUCH_KEY_MOVES = frozenset(("MOVE_Slide", "MOVE_Crouch"))
# (forward, right) each move key pushes along: UE3's right is yaw + 90
KEY_AXES = {"w": (1.0, 0.0), "s": (-1.0, 0.0), "d": (0.0, 1.0), "a": (0.0, -1.0)}


def _move_of(s):
    return s.get("move_name") or ""


def _barges(m):
    return "Barge" in m or "Melee" in m


def _frame_accel(samples, k):
    """Frame k's acceleration, uu/s^2, as (forward, right) in the facing its
    PlayerMove read (GetAxes(Pawn.Rotation) @0: sample k-1's pawn yaw)."""
    p, s = samples[k - 1], samples[k]
    dt = s["t"] - p["t"]
    if dt <= 0.0:
        return None
    ax, ay = (s["vx"] - p["vx"])/dt, (s["vy"] - p["vy"])/dt
    y = math.radians(p.get("pyaw", p.get("yaw", 0.0)) or 0.0)
    return (ax*math.cos(y) + ay*math.sin(y), -ax*math.sin(y) + ay*math.cos(y))


def _axis_response(samples, i, k, down):
    """A move key pressed or released while she runs: i when frame i's
    acceleration along the key's axis turns its way, i + 1 when frame i's
    stays put and the next one's turns, None when the recording cannot say -
    off the ground, a turning view (which turns the acceleration itself), a
    collision."""
    if i < 3 or i + 1 >= len(samples):
        return None
    win = samples[i - 2:i + 2]
    if any(_move_of(s) != "MOVE_Walking" or s.get("physics") != UE_PHYS_WALKING for s in win):
        return None
    if math.hypot(samples[i - 1]["vx"], samples[i - 1]["vy"]) < RUNNING_UU:
        return None
    yaws = [s.get("look_yaw") for s in win]
    if None in yaws or any(abs((b - a + 180.0) % 360.0 - 180.0) > 0.01 for a, b in zip(yaws, yaws[1:])):
        return None
    a0, a1, a2 = (_frame_accel(samples, j) for j in (i - 1, i, i + 1))
    if not (a0 and a1 and a2):
        return None
    ax = KEY_AXES[k]
    sgn = 1.0 if down else -1.0
    d1 = sgn*((a1[0] - a0[0])*ax[0] + (a1[1] - a0[1])*ax[1])
    d2 = sgn*((a2[0] - a1[0])*ax[0] + (a2[1] - a1[1])*ax[1])
    if d1 > ACCEL_TOOK:
        return i
    if abs(d1) < ACCEL_QUIET and d2 > ACCEL_TOOK:
        return i + 1
    return None


def key_response(samples, i, k, down):
    """The sample whose frame SHOWS the key event stamped in sample i's
    frame - i, or up to RESPONSE_LATE frames on - or None where it starts
    nothing the recording shows."""
    p = samples[i - 1]
    pm = _move_of(p)
    if k in KEY_AXES and (not down or math.hypot(p["vx"], p["vy"]) >= RUNNING_UU):
        return _axis_response(samples, i, k, down)
    if not down:
        return None
    if k in KEY_MOVE or k in KEY_STRAFE:
        if pm != "MOVE_Walking" or math.hypot(p["vx"], p["vy"]) >= REST_UU:
            return None

        def shows(s):
            return math.hypot(s["vx"], s["vy"]) > MOVING_UU
    elif k == "space" or k == "lshift":
        if pm != "MOVE_Walking":
            return None
        want = JUMP_KEY_MOVES if k == "space" else CROUCH_KEY_MOVES

        def shows(s):
            return _move_of(s) in want
    elif k == "q":
        if "180" in pm:
            return None

        def shows(s):
            return "180" in _move_of(s)
    elif k == "lmb":
        if _barges(pm):
            return None

        def shows(s):
            return _barges(_move_of(s))
    else:
        return None
    for j in range(i, min(i + RESPONSE_LATE + 1, len(samples))):
        if shows(samples[j]):
            return j
    return None


def frame_start_keys(keys, samples):
    """`keys` moved to the start of the recorded frame that took each: the
    frame its stamp falls in (a stamp ON a sample belongs to the frame that
    sample closes), or the later one its response shows in (key_response).
    A release never goes before the frame after the one its press showed in
    - the poll saw the key held there, so the release came after it - and no
    key's events change order. Keys outside the recording keep their stamps.
    In time order, ties as given."""
    ts = [s["t"] for s in samples]
    out = []
    shown = {}          # key -> the sample its last press showed in
    last = {}           # key -> the frame its last event went to
    for n, (kt, k, down) in enumerate(keys):
        i = bisect.bisect_left(ts, kt)
        if i < 1 or i >= len(samples):
            out.append((kt, n, k, down))
            continue
        r = key_response(samples, i, k, down)
        j = i if r is None else r
        if down:
            if r is not None:
                shown[k] = r
            else:
                shown.pop(k, None)
        elif k in shown:
            j = max(j, shown.pop(k) + 1)
        j = max(j, last.get(k, j))
        last[k] = j
        out.append((ts[j - 1] + FRAME_EPS, n, k, down))
    out.sort()
    return [(t, k, down) for (t, _n, k, down) in out]


def retail_ground_state(samples, keys, t):
    """What retail's frame carries at time t besides the pose, rebuilt from
    the recording by retail's own rules - none of it is recorded, and an
    anchor that drops it hands the port a pawn retail never was. A dict:
    energy (SpeedSprintEnergy, uu/s), acctime (AccelerationTime, s), stop
    (seconds into a walk-stop's PlayStop, or -1), jump (2 in MOVE_Jump, 1 in
    the Falling after one, else 0) and prejump (its PreJumpMomentum, uu/s).

    Per recorded frame, from the velocity and pawn yaw the frame's PlayerMove
    saw (the sample before it), in a PlayerWalking move (WALKING_MOVES): a
    running stop takes the frame (keys, energy and AccelerationTime untouched,
    PlayerMove @42-147); no key - or a move that ignores them, NOKEY_MOVES -
    zeroes the energy and, on the ground, AccelerationTime, after a TAP
    (AccelerationTime under 0.15 s) starting the stop; in the air a key sets
    the energy to max(|V2D| - 400, 0) (GetSprintAcceleration, Physics 2); on
    the ground a key adds the frame to AccelerationTime and either SPRINTS -
    forward over 0.7, InputSize over 0.7, Velocity . wish > 0: the energy is
    max(|V2D| - 400, 0) - or WALKS, the energy decaying at (720 - 400)/3 x (1
    - sqrt(clamp(wish . facing, 0, 0.9))) a second. A vault sets
    AccelerationTime 0.2. Other moves leave all of it alone. A jump's launch
    frame takes PreJumpMomentum off the velocity PlayerMove left - 35 uu/s
    if the frame's stop rewrote it. The first human run's window 7 anchors in
    a backpedal out of a roll off a 714 uu/s fall: retail walks toward 400 +
    314, the port anchored with none walked toward 400."""
    energy = 0.0
    acctime = 0.0
    stop_at = None
    jump = 0
    prejump = 0.0
    held = held_from_start(keys)
    ki = 0
    prev = None
    for s in samples:
        if s["t"] > t + 1e-6:
            break
        while ki < len(keys) and keys[ki][0] <= s["t"]:
            _kt, k, down = keys[ki]
            (held.add if down else held.discard)(k)
            ki += 1
        move = s.get("move_name", "")
        if stop_at is not None and s["t"] - stop_at >= STOP_TIME_S:
            stop_at = None
        if prev is None or move not in WALKING_MOVES:
            if move not in ("MOVE_Jump", "MOVE_Falling"):
                jump = 0
            prev = s
            continue
        dt = s["t"] - prev["t"]
        vx, vy = prev["vx"], prev["vy"]
        sp = math.hypot(vx, vy)
        ground = prev.get("physics") == UE_PHYS_WALKING
        stopped = stop_at is not None
        if not stopped:
            if move in NOKEY_MOVES:
                af = ast = 0.0
            else:
                af = float(sum(v for k, v in KEY_MOVE.items() if k in held))
                # retail's aStrafe is +1 for D (right); the port's strafe is +1 for A
                ast = -float(sum(v for k, v in KEY_STRAFE.items() if k in held))
            insize = math.hypot(af, ast)
            if insize < 1e-4:
                energy = 0.0
                if ground:
                    if 0.0 < acctime < STOP_TAP_S:
                        stop_at = s["t"]
                        stopped = True
                    acctime = 0.0
            elif prev.get("physics") == UE_PHYS_FALLING:
                energy = max(sp - 400.0, 0.0)
            else:
                if insize > 1.0:
                    af /= insize
                    ast /= insize
                yaw = math.radians(prev.get("pyaw", prev.get("yaw", 0.0)) or 0.0)
                fx, fy = math.cos(yaw), math.sin(yaw)          # X, the facing
                rx, ry = -math.sin(yaw), math.cos(yaw)         # Y, to the right
                wx, wy = af*fx + ast*rx, af*fy + ast*ry
                acctime += dt
                if af > 0.7 and insize > 0.7 and vx*wx + vy*wy > 0.0:
                    energy = max(sp - 400.0, 0.0)
                else:
                    wl = math.hypot(wx, wy)
                    dx = (wx*fx + wy*fy)/wl if wl > 1e-6 else 0.0
                    c = min(max(dx, 0.0), 0.9)
                    energy = max(energy - (720.0 - 400.0)/3.0*(1.0 - math.sqrt(c))*dt, 0.0)
        if move in VAULT_MOVES:
            acctime = 0.2
        if move == "MOVE_Jump":
            if prev.get("move_name", "") != "MOVE_Jump":
                # the stop's Velocity := Normal(V) 35 (@77 / @978) comes first
                prejump = min(sp, STOP_VEL_UU) if (stopped and ground) else sp
            jump = 2
        elif move == "MOVE_Falling":
            jump = 1 if jump else 0
        else:
            jump = 0
        prev = s
    stop = (t - stop_at) if stop_at is not None and t - stop_at < STOP_TIME_S else -1.0
    return {"energy": energy, "acctime": acctime, "stop": stop, "jump": jump,
            "prejump": prejump if jump else 0.0}


def retail_at(samples, t):
    """Retail pawn feet position interpolated at absolute time t."""
    lo, hi = 0, len(samples) - 1
    if t <= samples[0]["t"]:
        s = samples[0]
        return (s["px"], s["py"], s["pz"] - centre_above_feet(s)), s
    if t >= samples[-1]["t"]:
        s = samples[-1]
        return (s["px"], s["py"], s["pz"] - centre_above_feet(s)), s
    while hi - lo > 1:
        mid = (lo + hi)//2
        if samples[mid]["t"] <= t:
            lo = mid
        else:
            hi = mid
    a, b = samples[lo], samples[hi]
    u = (t - a["t"])/max(b["t"] - a["t"], 1e-6)
    p = tuple(a[k] + (b[k] - a[k])*u for k in ("px", "py", "pz"))
    # the feet, interpolated as feet (a capsule change between the two
    # samples moves the centre 29, never the feet)
    fz = (a["pz"] - centre_above_feet(a)) + ((b["pz"] - centre_above_feet(b)) -
                                             (a["pz"] - centre_above_feet(a)))*u
    return (p[0], p[1], fz), (a if u < 0.5 else b)


def moves_in(samples, t_start, t_end):
    """The retail moves in a window, in order, with their durations."""
    out = []
    for s in samples:
        if s["t"] < t_start or s["t"] > t_end:
            continue
        name = s.get("move_name", "?").replace("MOVE_", "")
        if out and out[-1][0] == name:
            out[-1][2] = s["t"]
        else:
            out.append([name, s["t"], s["t"]])
    return [(n, max(0.0, b - a)) for (n, a, b) in out]
