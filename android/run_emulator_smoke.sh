#!/usr/bin/env bash
# The Android smoke test on the emulator (or whatever device adb sees): installs the APK, pushes a
# minimal asset subset if it is not there yet, runs chapter 0 for a few hundred frames with
# --exit-screenshot, follows logcat until the process exits, and pulls the screenshot.
#
#   android/run_emulator_smoke.sh [output dir]           (default /tmp/android_smoke)
#
# Environment:
#   ME_GAME_ROOT   the retail install to push from (default ~/mirrorsedge)
#   ME_APK         the APK (default app/build/outputs/apk/debug/app-debug.apk, built if missing)
#   ME_AVD         the AVD to start when no device is connected (default Medium_Phone)
#   ME_FRAMES      --max-frames (default 400)
#   ME_TIMEOUT     seconds to wait for the run (default 900: the first run compiles every material)
#   ME_ARGS        the whole command line, replacing the default one
#   ME_PUSH_ALL=1  push the complete TdGame rather than the subset
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-/tmp/android_smoke}"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
ADB="${ADB:-${SDK}/platform-tools/adb}"
EMULATOR="${SDK}/emulator/emulator"
GAME_ROOT="${ME_GAME_ROOT:-$HOME/mirrorsedge}"
APK="${ME_APK:-${HERE}/app/build/outputs/apk/debug/app-debug.apk}"
AVD="${ME_AVD:-Medium_Phone}"
FRAMES="${ME_FRAMES:-400}"
TIMEOUT="${ME_TIMEOUT:-900}"
PKG=com.pippy360.mierrorsedgere
ACTIVITY="${PKG}/.MirrorsEdgeActivity"
FILES="/sdcard/Android/data/${PKG}/files"
DEST="${FILES}/mirrorsedge"
SHOT="screenshots/android_smoke.png"
ARGS="${ME_ARGS:---chapter 0 --max-frames ${FRAMES} --exit-screenshot ${SHOT}}"

mkdir -p "${OUT}"
log() { echo "[smoke] $*"; }

command -v "${ADB}" >/dev/null || { echo "adb not found at ${ADB}" >&2; exit 1; }

# 1. A device: the one connected, else the AVD.
device_ready() { "${ADB}" get-state >/dev/null 2>&1 && [ "$("${ADB}" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]; }
if ! "${ADB}" devices | awk 'NR>1 && $2=="device"' | grep -q .; then
    [ -x "${EMULATOR}" ] || { echo "no device connected and no emulator at ${EMULATOR}" >&2; exit 1; }
    log "no device connected: starting AVD ${AVD}"
    nohup "${EMULATOR}" -avd "${AVD}" -no-snapshot-load -gpu auto -no-boot-anim >"${OUT}/emulator.log" 2>&1 &
    "${ADB}" wait-for-device
fi
log "waiting for boot"
for _ in $(seq 1 180); do
    device_ready && break
    sleep 2
done
device_ready || { echo "device did not finish booting" >&2; exit 1; }
"${ADB}" shell input keyevent 82 >/dev/null 2>&1 || true   # past the lock screen
log "device: $("${ADB}" shell getprop ro.product.model | tr -d '\r'), Android $("${ADB}" shell getprop ro.build.version.release | tr -d '\r'), GLES $("${ADB}" shell getprop ro.opengles.version | tr -d '\r')"

# 2. The APK.
if [ ! -f "${APK}" ]; then
    log "building the APK"
    (cd "${HERE}" && ./gradlew assembleDebug)
fi
log "installing ${APK}"
"${ADB}" install -r -g "${APK}" >/dev/null
# A first launch creates Android/data/<pkg>/files; stop it again at once.
"${ADB}" shell "am start -W -n ${ACTIVITY} --es args \"--max-frames 1\"" >/dev/null 2>&1 || true
sleep 3
"${ADB}" shell am force-stop "${PKG}" || true
"${ADB}" shell mkdir -p "${DEST}" "${FILES}/screenshots"

# 3. The assets: a subset that plays chapter 0 (the training area). Everything in TdGame except
#    Maps/SP01..SP09 and Movies: Config, Localization, Splash, CookedPC's loose packages, Maps/SP00,
#    Maps/Menu, Maps/Entry.upk and every other CookedPC directory.
if [ -d "${GAME_ROOT}/TdGame/CookedPC" ]; then
    if "${ADB}" shell "test -d ${DEST}/TdGame/CookedPC/Maps/SP00" 2>/dev/null; then
        log "assets already on the device at ${DEST}"
    else
        log "pushing assets from ${GAME_ROOT} (this takes a while)"
        push() {  # push <relative path under TdGame>
            local rel="$1"
            [ -e "${GAME_ROOT}/TdGame/${rel}" ] || return 0
            "${ADB}" shell mkdir -p "${DEST}/TdGame/$(dirname "${rel}")"
            # (adb push reads stdin; keep it off any list being iterated)
            "${ADB}" push "${GAME_ROOT}/TdGame/${rel}" "${DEST}/TdGame/$(dirname "${rel}")/" </dev/null >/dev/null
        }
        if [ "${ME_PUSH_ALL:-0}" = "1" ]; then
            "${ADB}" push "${GAME_ROOT}/TdGame" "${DEST}/" </dev/null >/dev/null
        else
            for top in Config Localization Splash; do push "${top}"; done
            # CookedPC: the loose files first, then each directory but Maps, then Maps' subset.
            for f in "${GAME_ROOT}"/TdGame/CookedPC/*; do
                name="$(basename "${f}")"
                if [ -f "${f}" ]; then
                    push "CookedPC/${name}"
                elif [ -d "${f}" ] && [ "${name}" != "Maps" ]; then
                    push "CookedPC/${name}"
                fi
            done
            for m in "${GAME_ROOT}"/TdGame/CookedPC/Maps/*; do
                name="$(basename "${m}")"
                case "${name}" in SP0[1-9]) continue ;; esac
                push "CookedPC/Maps/${name}"
            done
        fi
        log "assets pushed"
    fi
else
    log "no retail install at ${GAME_ROOT}: assuming the device already has one at ${DEST}"
fi

# 4. Run it, following logcat until the process is gone.
"${ADB}" shell rm -f "${FILES}/${SHOT}" || true
"${ADB}" logcat -c || true
log "starting: ${ARGS}"
# adb shell hands the words to the device's shell, which splits them again: the whole command
# goes as one string so the args extra keeps its spaces.
"${ADB}" shell "am start -W -n ${ACTIVITY} --es args \"${ARGS}\"" >/dev/null
sleep 2
PID="$("${ADB}" shell pidof "${PKG}" | tr -d '\r' || true)"
log "pid ${PID:-?}; logcat -> ${OUT}/logcat.txt"
"${ADB}" logcat -v time -s mirrorsedge SDL AndroidRuntime DEBUG libc >"${OUT}/logcat.txt" 2>&1 &
LOGCAT_PID=$!
trap 'kill ${LOGCAT_PID} 2>/dev/null || true' EXIT
start=$(date +%s)
status="timeout"
while :; do
    if ! "${ADB}" shell pidof "${PKG}" >/dev/null 2>&1; then status="exited"; break; fi
    if grep -q "Reached max-frames limit\|Shutdown cleanly" "${OUT}/logcat.txt" 2>/dev/null; then
        status="finished"
        sleep 3
        break
    fi
    if [ $(( $(date +%s) - start )) -ge "${TIMEOUT}" ]; then break; fi
    sleep 2
done
"${ADB}" shell am force-stop "${PKG}" || true
log "run ${status} after $(( $(date +%s) - start )) s"
tail -n 25 "${OUT}/logcat.txt" | sed 's/^/[logcat] /'

# 5. The screenshot.
if "${ADB}" pull "${FILES}/${SHOT}" "${OUT}/android_smoke.png" >/dev/null 2>&1; then
    log "screenshot: ${OUT}/android_smoke.png"
else
    log "no screenshot at ${FILES}/${SHOT}"
    grep -n "FATAL\|SIGSEGV\|SIGABRT\|backtrace\|ERROR" "${OUT}/logcat.txt" | head -20 || true
    exit 1
fi
[ "${status}" = "finished" ] || [ "${status}" = "exited" ]
