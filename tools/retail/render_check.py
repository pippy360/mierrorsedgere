"""Hold the port's rendering against retail's, picture by picture, from the same camera.

    python -m tools.retail.render_check shots <mirrorsedge_windows.exe> [map ...]
    python -m tools.retail.render_check compare [map ...] [--sheet] [--json]

The level intros give the matched cameras: the port's intro camera is retail's to within a unit
(docs/LEVEL_INTROS.md), and tools/retail/intro_capture.py --frames saved a retail back buffer every
second or so of each (build/retail/intros/<map>_frames/). `shots` asks the port, headless, for the
same Matinee times (--intro-shots) into build/retail/render_port/<map>/. `compare` pairs them and
measures, per pair and per chapter:

  * mean colour and luminance of each picture (display values, 0..255),
  * saturation (mean of max-min over the channels),
  * the difference of the two pictures on a 32 x 18 grid of cell means, as a root mean square over
    cells and channels. The grid forgives the sub-pixel things a still cannot match (grain, the
    hands' exact pose) and keeps what rendering decides: exposure, colour, where light and shade fall.

A retail frame carries the skip prompt and, in a chapter's first seconds, its title; both are small.
The clock shift between the two recordings is taken from tools/retail/intro_check.py.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from tools.retail import intro_check, paths  # noqa: E402

MAP_FILES = {
    "edge_p": "Maps/SP01/Edge_p.me1", "escape_p": "Maps/SP01/Escape_p.me1", "stormdrain_p": "Maps/SP02/Stormdrain_p.me1",
    "cranes_p": "Maps/SP03/Cranes_p.me1", "subway_p": "Maps/SP04/Subway_p.me1", "mall_p": "Maps/SP05/Mall_p.me1",
    "factory_p": "Maps/SP06/Factory_p.me1", "boat_p": "Maps/SP07/Boat_p.me1", "convoy_p": "Maps/SP08/Convoy_p.me1",
    "scraper_p": "Maps/SP09/Scraper_p.me1",
}
GRID = (32, 18)


def port_dir(name=None):
    d = paths.build_dir("render_port") if name is None else paths.build_dir("render_port", name)
    return paths.ensure_dir(d)


def clock_shifts():
    """The port's intro clock against retail's, per map (cached: fitting it reads both traces)."""
    cache = os.path.join(port_dir(), "clock_shifts.json")
    if os.path.exists(cache):
        with open(cache) as f:
            return json.load(f)
    out = {}
    for name in intro_check.LEVELS:
        r = intro_check.compare_one(name)
        if "clock_shift_s" in r:
            out[name] = r["clock_shift_s"]
    with open(cache, "w") as f:
        json.dump(out, f)
    return out


def retail_frames(name):
    """[(Matinee time, path)] for the retail frames that fall inside the intro."""
    trace = os.path.join(intro_check.retail_dir(), name + ".jsonl")
    r0 = None
    with open(trace, encoding="utf-8") as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if r.get("type") == "sample" and r.get("valid"):
                r0 = r["t"]
                break
    shift = clock_shifts().get(name, 0.0)
    length = intro_check.LEVELS[name][1]
    out = []
    for path in sorted(glob.glob(os.path.join(intro_check.retail_dir(), name + "_frames", "f_*.png"))):
        m = re.search(r"_t(\d+\.\d+)\.png$", path)
        t = float(m.group(1)) - r0 + shift
        if 0.8 <= t <= length - 0.3:
            out.append((round(t, 2), path))
    # one frame per time (a recording run twice leaves two series)
    seen, uniq = set(), []
    for t, p in out:
        if t not in seen:
            seen.add(t)
            uniq.append((t, p))
    return uniq


def shots(exe, names):
    for name in names:
        frames = retail_frames(name)
        if not frames:
            print("%-13s no retail frames" % name)
            continue
        out = port_dir(name)
        for old in glob.glob(os.path.join(out, "*.png")):
            os.remove(old)
        times = ",".join("%.2f" % t for t, _ in frames)
        with open(os.path.join(out, "shots.log"), "w") as log:
            code = subprocess.call([exe, "--intro-shots", MAP_FILES[name], times, out], stdout=log, stderr=subprocess.STDOUT, cwd=out,
                                   env=dict(os.environ, ME_NO_HUD="1"))
        print("%-13s %d shots, exit code %d" % (name, len(glob.glob(os.path.join(out, "*.png"))), code))


def pairs(name):
    out = []
    for t, retail_path in retail_frames(name):
        port_path = os.path.join(port_dir(name), "%s_%06.2f.png" % (name, t))
        if os.path.exists(port_path):
            out.append((t, retail_path, port_path))
    return out


def measure(image):
    """Mean colour, luminance, saturation and the grid of cell means of an RGB picture."""
    import numpy as np
    a = np.asarray(image.convert("RGB"), dtype=np.float32)
    mean = a.reshape(-1, 3).mean(axis=0)
    lum = float((a * np.array([0.299, 0.587, 0.114], dtype=np.float32)).sum(axis=2).mean())
    sat = float((a.max(axis=2) - a.min(axis=2)).mean())
    small = np.asarray(image.convert("RGB").resize(GRID, resample=2), dtype=np.float32)  # 2 = bilinear over a box-ish reduce
    return mean, lum, sat, small


def compare(names, sheet, as_json):
    import numpy as np
    from PIL import Image
    total = []
    for name in names:
        rows = []
        for t, rp, pp in pairs(name):
            ri, pi = Image.open(rp), Image.open(pp)
            rm, rl, rs, rg = measure(ri)
            pm, pl, ps, pg = measure(pi)
            rms = float(np.sqrt(((rg - pg) ** 2).mean()))
            rows.append({"t": t, "retail_rgb": [round(float(x), 1) for x in rm], "port_rgb": [round(float(x), 1) for x in pm],
                         "retail_lum": round(rl, 1), "port_lum": round(pl, 1), "retail_sat": round(rs, 1), "port_sat": round(ps, 1),
                         "grid_rms": round(rms, 1)})
        if not rows:
            print("%-13s no pairs (run `shots` first)" % name)
            continue
        avg = lambda k: sum(r[k] for r in rows) / len(rows)  # noqa: E731
        summary = {"map": name, "pairs": len(rows), "grid_rms": round(avg("grid_rms"), 1),
                   "retail_lum": round(avg("retail_lum"), 1), "port_lum": round(avg("port_lum"), 1),
                   "retail_sat": round(avg("retail_sat"), 1), "port_sat": round(avg("port_sat"), 1),
                   "retail_rgb": [round(sum(r["retail_rgb"][c] for r in rows) / len(rows), 1) for c in range(3)],
                   "port_rgb": [round(sum(r["port_rgb"][c] for r in rows) / len(rows), 1) for c in range(3)]}
        total.append(summary)
        if as_json:
            print(json.dumps(dict(summary, frames=rows)))
        else:
            print("%-13s %2d pairs | grid rms %5.1f | luminance retail %5.1f port %5.1f | saturation retail %5.1f port %5.1f | rgb retail %s port %s" % (
                name, summary["pairs"], summary["grid_rms"], summary["retail_lum"], summary["port_lum"], summary["retail_sat"],
                summary["port_sat"], summary["retail_rgb"], summary["port_rgb"]))
        if sheet:
            picked = pairs(name)
            step = max(1, len(picked) // 6)
            picked = picked[::step][:6]
            h = 270
            tiles = []
            for _, rp, pp in picked:
                a = Image.open(rp).convert("RGB")
                b = Image.open(pp).convert("RGB")
                a = a.resize((int(a.width * h / a.height), h))
                b = b.resize((int(b.width * h / b.height), h))
                row = Image.new("RGB", (a.width + b.width + 6, h), (0, 0, 0))
                row.paste(a, (0, 0))
                row.paste(b, (a.width + 6, 0))
                tiles.append(row)
            page = Image.new("RGB", (max(x.width for x in tiles), sum(x.height for x in tiles) + 4 * (len(tiles) - 1)), (0, 0, 0))
            y = 0
            for x in tiles:
                page.paste(x, (0, y))
                y += x.height + 4
            path = os.path.join(port_dir(), "sheet_%s.png" % name)
            page.save(path)
            print("              %s (retail left, port right)" % path)
    if total and not as_json:
        n = len(total)
        print("%-13s grid rms %5.1f | luminance retail %5.1f port %5.1f | saturation retail %5.1f port %5.1f" % (
            "all %d" % n, sum(s["grid_rms"] for s in total) / n, sum(s["retail_lum"] for s in total) / n,
            sum(s["port_lum"] for s in total) / n, sum(s["retail_sat"] for s in total) / n, sum(s["port_sat"] for s in total) / n))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sh = sub.add_parser("shots", help="render the port at the retail frames' moments")
    sh.add_argument("exe")
    sh.add_argument("maps", nargs="*")
    cm = sub.add_parser("compare", help="measure the pairs")
    cm.add_argument("maps", nargs="*")
    cm.add_argument("--sheet", action="store_true", help="also write a side-by-side page per chapter")
    cm.add_argument("--json", action="store_true")
    args = ap.parse_args()
    names = [m.lower() for m in args.maps] or list(MAP_FILES)
    if args.cmd == "shots":
        shots(os.path.abspath(args.exe), names)
    else:
        compare(names, args.sheet, args.json)


if __name__ == "__main__":
    main()
