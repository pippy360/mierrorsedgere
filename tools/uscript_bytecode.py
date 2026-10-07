#!/usr/bin/env python3
"""
uscript_bytecode.py - UnrealScript (UE3, Mirror's Edge v536/43) bytecode decompiler.

Decodes the token tree of a function's script into readable pseudo-UnrealScript, one
statement per line, prefixed with its code offset (jumps refer to these offsets).

Usage:
    python3 tools/uscript_bytecode.py <package.u> Class.Function [Class.Function ...]
    python3 tools/uscript_bytecode.py <package.u> Class            # every function in the class
    python3 tools/uscript_bytecode.py <package.u> --selftest       # decode every function, report failures
    python3 tools/uscript_bytecode.py <package.u> --grep NAME      # functions whose code mentions NAME

Native function names / operators are resolved from Core.u, Engine.u, GameFramework.u and the
package itself (found next to the package).
"""

import os
import struct
import sys
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from uscript_decompiler import UE3Package  # noqa: E402

FUNC_FINAL = 0x00000001
FUNC_PREOPERATOR = 0x00000010
FUNC_NET = 0x00000040
FUNC_NATIVE = 0x00000400
FUNC_OPERATOR = 0x00001000

CAST_NAMES = {
    0x36: "InterfaceToObject", 0x37: "InterfaceToString", 0x38: "InterfaceToBool",
    0x39: "RotatorToVector", 0x3A: "ByteToInt", 0x3B: "ByteToBool", 0x3C: "ByteToFloat",
    0x3D: "IntToByte", 0x3E: "IntToBool", 0x3F: "IntToFloat", 0x40: "BoolToByte",
    0x41: "BoolToInt", 0x42: "BoolToFloat", 0x43: "FloatToByte", 0x44: "FloatToInt",
    0x45: "FloatToBool", 0x46: "ObjectToInterface", 0x47: "ObjectToBool", 0x48: "NameToBool",
    0x49: "StringToByte", 0x4A: "StringToInt", 0x4B: "StringToBool", 0x4C: "StringToFloat",
    0x4D: "StringToVector", 0x4E: "StringToRotator", 0x4F: "VectorToBool", 0x50: "VectorToRotator",
    0x51: "RotatorToBool", 0x52: "ByteToString", 0x53: "IntToString", 0x54: "BoolToString",
    0x55: "FloatToString", 0x56: "ObjectToString", 0x57: "NameToString", 0x58: "VectorToString",
    0x59: "RotatorToString", 0x5A: "DelegateToString", 0x60: "StringToName",
}
CAST_SHORT = {
    "ByteToInt": "int", "ByteToFloat": "float", "IntToByte": "byte", "IntToFloat": "float",
    "FloatToInt": "int", "FloatToByte": "byte", "BoolToInt": "int", "BoolToFloat": "float",
    "IntToBool": "bool", "FloatToBool": "bool", "ByteToBool": "bool", "ObjectToBool": "bool",
    "RotatorToVector": "vector", "VectorToRotator": "rotator", "IntToString": "string",
    "FloatToString": "string", "NameToString": "string", "ObjectToString": "string",
    "BoolToString": "string", "ByteToString": "string", "VectorToString": "string",
    "RotatorToString": "string", "StringToName": "name", "StringToInt": "int",
    "StringToFloat": "float", "VectorToBool": "bool", "RotatorToBool": "bool", "NameToBool": "bool",
}


class DecodeError(Exception):
    pass


class NativeTable:
    """iNative -> (name, friendly name, flags) across the script packages."""

    def __init__(self, pkg_paths: List[str]):
        self.by_index: Dict[int, Tuple[str, str, int, int]] = {}
        for p in pkg_paths:
            if not os.path.isfile(p):
                continue
            pkg = UE3Package(p)
            for cls in pkg.classes.values():
                for fn in cls["functions"]:
                    info = function_info(pkg, fn)
                    if info["native"] and info["iNative"]:
                        self.by_index.setdefault(info["iNative"], (fn["name"], info["friendly"],
                                                                    info["flags"], info["precedence"]))

    def get(self, idx: int):
        return self.by_index.get(idx)


def function_info(pkg: UE3Package, fn: dict) -> dict:
    """Trailing UFunction fields after the script: iNative, precedence, flags, [RepOffset], FriendlyName."""
    exp = pkg.exports[fn["export_index"] - 1] if "export_index" in fn else None
    data = pkg.raw_data[fn["offset"]: fn["offset"] + fn["size"]]
    code_sz = struct.unpack_from("<i", data, 40)[0]
    t = 44 + code_sz
    i_native, prec = struct.unpack_from("<HB", data, t)
    flags = struct.unpack_from("<I", data, t + 3)[0]
    p = t + 7
    if flags & FUNC_NET:
        p += 2
    friendly = fn["name"]
    if p + 8 <= len(data):
        n_idx, n_num = struct.unpack_from("<ii", data, p)
        if 0 <= n_idx < len(pkg.names):
            friendly = pkg._get_name(n_idx, n_num)
    return {"iNative": i_native, "precedence": prec, "flags": flags, "friendly": friendly,
            "native": bool(flags & FUNC_NATIVE), "code": data[44:44 + code_sz]}


class Decoder:
    def __init__(self, pkg: UE3Package, natives: NativeTable, code: bytes):
        self.pkg = pkg
        self.nat = natives
        self.code = code
        self.pos = 0

    # --- primitive readers ---------------------------------------------------------------
    def u8(self) -> int:
        if self.pos >= len(self.code):
            raise DecodeError("read past end")
        v = self.code[self.pos]
        self.pos += 1
        return v

    def u16(self) -> int:
        v = struct.unpack_from("<H", self.code, self.pos)[0]
        self.pos += 2
        return v

    def i32(self) -> int:
        v = struct.unpack_from("<i", self.code, self.pos)[0]
        self.pos += 4
        return v

    def f32(self) -> float:
        v = struct.unpack_from("<f", self.code, self.pos)[0]
        self.pos += 4
        return v

    def ref(self) -> str:
        return self.pkg.resolve_ref(self.i32())

    def name(self) -> str:
        idx, num = struct.unpack_from("<ii", self.code, self.pos)
        self.pos += 8
        return self.pkg._get_name(idx, num)

    def zstr(self) -> str:
        end = self.code.index(b"\x00", self.pos)
        s = self.code[self.pos:end].decode("latin1")
        self.pos = end + 1
        return s

    def wstr(self) -> str:
        out = []
        while True:
            c = self.u16()
            if c == 0:
                break
            out.append(chr(c))
        return "".join(out)

    # --- expressions -----------------------------------------------------------------------
    def params(self) -> List[str]:
        args = []
        while True:
            if self.pos >= len(self.code):
                raise DecodeError("unterminated parameter list")
            if self.code[self.pos] == 0x16:
                self.pos += 1
                return args
            args.append(self.expr())

    def call_native(self, idx: int) -> str:
        info = self.nat.get(idx)
        args = self.params()
        if info is None:
            return f"native{idx}({', '.join(args)})"
        name, friendly, flags, _prec = info
        if flags & FUNC_OPERATOR:
            if flags & FUNC_PREOPERATOR or len(args) == 1:
                if len(args) == 1:
                    return f"{friendly}({args[0]})" if friendly.isalpha() else f"{friendly}{args[0]}"
            if len(args) == 2:
                return f"({args[0]} {friendly} {args[1]})"
            if len(args) == 1:  # post operator
                return f"({args[0]}{friendly})"
        return f"{name}({', '.join(args)})"

    def expr(self) -> str:
        start = self.pos
        op = self.u8()
        if op in (0x00, 0x01, 0x02, 0x48):
            r = self.ref()
            return f"default.{r}" if op == 0x02 else r
        if op == 0x03:
            return self.ref()
        if op == 0x04:
            inner = self.expr()
            return "return" if inner in ("<nothing>", "<retval>") else f"return {inner}"
        if op == 0x05:
            self.u16()  # size of the switched value (0 for strings)
            return f"switch ({self.expr()})"
        if op == 0x06:
            return f"goto 0x{self.u16():04x}"
        if op == 0x07:
            tgt = self.u16()
            return f"if (!({self.expr()})) goto 0x{tgt:04x}"
        if op == 0x08:
            return "stop"
        if op == 0x09:
            line = self.u16()
            self.u8()
            return f"assert({self.expr()})  // line {line}"
        if op == 0x0A:
            nxt = self.u16()
            if nxt == 0xFFFF:
                return "default:"
            return f"case {self.expr()}:  // next 0x{nxt:04x}"
        if op == 0x0B:
            return "<nothing>"
        if op == 0x0C:
            labels = []
            while True:
                nm = self.name()
                off = struct.unpack_from("<I", self.code, self.pos)[0]
                self.pos += 4
                if nm == "None":
                    break
                labels.append(f"{nm}@0x{off:04x}")
            return f"labeltable {' '.join(labels)}"
        if op == 0x0D:
            return f"goto label {self.expr()}"
        if op == 0x0E:
            self.ref()
            return self.expr()
        if op == 0x0F:
            lhs = self.expr()
            return f"{lhs} = {self.expr()}"
        if op in (0x10, 0x1A):
            idx = self.expr()
            arr = self.expr()
            return f"{arr}[{idx}]"
        if op == 0x11:
            parts = [self.expr() for _ in range(5)]
            return f"new({parts[0]}, {parts[1]}, {parts[2]}) {parts[3]}({parts[4]})"
        if op in (0x12, 0x19):
            obj = self.expr()
            self.u16()  # skip size: the context expression's length
            self.u16()  # property size / flags (2 bytes in v536/43)
            member = self.expr()
            if op == 0x12:
                return f"{obj}.static.{member}"
            return f"{obj}.{member}"
        if op == 0x13:
            cls = self.ref()
            return f"class<{cls}>({self.expr()})"
        if op == 0x14:
            lhs = self.expr()
            return f"{lhs} = {self.expr()}"
        if op == 0x15:
            return "<endparm>"
        if op == 0x16:
            raise DecodeError(f"unexpected EndFunctionParms at 0x{start:04x}")
        if op == 0x17:
            return "self"
        if op == 0x18:
            self.u16()
            return self.expr()
        if op == 0x1B:
            nm = self.name()
            return f"{nm}({', '.join(self.params())})"
        if op == 0x1C:
            fn = self.ref()
            return f"{fn}({', '.join(self.params())})"
        if op == 0x1D:
            return str(self.i32())
        if op == 0x1E:
            return repr(round(self.f32(), 6))
        if op == 0x1F:
            return f'"{self.zstr()}"'
        if op == 0x20:
            return self.ref()
        if op == 0x21:
            return f"'{self.name()}'"
        if op == 0x22:
            p, y, r = self.i32(), self.i32(), self.i32()
            return f"rot({p}, {y}, {r})"
        if op == 0x23:
            x, y, z = self.f32(), self.f32(), self.f32()
            return f"vect({x:g}, {y:g}, {z:g})"
        if op == 0x24:
            return str(self.u8())
        if op == 0x25:
            return "0"
        if op == 0x26:
            return "1"
        if op == 0x27:
            return "true"
        if op == 0x28:
            return "false"
        if op == 0x29:
            return f"<nativeparm {self.ref()}>"
        if op == 0x2A:
            return "None"
        if op == 0x2C:
            return str(self.u8())
        if op == 0x2D:
            return self.expr()
        if op == 0x2E:
            cls = self.ref()
            return f"{cls}({self.expr()})"
        if op == 0x2F:
            it = self.expr()
            end = self.u16()
            return f"foreach {it}  // end 0x{end:04x}"
        if op == 0x30:
            return "iterator pop"
        if op == 0x31:
            return "iterator next"
        if op in (0x32, 0x33):
            self.ref()
            a = self.expr()
            b = self.expr()
            return f"({a} {'==' if op == 0x32 else '!='} {b})"
        if op == 0x34:
            return f'"{self.wstr()}"'
        if op == 0x35:
            member = self.ref()
            self.ref()  # owning struct
            self.u8()
            self.u8()
            return f"{self.expr()}.{member}"
        if op == 0x36:
            return f"{self.expr()}.Length"
        if op == 0x37:
            nm = self.name()
            return f"global.{nm}({', '.join(self.params())})"
        if op == 0x38:
            ct = self.u8()
            inner = self.expr()
            cn = CAST_NAMES.get(ct, f"cast{ct:02x}")
            return f"{CAST_SHORT.get(cn, cn)}({inner})"
        if op == 0x39:
            arr = self.expr()
            a = self.expr()
            b = self.expr()
            return f"{arr}.Insert({a}, {b})"
        if op == 0x3A:
            self.ref()
            return "<retval>"
        if op in (0x3B, 0x3C, 0x3D, 0x3E):
            a = self.expr()
            b = self.expr()
            self.u8()  # EndFunctionParms
            return f"({a} {'==' if op in (0x3B, 0x3D) else '!='} {b})"
        if op == 0x3F:
            return "None"
        if op == 0x40:
            arr = self.expr()
            a = self.expr()
            b = self.expr()
            return f"{arr}.Remove({a}, {b})"
        if op == 0x41:
            self.i32()
            line = self.i32()
            self.i32()
            self.u8()
            return f"<debuginfo line {line}>"
        if op == 0x42:
            self.u8()
            prop = self.ref()
            nm = self.name()
            return f"{prop}({', '.join(self.params())})  // delegate {nm}"
        if op == 0x43:
            return f"<delegate {self.name()}>"
        if op == 0x44:
            lhs = self.expr()
            return f"{lhs} = {self.expr()}"
        if op == 0x45:
            c = self.expr()
            self.u16()
            a = self.expr()
            self.u16()
            b = self.expr()
            return f"(({c}) ? {a} : {b})"
        if op == 0x46:
            arr = self.expr()
            self.u16()
            v = self.expr()
            return f"{arr}.Find({v})"
        if op == 0x47:
            arr = self.expr()
            self.u16()
            n = self.expr()
            v = self.expr()
            return f"{arr}.Find({n}, {v})"
        if op == 0x49:
            self.u16()
            v = self.expr()
            self.u8()  # EndParmValue
            return f"<default parm {v}>"
        if op == 0x4A:
            return "<empty>"
        if op == 0x4B:
            return f"<instance delegate {self.name()}>"
        if op == 0x51:
            return self.expr()
        if op == 0x52:
            cls = self.ref()
            return f"{cls}({self.expr()})"
        if op == 0x53:
            return "<end>"
        if op == 0x54:
            arr = self.expr()
            return f"{arr}.Add({self.expr()})"
        if op in (0x55, 0x56):
            arr = self.expr()
            self.u16()
            v = self.expr()
            return f"{arr}.{'AddItem' if op == 0x55 else 'RemoveItem'}({v})"
        if op == 0x57:
            arr = self.expr()
            self.u16()
            i = self.expr()
            v = self.expr()
            return f"{arr}.InsertItem({i}, {v})"
        if op == 0x58:
            arr = self.expr()
            item = self.expr()
            has_index = self.u8()
            index = self.expr()
            end = self.u16()
            idx = f", {index}" if has_index else ""
            return f"foreach {arr}({item}{idx})  // end 0x{end:04x}"
        if 0x60 <= op <= 0x6F:
            idx = ((op & 0x0F) << 8) | self.u8()
            return self.call_native(idx)
        if op >= 0x70:
            return self.call_native(op)
        raise DecodeError(f"unknown token 0x{op:02x} at 0x{start:04x}")

    def statements(self) -> List[Tuple[int, str]]:
        out = []
        while self.pos < len(self.code):
            at = self.pos
            if self.code[at] == 0x53:
                out.append((at, "<end>"))
                self.pos += 1
                break
            out.append((at, self.expr()))
        return out


def script_paths(pkg_path: str) -> List[str]:
    d = os.path.dirname(os.path.abspath(pkg_path))
    names = ["Core.u", "Engine.u", "GameFramework.u", "TdGame.u"]
    paths = [os.path.join(d, n) for n in names]
    if os.path.abspath(pkg_path) not in paths:
        paths.append(os.path.abspath(pkg_path))
    return paths


def state_functions(pkg: UE3Package, cname: str, sname: str, fname: str = ""):
    """Functions of state `sname` (of class `cname`, any class if empty): their outer is the State."""
    out = []
    n = len(pkg.exports)
    for i, e in enumerate(pkg.exports):
        if pkg.resolve_ref(e.class_index) != "Function":
            continue
        if fname and e.object_name.lower() != fname.lower():
            continue
        if not 0 < e.outer_index <= n:
            continue
        st = pkg.exports[e.outer_index - 1]
        if pkg.resolve_ref(st.class_index) != "State" or st.object_name.lower() != sname.lower():
            continue
        owner = pkg.exports[st.outer_index - 1].object_name if 0 < st.outer_index <= n else "?"
        if cname and owner.lower() != cname.lower():
            continue
        fn = pkg._parse_function(e)
        fn["export_index"] = i + 1
        out.append((f"{owner}.{st.object_name}", fn))
    return out


def find_functions(pkg: UE3Package, target: str):
    if target.count(".") == 2:  # Class.State.Function
        cname, sname, fname = target.split(".")
        return state_functions(pkg, cname, sname, fname)
    if "." in target:
        cname, fname = target.split(".", 1)
        cls = pkg.classes.get(cname)
        if not cls:
            return state_functions(pkg, "", cname, fname)  # State.Function
        return [(cname, fn) for fn in cls["functions"] if fn["name"].lower() == fname.lower()]
    cls = pkg.classes.get(target)
    if cls:
        return [(target, fn) for fn in cls["functions"]]
    return [(c, fn) for c, cl in pkg.classes.items() for fn in cl["functions"] if fn["name"].lower() == target.lower()]


def decode(pkg: UE3Package, natives: NativeTable, fn: dict) -> List[Tuple[int, str]]:
    info = function_info(pkg, fn)
    return Decoder(pkg, natives, info["code"]).statements()


def function_signature(pkg: UE3Package, cname: str, fn: dict) -> str:
    """Parameters and locals: the properties whose outer is this function."""
    idx = next((i + 1 for i, e in enumerate(pkg.exports)
                if e.serial_offset == fn["offset"] and e.object_name == fn["name"]), 0)
    parms = []
    for e in pkg.exports:
        if idx and e.outer_index == idx:
            t = pkg.resolve_ref(e.class_index).replace("Property", "")
            parms.append(f"{t} {e.object_name}")
    return f"{cname}.{fn['name']}  [{'; '.join(parms)}]"


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    pkg_path = sys.argv[1]
    pkg = UE3Package(pkg_path)
    natives = NativeTable(script_paths(pkg_path))
    args = sys.argv[2:]
    if args[0] == "--selftest":
        ok = bad = 0
        fails = []
        for cname, cls in pkg.classes.items():
            for fn in cls["functions"]:
                info = function_info(pkg, fn)
                if not info["code"]:
                    continue
                d = Decoder(pkg, natives, info["code"])
                try:
                    st = d.statements()
                    if d.pos != len(info["code"]) or not st or st[-1][1] != "<end>":
                        raise DecodeError(f"stopped at {d.pos}/{len(info['code'])}")
                    ok += 1
                except Exception as e:  # noqa: BLE001
                    bad += 1
                    fails.append(f"{cname}.{fn['name']}: {e}")
        print(f"decoded {ok}, failed {bad}")
        for f in fails[:40]:
            print("  " + f)
        return
    if args[0] == "--grep":
        needle = args[1].lower()
        for cname, cls in sorted(pkg.classes.items()):
            for fn in cls["functions"]:
                info = function_info(pkg, fn)
                if not info["code"]:
                    continue
                try:
                    st = Decoder(pkg, natives, info["code"]).statements()
                except Exception:  # noqa: BLE001
                    continue
                hits = [s for _, s in st if needle in s.lower()]
                if hits:
                    print(f"{cname}.{fn['name']}: {len(hits)} hit(s)")
        return
    for target in args:
        for cname, fn in find_functions(pkg, target):
            print(f"// {function_signature(pkg, cname, fn)}")
            try:
                for at, s in decode(pkg, natives, fn):
                    print(f"  [{at:04x}] {s}")
            except Exception as e:  # noqa: BLE001
                print(f"  !! decode error: {e}")
            print()


if __name__ == "__main__":
    main()
