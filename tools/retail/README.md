# Retail recorder and replay harness (`tools/retail/`)

Record a person playing the **retail** Mirror's Edge, then replay the same run, input for input, through this engine's `ParkourController` against the same level's real collision, and see frame by frame where the two part.

The recorder half comes from [tesseract](https://github.com/pippy360/tesseract)'s `tools/medge/`, where it was built to validate a different port of the game. It is unchanged apart from the package name, and its comments still point at that repo's docs. The replay back end (`replay.py`, `src/tools/replay_main.cpp`) is new and written for this engine.

## The pieces

| File | Runs on | What it is |
|---|---|---|
| `hook/d3d9_proxy.cpp`, `hook/build.py` | Windows | A `d3d9.dll` proxy loaded by the shipping `MirrorsEdge.exe`. Each frame it reads the pawn out of the game's object table: Location, Velocity, move, physics, the controller's yaw/pitch and the animation and sound records. It also logs every key the window receives, and streams it all to shared memory (`medge_telemetry_v6`). |
| `telemetry.py`, `movenames.py` | Windows | Reads that shared-memory ring. |
| `drive.py`, `desktop.py` | Windows | Boots the game to a map, kills it, takes screenshots. |
| `record_session.py`, `tracefile.py` | Windows | Drains the ring to a JSONL trace while a human plays. |
| `trace.py` | anywhere | Reads a trace. Cuts paused stretches and recovers keys held from before the recorder attached. Picks where the view comes from, moves each key to the recorded frame that took it, and rebuilds the frame state retail's `PlayerMove` carries (sprint energy, AccelerationTime, the walk-stop, the jump chain). Copied from tesseract's `replay.py`. |
| `replay.py` | anywhere | Turns a trace into a `me_replay` script, runs it and scores the result. |
| `intro_capture.py` | Windows | Boots retail at each chapter's first checkpoint and records the start-of-level intro (camera, pawn, sounds, optionally a frame a second). No key is sent to the game. |
| `intro_check.py` | Windows to play, anywhere to compare | Plays the port's intros with `--trace` and lays them over those recordings: eye position, view direction, the hand-over, and every sound cue. `docs/LEVEL_INTROS.md` has the method and the results. |
| `anim_check.py` | anywhere, with `me_anim` built | Runs the port's first-person animation tree (`me_anim`) over the recordings and scores what it plays against the `anim1p` records retail logged, per movement state. `docs/FIRST_PERSON_ANIMATION_RE.md` has the method and the results. |
| `src/tools/replay_main.cpp` → `me_replay` | anywhere | Headless: loads the level from the retail packages and steps the controller once per recorded retail frame. |

## 1. Record (Windows, retail installed)

```bash
pip install pillow                       # drive.py's screenshots
python -m tools.retail.hook.build        # build the x86 proxy and install it next to MirrorsEdge.exe
python -m tools.retail.record_session --name myrun --map escape_p
#   ...play; Ctrl-C (or quit the game) when done
#   -> build/retail/trials/<stamp>_myrun.jsonl
```

* Building the hook needs the VS 2022 Build Tools C++ workload (it calls `vcvarsall.bat amd64_x86`). The game is 32-bit.
* **Kill the game before rebuilding the hook** (`python -m tools.retail.drive --kill`). Windows locks a loaded DLL, so the install fails, and the next run would silently use the old hook.
* `python -m tools.retail.hook.build --remove` restores the original `d3d9.dll`.
* `--no-boot` attaches to a game that is already running. Use `edge_p` or `escape_p` to record movement. The Training Area is a scripted tutorial that gates input.
* Never press keys blindly in the game's menus. "NEW GAME" sits right under "PLAY CHAPTER", and its confirmation erases the save.

### Level intros

```bash
python -m tools.retail.intro_capture --frames 1.0     # -> build/retail/intros/<map>.jsonl (+ <map>_frames/)
python -m tools.retail.intro_check play build-win/mirrorsedge_windows.exe
python -m tools.retail.intro_check compare
```

* `intro_capture` restarts the game once per chapter and points `[URL] Map` / `LocalMap` of your `TdEngine.ini` at it for each launch, putting them back at the end. It copies the save folder first and compares it afterwards. Do not run it while something else is driving the game.
* A camera position in any retail trace is 10 uu ahead of the eye, along the view direction: the hook solves it out of the view-projection, whose depth column carries the near plane. `intro_check` takes it out; anything else that compares camera positions has to as well.
* A sample's camera fields (`x y z yaw pitch roll`) are up to a frame older than its pawn fields (`px py pz`, `cyaw cpitch`, `anim1p`): the hook reads the camera out of the frame being presented and the pawn out of the game thread as it is at that moment, which runs up to a frame ahead. In stretches the two are a sample apart (about two thirds of a recording) and in others level; across a hitch two or three apart. Sample for sample the camera then seems to trail the pawn by a frame's travel (10 uu at 600 uu/s). Anything that compares the two has to find the lag per stretch.
* `anim1p` is the three heaviest sequence players and no more. A sequence under a per-bone blend's target (the weapon arm's `standready`, `standfire`) does not show in it at all.
* For about six seconds at the start of a chapter the trace's yaw and pitch read 0 and 90, and no sound is logged for the first second or so.

## 2. Replay

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target me_replay
python -m tools.retail.replay --trace build/retail/trials/<stamp>_myrun.jsonl
python -m tools.retail.replay --window 7       # one 4 s window, with its frame-by-frame detail
python -m tools.retail.replay --segment 2      # shorter re-anchored windows
```

The install is found through Steam, or set `MEDGE_ME_INSTALL`. The level defaults to the recording's own map, found under `CookedPC/Maps`; `--level Maps/SP01/Escape_p.me1` overrides it. Output goes to `build/retail/replay/`:

* `<stem>_report.txt`: one row per window, with its status (`reproduces`, `FELL`, `short N uu`, `long N uu`, `off line`, `height differs`), the end gap, the worst gap, and when the paths parted;
* `<stem>_<window>.txt`: retail and port side by side every 4 frames, with the keys in the window;
* `<stem>_windows.json`, `<stem>_script.txt`, `<stem>_port.txt`, `<stem>_me_replay.log`.

On Windows with MinGW (WinLibs g++, CMake, Ninja), zlib has to be found by hand, and `zlib1.dll` must sit next to the exe:

```bash
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DZLIB_INCLUDE_DIR=<dir with zlib.h> -DZLIB_LIBRARY=<path to zlib1.dll>
cmake --build build-win --target me_replay
```

On non-Apple platforms CMake builds only `me_replay`; the Metal app stays macOS-only.

## How a window is replayed

* **One step per retail frame.** Frame *k* of a window anchored on sample *i0* is stepped with `dt = t[i0+k] - t[i0+k-1]`. It gets the keys that frame took and the view it turned to (sample *i0+k*'s), so port frame *k* and retail sample *i0+k* are the same moment. The view arrives as a mouse delta, because the controller's turn damping reads one.
* **The anchor** is the retail frame's feet (`pz - 93`, or `- 64` on the 122 uu short capsule), lifted 3 uu to settle. With it go the velocity, the view, and `ParkourController::anchor`'s state. Windows prefer to anchor where retail is walking, because a wall run or a grab has state an anchor cannot hand over.
* **No coordinate conversion.** This engine works in UE units in the level's own space, as retail does.
* Key bits: W S A D, jump, crouch (left shift), Q and the left button (the barge). A jump pressed and released inside one frame still counts as a press there; Q and the left button are presses, never holds.

## Traps carried over from tesseract

All of these were paid for there; see `trace.py` for the detail.

* A key already down when the recorder attaches arrives only as its release (`held_from_start`).
* Turning noclip on posts a release for every blocked key; those are dropped.
* A paused game keeps the recorder's clock running. Frozen samples are cut out and the held keys released (`excise_pauses`).
* A device Reset can freeze the camera record while the pawn record stays live. The view then comes from the controller rotation (telemetry v5+), or failing that from the pawn's velocity (`stamp_look`).
* A key's timestamp is when the window got it, not the frame that used it (`frame_start_keys`).
