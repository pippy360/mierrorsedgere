#!/usr/bin/env python3
"""
uscript_decompiler.py - UnrealScript Package Parser and Decompiler for Mirror's Edge (UE3)

Extracts class hierarchy, properties, DefaultProperties archetypes, and UnrealScript
bytecode from Unreal Engine 3 cooked packages (e.g. TdGame.u, Core.u, Engine.u, etc.).

Usage:
    python3 tools/uscript_decompiler.py <path/to/package.u> [options]

Options:
    --info                  Print package header summary, export/import counts.
    --list-classes          List all classes defined in this package.
    --dump-class <Name>     Print decompiled UnrealScript source (.uc) for a class.
    --dump-bytecode <Name>  Disassemble bytecode for a specific function or all functions in a class.
    --dump-all [DIR]        Decompile all classes in package to .uc files (default: /tmp/mirrorsedge-uscript-dump/<Package>/).
    --json                  Output class metadata and hierarchy in JSON format.
"""

import sys
import os
import struct
import ctypes
import ctypes.util
import argparse
import json
from typing import Dict, List, Tuple, Optional, Any


# ==============================================================================
# LZO Decompression Support
# ==============================================================================

class LZODecompressor:
    """Handles LZO1X-1 decompression with ctypes liblzo2 or pure Python fallback."""
    
    _lib = None
    _func = None

    @classmethod
    def _init_ctypes(cls):
        if cls._func is not None:
            return True
        candidates = [
            "/opt/homebrew/lib/liblzo2.dylib",
            "/usr/local/lib/liblzo2.dylib",
            "/usr/lib/liblzo2.dylib",
            ctypes.util.find_library("lzo2"),
            ctypes.util.find_library("lzo")
        ]
        for c in candidates:
            if not c:
                continue
            try:
                lib = ctypes.CDLL(c)
                if hasattr(lib, "lzo1x_decompress_safe"):
                    cls._lib = lib
                    cls._func = lib.lzo1x_decompress_safe
                    cls._func.argtypes = [
                        ctypes.c_char_p,
                        ctypes.c_size_t,
                        ctypes.c_char_p,
                        ctypes.POINTER(ctypes.c_size_t),
                        ctypes.c_void_p
                    ]
                    cls._func.restype = ctypes.c_int
                    return True
            except Exception:
                continue
        return False

    @classmethod
    def decompress(cls, comp_data: bytes, uncomp_size: int) -> bytes:
        if cls._init_ctypes():
            dst = ctypes.create_string_buffer(uncomp_size)
            dst_len = ctypes.c_size_t(uncomp_size)
            res = cls._func(comp_data, len(comp_data), dst, ctypes.byref(dst_len), None)
            if res != 0:
                raise RuntimeError(f"lzo1x_decompress_safe returned error {res}")
            return dst.raw[:dst_len.value]
        else:
            return cls._decompress_pure_python(comp_data, uncomp_size)

    @classmethod
    def _decompress_pure_python(cls, src: bytes, expected_len: int) -> bytes:
        """Pure-Python fallback decompressor for LZO1X format."""
        dst = bytearray()
        ip = 0
        src_len = len(src)
        
        t = src[ip]
        ip += 1
        if t > 17:
            t -= 17
            dst.extend(src[ip:ip+t])
            ip += t
            if ip >= src_len:
                return bytes(dst)
            t = src[ip]
            ip += 1

        while ip < src_len:
            if t < 16:
                if t == 0:
                    while ip < src_len and src[ip] == 0:
                        t += 255
                        ip += 1
                    if ip >= src_len:
                        break
                    t += 15 + src[ip]
                    ip += 1
                t += 3
                dst.extend(src[ip:ip+t])
                ip += t
                if ip >= src_len:
                    break
                t = src[ip]
                ip += 1
                if t < 16:
                    m_off = 1 + 0x0800 + (t >> 2) + (src[ip] << 2)
                    ip += 1
                    m_pos = len(dst) - m_off
                    dst.append(dst[m_pos])
                    dst.append(dst[m_pos+1])
                    dst.append(dst[m_pos+2])
                    t = src[ip-2] & 3
                    if t == 0:
                        if ip >= src_len:
                            break
                        t = src[ip]
                        ip += 1
                        continue
                    dst.extend(src[ip:ip+t])
                    ip += t
                    if ip >= src_len:
                        break
                    t = src[ip]
                    ip += 1

            while True:
                if t >= 64:
                    m_len = (t >> 5) - 1 + 2
                    m_off = ((t >> 2) & 7) + (src[ip] << 3) + 1
                    ip += 1
                elif t >= 32:
                    m_len = t & 31
                    if m_len == 0:
                        while ip < src_len and src[ip] == 0:
                            m_len += 255
                            ip += 1
                        m_len += 31 + src[ip]
                        ip += 1
                    m_len += 2
                    m_off = src[ip] | (src[ip+1] << 8)
                    ip += 2
                    m_off = (m_off >> 2) + 1
                elif t >= 16:
                    m_off = (t & 8) << 11
                    m_len = t & 7
                    if m_len == 0:
                        while ip < src_len and src[ip] == 0:
                            m_len += 255
                            ip += 1
                        m_len += 7 + src[ip]
                        ip += 1
                    m_len += 2
                    m_off |= (src[ip] | (src[ip+1] << 8)) >> 2
                    ip += 2
                    if m_off == 0:
                        return bytes(dst)
                    m_off += 0x4000
                else:
                    break

                m_pos = len(dst) - m_off
                for _ in range(m_len):
                    dst.append(dst[m_pos])
                    m_pos += 1
                
                t = src[ip-2] & 3
                if t == 0:
                    if ip >= src_len:
                        return bytes(dst)
                    t = src[ip]
                    ip += 1
                    break
                dst.extend(src[ip:ip+t])
                ip += t
                if ip >= src_len:
                    return bytes(dst)
                t = src[ip]
                ip += 1
                break

        return bytes(dst)


# ==============================================================================
# UnrealScript Bytecode Opcodes
# ==============================================================================

EX_OPCODES = {
    0x00: "EX_LocalVariable",
    0x01: "EX_InstanceVariable",
    0x02: "EX_DefaultVariable",
    0x03: "EX_StateVariable",
    0x04: "EX_Return",
    0x05: "EX_Switch",
    0x06: "EX_Jump",
    0x07: "EX_JumpIfNot",
    0x08: "EX_Stop",
    0x09: "EX_Assert",
    0x0A: "EX_Case",
    0x0B: "EX_Nothing",
    0x0C: "EX_LabelTable",
    0x0D: "EX_GotoLabel",
    0x0E: "EX_EatReturnValue",
    0x0F: "EX_Let",
    0x10: "EX_DynArrayElement",
    0x11: "EX_New",
    0x12: "EX_ClassContext",
    0x13: "EX_Metacast",
    0x14: "EX_LetBool",
    0x15: "EX_EndCode",
    0x16: "EX_EndParmValue",
    0x17: "EX_Self",
    0x18: "EX_Skip",
    0x19: "EX_Context",
    0x1A: "EX_ArrayElement",
    0x1B: "EX_VirtualFunction",
    0x1C: "EX_FinalFunction",
    0x1D: "EX_IntConst",
    0x1E: "EX_FloatConst",
    0x1F: "EX_StringConst",
    0x20: "EX_ObjectConst",
    0x21: "EX_NameConst",
    0x22: "EX_RotatorConst",
    0x23: "EX_VectorConst",
    0x24: "EX_ByteConst",
    0x25: "EX_IntZero",
    0x26: "EX_IntOne",
    0x27: "EX_True",
    0x28: "EX_False",
    0x29: "EX_NativeParm",
    0x2A: "EX_NoObject",
    0x2B: "EX_IntConstByte",
    0x2C: "EX_BoolVariable",
    0x2D: "EX_DynamicCast",
    0x2E: "EX_Iterator",
    0x2F: "EX_IteratorPop",
    0x30: "EX_IteratorNext",
    0x31: "EX_StructCmpEq",
    0x32: "EX_StructCmpNe",
    0x33: "EX_UnicodeStringConst",
    0x34: "EX_StructMember",
    0x35: "EX_DynArrayLength",
    0x36: "EX_GlobalFunction",
    0x37: "EX_PrimitiveCast",
    0x38: "EX_DynArrayInsert",
    0x39: "EX_DynArrayRemove",
    0x3A: "EX_DebugInfo",
    0x3B: "EX_DelegateFunction",
    0x3C: "EX_DelegateProperty",
    0x3D: "EX_LetDelegate",
    0x3E: "EX_Conditional",
    0x3F: "EX_DynArrayFind",
    0x40: "EX_DynArrayFindStruct",
    0x41: "EX_DynArrayAdd",
    0x42: "EX_DynArrayAddItem",
    0x43: "EX_DynArrayRemoveItem",
    0x44: "EX_DynArrayInsertItem",
    0x45: "EX_DynArraySort",
    0x46: "EX_FilterEditorOnly",
    0x53: "EX_EndOfScript",
}


# ==============================================================================
# UE3 Package Parser
# ==============================================================================

class FObjectImport:
    __slots__ = ("class_package", "class_name", "outer_index", "object_name")
    def __init__(self, cp: str, cn: str, outer: int, on: str):
        self.class_package = cp
        self.class_name = cn
        self.outer_index = outer
        self.object_name = on

    def __repr__(self):
        return f"<Import {self.class_package}.{self.class_name} '{self.object_name}'>"


class FObjectExport:
    __slots__ = (
        "class_index", "super_index", "outer_index", "object_name",
        "archetype", "object_flags", "serial_size", "serial_offset",
        "export_flags"
    )
    def __init__(self, c_idx, s_idx, o_idx, name, arch, flags, sz, off, exp_flags=0):
        self.class_index = c_idx
        self.super_index = s_idx
        self.outer_index = o_idx
        self.object_name = name
        self.archetype = arch
        self.object_flags = flags
        self.serial_size = sz
        self.serial_offset = off
        self.export_flags = exp_flags

    def __repr__(self):
        return f"<Export '{self.object_name}' sz={self.serial_size} off={self.serial_offset}>"


class UE3Package:
    PACKAGE_FILE_TAG = 0x9E2A83C1

    def __init__(self, path: str):
        self.path = os.path.abspath(path)
        self.package_name = os.path.splitext(os.path.basename(path))[0]
        self.raw_data = b""
        self.file_version = 0
        self.licensee_version = 0
        self.total_header_size = 0
        self.folder_name = ""
        self.package_flags = 0
        self.names: List[str] = []
        self.imports: List[FObjectImport] = []
        self.exports: List[FObjectExport] = []
        self.classes: Dict[str, Dict[str, Any]] = {}

        self._load()

    def _load(self):
        with open(self.path, "rb") as f:
            file_bytes = f.read()

        if len(file_bytes) < 64:
            raise ValueError(f"File {self.path} too small to be a UE3 package.")

        tag, self.file_version, self.licensee_version = struct.unpack_from("<IHH", file_bytes, 0)
        if tag != self.PACKAGE_FILE_TAG:
            raise ValueError(f"Invalid UE3 package magic: 0x{tag:08x} (expected 0x{self.PACKAGE_FILE_TAG:08x})")

        self.total_header_size = struct.unpack_from("<i", file_bytes, 8)[0]
        folder_name_len = struct.unpack_from("<i", file_bytes, 12)[0]
        pos = 16 + max(0, folder_name_len)
        
        (
            self.package_flags,
            name_count, name_offset,
            export_count, export_offset,
            import_count, import_offset,
            depends_offset
        ) = struct.unpack_from("<8I", file_bytes, pos)
        pos += 32 + 16 # GUID (16 bytes)

        gen_count = struct.unpack_from("<i", file_bytes, pos)[0]
        pos += 4 + gen_count * 12

        eng_ver, cooked_ver, comp_flags = struct.unpack_from("<3I", file_bytes, pos)
        pos += 12

        chunk_count = struct.unpack_from("<i", file_bytes, pos)[0]
        pos += 4

        # Decompress if package uses LZO chunks
        if chunk_count > 0:
            chunks = [struct.unpack_from("<4I", file_bytes, pos + i * 16) for i in range(chunk_count)]
            max_uncomp = max(c[0] + c[1] for c in chunks)
            decomp_buf = bytearray(max_uncomp)
            # Copy uncompressed header
            decomp_buf[:chunks[0][0]] = file_bytes[:chunks[0][0]]

            for u_off, u_sz, c_off, c_sz in chunks:
                c_tag, blk_sz, c_comp_sz, c_uncomp_sz = struct.unpack_from("<4I", file_bytes, c_off)
                assert c_tag == self.PACKAGE_FILE_TAG, f"Invalid chunk tag: {hex(c_tag)}"
                num_blks = (c_uncomp_sz + blk_sz - 1) // blk_sz
                b_pos = c_off + 16
                cur_u = u_off
                b_data = c_off + 16 + num_blks * 8
                for _ in range(num_blks):
                    b_csz, b_usz = struct.unpack_from("<2I", file_bytes, b_pos)
                    b_pos += 8
                    comp_block = file_bytes[b_data : b_data + b_csz]
                    b_data += b_csz
                    uncomp_block = LZODecompressor.decompress(comp_block, b_usz)
                    decomp_buf[cur_u : cur_u + b_usz] = uncomp_block
                    cur_u += b_usz
            self.raw_data = bytes(decomp_buf)
        else:
            self.raw_data = file_bytes

        # Read Name Table
        self.names = []
        n_pos = name_offset
        for _ in range(name_count):
            s_len = struct.unpack_from("<i", self.raw_data, n_pos)[0]
            n_pos += 4
            if s_len < 0:
                s = self.raw_data[n_pos : n_pos + (-s_len) * 2].decode("utf-16le")
                n_pos += (-s_len) * 2
            else:
                s = self.raw_data[n_pos : n_pos + s_len].decode("latin1", "replace")
                n_pos += s_len
            n_pos += 8 # FNameEntry ObjectFlags uint64
            self.names.append(s.rstrip("\x00"))

        # Read Import Table
        self.imports = []
        i_pos = import_offset
        for _ in range(import_count):
            cp_idx, cp_num, cn_idx, cn_num, pkg_idx, on_idx, on_num = struct.unpack_from("<iiiii ii", self.raw_data, i_pos)
            i_pos += 28
            self.imports.append(FObjectImport(
                self._get_name(cp_idx, cp_num),
                self._get_name(cn_idx, cn_num),
                pkg_idx,
                self._get_name(on_idx, on_num)
            ))

        # Read Export Table
        self.exports = []
        e_pos = export_offset
        for _ in range(export_count):
            c_idx, s_idx, o_idx, nm_idx, nm_num, arch = struct.unpack_from("<iiiiii", self.raw_data, e_pos)
            fl_lo, fl_hi = struct.unpack_from("<II", self.raw_data, e_pos + 24)
            sz, off = struct.unpack_from("<ii", self.raw_data, e_pos + 32)
            comp_map_cnt = struct.unpack_from("<i", self.raw_data, e_pos + 40)[0]
            e_pos += 44 + comp_map_cnt * 12
            exp_flags = struct.unpack_from("<i", self.raw_data, e_pos)[0]
            e_pos += 4
            gen_net_cnt = struct.unpack_from("<i", self.raw_data, e_pos)[0]
            e_pos += 4 + gen_net_cnt * 4 + 16 + 4 # 16 byte guid + 4 byte pkg flags

            self.exports.append(FObjectExport(
                c_idx, s_idx, o_idx,
                self._get_name(nm_idx, nm_num),
                arch, (fl_hi << 32) | fl_lo, sz, off, exp_flags
            ))

        # Reconstruct Classes and Children
        self._build_hierarchy()

    def _get_name(self, idx: int, num: int = 0) -> str:
        if 0 <= idx < len(self.names):
            nm = self.names[idx]
            return f"{nm}_{num-1}" if num > 0 else nm
        return f"UnknownName_{idx}"

    def resolve_ref(self, idx: int) -> str:
        if idx == 0:
            return "None"
        elif idx > 0 and idx <= len(self.exports):
            return self.exports[idx - 1].object_name
        elif idx < 0 and -idx <= len(self.imports):
            return self.imports[-idx - 1].object_name
        return f"Ref_{idx}"

    def resolve_full_ref(self, idx: int) -> str:
        if idx == 0:
            return "None"
        elif idx > 0 and idx <= len(self.exports):
            exp = self.exports[idx - 1]
            return exp.object_name
        elif idx < 0 and -idx <= len(self.imports):
            imp = self.imports[-idx - 1]
            return f"{imp.class_package}.{imp.object_name}"
        return f"Ref_{idx}"

    def _build_hierarchy(self):
        self.classes = {}

        # 1. Identify UClass exports (class_index == 0 or resolves to "Class")
        for i, exp in enumerate(self.exports):
            c_type = self.resolve_ref(exp.class_index)
            if exp.class_index == 0 or c_type == "Class":
                self.classes[exp.object_name] = {
                    "export_index": i + 1,
                    "name": exp.object_name,
                    "super_class": self.resolve_full_ref(exp.super_index),
                    "outer": self.resolve_ref(exp.outer_index),
                    "flags": exp.object_flags,
                    "consts": [],
                    "enums": [],
                    "structs": [],
                    "properties": [],
                    "functions": [],
                    "states": [],
                    "default_properties": {}
                }

        # 2. Attach children to their owning class
        for i, exp in enumerate(self.exports):
            if exp.outer_index > 0 and exp.outer_index <= len(self.exports):
                parent_name = self.exports[exp.outer_index - 1].object_name
                if parent_name in self.classes:
                    c_type = self.resolve_ref(exp.class_index)
                    cls = self.classes[parent_name]
                    entry = {
                        "export_index": i + 1,
                        "name": exp.object_name,
                        "type": c_type,
                        "size": exp.serial_size,
                        "offset": exp.serial_offset
                    }
                    if c_type == "Const":
                        cls["consts"].append(entry)
                    elif c_type == "Enum":
                        cls["enums"].append(self._parse_enum(exp))
                    elif c_type == "ScriptStruct":
                        cls["structs"].append(self._parse_struct(exp))
                    elif "Property" in c_type:
                        cls["properties"].append(entry)
                    elif c_type == "Function":
                        cls["functions"].append(self._parse_function(exp))
                    elif c_type == "State":
                        cls["states"].append(entry)

        # 3. Parse DefaultProperties (Default__* archetype objects)
        for exp in self.exports:
            if exp.object_name.startswith("Default__"):
                cls_name = exp.object_name[9:]
                if cls_name in self.classes:
                    p_bytes = self.raw_data[exp.serial_offset : exp.serial_offset + exp.serial_size]
                    self.classes[cls_name]["default_properties"] = self._parse_property_tags(p_bytes, 4)

    def _parse_enum(self, exp: FObjectExport) -> Dict[str, Any]:
        data = self.raw_data[exp.serial_offset : exp.serial_offset + exp.serial_size]
        super_field, next_field = struct.unpack_from("<ii", data, 0)
        num_names = struct.unpack_from("<i", data, 20)[0]
        pos = 24
        names = []
        for _ in range(num_names):
            n_idx, n_num = struct.unpack_from("<ii", data, pos)
            pos += 8
            names.append(self._get_name(n_idx, n_num))
        return {
            "name": exp.object_name,
            "values": names
        }

    def _parse_struct(self, exp: FObjectExport) -> Dict[str, Any]:
        return {
            "name": exp.object_name,
            "size": exp.serial_size,
            "offset": exp.serial_offset
        }

    def _parse_function(self, exp: FObjectExport) -> Dict[str, Any]:
        data = self.raw_data[exp.serial_offset : exp.serial_offset + exp.serial_size]
        code_sz = struct.unpack_from("<i", data, 40)[0] if len(data) >= 44 else 0
        bytecode = data[44 : 44 + code_sz] if code_sz > 0 else b""
        trailing = data[44 + code_sz :]
        
        nat_tok = 0
        op_prec = 0
        fn_flags = 0
        if len(trailing) >= 7:
            nat_tok, op_prec = struct.unpack_from("<HB", trailing, 0)
            fn_flags = struct.unpack_from("<I", trailing, 3)[0]

        return {
            "name": exp.object_name,
            "size": exp.serial_size,
            "offset": exp.serial_offset,
            "bytecode_size": code_sz,
            "bytecode": bytecode,
            "native_token": nat_tok,
            "operator_precedence": op_prec,
            "function_flags": fn_flags
        }

    def _parse_property_tags(self, p_bytes: bytes, start_pos: int = 4) -> Dict[str, Any]:
        """Parses UE3 FPropertyTag serialized key-value pairs."""
        pos = start_pos
        props = {}
        while pos < len(p_bytes):
            n_idx, n_num = struct.unpack_from("<ii", p_bytes, pos)
            pos += 8
            tag_name = self._get_name(n_idx, n_num)
            if tag_name == "None":
                break
            t_idx, t_num = struct.unpack_from("<ii", p_bytes, pos)
            pos += 8
            type_name = self._get_name(t_idx, t_num)
            sz, arr_idx = struct.unpack_from("<ii", p_bytes, pos)
            pos += 8
            extra = None
            if type_name == "StructProperty":
                st_idx, st_num = struct.unpack_from("<ii", p_bytes, pos)
                pos += 8
                extra = self._get_name(st_idx, st_num)
            elif type_name == "BoolProperty":
                b_val = struct.unpack_from("<i", p_bytes, pos)[0]
                pos += 4
                extra = bool(b_val)
            
            val_bytes = p_bytes[pos : pos + sz]
            pos += sz
            
            formatted_val = self._format_value(type_name, sz, val_bytes, extra)
            key = f"{tag_name}[{arr_idx}]" if arr_idx > 0 else tag_name
            props[key] = {
                "type": type_name,
                "extra": extra,
                "size": sz,
                "value": formatted_val
            }
        return props

    def _format_value(self, type_name: str, sz: int, val_bytes: bytes, extra: Any) -> Any:
        if type_name == "FloatProperty" and sz == 4:
            return round(struct.unpack("<f", val_bytes)[0], 4)
        elif type_name == "IntProperty" and sz == 4:
            return struct.unpack("<i", val_bytes)[0]
        elif type_name == "BoolProperty":
            return extra
        elif type_name == "ByteProperty":
            if sz == 8:
                return self._get_name(struct.unpack("<i", val_bytes[:4])[0], struct.unpack("<i", val_bytes[4:8])[0])
            elif sz == 1:
                return val_bytes[0]
        elif type_name == "NameProperty" and sz == 8:
            return self._get_name(struct.unpack("<i", val_bytes[:4])[0], struct.unpack("<i", val_bytes[4:8])[0])
        elif type_name == "ObjectProperty" and sz == 4:
            return self.resolve_ref(struct.unpack("<i", val_bytes)[0])
        elif type_name == "StrProperty" and sz >= 4:
            sl = struct.unpack("<i", val_bytes[:4])[0]
            if sl > 0:
                return val_bytes[4 : 4 + sl].decode("latin1", "replace").rstrip("\0")
        elif type_name == "StructProperty":
            if extra == "Vector" and sz == 12:
                x, y, z = struct.unpack("<fff", val_bytes)
                return f"(X={x:.2f}, Y={y:.2f}, Z={z:.2f})"
            elif extra == "Rotator" and sz == 12:
                p, y, r = struct.unpack("<iii", val_bytes)
                return f"(Pitch={p}, Yaw={y}, Roll={r})"
            return f"<Struct {extra}>"
        return f"<{type_name} sz={sz}>"

    def disassemble_bytecode(self, bytecode: bytes) -> List[str]:
        """Disassembles UnrealScript bytecode bytes into readable instructions."""
        lines = []
        pc = 0
        b_len = len(bytecode)
        while pc < b_len:
            op = bytecode[pc]
            op_name = EX_OPCODES.get(op, f"EX_0x{op:02x}")
            start_pc = pc
            pc += 1

            arg_str = ""
            if op == 0x06: # EX_Jump
                target = struct.unpack_from("<H", bytecode, pc)[0] if pc + 2 <= b_len else 0
                pc += 2
                arg_str = f"0x{target:04x}"
            elif op == 0x07: # EX_JumpIfNot
                target = struct.unpack_from("<H", bytecode, pc)[0] if pc + 2 <= b_len else 0
                pc += 2
                arg_str = f"0x{target:04x}"
            elif op == 0x1D: # EX_IntConst
                val = struct.unpack_from("<i", bytecode, pc)[0] if pc + 4 <= b_len else 0
                pc += 4
                arg_str = f"{val}"
            elif op == 0x1E: # EX_FloatConst
                val = struct.unpack_from("<f", bytecode, pc)[0] if pc + 4 <= b_len else 0.0
                pc += 4
                arg_str = f"{val:.4f}"
            elif op == 0x2B: # EX_IntConstByte
                val = bytecode[pc] if pc < b_len else 0
                pc += 1
                arg_str = f"{val}"
            elif op in (0x01, 0x02, 0x20): # Object / Instance / Default Variable
                ref = struct.unpack_from("<i", bytecode, pc)[0] if pc + 4 <= b_len else 0
                pc += 4
                arg_str = f"{self.resolve_ref(ref)}"
            elif op == 0x21: # EX_NameConst
                if pc + 8 <= b_len:
                    n_idx, n_num = struct.unpack_from("<ii", bytecode, pc)
                    pc += 8
                    arg_str = f"'{self._get_name(n_idx, n_num)}'"

            lines.append(f"  [0x{start_pc:04x}] {op_name:<24} {arg_str}")
            if op == 0x53: # EX_EndOfScript
                break
        return lines

    def decompile_class_uc(self, class_name: str, include_bytecode: bool = False) -> str:
        """Generates a complete .uc representation for a class."""
        if class_name not in self.classes:
            return f"// Error: Class '{class_name}' not found in package {self.package_name}"

        cls = self.classes[class_name]
        lines = []
        lines.append(f"//-----------------------------------------------------------")
        lines.append(f"// Decompiled by uscript_decompiler from {self.package_name}.u")
        lines.append(f"// Class: {class_name}")
        lines.append(f"//-----------------------------------------------------------")
        
        super_str = f" extends {cls['super_class']}" if cls['super_class'] != "None" else ""
        lines.append(f"class {class_name}{super_str};")
        lines.append("")

        # Consts
        if cls["consts"]:
            lines.append("// --- Constants ---")
            for c in cls["consts"]:
                lines.append(f"const {c['name']} = ...;")
            lines.append("")

        # Enums
        if cls["enums"]:
            lines.append("// --- Enums ---")
            for e in cls["enums"]:
                lines.append(f"enum {e['name']}")
                lines.append("{")
                for val in e["values"]:
                    lines.append(f"    {val},")
                lines.append("};")
                lines.append("")

        # Properties
        if cls["properties"]:
            lines.append("// --- Properties ---")
            for p in cls["properties"]:
                prop_type = p["type"].replace("Property", "").lower()
                lines.append(f"var {prop_type} {p['name']};")
            lines.append("")

        # Functions
        if cls["functions"]:
            lines.append("// --- Functions ---")
            for fn in cls["functions"]:
                prefix = []
                flags = fn["function_flags"]
                if flags & 0x00000001: prefix.append("final")
                if flags & 0x00000400: prefix.append("native")
                if flags & 0x00000002: prefix.append("exec")
                if flags & 0x00000100: prefix.append("simulated")
                if flags & 0x08000000: prefix.append("event")
                prefix_str = " ".join(prefix) + " " if prefix else ""
                
                lines.append(f"{prefix_str}function {fn['name']}()")
                lines.append("{")
                if include_bytecode and fn["bytecode"]:
                    lines.append("    // Bytecode:")
                    for asm in self.disassemble_bytecode(fn["bytecode"]):
                        lines.append(f"  {asm}")
                else:
                    lines.append(f"    // CodeSize: {fn['bytecode_size']} bytes")
                lines.append("}")
                lines.append("")

        # Defaultproperties
        if cls["default_properties"]:
            lines.append("defaultproperties")
            lines.append("{")
            for k, p in sorted(cls["default_properties"].items()):
                lines.append(f"    {k}={p['value']}")
            lines.append("}")
            lines.append("")

        return "\n".join(lines)


# ==============================================================================
# CLI Entry Point
# ==============================================================================

def main():
    parser = argparse.ArgumentParser(description="UnrealScript package decompiler for Mirror's Edge (UE3)")
    parser.add_argument("package", help="Path to .u script package (e.g. TdGame.u)")
    parser.add_argument("--info", action="store_true", help="Print package summary")
    parser.add_argument("--list-classes", action="store_true", help="List all classes")
    parser.add_argument("--dump-class", type=str, metavar="NAME", help="Decompile specific class to UnrealScript .uc")
    parser.add_argument("--dump-bytecode", type=str, metavar="NAME", help="Disassemble bytecode for a function or class")
    parser.add_argument("--dump-all", nargs="?", const="/tmp/mirrorsedge-uscript-dump", metavar="DIR",
                        help="Decompile all classes to .uc files (default: /tmp/mirrorsedge-uscript-dump/<pkg>/)")
    parser.add_argument("--json", action="store_true", help="Output class hierarchy as JSON")

    args = parser.parse_args()

    if not os.path.isfile(args.package):
        print(f"Error: File not found: {args.package}", file=sys.stderr)
        sys.exit(1)

    try:
        pkg = UE3Package(args.package)
    except Exception as e:
        print(f"Error loading package {args.package}: {e}", file=sys.stderr)
        sys.exit(1)

    if args.info:
        print(f"Package: {pkg.package_name}")
        print(f"Version: {pkg.file_version} (Licensee: {pkg.licensee_version})")
        print(f"Total Header Size: {pkg.total_header_size} bytes")
        print(f"Flags: 0x{pkg.package_flags:08x}")
        print(f"Names: {len(pkg.names)}")
        print(f"Imports: {len(pkg.imports)}")
        print(f"Exports: {len(pkg.exports)}")
        print(f"Classes: {len(pkg.classes)}")
        return

    if args.list_classes:
        print(f"Classes defined in {pkg.package_name}.u ({len(pkg.classes)}):")
        for name, cinfo in sorted(pkg.classes.items()):
            super_str = f" extends {cinfo['super_class']}" if cinfo['super_class'] != "None" else ""
            print(f"  {name}{super_str}")
        return

    if args.dump_class:
        uc = pkg.decompile_class_uc(args.dump_class, include_bytecode=False)
        print(uc)
        return

    if args.dump_bytecode:
        target = args.dump_bytecode
        found = False
        # Search for class first
        if target in pkg.classes:
            cls = pkg.classes[target]
            print(f"Bytecode for class {target}:")
            for fn in cls["functions"]:
                print(f"\nFunction {fn['name']} ({fn['bytecode_size']} bytes):")
                for line in pkg.disassemble_bytecode(fn["bytecode"]):
                    print(line)
            found = True
        else:
            # Search for individual function across classes
            for cls_name, cls in pkg.classes.items():
                for fn in cls["functions"]:
                    if fn["name"].lower() == target.lower():
                        print(f"Function {cls_name}.{fn['name']} ({fn['bytecode_size']} bytes):")
                        for line in pkg.disassemble_bytecode(fn["bytecode"]):
                            print(line)
                        found = True
        if not found:
            print(f"Function or class '{target}' not found.", file=sys.stderr)
        return

    if args.dump_all is not None:
        out_root = args.dump_all
        out_dir = os.path.join(out_root, pkg.package_name)
        os.makedirs(out_dir, exist_ok=True)
        print(f"Decompiling {len(pkg.classes)} classes to {out_dir}...")
        for name in pkg.classes:
            content = pkg.decompile_class_uc(name, include_bytecode=True)
            fpath = os.path.join(out_dir, f"{name}.uc")
            with open(fpath, "w", encoding="utf-8") as f:
                f.write(content)
        print(f"Decompiled {len(pkg.classes)} classes successfully!")
        return

    if args.json:
        out_data = {
            "package": pkg.package_name,
            "classes": pkg.classes
        }
        print(json.dumps(out_data, indent=2, default=str))
        return

    # Default output if no flags specified
    print(f"Package {pkg.package_name}.u: {len(pkg.classes)} classes, {len(pkg.exports)} exports.")
    print("Use --help to see available commands.")


if __name__ == "__main__":
    main()
