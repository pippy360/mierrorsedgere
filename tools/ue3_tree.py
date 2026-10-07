#!/usr/bin/env python3
"""Tagged-property tree dump for Mirror's Edge (UE3 v536) packages.

upk_inspector.py's `dump` keeps one flat level of properties and guesses the
property start badly for UI widgets. This mirrors src/assets/ue3_props.cpp
instead: every property, array indices kept, tagged structs and arrays of
structs decoded recursively, object references resolved to a path.

    python tools/ue3_tree.py <package> --tree <ObjectName>      # outer hierarchy
    python tools/ue3_tree.py <package> --dump <index|Name>      # one export
    python tools/ue3_tree.py <package> --dump-tree <Name>       # every export under it
    python tools/ue3_tree.py <package> --classes                # class histogram
    python tools/ue3_tree.py <package> --find <substr>          # exports by name/class
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upk_inspector import UPKPackage  # noqa: E402

PROPERTY_TYPES = {
    "IntProperty", "FloatProperty", "BoolProperty", "ByteProperty", "ObjectProperty",
    "ClassProperty", "ComponentProperty", "NameProperty", "StrProperty", "StructProperty",
    "ArrayProperty", "DelegateProperty", "InterfaceProperty", "MapProperty",
}

# Structs serialized natively rather than as a tagged list.
IMMUTABLE = {
    "Vector": 12, "Vector2D": 8, "Vector4": 16, "Rotator": 12, "Color": 4,
    "LinearColor": 16, "Guid": 16, "Plane": 16, "Quat": 16, "Matrix": 64,
    "Box": 25, "IntPoint": 8, "Sphere": 16, "TwoVectors": 24, "BoxSphereBounds": 28,
}

RF_HAS_STACK = 0x0200000000000000


class Prop:
    __slots__ = ("name", "type", "struct", "index", "size", "offset", "value")

    def __init__(self):
        self.name = self.type = self.struct = ""
        self.index = self.size = self.offset = 0
        self.value = None


class Tree:
    def __init__(self, pkg):
        self.pkg = pkg
        self.d = pkg.data
        self.names = pkg.names

    # --- names and references -------------------------------------------------

    def fname(self, off):
        idx, num = struct.unpack_from("<ii", self.d, off)
        if idx < 0 or idx >= len(self.names):
            return None
        return self.names[idx] if num == 0 else "%s_%d" % (self.names[idx], num - 1)

    def export_name(self, i):
        e = self.pkg.exports[i - 1]
        n = e["object_name"]
        return n if not e["object_number"] else "%s_%d" % (n, e["object_number"] - 1)

    def path(self, idx):
        parts = []
        guard = 0
        while idx != 0 and guard < 64:
            guard += 1
            if idx > 0:
                if idx > len(self.pkg.exports):
                    break
                parts.append(self.export_name(idx))
                idx = self.pkg.exports[idx - 1]["outer_index"]
            else:
                ii = -idx - 1
                if ii >= len(self.pkg.imports):
                    break
                imp = self.pkg.imports[ii]
                parts.append(imp["object_name"])
                idx = imp["outer_index"]
        return ".".join(reversed(parts))

    def ref(self, idx):
        if idx == 0:
            return "None"
        if idx > 0:
            if idx > len(self.pkg.exports):
                return "<bad export %d>" % idx
            cls = self.pkg.get_export_class(self.pkg.exports[idx - 1])
            return "%s'%s' [%d]" % (cls, self.path(idx), idx)
        ii = -idx - 1
        if ii >= len(self.pkg.imports):
            return "<bad import %d>" % idx
        return "%s'%s' [import]" % (self.pkg.imports[ii]["class_name"], self.path(idx))

    # --- the tag stream -------------------------------------------------------

    def looks_like_tag(self, off, end):
        end = min(end, len(self.d))
        if off < 0 or off + 8 > end:
            return False
        ni, nn = struct.unpack_from("<ii", self.d, off)
        if ni < 0 or ni >= len(self.names) or nn < 0:
            return False
        if self.names[ni] == "None":
            return True
        if off + 24 > end:
            return False
        ti, tn, sz = struct.unpack_from("<iii", self.d, off + 8)
        if ti < 0 or ti >= len(self.names) or tn != 0 or sz < 0:
            return False
        return self.names[ti] in PROPERTY_TYPES

    def property_start(self, exp):
        so, ss = exp["serial_offset"], exp["serial_size"]
        cands = ([32, 4, 8, 12, 16, 20, 24, 28, 36, 40, 44] if exp["object_flags"] & RF_HAS_STACK
                 else [4, 8, 32, 12, 16, 20, 24, 28])
        for c in cands:
            if c + 8 <= ss and self.looks_like_tag(so + c, so + ss):
                return so + c
        return so + 4

    def parse(self, off, end, depth=0):
        """Returns (props, offset past the terminating None)."""
        out = []
        pos = off
        end = min(end, len(self.d))
        if depth > 12:
            return out, end
        while pos + 8 <= end:
            name = self.fname(pos)
            if name is None:
                break
            pos += 8
            if name == "None":
                break
            if pos + 16 > end:
                break
            type_name = self.fname(pos)
            size, index = struct.unpack_from("<ii", self.d, pos + 8)
            pos += 16
            if type_name not in PROPERTY_TYPES or size < 0:
                break
            p = Prop()
            p.name, p.type, p.size, p.index = name, type_name, size, index
            if type_name == "StructProperty":
                p.struct = self.fname(pos)
                pos += 8
            elif type_name == "BoolProperty":
                p.offset = pos
                p.value = struct.unpack_from("<i", self.d, pos)[0] != 0
                pos += 4
                out.append(p)
                continue
            vpos, vend = pos, pos + size
            if vend > end:
                break
            p.offset = vpos
            pos = vend
            p.value = self.decode(p, vpos, vend, depth)
            out.append(p)
        return out, pos

    def decode(self, p, vpos, vend, depth):
        d = self.d
        t = p.type
        if t == "IntProperty":
            return struct.unpack_from("<i", d, vpos)[0]
        if t == "FloatProperty":
            return struct.unpack_from("<f", d, vpos)[0]
        if t in ("ObjectProperty", "ClassProperty", "ComponentProperty", "InterfaceProperty"):
            return ("ref", struct.unpack_from("<i", d, vpos)[0])
        if t == "NameProperty":
            return ("name", self.fname(vpos))
        if t == "ByteProperty":
            return ("name", self.fname(vpos)) if p.size == 8 else d[vpos]
        if t == "StrProperty":
            return self.pkg._read_fstring(d, vpos)[0]
        if t == "DelegateProperty":
            obj = struct.unpack_from("<i", d, vpos)[0]
            return ("delegate", obj, self.fname(vpos + 4))
        if t == "StructProperty":
            if IMMUTABLE.get(p.struct) == p.size:
                if p.struct == "Color":
                    b, g, r, a = d[vpos:vpos + 4]
                    return ("color", r, g, b, a)
                if p.struct in ("Rotator", "IntPoint"):
                    return ("ints",) + struct.unpack_from("<%di" % (p.size // 4), d, vpos)
                if p.struct == "Guid":
                    return ("guid", bytes(d[vpos:vend]).hex())
                if p.struct == "Box":
                    return ("floats",) + struct.unpack_from("<6f", d, vpos)
                return ("floats",) + struct.unpack_from("<%df" % (p.size // 4), d, vpos)
            fields, _ = self.parse(vpos, vend, depth + 1)
            return ("struct", fields)
        if t == "ArrayProperty":
            return self.decode_array(vpos, vend, depth)
        return ("raw", bytes(d[vpos:vend]).hex())

    def decode_array(self, vpos, vend, depth):
        d = self.d
        if vpos + 4 > vend:
            return ("array", 0, [])
        count = struct.unpack_from("<i", d, vpos)[0]
        body = vpos + 4
        blen = vend - body
        if count <= 0:
            return ("array", count, [])
        if self.looks_like_tag(body, vend) and blen != count * 4:
            cur = body
            elems = []
            ok = True
            for _ in range(count):
                if not self.looks_like_tag(cur, vend):
                    ok = False
                    break
                el, cur = self.parse(cur, vend, depth + 1)
                elems.append(("struct", el))
            if ok and cur == vend:
                return ("array", count, elems)
        if blen == count * 4:
            return ("array4", count, list(struct.unpack_from("<%di" % count, d, body)))
        if blen == count * 8:
            names = [self.fname(body + k * 8) for k in range(count)]
            if all(n is not None for n in names):
                return ("array", count, [("name", n) for n in names])
        if blen == count:
            return ("array", count, list(d[body:vend]))
        # FString arrays
        try:
            cur = body
            strs = []
            for _ in range(count):
                s, cur = self.pkg._read_fstring(d, cur)
                strs.append(s)
            if cur == vend:
                return ("array", count, strs)
        except Exception:
            pass
        return ("raw", bytes(d[body:vend]).hex())

    def export_props(self, i):
        exp = self.pkg.exports[i - 1]
        so, ss = exp["serial_offset"], exp["serial_size"]
        if ss <= 0 or so < 0 or so >= len(self.d):
            return [], so
        return self.parse(self.property_start(exp), so + ss)

    # --- printing -------------------------------------------------------------

    def fmt(self, v, ind):
        if isinstance(v, float):
            return "%.6g" % v
        if not isinstance(v, tuple):
            return repr(v)
        kind = v[0]
        if kind == "ref":
            return self.ref(v[1])
        if kind == "name":
            return "'%s'" % v[1]
        if kind == "delegate":
            return "delegate(%s.%s)" % (self.ref(v[1]), v[2])
        if kind == "color":
            return "Color(R=%d,G=%d,B=%d,A=%d)" % v[1:]
        if kind == "floats":
            return "(" + ", ".join("%.6g" % x for x in v[1:]) + ")"
        if kind == "ints":
            return "(" + ", ".join(str(x) for x in v[1:]) + ")"
        if kind == "guid":
            return "Guid(%s)" % v[1]
        if kind == "raw":
            h = v[1]
            return "raw[%d] %s%s" % (len(h) // 2, h[:96], "..." if len(h) > 96 else "")
        if kind == "struct":
            return "{\n" + self.fmt_props(v[1], ind + 1) + "  " * ind + "}"
        if kind == "array4":
            # 4-byte elements: object refs, ints or floats. Show all three readings that make sense.
            vals = v[2]
            exports, imports = len(self.pkg.exports), len(self.pkg.imports)
            if all(-imports <= x <= exports for x in vals):
                return "[%d] refs: %s" % (v[1], ", ".join(self.ref(x) if x else "None" for x in vals))
            fl = struct.unpack("<%df" % len(vals), struct.pack("<%di" % len(vals), *vals))
            return "[%d] ints %s / floats (%s)" % (v[1], vals, ", ".join("%.6g" % x for x in fl))
        if kind == "array":
            if not v[2]:
                return "[%d]" % v[1]
            if isinstance(v[2][0], tuple) and v[2][0][0] == "struct":
                s = "[%d] {\n" % v[1]
                for k, el in enumerate(v[2]):
                    s += "  " * (ind + 1) + "[%d] = %s\n" % (k, self.fmt(el, ind + 1))
                return s + "  " * ind + "}"
            return "[%d] %s" % (v[1], ", ".join(self.fmt(x, ind) for x in v[2]))
        return repr(v)

    def fmt_props(self, props, ind=1):
        s = ""
        for p in props:
            label = p.name if p.index == 0 else "%s[%d]" % (p.name, p.index)
            tn = p.struct if p.type == "StructProperty" else p.type.replace("Property", "")
            s += "  " * ind + "%s (%s) = %s\n" % (label, tn, self.fmt(p.value, ind))
        return s

    def dump(self, i, show_tail=True):
        exp = self.pkg.exports[i - 1]
        cls = self.pkg.get_export_class(exp)
        props, end = self.export_props(i)
        so, ss = exp["serial_offset"], exp["serial_size"]
        s = "[%d] %s : %s   (size %d, archetype %s)\n" % (
            i, self.path(i), cls, ss, self.ref(exp["archetype"]) if exp["archetype"] else "None")
        if exp["component_map"]:
            s += "  components: %s\n" % ", ".join("%s=[%d]" % (n, o) for n, o in exp["component_map"])
        s += self.fmt_props(props)
        tail = min(so + ss, len(self.d)) - end
        if show_tail and tail > 0:
            raw = bytes(self.d[end:so + ss])
            s += "  <native tail %d bytes> %s%s\n" % (tail, raw[:64].hex(), "..." if tail > 64 else "")
        return s

    def children(self):
        by_outer = {}
        for i, e in enumerate(self.pkg.exports):
            by_outer.setdefault(e["outer_index"], []).append(i + 1)
        return by_outer

    def find(self, target):
        if target.lstrip("-").isdigit():
            return [int(target)]
        t = target.lower()
        hits = [i + 1 for i, e in enumerate(self.pkg.exports) if self.export_name(i + 1).lower() == t]
        if not hits:
            hits = [i + 1 for i, e in enumerate(self.pkg.exports) if self.path(i + 1).lower() == t]
        return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("package")
    ap.add_argument("--tree", metavar="NAME", help="print the outer hierarchy under an export")
    ap.add_argument("--dump", metavar="TARGET", help="dump one export's properties")
    ap.add_argument("--dump-tree", metavar="NAME", help="dump every export under one")
    ap.add_argument("--skip", default="", help="comma-separated class names to leave out of --tree/--dump-tree")
    ap.add_argument("--classes", action="store_true")
    ap.add_argument("--find", metavar="SUBSTR")
    ap.add_argument("--imports", action="store_true")
    a = ap.parse_args()

    t = Tree(UPKPackage(a.package))
    skip = {s for s in a.skip.split(",") if s}
    kids = t.children()

    if a.classes:
        hist = {}
        for e in t.pkg.exports:
            c = t.pkg.get_export_class(e)
            hist[c] = hist.get(c, 0) + 1
        for c, n in sorted(hist.items(), key=lambda kv: -kv[1]):
            print("%6d  %s" % (n, c))
    if a.imports:
        for i, imp in enumerate(t.pkg.imports):
            print("[-%d] %s %s" % (i + 1, imp["class_name"], t.path(-(i + 1))))
    if a.find:
        s = a.find.lower()
        for i, e in enumerate(t.pkg.exports):
            cls = t.pkg.get_export_class(e)
            if s in t.export_name(i + 1).lower() or s in cls.lower():
                print("[%d] %s : %s (size %d)" % (i + 1, t.path(i + 1), cls, e["serial_size"]))

    def walk(i, depth, fn):
        cls = t.pkg.get_export_class(t.pkg.exports[i - 1])
        if cls in skip:
            return
        fn(i, depth, cls)
        for c in kids.get(i, []):
            walk(c, depth + 1, fn)

    if a.tree:
        for i in t.find(a.tree):
            walk(i, 0, lambda i, d, cls: print("%s[%d] %s : %s" % ("  " * d, i, t.export_name(i), cls)))
    if a.dump:
        for i in t.find(a.dump):
            print(t.dump(i))
    if a.dump_tree:
        for i in t.find(a.dump_tree):
            walk(i, 0, lambda i, d, cls: print(t.dump(i)))


if __name__ == "__main__":
    main()
