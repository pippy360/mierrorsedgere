"""Filesystem discovery for the repo and the retail Mirror's Edge install.

Copied from tesseract's tools/medge/paths.py, less what only that repo's
converter needs.

Environment overrides, all optional:
    MEDGE_REPO_ROOT         repo root (default: inferred from this file)
    MEDGE_ME_INSTALL        Mirror's Edge install dir (default: found via Steam)
    MEDGE_STEAM_ROOT        an extra Steam library to search
    MEDGE_BUILD_DIR         recordings and replay output (default: <repo>/build/retail)
    MEDGE_ME_USER_CONFIG    the game's per-user config dir
"""

import os
import re

MEDGE_STEAM_APPID = 17410

# Steam's default install roots, checked before parsing libraryfolders.vdf.
_DEFAULT_STEAM_ROOTS = [
    r"C:\Program Files (x86)\Steam",
    r"C:\Program Files\Steam",
    os.path.expanduser("~/.steam/steam"),
    os.path.expanduser("~/.local/share/Steam"),
    os.path.expanduser("~/Library/Application Support/Steam"),
]


class MedgePathError(RuntimeError):
    """Raised when a required path cannot be located."""


def repo_root():
    """Absolute path to this checkout."""
    env = os.environ.get("MEDGE_REPO_ROOT")
    if env:
        return os.path.abspath(env)
    # tools/retail/paths.py -> tools/retail -> tools -> <repo>
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def build_dir(*parts):
    """Derived output that is safe to delete and must never be committed."""
    base = os.environ.get("MEDGE_BUILD_DIR") or os.path.join(repo_root(), "build", "retail")
    return os.path.join(os.path.abspath(base), *parts)


def ensure_dir(path):
    """Create `path` (a directory) if missing; return it."""
    os.makedirs(path, exist_ok=True)
    return path


# --- Steam / Mirror's Edge discovery -----------------------------------------


def _steam_library_roots():
    """Every Steam library folder we can find, in probe order."""
    roots = []

    def add(p):
        if p and os.path.isdir(p) and p not in roots:
            roots.append(p)

    env = os.environ.get("MEDGE_STEAM_ROOT")
    add(os.path.abspath(env) if env else None)
    for p in _DEFAULT_STEAM_ROOTS:
        add(p)

    # libraryfolders.vdf lists libraries on other drives. Rather than pull in a
    # VDF parser for one field, scrape the quoted "path" values.
    for root in list(roots):
        vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
        if not os.path.isfile(vdf):
            continue
        try:
            with open(vdf, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        for m in re.finditer(r'"path"\s*"([^"]+)"', text):
            add(os.path.abspath(m.group(1).replace("\\\\", "\\")))

    return roots


def _read_appmanifest_installdir(manifest_path):
    try:
        with open(manifest_path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return None
    m = re.search(r'"installdir"\s*"([^"]+)"', text)
    return m.group(1) if m else None


def find_medge_install(required=True):
    """Locate the retail Mirror's Edge install directory.

    Returns the absolute path, or None when `required` is False and it is absent.
    """
    env = os.environ.get("MEDGE_ME_INSTALL")
    if env:
        path = os.path.abspath(env)
        if _looks_like_medge(path):
            return path
        if required:
            raise MedgePathError(
                "MEDGE_ME_INSTALL is set to %r but that does not look like a "
                "Mirror's Edge install (expected TdGame/CookedPC underneath)." % path
            )
        return None

    # A copy sitting in the repo root, which is how the converter work was run
    # on machines with no Steam library. Checked before Steam so an explicit
    # local copy wins over whatever happens to be installed.
    repo_local = os.path.join(repo_root(), "mirrors edge")
    if _looks_like_medge(repo_local):
        return os.path.abspath(repo_local)

    for root in _steam_library_roots():
        steamapps = os.path.join(root, "steamapps")
        manifest = os.path.join(steamapps, "appmanifest_%d.acf" % MEDGE_STEAM_APPID)
        installdir = _read_appmanifest_installdir(manifest) if os.path.isfile(manifest) else None
        candidates = [installdir] if installdir else []
        candidates.append("mirrors edge")  # the shipped installdir value
        for name in candidates:
            path = os.path.join(steamapps, "common", name)
            if _looks_like_medge(path):
                return os.path.abspath(path)

    if required:
        raise MedgePathError(
            "Could not find a Mirror's Edge install. Set MEDGE_ME_INSTALL to the "
            "directory containing TdGame/ and Binaries/."
        )
    return None


def _looks_like_medge(path):
    return os.path.isdir(os.path.join(path, "TdGame", "CookedPC"))


def medge_path(*parts, **kwargs):
    """A path inside the retail install, e.g. medge_path('TdGame', 'Config')."""
    return os.path.join(find_medge_install(**kwargs), *parts)


def medge_config_dir():
    """TdGame/Config - the plaintext movement/game ground truth."""
    return medge_path("TdGame", "Config")


def medge_engine_config_dir():
    """Engine/Config - the Base*.ini that Default*.ini inherit from."""
    return medge_path("Engine", "Config")


def medge_cooked_dir(*parts):
    return medge_path("TdGame", "CookedPC", *parts)


def medge_maps_dir(*parts):
    return medge_cooked_dir("Maps", *parts)


def medge_exe():
    return medge_path("Binaries", "MirrorsEdge.exe")


def medge_user_config_dir(required=False):
    """The per-user config the game writes (TdInput.ini, TdEngine.ini).

    This is where the dev console gets enabled. Returns None if absent.
    """
    env = os.environ.get("MEDGE_ME_USER_CONFIG")
    if env:
        return os.path.abspath(env)
    candidates = [
        os.path.expanduser(r"~/Documents/EA Games/Mirror's Edge/TdGame/Config"),
        os.path.expanduser(r"~/My Documents/EA Games/Mirror's Edge/TdGame/Config"),
    ]
    for c in candidates:
        if os.path.isdir(c):
            return c
    if required:
        raise MedgePathError(
            "Could not find the Mirror's Edge user config directory. Run the game "
            "once, or set MEDGE_ME_USER_CONFIG."
        )
    return None


if __name__ == "__main__":
    print("repo_root          %s" % repo_root())
    print("build_dir          %s" % build_dir())
    print("medge_install      %s" % find_medge_install(required=False))
    print("medge_user_config  %s" % medge_user_config_dir())
