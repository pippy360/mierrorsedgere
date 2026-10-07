"""Screen capture and synthetic keyboard input for driving Mirror's Edge.

Pure ctypes against user32/gdi32 - no extra dependencies beyond Pillow, which
the repo already uses.

Two things matter for games specifically:

  * Input must be sent as **scan codes** (KEYEVENTF_SCANCODE), not virtual key
    codes. DirectInput and raw-input readers look at scan codes and ignore the
    virtual-key field that SendKeys/keybd_event populate, so the usual
    automation approaches silently do nothing in-game.

  * Capture of a fullscreen-exclusive D3D9 swapchain via GDI BitBlt can come
    back black. `capture()` reports the mean pixel value so a black frame is
    detectable rather than silently wrong; windowed mode avoids the problem.
"""

import contextlib
import ctypes
import ctypes.wintypes as wt
import time

from PIL import Image

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)


def _make_dpi_aware():
    """Without this, GetSystemMetrics reports DPI-scaled logical pixels.

    On a 3840x2160 display at 250% scaling that means capturing a 1536x864
    region and getting a zoomed-in crop of the top-left of the screen.
    """
    try:
        ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)  # PER_MONITOR_DPI_AWARE
        return
    except Exception:
        pass
    try:
        user32.SetProcessDPIAware()
    except Exception:
        pass


_make_dpi_aware()

SRCCOPY = 0x00CC0020
CAPTUREBLT = 0x40000000

INPUT_KEYBOARD = 1
KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008

SW_RESTORE = 9


# Stamped into dwExtraInfo of every event this module injects. Low-level hooks
# receive the field verbatim, which makes "ours vs everyone else's" decidable -
# the INJECTED flag alone is not: on this machine every HUMAN event also
# arrives flagged injected (measured: 33 keys + 33 mouse events during a block
# test, all injected, zero physical - something in the input stack, a remote
# desktop or peripheral driver, re-injects hardware input via SendInput).
INJECT_TAG = 0x4D454447          # "MEDG"


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_void_p)]


class _INPUTunion(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("padding", ctypes.c_ubyte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _INPUTunion)]


# Scan codes (set 1). Extended keys are flagged separately.
SCAN = {
    "escape": 0x01, "1": 0x02, "2": 0x03, "3": 0x04, "4": 0x05, "5": 0x06,
    "6": 0x07, "7": 0x08, "8": 0x09, "9": 0x0A, "0": 0x0B, "minus": 0x0C,
    "equals": 0x0D, "backspace": 0x0E, "tab": 0x0F,
    "q": 0x10, "w": 0x11, "e": 0x12, "r": 0x13, "t": 0x14, "y": 0x15,
    "u": 0x16, "i": 0x17, "o": 0x18, "p": 0x19,
    "lbracket": 0x1A, "rbracket": 0x1B, "enter": 0x1C, "lctrl": 0x1D,
    "a": 0x1E, "s": 0x1F, "d": 0x20, "f": 0x21, "g": 0x22, "h": 0x23,
    "j": 0x24, "k": 0x25, "l": 0x26, "semicolon": 0x27, "quote": 0x28,
    "tilde": 0x29, "grave": 0x29, "lshift": 0x2A, "backslash": 0x2B,
    "z": 0x2C, "x": 0x2D, "c": 0x2E, "v": 0x2F, "b": 0x30, "n": 0x31,
    "m": 0x32, "comma": 0x33, "period": 0x34, "slash": 0x35, "rshift": 0x36,
    "lalt": 0x38, "space": 0x39, "capslock": 0x3A,
    "f1": 0x3B, "f2": 0x3C, "f3": 0x3D, "f4": 0x3E, "f5": 0x3F, "f6": 0x40,
    "f7": 0x41, "f8": 0x42, "f9": 0x43, "f10": 0x44, "f11": 0x57, "f12": 0x58,
    # Navigation cluster. These share scan codes with the numeric keypad and are
    # distinguished only by the extended-key flag, so they must also appear in
    # EXTENDED below or they arrive as numpad presses.
    "up": 0x48, "down": 0x50, "left": 0x4B, "right": 0x4D,
    "home": 0x47, "end": 0x4F, "pageup": 0x49, "pagedown": 0x51,
    "insert": 0x52, "delete": 0x53,
    "rctrl": 0x1D, "ralt": 0x38,
}
EXTENDED = {"rctrl", "ralt", "insert", "delete", "home", "end", "pageup",
            "pagedown", "up", "down", "left", "right", "numlock"}

# Characters typed as shift + base key.
SHIFTED = {
    "~": "grave", "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6",
    "&": "7", "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equals",
    "{": "lbracket", "}": "rbracket", ":": "semicolon", '"': "quote",
    "<": "comma", ">": "period", "?": "slash", "|": "backslash",
}
PLAIN = {
    " ": "space", "-": "minus", "=": "equals", "[": "lbracket",
    "]": "rbracket", ";": "semicolon", "'": "quote", ",": "comma",
    ".": "period", "/": "slash", "\\": "backslash", "`": "grave",
    "\n": "enter", "\t": "tab",
}


def _send(inputs):
    n = len(inputs)
    arr = (INPUT * n)(*inputs)
    sent = user32.SendInput(n, ctypes.byref(arr), ctypes.sizeof(INPUT))
    if sent != n:
        raise OSError("SendInput sent %d of %d (err %d)"
                      % (sent, n, ctypes.get_last_error()))


def _key_input(scan, up, extended=False):
    flags = KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if up else 0)
    if extended:
        flags |= KEYEVENTF_EXTENDEDKEY
    return INPUT(type=INPUT_KEYBOARD,
                 u=_INPUTunion(ki=KEYBDINPUT(wVk=0, wScan=scan, dwFlags=flags,
                                             time=0, dwExtraInfo=INJECT_TAG)))


def key(name, hold=0.03):
    """Press and release one named key."""
    name = name.lower()
    if name not in SCAN:
        raise KeyError("unknown key %r" % name)
    ext = name in EXTENDED
    _send([_key_input(SCAN[name], False, ext)])
    time.sleep(hold)
    _send([_key_input(SCAN[name], True, ext)])
    time.sleep(hold)


def cursor_clip():
    """The rectangle the cursor is currently confined to, in screen coords.

    Equals virtual_screen() when nothing is confining it. Mirror's Edge never
    calls ClipCursor during gameplay - fullscreen confined the pointer for it -
    so under the proxy's forced windowed mode the confinement is supplied by the
    hook instead. See drive.confine_cursor().
    """
    r = wt.RECT()
    user32.GetClipCursor(ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def virtual_screen():
    return (0, 0, user32.GetSystemMetrics(0), user32.GetSystemMetrics(1))


def cursor_pos():
    p = wt.POINT()
    user32.GetCursorPos(ctypes.byref(p))
    return (p.x, p.y)


def set_cursor_pos(x, y):
    """Move the OS cursor. Honours any active ClipCursor rectangle.

    Useful for testing confinement without generating mouse MOTION - unlike
    mouse_move(), this does not turn the game camera, so it can be used before
    sampling a reference pose.
    """
    user32.SetCursorPos(int(x), int(y))


def key_down(name):
    """Press one named key and leave it down.

    Separate from hold_key because some inputs overlap: a running jump is
    forward held *while* jump is tapped, and a version that released forward to
    press jump would be measuring a standing jump from a run-up. Always pair
    with key_up in a finally - a key left down survives the process exiting and
    the next thing the user does will have W stuck on.
    """
    name = name.lower()
    if name not in SCAN:
        raise KeyError("unknown key %r" % name)
    _send([_key_input(SCAN[name], False, name in EXTENDED)])


def key_up(name):
    """Release a key pressed with key_down. Safe to call when it is already up."""
    name = name.lower()
    if name not in SCAN:
        raise KeyError("unknown key %r" % name)
    _send([_key_input(SCAN[name], True, name in EXTENDED)])


def hold_key(name, seconds, settle=0.15):
    """Hold one named key down for `seconds`, then release.

    Use this rather than hand-rolling _key_input() calls: the navigation
    cluster shares scan codes with the numeric keypad and is told apart only by
    the extended-key flag, so a hand-rolled press of "down" arrives as numpad-2
    and the game never sees the arrow key at all.
    """
    key_down(name)
    try:
        time.sleep(seconds)
    finally:
        key_up(name)
    time.sleep(settle)


INPUT_MOUSE = 0
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.c_void_p)]


class _MINPUTunion(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("padding", ctypes.c_ubyte * 32)]


class MINPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _MINPUTunion)]


def mouse_move(dx, dy):
    """Relative mouse motion, for turning an in-game camera.

    Sent as a relative MOUSEEVENTF_MOVE rather than by warping the cursor:
    games read raw relative deltas, and SetCursorPos produces no motion event
    they will act on.
    """
    inp = MINPUT(type=INPUT_MOUSE,
                 u=_MINPUTunion(mi=MOUSEINPUT(dx=int(dx), dy=int(dy), mouseData=0,
                                              dwFlags=MOUSEEVENTF_MOVE, time=0,
                                              dwExtraInfo=INJECT_TAG)))
    arr = (MINPUT * 1)(inp)
    user32.SendInput(1, ctypes.byref(arr), ctypes.sizeof(MINPUT))


def click(hold=0.05):
    """One left-button press+release - the retail attack/interact button
    (Faith barges doors open with it)."""
    def _btn(flags):
        inp = MINPUT(type=INPUT_MOUSE,
                     u=_MINPUTunion(mi=MOUSEINPUT(dx=0, dy=0, mouseData=0,
                                                  dwFlags=flags, time=0,
                                                  dwExtraInfo=INJECT_TAG)))
        arr = (MINPUT * 1)(inp)
        user32.SendInput(1, ctypes.byref(arr), ctypes.sizeof(MINPUT))
    _btn(MOUSEEVENTF_LEFTDOWN)
    time.sleep(hold)
    _btn(MOUSEEVENTF_LEFTUP)


# --- blocking physical input while scripted input flows ------------------------
#
# Two mechanisms, tried in order:
#
#   * BlockInput() is the obvious API, but under UIPI it needs an ELEVATED
#     process (measured here: returns 0 with ERROR_ACCESS_DENIED from a normal
#     one), so it only ever works when the tooling happens to run as admin.
#
#   * Low-level hooks (WH_KEYBOARD_LL / WH_MOUSE_LL) need no elevation. The
#     callback swallows every event not stamped with our INJECT_TAG in
#     dwExtraInfo - a human wiggling the mouse mid-measurement - while this
#     module's own SendInput stream passes untouched. The tag, not the
#     INJECTED flag, is what separates them: on this machine the human's
#     input arrives flagged injected too (see INJECT_TAG).
#
# Both are recoverable by construction: Ctrl+Alt+Del bypasses either (the
# secure attention sequence cannot be blocked or hooked), hooks die with the
# installing thread so a crashed script cannot wedge the machine, and Windows
# silently REMOVES a low-level hook whose callback stalls past the system
# timeout - the failure mode is "input comes back", never "machine locked".

WH_KEYBOARD_LL = 13
WH_MOUSE_LL = 14
LLKHF_INJECTED = 0x10
LLMHF_INJECTED = 0x01
HC_ACTION = 0

_HOOKPROC = ctypes.WINFUNCTYPE(wt.LPARAM, ctypes.c_int, wt.WPARAM, wt.LPARAM)

user32.SetWindowsHookExW.restype = ctypes.c_void_p
user32.SetWindowsHookExW.argtypes = [ctypes.c_int, _HOOKPROC, wt.HINSTANCE,
                                     wt.DWORD]
user32.CallNextHookEx.restype = wt.LPARAM
user32.CallNextHookEx.argtypes = [ctypes.c_void_p, ctypes.c_int, wt.WPARAM,
                                  wt.LPARAM]
user32.UnhookWindowsHookEx.argtypes = [ctypes.c_void_p]


class _KBDLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [("vkCode", wt.DWORD), ("scanCode", wt.DWORD),
                ("flags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class _MSLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [("pt", wt.POINT), ("mouseData", wt.DWORD),
                ("flags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class _LLBlocker:
    """Swallows physical keyboard/mouse events via low-level hooks.

    The hooks live on a dedicated daemon thread (a low-level hook needs its
    installing thread to pump messages), so the main thread is free to sleep
    through hold_key() while the block stays live.
    """

    def __init__(self):
        import threading

        self._threading = threading
        self._thread = None
        self._tid = None
        self._ready = threading.Event()
        self._installed = False
        # Diagnostics: how many events the callbacks actually saw. "phys"
        # counts UNTAGGED events (anything not ours - hardware or the human's
        # re-injected input), "inj" counts our own tagged stream. If untagged
        # input leaks through while phys stays zero, the hook is dead or was
        # silently removed; if phys counts and input still leaks, the swallow
        # verdict is being ignored - different failures, different fixes.
        self.kbd_phys = 0
        self.kbd_inj = 0
        self.mouse_phys = 0
        self.mouse_inj = 0

    def _run(self):
        kernel32 = ctypes.WinDLL("kernel32")
        self._tid = kernel32.GetCurrentThreadId()

        # The callbacks must outlive this frame or ctypes frees them under the
        # hook's feet; keep them on self.
        #
        # The verdict is the dwExtraInfo TAG, not the INJECTED flag: everything
        # this module sends is stamped INJECT_TAG, and everything else - real
        # hardware or the human's input re-injected by a remote desktop or
        # peripheral driver - is swallowed. The flag cannot make that
        # distinction (see INJECT_TAG above for the measurement that proved
        # the human's events arrive flagged injected here).
        @_HOOKPROC
        def kbd(nCode, wParam, lParam):
            if nCode == HC_ACTION:
                info = ctypes.cast(lParam,
                                   ctypes.POINTER(_KBDLLHOOKSTRUCT)).contents
                if info.dwExtraInfo != INJECT_TAG:
                    self.kbd_phys += 1
                    return 1              # not ours: swallow
                self.kbd_inj += 1
            return user32.CallNextHookEx(None, nCode, wParam, lParam)

        @_HOOKPROC
        def mouse(nCode, wParam, lParam):
            if nCode == HC_ACTION:
                info = ctypes.cast(lParam,
                                   ctypes.POINTER(_MSLLHOOKSTRUCT)).contents
                if info.dwExtraInfo != INJECT_TAG:
                    self.mouse_phys += 1
                    return 1              # not ours: swallow
                self.mouse_inj += 1
            return user32.CallNextHookEx(None, nCode, wParam, lParam)

        self._kbd_cb, self._mouse_cb = kbd, mouse
        hk = user32.SetWindowsHookExW(WH_KEYBOARD_LL, kbd, None, 0)
        hm = user32.SetWindowsHookExW(WH_MOUSE_LL, mouse, None, 0)
        self._installed = bool(hk) and bool(hm)
        self._ready.set()
        if not self._installed:
            if hk:
                user32.UnhookWindowsHookEx(hk)
            if hm:
                user32.UnhookWindowsHookEx(hm)
            return

        msg = wt.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            pass                          # only WM_QUIT ever arrives

        user32.UnhookWindowsHookEx(hk)
        user32.UnhookWindowsHookEx(hm)

    def start(self, timeout=2.0):
        self._thread = self._threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        self._ready.wait(timeout)
        return self._installed

    def stop(self):
        WM_QUIT = 0x0012
        if self._tid:
            user32.PostThreadMessageW(self._tid, WM_QUIT, 0, 0)
        if self._thread:
            self._thread.join(1.0)


@contextlib.contextmanager
def block_input():
    """Block PHYSICAL keyboard and mouse while scripted input still flows.

    Tries BlockInput (works when elevated), then the low-level-hook blocker
    (works from a normal process). Yields True if a block took effect; False
    means "measurement may be contaminated by user input", which callers treat
    as a warning, not an error - the closed-loop controllers tolerate it.
    Ctrl+Alt+Del always restores control; see the block comment above.
    """
    blocker = None
    ok = bool(user32.BlockInput(True))
    if not ok:
        blocker = _LLBlocker()
        ok = blocker.start()
    try:
        yield ok
    finally:
        if blocker is not None:
            blocker.stop()
        elif ok:
            user32.BlockInput(False)


def type_text(text, delay=0.035):
    """Type a literal string, handling shifted characters."""
    for ch in text:
        if ch in SHIFTED:
            _send([_key_input(SCAN["lshift"], False)])
            key(SHIFTED[ch])
            _send([_key_input(SCAN["lshift"], True)])
        elif ch in PLAIN:
            key(PLAIN[ch])
        elif ch.isupper():
            _send([_key_input(SCAN["lshift"], False)])
            key(ch.lower())
            _send([_key_input(SCAN["lshift"], True)])
        elif ch.lower() in SCAN:
            key(ch.lower())
        else:
            raise KeyError("cannot type %r" % ch)
        time.sleep(delay)


# --- window handling ----------------------------------------------------------

def find_window(title_substr):
    """HWND of the first top-level visible window whose title contains the text."""
    result = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n:
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            if title_substr.lower() in buf.value.lower():
                result.append((hwnd, buf.value))
                return False
        return True

    user32.EnumWindows(cb, 0)
    return result[0] if result else (None, None)


def find_window_by_process(exe_name):
    """HWND of the main visible window belonging to a process.

    Matching on title is unreliable here - a browser tab named "mirrors edge ..."
    wins over the game itself - so resolve by owning process instead.
    """
    import subprocess

    pids = set()
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq %s" % exe_name, "/FO", "CSV", "/NH"],
            capture_output=True, text=True, timeout=20).stdout
        for line in out.splitlines():
            parts = [p.strip('"') for p in line.split('","')]
            if len(parts) > 1 and parts[0].lower() == exe_name.lower():
                pids.add(int(parts[1]))
    except Exception:
        return (None, None)
    if not pids:
        return (None, None)

    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids and user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(n + 1)
            if n:
                user32.GetWindowTextW(hwnd, buf, n + 1)
            found.append((hwnd, buf.value, _area(hwnd)))
        return True

    user32.EnumWindows(cb, 0)
    if not found:
        return (None, None)
    # The game's main window is the largest one it owns.
    found.sort(key=lambda t: -t[2])
    return found[0][0], found[0][1]


def _area(hwnd):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return (r.right - r.left) * (r.bottom - r.top)


def window_rect(hwnd):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def focus(hwnd, settle=0.6):
    """Bring a window to the foreground so it receives input.

    A bare SetForegroundWindow is usually refused: Windows only grants the
    foreground to the process that already owns it (or one responding to user
    input). The reliable workaround is to attach our input queue to the current
    foreground thread first, which puts us in the same "input context" and makes
    the call succeed. This matters far more in windowed mode - a fullscreen game
    already holds focus, so the earlier probes worked despite focus() failing.
    """
    if not hwnd:
        return False

    user32.ShowWindow(hwnd, SW_RESTORE)

    fg = user32.GetForegroundWindow()
    if fg == hwnd:
        return True

    cur_tid = ctypes.windll.kernel32.GetCurrentThreadId()
    fg_tid = user32.GetWindowThreadProcessId(fg, None) if fg else 0
    tgt_tid = user32.GetWindowThreadProcessId(hwnd, None)

    attached = []
    for tid in {fg_tid, tgt_tid}:
        if tid and tid != cur_tid and user32.AttachThreadInput(cur_tid, tid, True):
            attached.append(tid)
    try:
        user32.BringWindowToTop(hwnd)
        user32.SetForegroundWindow(hwnd)
        user32.SetActiveWindow(hwnd)
        user32.SetFocus(hwnd)
    finally:
        for tid in attached:
            user32.AttachThreadInput(cur_tid, tid, False)

    time.sleep(settle)
    return user32.GetForegroundWindow() == hwnd


def ensure_focus(hwnd, attempts=3):
    """Focus, retrying; raises if the window will not come forward."""
    for _ in range(attempts):
        if focus(hwnd):
            return True
        time.sleep(0.4)
    return False


# --- capture ------------------------------------------------------------------

def capture(path, region=None):
    """Grab the screen (or a region) to a PNG.

    Returns a dict with the size and mean pixel value; a mean near zero means
    GDI could not read the surface, which happens with fullscreen-exclusive
    D3D9. Windowed mode captures reliably.
    """
    if region is None:
        x = user32.GetSystemMetrics(76)   # SM_XVIRTUALSCREEN
        y = user32.GetSystemMetrics(77)   # SM_YVIRTUALSCREEN
        w = user32.GetSystemMetrics(78)   # SM_CXVIRTUALSCREEN
        h = user32.GetSystemMetrics(79)   # SM_CYVIRTUALSCREEN
    else:
        x, y, x2, y2 = region
        w, h = x2 - x, y2 - y

    hdc = user32.GetDC(0)
    memdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(memdc, bmp)
    gdi32.BitBlt(memdc, 0, 0, w, h, hdc, x, y, SRCCOPY | CAPTUREBLT)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG),
                    ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                    ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth = w
    bi.biHeight = -h          # negative => top-down rows
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0

    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(memdc, bmp, 0, h, buf, ctypes.byref(bi), 0)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(memdc)
    user32.ReleaseDC(0, hdc)

    img = Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1)
    img.save(path)

    small = img.convert("L").resize((64, 64))
    px = list(small.tobytes())
    mean = sum(px) / len(px)
    return {"path": path, "size": (w, h), "mean": round(mean, 2),
            "looks_black": mean < 2.0}


PW_RENDERFULLCONTENT = 0x00000002


def capture_window(hwnd, path):
    """Capture a window's own contents, even when it is covered or off-screen.

    Uses PrintWindow with PW_RENDERFULLCONTENT, which asks the window to render
    itself through DWM rather than copying pixels off the screen. That means
    other windows can sit on top of the game without corrupting captures - a
    plain screen-region BitBlt grabs whatever is visually in front.

    Returns None if PrintWindow produces nothing usable (some exclusive-mode
    surfaces cannot be redirected), so the caller can fall back.
    """
    left, top, right, bottom = window_rect(hwnd)
    w, h = right - left, bottom - top
    if w <= 0 or h <= 0:
        return None

    hdc = user32.GetDC(0)
    memdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(memdc, bmp)

    ok = user32.PrintWindow(hwnd, memdc, PW_RENDERFULLCONTENT)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG),
                    ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                    ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth = w
    bi.biHeight = -h
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0

    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(memdc, bmp, 0, h, buf, ctypes.byref(bi), 0)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(memdc)
    user32.ReleaseDC(0, hdc)

    if not ok:
        return None

    img = Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1)
    small = img.convert("L").resize((64, 64))
    px = list(small.tobytes())
    mean = sum(px) / len(px)
    if mean < 1.0:
        return None          # blank redirect surface; caller should fall back

    img.save(path)
    return {"path": path, "size": (w, h), "mean": round(mean, 2),
            "looks_black": False, "method": "printwindow"}


def capture_game(path, exe="MirrorsEdge.exe", prefer_window=True):
    """Capture the game.

    Prefers PrintWindow so the game can be covered by other windows; falls back
    to a screen-region grab if the redirect surface comes back blank.
    """
    hwnd, title = find_window_by_process(exe)
    if not hwnd:
        info = capture(path)
        info.update({"hwnd": None, "title": None, "rect": None, "method": "screen"})
        return info

    rect = window_rect(hwnd)
    info = capture_window(hwnd, path) if prefer_window else None
    if info is None:
        info = capture(path, region=rect)
        info["method"] = "screen"
    info.update({"hwnd": hwnd, "title": title, "rect": rect})
    return info


if __name__ == "__main__":
    import sys
    from . import paths

    hwnd, title = find_window_by_process("MirrorsEdge.exe")
    print("game window: title=%r hwnd=%s rect=%s"
          % (title, hwnd, window_rect(hwnd) if hwnd else None))
    paths.ensure_dir(paths.build_dir("shots"))
    out = sys.argv[1] if len(sys.argv) > 1 else paths.build_dir("shots", "desktop.png")
    print(capture(out))
