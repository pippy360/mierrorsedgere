# Mirror's Edge on Android

The game as an Android app: `libmain.so` (the engine with the OpenGL ES 3.0 renderer, built from
the repository's `CMakeLists.txt` with the NDK) inside SDL2's Java activity. arm64-v8a only,
minSdk 26, targetSdk 35. The fuller story is in `docs/ANDROID_PORT.md`.

## Prerequisites

- Android SDK with platform `android-35`, build-tools 35, NDK **27.0.12077973** and
  platform-tools (`adb`). The scripts assume `~/Library/Android/sdk`.
- A JDK 17. Android Studio's bundled one works:
  `export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"`
- CMake 3.22 or newer **and** ninja on the `PATH` (Homebrew's CMake 4.x is fine). The SDK's own
  `cmake/` package is not needed: `local.properties` points the Android Gradle plugin at the
  Homebrew prefix with `cmake.dir=/opt/homebrew` (it runs `<cmake.dir>/bin/cmake` and finds
  `ninja` next to it or on the `PATH`). If you install the SDK's CMake instead, drop that line.
- Gradle 8.11.1 through the wrapper (`./gradlew`; downloaded on first use if not cached).
- `curl` and `tar`, for the third-party sources.

## Build

```bash
cd android
./fetch_deps.sh                       # SDL2 2.32.x, OpenAL Soft 1.24.x, libogg, libvorbis -> android/third_party/
cat > local.properties <<EOF          # machine-local, gitignored
sdk.dir=$HOME/Library/Android/sdk
cmake.dir=/opt/homebrew
EOF
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
./gradlew assembleDebug               # -> app/build/outputs/apk/debug/app-debug.apk
```

`fetch_deps.sh` is idempotent. SDL2's Java classes (`org.libsdl.app.*`) are compiled straight
from the fetched tree through a `sourceSets` entry, so nothing third-party is committed.

What ends up in the APK's `lib/arm64-v8a/`: `libSDL2.so`, `libmain.so` (the game, with OpenAL
Soft, ogg, vorbis and vorbisfile linked in statically) and `libc++_shared.so`.

There is no FFmpeg on Android: the Bink chapter movies are skipped (`ME_NO_FFMPEG`) and the game
goes straight to the level's in-engine intro, as it does on the desktop when a `.bik` is missing.

## Install and run

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.pippy360.mierrorsedgere/.MirrorsEdgeActivity   # once: creates the files dir
```

The assets go into the app's external files directory (no storage permission needed, and `adb`
can write there):

```bash
DEST=/sdcard/Android/data/com.pippy360.mierrorsedgere/files/mirrorsedge
adb shell mkdir -p $DEST
adb push ~/mirrorsedge/TdGame $DEST/TdGame        # the whole install: several GB, takes a while
```

The game looks for `TdGame/CookedPC` under `<external files dir>/mirrorsedge`, then
`/sdcard/mirrorsedge`; `--game-root` and `MEDGE_ME_INSTALL` override it as on the desktop.

### Command line

The desktop options work (`--chapter N`, `--level`, `--max-frames N`, `--trace`,
`--exit-screenshot`, `--game-root`, ...). They are read from, in order:

1. the intent extra `args` (one `adb shell` string: the device's shell splits the words again
   otherwise and only `--chapter` would arrive):
   ```bash
   adb shell 'am start -n com.pippy360.mierrorsedgere/.MirrorsEdgeActivity \
       --es args "--chapter 0 --max-frames 600 --exit-screenshot screenshots/run.png"'
   ```
2. `<external files dir>/me_args.txt` (one command line, `#` comments allowed):
   ```bash
   adb shell 'echo "--chapter 1" > /sdcard/Android/data/com.pippy360.mierrorsedgere/files/me_args.txt'
   ```
3. nothing: the main menu.

The working directory is the external files directory, so `screenshots/` and relative paths land
there.

### Controls

Touch: the left 40 % of the screen is a move stick (where the finger lands is its centre), the
rest is look-by-drag, with buttons for JUMP, CROUCH, ACTION (use / disarm), ATTACK, 180, RT
(reaction time) and MENU. Android's Back button is Escape. A Bluetooth gamepad or keyboard works
as on the desktop.

### Logs and screenshots

```bash
adb logcat -s mirrorsedge SDL          # everything the game prints (stdout/stderr -> logcat)
adb pull /sdcard/Android/data/com.pippy360.mierrorsedgere/files/screenshots ./android_shots
```

## Emulator smoke test

`run_emulator_smoke.sh` installs the APK on the connected device (starting the `Medium_Phone`
AVD if none is attached), pushes a minimal asset subset (everything but `Maps/SP01..SP09` and
`Movies`) when it is not there yet, runs chapter 0 for 400 frames with
`--exit-screenshot screenshots/android_smoke.png`, follows logcat until the process exits and
pulls the screenshot:

```bash
./run_emulator_smoke.sh /tmp/android_smoke          # output directory for the PNG and the log
```

The emulator reports OpenGL ES 3.0 only, hence the manifest's `glEsVersion="0x00030000"`.

## Optional: FFmpeg for the Bink movies

Not wired up. The hook is `ME_NO_FFMPEG` in `src/cutscene/cutscene_player.cpp`: a build with
FFmpeg prebuilts for arm64-v8a would drop that define from the `main` target in `CMakeLists.txt`
and link `avformat avcodec swscale swresample avutil` from `<prefix>/arm64-v8a/{include,lib}`.
