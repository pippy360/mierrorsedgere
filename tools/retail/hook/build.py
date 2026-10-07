"""Build the 32-bit d3d9 proxy and install it next to MirrorsEdge.exe.

Mirror's Edge is a 32-bit process, so the DLL must be x86 regardless of the
host. This shells out through vcvarsall.bat because cl.exe needs the INCLUDE and
LIB environment it sets up.

    python -m tools.retail.hook.build           build + install
    python -m tools.retail.hook.build --build   build only
    python -m tools.retail.hook.build --remove  uninstall the proxy
"""

import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))))

from tools.retail import paths  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "d3d9_proxy.cpp")
DLL_NAME = "d3d9.dll"

VCVARS_CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat",
    r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat",
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
]


def find_vcvars():
    for p in VCVARS_CANDIDATES:
        if os.path.isfile(p):
            return p
    raise SystemExit("vcvarsall.bat not found; install VS Build Tools with the "
                     "C++ workload (winget install Microsoft.VisualStudio.2022.BuildTools)")


def build():
    out_dir = paths.ensure_dir(paths.build_dir("hook"))
    out_dll = os.path.join(out_dir, DLL_NAME)
    vcvars = find_vcvars()

    # Driven through a .bat rather than `cmd /c "..."`: the vcvarsall path
    # contains spaces and parentheses, which cmd's own quote handling mangles.
    #
    # x86 target: the game is 32-bit. amd64_x86 = 64-bit host tools, 32-bit output.
    bat = os.path.join(out_dir, "_build.bat")
    with open(bat, "w") as f:
        f.write("@echo off\r\n")
        f.write('call "%s" amd64_x86 >nul\r\n' % vcvars)
        f.write('if errorlevel 1 exit /b 1\r\n')
        # Build inside out_dir with relative output names. A quoted path ending
        # in a backslash (/Fo:"C:\dir\") would escape its own closing quote.
        f.write('cd /d "%s"\r\n' % out_dir)
        # renderdoc_app.h ships in the RenderDoc install; it is a standalone
        # header with no library to link against (everything is resolved through
        # RENDERDOC_GetAPI at runtime).
        inc = ' /I"%s"' % HERE
        for d in (r"C:\Program Files\RenderDoc", r"C:\Program Files (x86)\RenderDoc"):
            if os.path.isfile(os.path.join(d, "renderdoc_app.h")):
                inc = ' /I"%s"' % d
                break
        f.write('cl /nologo /LD /O2 /EHsc /W3%s /Fe:%s /Fo:proxy.obj "%s" '
                '/link /DLL user32.lib gdi32.lib\r\n' % (inc, DLL_NAME, SRC))
        f.write('exit /b %errorlevel%\r\n')

    print("building x86 proxy...")
    r = subprocess.run([bat], capture_output=True, text=True, cwd=out_dir, shell=True)
    sys.stdout.write(r.stdout[-3000:] if r.stdout else "")
    if r.returncode != 0:
        sys.stderr.write(r.stderr[-3000:] if r.stderr else "")
        raise SystemExit("build failed (exit %d)" % r.returncode)
    if not os.path.isfile(out_dll):
        raise SystemExit("build reported success but %s is missing" % out_dll)
    print("built %s (%d bytes)" % (out_dll, os.path.getsize(out_dll)))
    return out_dll


def install(dll):
    dest_dir = os.path.dirname(paths.medge_exe())
    dest = os.path.join(dest_dir, DLL_NAME)
    if os.path.exists(dest):
        # Never clobber a real d3d9.dll that we did not put there.
        backup = dest + ".medge-bak"
        if not os.path.exists(backup):
            shutil.copyfile(dest, backup)
            print("backed up existing %s -> %s" % (dest, backup))
    try:
        shutil.copyfile(dll, dest)
    except PermissionError:
        # Windows locks a loaded DLL. If the game is up, the copy fails while
        # the build above still reports success - so the next run silently
        # tests the OLD hook and every new command looks like it does nothing.
        raise SystemExit(
            "cannot install %s - Mirror's Edge is still running and has the "
            "DLL loaded.\nKill it first:  python -m tools.retail.drive --kill"
            % dest)
    print("installed %s" % dest)
    return dest


def remove():
    dest_dir = os.path.dirname(paths.medge_exe())
    dest = os.path.join(dest_dir, DLL_NAME)
    backup = dest + ".medge-bak"
    if os.path.isfile(backup):
        shutil.move(backup, dest)
        print("restored original %s" % dest)
    elif os.path.isfile(dest):
        os.remove(dest)
        print("removed %s" % dest)
    else:
        print("nothing installed at %s" % dest)


if __name__ == "__main__":
    if "--remove" in sys.argv:
        remove()
    elif "--build" in sys.argv:
        build()
    else:
        install(build())
