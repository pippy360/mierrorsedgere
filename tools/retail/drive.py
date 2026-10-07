"""Launch and drive retail Mirror's Edge: keys in, screenshots out.

Encodes two things that are easy to get wrong:

  * **The Steam re-exec.** MirrorsEdge.exe is wrapped in a Steam DRM stub (a
    `.bind` PE section). Launching it exits the process you started almost
    immediately and spawns a *new* one, so any code that waits on the original
    handle concludes the game never ran. It does run, and the replacement keeps
    the original command line verbatim. `launch()` therefore polls for a process
    by name rather than waiting on a handle.

  * **Input must be scan codes.** See tools/retail/desktop.py.
"""

import os
import subprocess
import time

from . import desktop, paths

EXE = "MirrorsEdge.exe"


def is_running():
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq %s" % EXE, "/NH"],
                             capture_output=True, text=True, timeout=20).stdout
        return EXE.lower() in out.lower()
    except Exception:
        return False


def kill():
    subprocess.run(["taskkill", "/F", "/IM", EXE], capture_output=True, timeout=30)
    for _ in range(20):
        if not is_running():
            return True
        time.sleep(0.5)
    return False


def launch(args=(), wait_window=90):
    """Start the game and wait for its window to exist.

    Returns (hwnd, title, rect) or raises.
    """
    exe = paths.medge_exe()
    subprocess.Popen([exe] + list(args), cwd=os.path.dirname(exe))

    deadline = time.time() + wait_window
    while time.time() < deadline:
        hwnd, title = desktop.find_window_by_process(EXE)
        if hwnd:
            rect = desktop.window_rect(hwnd)
            if rect[2] - rect[0] > 100 and rect[3] - rect[1] > 100:
                return hwnd, title, rect
        time.sleep(2)
    raise RuntimeError("Mirror's Edge window did not appear within %ds" % wait_window)


def _binaries_dir():
    return os.path.dirname(paths.medge_exe())


def grab_frame(path, timeout=4.0):
    """Ask the injected d3d9 proxy for the game's actual back buffer.

    This is the only capture method that is correct while the game is covered,
    unfocused, or off-screen:

      * a screen-region BitBlt copies whatever is visually on top;
      * PrintWindow with PW_RENDERFULLCONTENT only partially redirects a D3D9
        scene - sky renders, world geometry comes back as black silhouettes.

    The proxy polls for a sentinel file inside its Present hook and writes the
    back buffer as a BMP. Returns None if the hook is not installed.
    """
    from PIL import Image

    bmp = os.path.join(_binaries_dir(), "medge_frame.bmp")
    trigger = os.path.join(_binaries_dir(), "medge_shot.trigger")
    before = os.path.getmtime(bmp) if os.path.isfile(bmp) else 0

    try:
        with open(trigger, "w") as f:
            f.write("shot\n")
    except OSError:
        return None

    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.15)
        if os.path.isfile(bmp) and os.path.getmtime(bmp) > before:
            time.sleep(0.15)          # let the write finish
            try:
                img = Image.open(bmp)
                img.load()
            except Exception:
                continue
            paths.ensure_dir(os.path.dirname(os.path.abspath(path)))
            img.convert("RGB").save(path)
            small = img.convert("L").resize((64, 64))
            px = list(small.tobytes())
            return {"path": path, "size": img.size,
                    "mean": round(sum(px) / len(px), 2),
                    "looks_black": False, "method": "backbuffer"}
    return None


def dump_drawlog(timeout=12.0):
    """Ask the proxy for one frame's pixel-constant / texture / draw log.

    Writes `medge_drawlog.trigger`; the Present hook captures the next whole
    frame to `medge_drawlog.txt` (tagged P/D/X lines - the colour oracle's
    capture side). Returns the path once the file is refreshed, or None if the
    hook is not installed. Park the camera on the surface of interest first so
    its draws are in the frame.
    """
    out = os.path.join(_binaries_dir(), "medge_drawlog.txt")
    trigger = os.path.join(_binaries_dir(), "medge_drawlog.trigger")
    before = os.path.getmtime(out) if os.path.isfile(out) else 0
    try:
        with open(trigger, "w") as f:
            f.write("drawlog\n")
    except OSError:
        return None
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.2)
        if os.path.isfile(out) and os.path.getmtime(out) > before:
            time.sleep(0.3)          # let the one-frame write finish
            return out
    return None


def player_next_candidate():
    """Switch to the next matching address.

    A scan usually returns several addresses that all track the position; only
    one is the value the engine integrates. The others are per-frame copies -
    writes to them are overwritten before the next render.
    """
    _camera_cmd("next", settle=1.0)


def player_scan(z_offset=0.0):
    """Start locating the player's position in the game's memory.

    Every CheatManager command is inert in retail Mirror's Edge - God, Ghost,
    Fly, DropMe and SpawnAt all do nothing - so there is no console route to a
    flycam. But the view-projection tells the proxy exactly where the camera is,
    which is enough to find the same value on the heap and then write to it.
    Moving the player (rather than just the camera matrix) keeps culling,
    streaming and the sky pass consistent, which is what the matrix override
    cannot do.

    `z_offset` shifts the search target below the camera. The pawn's Location is
    the capsule centre, BaseEyeHeight = 76 uu under the eye, and it is the value
    the engine actually integrates - the camera position is a derived copy that
    gets rewritten every frame, so writing there does nothing.

    Call this, then move around for a few seconds so the value changes and the
    candidate list narrows.
    """
    _camera_cmd("scan %.3f" % z_offset, settle=2.5)


def player_teleport(x, y, z):
    """Teleport the player once `player_scan` has locked on."""
    _camera_cmd("tp %.4f %.4f %.4f" % (x, y, z))


def scan_state():
    """(candidates, locked) from the proxy's memory search."""
    st = camera_position()
    if not st:
        return None
    return st.get("candidates"), st.get("locked")


def camera_position(timeout=3.0):
    """Where the game's camera currently is, in Unreal units.

    Read out of the shared view-projection matrix by the proxy: the translation
    row satisfies row3[j] = -dot(camPos, col_j), so inverting it recovers the
    position exactly, without needing any UE3 reflection.
    """
    p = os.path.join(_binaries_dir(), "medge_camera.state")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.isfile(p):
            try:
                with open(p) as f:
                    parts = f.read().split()
                if len(parts) >= 4:
                    out = {"pos": tuple(float(v) for v in parts[:3]),
                           "freecam": parts[3] == "1"}
                    if len(parts) >= 6:
                        out["candidates"] = int(parts[4])
                        out["locked"] = parts[5] == "1"
                    if len(parts) >= 8:
                        out["yaw"] = float(parts[6])
                        out["pitch"] = float(parts[7])
                    if len(parts) >= 18:
                        out["pawn_found"] = parts[14] == "1"
                        out["find_state"] = int(parts[15])
                        out["find_index"] = int(parts[16])
                        out["find_total"] = int(parts[17])
                    if len(parts) >= 14:
                        # "pos"/"yaw"/"pitch" stay the GAME camera - where the
                        # player is looking from. While noclip flies, the frame
                        # is rendered from this pose instead.
                        out["noclip"] = parts[8] == "1"
                        out["fly_pos"] = tuple(float(v) for v in parts[9:12])
                        out["fly_yaw"] = float(parts[12])
                        out["fly_pitch"] = float(parts[13])
                    return out
            except (OSError, ValueError):
                pass
        time.sleep(0.2)
    return None


def _camera_cmd(text, settle=0.6):
    """Hand one command to the hook.

    The hook consumes the file by deleting it. While streaming a flight path at
    ~25 Hz that delete regularly lands between our open() and the write, and
    Windows fails the open outright rather than recreating the file - so retry
    briefly instead of dying mid-flight. Commands carry absolute poses, so a
    genuinely lost one only drops a sample from the path.
    """
    p = os.path.join(_binaries_dir(), "medge_camera.cmd")
    for attempt in range(12):
        try:
            with open(p, "w") as f:
                f.write(text + "\n")
            break
        except PermissionError:
            if attempt == 11:
                break
            time.sleep(0.004)
    if settle:
        time.sleep(settle)


def camera_goto(x, y, z):
    """Place the camera at an absolute world position (Unreal units)."""
    _camera_cmd("pos %.4f %.4f %.4f" % (x, y, z))


def camera_move(dx=0.0, dy=0.0, dz=0.0):
    """Nudge the camera relative to where it currently is."""
    _camera_cmd("rel %.4f %.4f %.4f" % (dx, dy, dz))


def camera_look(yaw, pitch):
    """Aim the camera at an absolute yaw/pitch in degrees.

    Same convention camera_position() reports: yaw = atan2(fwd.y, fwd.x) so 0
    looks down +X and 90 down +Y; pitch = asin(fwd.z), positive upward. Unlike
    driving the mouse, this is absolute, exact, and not clamped - straight down
    (pitch -90) is reachable, which the game's own look never allows.
    """
    _camera_cmd("look %.4f %.4f" % (yaw, pitch))


def camera_look_release():
    """Give orientation back to the game, keeping the position override."""
    _camera_cmd("lookoff")


def camera_pose(x, y, z, yaw, pitch, settle=0.0):
    """Place and aim the camera in one command.

    Sent as a single write so the hook can never poll between the position and
    the heading, which would render a frame at the new spot facing the old way.
    """
    _camera_cmd("pose %.4f %.4f %.4f %.4f %.4f" % (x, y, z, yaw, pitch), settle)


def _lerp_angle(a, b, t):
    """Interpolate degrees the short way round, so 350 -> 10 crosses zero."""
    d = (b - a + 180.0) % 360.0 - 180.0
    return a + d * t


def camera_fly(waypoints, seconds=4.0, hz=25.0, ease=True, on_frame=None):
    """Fly smoothly through a list of (x, y, z, yaw, pitch) keyframes.

    `seconds` is per leg. Yaw takes the short way round, and each leg is
    smoothstep-eased by default so the camera arrives and departs gently
    instead of snapping between headings.

    `on_frame(index, pose)` is called after each pose is sent - use it to grab
    frames mid-flight without stopping.
    """
    step = 1.0 / hz
    idx = 0
    for leg in range(len(waypoints) - 1):
        a, b = waypoints[leg], waypoints[leg + 1]
        n = max(1, int(seconds * hz))
        for i in range(n + 1):
            t = i / float(n)
            if ease:
                t = t * t * (3.0 - 2.0 * t)
            pose = (a[0] + (b[0] - a[0]) * t,
                    a[1] + (b[1] - a[1]) * t,
                    a[2] + (b[2] - a[2]) * t,
                    _lerp_angle(a[3], b[3], t),
                    a[4] + (b[4] - a[4]) * t)
            camera_pose(*pose)
            if on_frame:
                on_frame(idx, pose)
            idx += 1
            time.sleep(step)
    return waypoints[-1]


def pawn_follow(timeout=240.0, shape_filter=False, narrow_rounds=8):
    """Locate the player's Location in memory so noclip can carry her along.

    Without this the pawn stays at the spawn point while the camera flies, and
    anything keyed to the player - streaming, distance culling, LOD - is
    computed for a place you are no longer looking from.

    Two steps, both inside the hook: scan for addresses holding the camera
    position, then sweep the survivors ONE AT A TIME, writing +400 uu of Z to
    each and asking whether the camera actually rose. That last test is the
    whole point - an earlier attempt wrote ~90 addresses at once, saw nothing,
    and wrongly concluded the pawn was unreachable.

    Returns the state dict; check ["pawn_found"].
    """
    _camera_cmd("scan", settle=0.0)
    time.sleep(6.0)

    # Narrow before sweeping. The raw scan returns ~490 addresses holding the
    # camera position; walking the player and keeping only what tracks the move
    # cuts that by an order of magnitude. It matters for more than speed - the
    # sweep writes a bogus value to each address in turn, and enough of those
    # disturb rendering that camera detection drops out and the sweep stalls.
    hwnd, _t = desktop.find_window_by_process(EXE)
    if hwnd:
        desktop.ensure_focus(hwnd)
        for i in range(narrow_rounds):
            desktop.hold_key("d" if i % 2 == 0 else "a", 0.8)
            time.sleep(0.9)

    _camera_cmd("find %d" % (1 if shape_filter else 0), settle=0.0)

    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        s = camera_position(timeout=2.0) or {}
        last = s
        if s.get("pawn_found"):
            return s
        if s.get("find_state") == 0 and s.get("find_index", 0) > 0:
            return s                      # swept everything, found nothing
        time.sleep(1.0)
    return last


def confine_cursor(on=True):
    """Keep the OS cursor inside the game window while it has focus.

    On by default in the proxy. Mirror's Edge never calls ClipCursor during
    gameplay - fullscreen used to confine the pointer for it - so forcing
    windowed mode lets the cursor wander onto other monitors. Windows drops the
    clip by itself when another window is activated, so alt-tab still works.
    """
    _camera_cmd("cursor %d" % (1 if on else 0))


def camera_release():
    """Hand the camera back to the game."""
    _camera_cmd("off")


def pawn_set(x, y, z, yaw=None, settle=0.6):
    """Place the PAWN - not the camera - at a pose, standing, velocity zeroed.

    The anchor for A/B input replay: an open-loop input script only measures
    the engine if it starts from the same pose every run, and walking there
    accumulates drift the comparison then misreads as physics. Writes the
    pawn's own Location (the authoritative one the engine integrates; the
    camera is a derived copy), so the view follows by itself next frame.

    ``yaw`` is degrees and optional. The write goes to the pawn's rotator,
    which the controller can override - verify against telemetry afterwards
    rather than trusting it. z is the CAPSULE CENTRE: Faith's eye rides
    BaseEyeHeight 76 uu above it and her feet sit 96 below, so to stand on a
    floor at F pass z = F + 96 + a couple of uu of clearance; one tick of
    PHYS_Walking settles her the rest of the way.

    Refused by the hook when there is no valid pawn (menu, load, cutscene) -
    check the hook log rather than assuming it landed.
    """
    if yaw is None:
        _camera_cmd("pawnset %.4f %.4f %.4f" % (x, y, z), settle)
    else:
        _camera_cmd("pawnset %.4f %.4f %.4f %.4f" % (x, y, z, yaw), settle)


# --- flycam via the game's own detached camera --------------------------------
#
# Two ways to move the viewpoint, with very different quality:
#
#   camera_goto()  rewrites the view-projection in the proxy. Exact, but only
#                  the camera matrix moves - culling, streaming, near/far and
#                  the sky/reflection passes still use the player's position, so
#                  beyond a few hundred units geometry shreds into shards.
#
#   flycam_*()     uses the game's DropMe cheat to detach its real camera.
#                  Everything follows correctly, so the image stays clean at any
#                  distance. The trade is that it flies by input rather than by
#                  coordinate - which is fine, because the proxy reads the exact
#                  world position back out of the matrix anyway.
#
# Prefer the flycam for reference imagery; use camera_goto for small, precise
# offsets from a known pose.

FLY_KEYS = {"forward": "w", "back": "s", "left": "a", "right": "d",
            "up": "space", "down": "lctrl"}


def flycam_enable(settle=2.5):
    """Detach the game camera (God + DropMe) so it can be flown freely."""
    hwnd, _t = desktop.find_window_by_process(EXE)
    if not hwnd:
        raise RuntimeError("Mirror's Edge is not running")
    camera_release()          # never mix the matrix override with DropMe
    time.sleep(0.5)
    desktop.ensure_focus(hwnd)
    desktop.key("home")       # God - the debug menu notes free cam requires it
    time.sleep(1.2)
    desktop.key("f3")         # DropMe
    time.sleep(settle)
    return camera_position()


def flycam_move(direction, seconds=1.0):
    """Hold a movement key to fly. Returns the position afterwards."""
    key = FLY_KEYS.get(direction)
    if not key:
        raise KeyError("unknown direction %r; use one of %s"
                       % (direction, sorted(FLY_KEYS)))
    hwnd, _t = desktop.find_window_by_process(EXE)
    desktop.ensure_focus(hwnd)
    desktop._send([desktop._key_input(desktop.SCAN[key], False)])
    time.sleep(seconds)
    desktop._send([desktop._key_input(desktop.SCAN[key], True)])
    time.sleep(0.5)
    return camera_position()


def flycam_look(dx=0, dy=0, steps=10):
    """Turn the camera by moving the mouse (relative units)."""
    hwnd, _t = desktop.find_window_by_process(EXE)
    desktop.ensure_focus(hwnd)
    for _ in range(steps):
        desktop.mouse_move(int(dx / steps), int(dy / steps))
        time.sleep(0.02)
    time.sleep(0.4)
    return camera_position()


def shot(name, region=None, window_only=True):
    """Capture the game.

    Prefers the in-process back-buffer grab (correct even when the game is
    covered or unfocused); falls back to a window/screen capture if the proxy
    hook is not present.
    """
    path = paths.build_dir("me_shots", name if name.endswith(".png") else name + ".png")
    paths.ensure_dir(os.path.dirname(path))

    if region is None and window_only:
        info = grab_frame(path)
        if info is not None:
            return info
    if region is not None:
        return desktop.capture(path, region=region)
    if not window_only:
        return desktop.capture(path)
    return desktop.capture_game(path, exe=EXE)


def console(commands, open_key="tilde", settle=1.0, per_cmd=0.6, shot_prefix=None):
    """Open the in-game console, type commands, capture after each.

    `commands` is a list of strings. Returns a list of capture dicts.
    """
    hwnd, title = desktop.find_window_by_process(EXE)
    if not hwnd:
        raise RuntimeError("Mirror's Edge is not running")
    if not desktop.focus(hwnd):
        # Focus can fail silently if another window is topmost; carry on and let
        # the caller judge from the screenshots.
        pass
    time.sleep(settle)

    out = []
    desktop.key(open_key)
    time.sleep(settle)
    if shot_prefix:
        out.append(shot("%s_console_open" % shot_prefix))

    for i, cmd in enumerate(commands):
        desktop.type_text(cmd)
        time.sleep(0.2)
        desktop.key("enter")
        time.sleep(per_cmd)
        if shot_prefix:
            out.append(shot("%s_cmd%02d" % (shot_prefix, i)))
    return out


def screenshot_dirs():
    """Everywhere Mirror's Edge might drop a `screenshot` / `tiledshot`."""
    out = []
    install = paths.find_medge_install(required=False)
    if install:
        out += [os.path.join(install, "TdGame", "ScreenShots"),
                os.path.join(install, "Binaries", "ScreenShots"),
                os.path.join(install, "TdGame", "Screenshots")]
    user = paths.medge_user_config_dir()
    if user:
        root = os.path.dirname(os.path.dirname(user))
        out += [os.path.join(root, "TdGame", "ScreenShots"),
                os.path.join(root, "ScreenShots")]
    return out


def find_game_screenshots(since=0.0):
    """Screenshot files the game itself wrote after `since` (epoch seconds)."""
    hits = []
    for d in screenshot_dirs():
        if not os.path.isdir(d):
            continue
        for n in os.listdir(d):
            p = os.path.join(d, n)
            try:
                st = os.stat(p)
            except OSError:
                continue
            if os.path.isfile(p) and st.st_mtime >= since:
                hits.append((p, st.st_size, st.st_mtime))
    return sorted(hits, key=lambda t: t[2])


def _red_fraction(path, left_only=False):
    """Fraction of the frame that is Mirror's Edge menu red."""
    from PIL import Image

    im = Image.open(path).convert("RGB")
    w, h = im.size
    # Skip the window title bar: white chrome, not game output.
    im = im.crop((0, int(h * 0.09), int(w * 0.35) if left_only else w, h))
    px = im.load()
    w, h = im.size
    red = total = 0
    for y in range(0, h, 3):
        for x in range(0, w, 3):
            r, g, b = px[x, y]
            total += 1
            if r > 170 and g < 90 and b < 90:
                red += 1
    return red / max(1, total)


def _detail(path):
    """Edge density - how much structure is in the frame."""
    from PIL import Image, ImageFilter

    im = Image.open(path).convert("L")
    w, h = im.size
    im = im.crop((0, int(h * 0.09), w, h)).resize((320, 180))
    px = list(im.filter(ImageFilter.FIND_EDGES).tobytes())
    return sum(px) / len(px)


def _movie_watermark(path):
    """Saturated red in the lower-left, where the attract movie's logo sits."""
    from PIL import Image

    im = Image.open(path).convert("RGB")
    w, h = im.size
    crop = im.crop((int(w * 0.05), int(h * 0.75), int(w * 0.45), int(h * 0.95)))
    px = crop.load()
    cw, ch = crop.size
    red = total = 0
    for y in range(0, ch, 2):
        for x in range(0, cw, 2):
            r, g, b = px[x, y]
            total += 1
            if r > 170 and g < 90 and b < 90:
                red += 1
    return red / max(1, total)


def game_state(shot_path=None):
    """Classify what is on screen: 'gameplay', 'movie', 'loading', 'menu', 'title'.

    The 'movie' case is the important one and the least obvious. Leave Mirror's
    Edge sitting on its menu and it eventually plays an **attract-mode movie** -
    a full-screen video of real gameplay footage. It has no menu red, plenty of
    scene detail, and looks exactly like play. It was mistaken for gameplay once
    already, which produced a completely bogus "you can boot straight to a level
    from the command line" conclusion: the map URL was never honoured, the game
    simply idled into its demo reel.

    Three independent measurements, because each alone is fooled:

    * **Brightness is useless.** The main menu and a sunny rooftop both average
      ~199. A whole FOV measurement was once taken of the menu's 3D city
      backdrop on that basis.

    * **Red coverage** separates the menus, which are dominated by saturated red
      UI, from gameplay which has almost none:
          story menu ~0.230, title ~0.066, gameplay 0.00003-0.019
      But a *loading screen* is a featureless white wall with no red either.

    * **Edge density** catches loading: ~6-13 against 21+ for a real scene.

    * **The watermark** catches the attract movie, which passes all of the above.

    Measured separations:
        menu   red ~0.230        title    red ~0.066
        loading detail 6-13      gameplay detail 24-27, watermark 0.000
        movie   watermark ~0.126, and ~30 FPS against the engine's 60
    """
    path = shot_path or shot("_state_probe")["path"]
    all_red = _red_fraction(path)
    left_red = _red_fraction(path, left_only=True)
    detail = _detail(path)

    if left_red > 0.30 or all_red > 0.15:
        return "menu"
    if detail < 18.0:
        return "loading"
    # Must be tested before anything can be called gameplay.
    if _movie_watermark(path) > 0.05:
        return "movie"
    if all_red > 0.04:
        return "title"
    if all_red < 0.03:
        return "gameplay"
    return "unknown"


# Level URLs known to boot to a playable state, taken from the console history
# Mirror's Edge itself wrote into [TdGame.TdConsole] in TdInput.ini.
#
# Tutorial_p is deliberately absent: it loads (the pause menu reports "TRAINING
# AREA" and the intro camera renders real geometry) but the handoff to the
# player view leaves the screen pure white. Booting it cold appears to skip
# setup the story flow normally performs. Reference imagery can still be taken
# during its intro pan.
#   Edge_Start   the level's own beginning, which plays the opening cinematic
#   After_Intro  the checkpoint just past it - same level, no cinematic
# After_Intro is the default: the intro is several seconds of camera work that
# has to be sat through on every launch and yields no useful reference frames.
LEVEL_CHECKPOINTS = {
    "edge_p": {"default": "After_Intro",
               "checkpoints": ["Edge_Start", "After_Intro"]},
}

_URL_SUFFIX = "?AllowCPSaving=TRUE?AllowLevelAchievements=TRUE"


def level_url(map_name="edge_p", checkpoint=None):
    """Build a startup URL for a level, defaulting to its post-intro checkpoint."""
    info = LEVEL_CHECKPOINTS.get(map_name)
    if not info:
        return map_name
    cp = checkpoint or info["default"]
    return "%s?LoadCheckpoint=%s%s" % (map_name, cp, _URL_SUFFIX)


LEVEL_URLS = {name: level_url(name) for name in LEVEL_CHECKPOINTS}

MAP_GAMEINFO = {
    "Tutorial_p": "TdGame.TdSpTutorialGame",
    "TdMainMenu": "TdGame.TdMenuGameInfo",
}


def boot_to_map(map_name="edge_p", url=None, checkpoint=None, timeout=300,
                shot_prefix="map", settle=3):
    """Launch straight into a playable level, skipping the menus.

    The map URL is ignored on the *command line*, but the startup map is also
    config-driven: `[URL] Map=` / `LocalMap=` in TdEngine.ini normally point at
    TdMainMenu, which is why every launch lands on the menu. Repointing them at
    a level URL boots it directly - about 35 s to a controllable player, against
    roughly three minutes of menu navigation.

    Beware two traps when judging success:
      * idling on the menu starts an **attract movie** that looks like gameplay
        (see game_state); and
      * a level can load and render its intro camera while the player view
        never appears (Tutorial_p does exactly this).
    This waits for `settle` consecutive gameplay classifications before
    returning, so neither is mistaken for a working boot.
    """
    from . import probe_setup

    if not url:
        if map_name in LEVEL_CHECKPOINTS:
            url = level_url(map_name, checkpoint)
        else:
            game_class = MAP_GAMEINFO.get(map_name)
            url = "%s?Game=%s" % (map_name, game_class) if game_class else map_name

    if is_running():
        kill()
        time.sleep(2)

    probe_setup.set_startup_map(url)

    t0 = time.time()
    launch()

    st = "unknown"
    seen = []
    stable = 0
    i = 0
    while time.time() - t0 < timeout:
        time.sleep(5)
        info = shot("%s_%02d" % (shot_prefix, i))
        st = game_state(info["path"])
        if not seen or seen[-1] != st:
            seen.append(st)
        i += 1
        stable = stable + 1 if st == "gameplay" else 0
        if stable >= settle:
            return {"loaded": True, "state": st, "seconds": round(time.time() - t0, 1),
                    "shot": info, "map": map_name, "url": url, "sequence": seen}

    raise RuntimeError("did not reach playable %s within %ds (last state: %s, "
                       "sequence: %s)" % (map_name, timeout, st, " -> ".join(seen)))


# --- the Training Area (SP00 Tutorial) ----------------------------------------
#
# Booting Tutorial_p directly loads it, but the world comes up EMPTY - pure
# white, no geometry from any angle. The reason is structural, and visible in
# the package: Edge_p carries 3 LevelStreamingVolumes and 3
# SeqAct_MultiLevelStreaming, so its sublevels stream in from the player's
# position; Tutorial_p has NEITHER, only LevelStreamingKismet. Its sublevels
# load only when a Kismet sequence says so, and that sequence never runs when
# the map is entered cold.
#
# Loading a sublevel as the persistent map instead (Map=Tutorial_LW) does load
# the data, but there is no PlayerStart - the player falls to z = -262138 and
# the level never becomes playable.
#
# So the tutorial is entered the way the game enters it: main menu -> STORY ->
# PLAY CHAPTER -> TRAINING AREA. That runs the intended Kismet flow and the
# sublevels stream normally.
#
# SAFETY: "NEW GAME" sits directly below "PLAY CHAPTER" and its confirmation
# dialog ERASES the save. The menu's initial selection is not dependable - one
# "down" from the top was observed landing on NEW GAME, not PLAY CHAPTER - so
# this never counts keystrokes. It reads the highlight off the screen and only
# presses Enter once PLAY CHAPTER is confirmed selected.

_MENU_ROWS = (("continue", 348), ("chapter", 401), ("newgame", 455))


def _menu_selection(shot_name="menu_probe"):
    """Which STORY item is highlighted, read off the screen.

    The selected row is drawn as a white bar across the red panel. Sampling a
    slice to the right of the text avoids the glyphs, which otherwise drag the
    average around and make the rows indistinguishable.
    """
    from PIL import Image

    shot(shot_name)
    path = os.path.join(paths.build_dir("me_shots"), shot_name + ".png")
    im = Image.open(path).convert("RGB")
    best, best_score = None, 0.0
    for name, y in _MENU_ROWS:
        vals = []
        for yy in range(y - 10, y + 11, 2):
            for xx in range(330, 376, 3):
                r, g, b = im.getpixel((xx, yy))
                vals.append((r + g + b) / 3.0)
        score = sum(vals) / len(vals)
        if score > best_score:
            best, best_score = name, score
    # A white highlight bar reads near 255; unselected red panel is far darker.
    return (best if best_score > 180 else None), best_score


def boot_to_tutorial(timeout=300, shot_prefix="tut", max_steps=6):
    """Launch and drive the menus into the SP00 Training Area.

    Returns the same dict shape as boot_to_map(). Raises rather than guessing if
    the menu cannot be read - blind keystrokes here can wipe the save.
    """
    kill()
    time.sleep(2)
    restore_menu_boot()
    launch()

    deadline = time.time() + timeout
    hwnd = None
    while time.time() < deadline and not hwnd:
        hwnd, _t = desktop.find_window_by_process(EXE)
        time.sleep(1)
    if not hwnd:
        raise RuntimeError("game window never appeared")
    desktop.ensure_focus(hwnd)

    # Title card: "Press Any Key".
    while time.time() < deadline and game_state() == "title":
        desktop.key("space")
        time.sleep(3)
    desktop.ensure_focus(hwnd)
    time.sleep(2)

    sel, score = _menu_selection(shot_prefix + "_menu")
    if sel is None:
        raise RuntimeError("cannot read the STORY menu highlight (score %.1f); "
                           "refusing to press keys blindly next to NEW GAME"
                           % score)

    steps = 0
    while sel != "chapter":
        if steps >= max_steps:
            raise RuntimeError("could not select PLAY CHAPTER (stuck on %r)" % sel)
        desktop.key("up" if sel == "newgame" else "down")
        time.sleep(1.2)
        sel, score = _menu_selection(shot_prefix + "_menu")
        if sel is None:
            raise RuntimeError("lost the menu highlight mid-navigation")
        steps += 1

    desktop.key("enter")          # -> PLAY CHAPTER, chapter carousel
    time.sleep(4)
    desktop.key("enter")          # -> TRAINING AREA, the first chapter
    time.sleep(5)

    seq = []
    while time.time() < deadline:
        st = game_state()
        if not seq or seq[-1] != st:
            seq.append(st)
        if st == "gameplay":
            # Streaming lands a little after the first gameplay frame.
            time.sleep(6)
            return {"loaded": True, "state": "gameplay", "map": "Tutorial_p",
                    "via": "menu:PLAY CHAPTER/TRAINING AREA",
                    "seconds": timeout - (deadline - time.time()),
                    "sequence": seq, "shot": shot(shot_prefix + "_loaded")}
        time.sleep(4)
    raise RuntimeError("Training Area did not become playable (sequence: %s)" % seq)


def restore_menu_boot():
    """Undo boot_to_map: send the game back to its normal main-menu startup."""
    from . import probe_setup
    return probe_setup.set_startup_map("TdMainMenu")


def boot_to_level(timeout=300, shot_prefix="boot"):
    """Launch and drive the menus until gameplay is actually running.

    Console commands bound to keys only take effect inside a level, so every
    probe has to get through: title screen -> main menu -> Continue -> load.
    The game writes no log, so progress is judged from screenshots - but by
    classifying them (game_state) rather than thresholding brightness.

    Raises if gameplay is not reached, so a caller can never silently probe the
    menu believing it is in a level.
    """
    if not is_running():
        launch()
    time.sleep(8)

    hwnd, _t = desktop.find_window_by_process(EXE)
    desktop.ensure_focus(hwnd)
    shot("%s_00_title" % shot_prefix)

    # Title screen: any key. Then the menu, where CONTINUE GAME is preselected.
    for attempt in range(3):
        st = game_state(shot("%s_01_state%d" % (shot_prefix, attempt))["path"])
        if st == "title":
            desktop.ensure_focus(hwnd)
            desktop.key("space")
            time.sleep(18)
        elif st == "menu":
            break
        else:
            time.sleep(5)

    deadline = time.time() + timeout
    i = 0
    pressed = False
    while time.time() < deadline:
        info = shot("%s_02_probe%02d" % (shot_prefix, i))
        st = game_state(info["path"])
        if st == "gameplay":
            return {"loaded": True, "shot": info, "state": st}
        if st == "menu" and not pressed:
            desktop.ensure_focus(hwnd)
            desktop.key("enter")
            pressed = True
        elif st == "menu" and i % 6 == 5:
            # Enter did not take (focus can be stolen); try again.
            desktop.ensure_focus(hwnd)
            desktop.key("enter")
        i += 1
        time.sleep(10)

    raise RuntimeError("did not reach gameplay within %ds (last state: %s)"
                       % (timeout, st))


def describe():
    hwnd, title = desktop.find_window_by_process(EXE)
    if not hwnd:
        return {"running": is_running(), "hwnd": None}
    rect = desktop.window_rect(hwnd)
    return {"running": True, "hwnd": hwnd, "title": title, "rect": rect,
            "size": (rect[2] - rect[0], rect[3] - rect[1])}


if __name__ == "__main__":
    import sys

    if "--kill" in sys.argv:
        print("killed:", kill())
    elif "--status" in sys.argv:
        print(describe())
    elif "--shot" in sys.argv:
        print(shot("manual"))
    else:
        print("launching...")
        hwnd, title, rect = launch()
        print("window: %r hwnd=%s rect=%s size=%s"
              % (title, hwnd, rect, (rect[2] - rect[0], rect[3] - rect[1])))
        time.sleep(3)
        print(shot("after_launch"))
