"""Hold the port's sound cue loader against a scan of the cooked packages, node by node.

    mirrorsedge_windows.exe --dump-sound-cues build/retail/sound_cues.jsonl
    python -m tools.retail.attenuation_check build/retail/sound_cues.jsonl <scan.json> [--list N]

The first file is what the port's loader makes of every SoundCue under CookedPC (one JSON object a
line: UPKPackage::extract_sound_cues_and_ambients, src/assets/upk_loader.cpp). The second is a scan
of the same packages by a reader that shares no code with it: one record per SoundCue export, with
every USoundNodeAttenuation / UTdSoundNodeAttenuation / USoundNodeAmbient of its graph as the tagged
properties store it (the research scan `v_raw.json`: a property that is not stored is absent there,
and stands for the class default).

Compared for every cue (same package file, same object path) and every node (same export):
class, DistanceModel, the MinRadius / MaxRadius / LPFMinRadius / LPFMaxRadius pairs in stored order,
dBAttenuationAtMax, bAttenuate, bSpatialize, bAttenuateWithLowPassFilter, bDelay, the node's depth in
the graph and the attenuation nodes above it; and for the cue its Duration, VolumeMultiplier and
SoundGroup. Then the two things that decide how a cue is played at a place: which cues have two
attenuation nodes on one branch, and which have a wave with none above it.

Exit status 0 when nothing disagrees.
"""
import argparse
import json
import os
import sys
from collections import Counter, defaultdict

# Default__SoundNodeAttenuation and Default__SoundNodeAmbient (Engine.u).
DEFAULTS = {"MinRadius": 400.0, "MaxRadius": 5000.0, "LPFMinRadius": 1500.0, "LPFMaxRadius": 5000.0}
AMBIENT_DEFAULTS = {"MinRadius": 400.0, "MaxRadius": 5000.0, "LPFMinRadius": 1500.0, "LPFMaxRadius": 2500.0}
MODELS = ["ATTENUATION_Linear", "ATTENUATION_Logarithmic", "ATTENUATION_Inverse", "ATTENUATION_LogReverse",
          "ATTENUATION_NaturalSound"]
PORT_KEYS = {"MinRadius": "min", "MaxRadius": "max", "LPFMinRadius": "lpfmin", "LPFMaxRadius": "lpfmax"}


def stored_pair(node, key):
    """The (first, second) values a play draws between, as the scan stored them."""
    default = (AMBIENT_DEFAULTS if node["c"].startswith("SoundNodeAmbient") else DEFAULTS)[key]
    raw = node.get(key)
    if raw is None:
        return default, default
    table = raw.get("tab")
    if table and len(table) >= 4:
        return table[2], table[3]       # the first entry of the cooked table: a (Min, Max) pair
    if table and len(table) == 3:
        return table[2], table[2]
    if "dConstant" in raw:
        return raw["dConstant"], raw["dConstant"]
    if raw.get("dMin") is not None or raw.get("dMax") is not None:
        return (raw["dMin"] if raw.get("dMin") is not None else default,
                raw["dMax"] if raw.get("dMax") is not None else default)
    return default, default


def scan_node(node):
    model = node.get("dm", 0)
    if isinstance(model, str):
        model = MODELS.index(model.split(":", 1)[1])
    ambient = node["c"].startswith("SoundNodeAmbient")
    out = {"c": node["c"], "model": model, "db": node.get("dB", -60.0), "attenuate": node.get("bAttenuate", True),
           "spatialize": node.get("bSpatialize", True), "lowpass": node.get("bAttenuateWithLowPassFilter", not ambient),
           "delay": node.get("bDelay", False), "depth": node["depth"], "above": list(node["above"])}
    for key, short in PORT_KEYS.items():
        out[short] = list(stored_pair(node, key))
    return out


def same(a, b):
    if isinstance(a, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, bool) or isinstance(b, bool) or isinstance(a, str):
        return a == b
    return abs(a - b) <= 1e-4 + 1e-6 * max(abs(a), abs(b))   # the scan keeps four decimals


def unique_path(pkg, path, forced_export):
    """<Package>.<Group>.<Cue>: a package's own cue has no package in its path; a copy cooked into
    another file sits under a forced-export package named after its home."""
    if forced_export:
        return path.lower()
    return (os.path.splitext(os.path.basename(pkg))[0] + "." + path).lower()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="the port's --dump-sound-cues file")
    ap.add_argument("scan", help="the scan of the cooked packages (v_raw.json)")
    ap.add_argument("--list", type=int, default=40, help="how many disagreements to print (default 40)")
    args = ap.parse_args()

    port = {}
    emitters = []
    with open(args.dump, encoding="utf-8") as f:
        for line in f:
            if not line.strip():
                continue
            rec = json.loads(line)
            if "emitter" in rec:
                emitters.append(rec)
            else:
                port[(rec["pkg"].lower(), rec["path"])] = rec
    scan = {}
    with open(args.scan, encoding="utf-8") as f:
        for rec in json.load(f):
            if "error" not in rec:
                scan[(rec["pkg"].lower(), ".".join(rec["comps"]))] = rec

    print("cue records: port %d, scan %d, in both %d" % (len(port), len(scan), len(set(port) & set(scan))))
    only_port = sorted(set(port) - set(scan))
    only_scan = sorted(set(scan) - set(port))
    for key in only_port[:args.list]:
        print("  only in the port's dump:", key)
    for key in only_scan[:args.list]:
        print("  only in the scan:", key)

    problems = []          # (cue key, text)
    nodes_port = nodes_scan = nodes_equal = 0
    cues_equal = 0
    fields = Counter()
    by_unique = defaultdict(lambda: [0, 0])    # unique cue path -> [records, records that agree]
    chained = {"port": set(), "scan": set()}
    bare = {"port": set(), "scan": set()}
    for key in sorted(set(port) & set(scan)):
        p, s = port[key], scan[key]
        bad = []
        if not same(p["dur"], s["dur"] or 0.0):
            bad.append("Duration %r, scan %r" % (p["dur"], s["dur"]))
        if not same(p["vol"], 0.75 if s["vol"] is None else s["vol"]):
            bad.append("VolumeMultiplier %r, scan %r" % (p["vol"], s["vol"]))
        if p["group"] != (s["group"] or "InGameSFX"):
            bad.append("SoundGroup %r, scan %r" % (p["group"], s["group"]))
        pn = {a["n"]: a for a in p["att"]}
        sn = {a["n"]: scan_node(a) for a in s["att"]}
        nodes_port += len(pn)
        nodes_scan += len(sn)
        for n in sorted(set(pn) | set(sn)):
            if n not in pn:
                bad.append("node %d (%s) not in the port's dump" % (n, sn[n]["c"]))
                continue
            if n not in sn:
                bad.append("node %d (%s) not in the scan" % (n, pn[n]["c"]))
                continue
            wrong = [f for f in sn[n] if not same(pn[n][f], sn[n][f])]
            if wrong:
                for f in wrong:
                    fields[f] += 1
                bad.append("node %d: " % n + "; ".join("%s %r, scan %r" % (f, pn[n][f], sn[n][f]) for f in wrong))
            else:
                nodes_equal += 1
        uniq = unique_path(s["pkg"], key[1], (s.get("top_flags") or 0) & 1)
        level_actor = s["comps"][0] == "TheWorld" or "PersistentLevel" in s["comps"]
        if not level_actor:
            by_unique[uniq][0] += 1
            by_unique[uniq][1] += 0 if bad else 1
            if any(a["above"] for a in p["att"]):
                chained["port"].add(uniq)
            if s["maxchain"] >= 2:
                chained["scan"].add(uniq)
            if p["att"] and p["waves"][0] > 0:
                bare["port"].add(uniq)
            if s["att"] and s.get("leaf_att", {}).get("0"):
                bare["scan"].add(uniq)
        if bad:
            problems.append((key, bad))
        else:
            cues_equal += 1

    both = len(set(port) & set(scan))
    print("cue records that agree in every compared value: %d of %d" % (cues_equal, both))
    print("attenuation nodes: port %d, scan %d, equal in every compared value %d" % (nodes_port, nodes_scan, nodes_equal))
    print("unique cues (<Package>.<Group>.<Cue>, level actors' own cues left out): %d, every copy agreeing: %d" % (
        len(by_unique), sum(1 for total, ok in by_unique.values() if total == ok)))
    if fields:
        print("values that differ, by field:", dict(fields))
    for key, bad in problems[:args.list]:
        print("  DIFFERS %s %s" % key)
        for text in bad:
            print("      " + text)
    if len(problems) > args.list:
        print("  ... and %d more" % (len(problems) - args.list))

    for name, title in (("chained", "cues with two attenuation nodes on one branch"),
                        ("bare", "cues with an attenuation node and a wave that has none above it")):
        sets = chained if name == "chained" else bare
        print("%s: port %d, scan %d, the same cues: %s" % (title, len(sets["port"]), len(sets["scan"]), sets["port"] == sets["scan"]))
        for cue in sorted(sets["port"] - sets["scan"])[:args.list]:
            print("  only by the port:", cue)
        for cue in sorted(sets["scan"] - sets["port"])[:args.list]:
            print("  only by the scan:", cue)

    counts = Counter()
    for rec in port.values():
        for a in rec["att"]:
            counts[a["c"]] += 1
    print("port node classes (all records):", dict(counts), " ambient sound actors:", len(emitters))
    ok = not problems and not only_port and not only_scan and nodes_port == nodes_scan == nodes_equal
    print("RESULT:", "the loader agrees with the scan" if ok else "DISAGREEMENTS")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
