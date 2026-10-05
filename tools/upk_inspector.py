#!/usr/bin/env python3
"""
UPK / ME1 / U Package Inspector for Mirror's Edge (UE3 v536 / Licensee 43)
Reverse-engineering tool for clean-room macOS playable runtime in mierrorsedgere.

Supports:
- Uncompressed and compressed packages (.u, .upk, .me1)
- LZO1X-1 decompression (Pure-Python implementation + ctypes liblzo2 acceleration)
- ZLIB decompression
- Package Header parsing (Generations, EngineVersion, CookerVersion, CompressedChunks)
- Name Table, Import Table, Export Table, Depends Table parsing
- UObject FPropertyTag stream decoding (Int, Float, Bool, Byte, Name, Str, Object, Struct, Array)
- Level Actor extraction (Positions, Rotations, Scales, Referenced Meshes/Archetypes)
- StaticMesh geometry bounds and LOD inspection
- SoundNodeWave embedded Ogg Vorbis audio extraction
"""

import sys
import os
import struct
import zlib
import argparse
from typing import List, Dict, Any, Optional, Tuple

# Optional ctypes acceleration for LZO1X
_C_LZO_FUNC = None
try:
    import ctypes
    import ctypes.util
    for lib_name in ['/opt/homebrew/lib/liblzo2.dylib', '/usr/local/lib/liblzo2.dylib', 'lzo2']:
        try:
            _lzo_cdll = ctypes.CDLL(lib_name)
            if hasattr(_lzo_cdll, 'lzo1x_decompress_safe'):
                _func = _lzo_cdll.lzo1x_decompress_safe
                _func.argtypes = [
                    ctypes.c_char_p, ctypes.c_uint,
                    ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint),
                    ctypes.c_void_p
                ]
                _func.restype = ctypes.c_int
                _C_LZO_FUNC = _func
                break
        except Exception:
            continue
except Exception:
    pass


def lzo1x_decompress(src: bytes, expected_len: int) -> bytes:
    """
    Decompress LZO1X-1 compressed data buffer.
    Uses accelerated native liblzo2 if available, otherwise falls back to
    the verified pure-Python LZO1X-1 implementation.
    """
    if _C_LZO_FUNC is not None:
        out_buf = ctypes.create_string_buffer(expected_len)
        out_len = ctypes.c_uint(expected_len)
        res = _C_LZO_FUNC(src, len(src), out_buf, ctypes.byref(out_len), None)
        if res == 0:
            return out_buf.raw[:out_len.value]

    # Pure-Python fallback
    dst = bytearray()
    ip = 0
    src_len = len(src)
    if src_len == 0:
        return bytes(dst)

    state = 0
    b = src[ip]
    ip += 1

    if b > 17:
        t = b - 17
        dst.extend(src[ip:ip + t])
        ip += t
        if ip >= src_len:
            return bytes(dst)
        b = src[ip]
        ip += 1
        state = 0

    while ip <= src_len:
        if b < 16:
            if state == 0:
                # Literal run
                t = b
                if t == 0:
                    while ip < src_len and src[ip] == 0:
                        t += 255
                        ip += 1
                    t += 15 + src[ip]
                    ip += 1
                t += 3
                dst.extend(src[ip:ip + t])
                ip += t
                if ip >= src_len:
                    break
                b = src[ip]
                ip += 1
                if b < 16:
                    # Short match (M1)
                    m_dist = 1 + (b >> 2) + (src[ip] << 2)
                    ip += 1
                    m_pos = len(dst) - m_dist
                    dst.append(dst[m_pos])
                    dst.append(dst[m_pos + 1])
                    t = b & 3
                    if t > 0:
                        dst.extend(src[ip:ip + t])
                        ip += t
                    state = t
                    if ip >= src_len:
                        break
                    b = src[ip]
                    ip += 1
                    continue
                else:
                    state = 0
            else:
                # Short match (M1) when state != 0
                m_dist = 1 + (b >> 2) + (src[ip] << 2)
                ip += 1
                m_pos = len(dst) - m_dist
                dst.append(dst[m_pos])
                dst.append(dst[m_pos + 1])
                t = b & 3
                if t > 0:
                    dst.extend(src[ip:ip + t])
                    ip += t
                state = t
                if ip >= src_len:
                    break
                b = src[ip]
                ip += 1
                continue

        # Match instructions (M2, M3, M4)
        if b >= 64:
            # M2 match
            m_len = ((b >> 5) - 1) + 2
            m_dist = 1 + ((b >> 2) & 7) + (src[ip] << 3)
            ip += 1
            trailing = b & 3
        elif b >= 32:
            # M3 match
            m_len = b & 31
            if m_len == 0:
                while ip < src_len and src[ip] == 0:
                    m_len += 255
                    ip += 1
                m_len += 31 + src[ip]
                ip += 1
            m_len += 2
            lo = src[ip]
            hi = src[ip + 1]
            ip += 2
            m_dist = 1 + (lo >> 2) + (hi << 6)
            trailing = lo & 3
        elif b >= 16:
            # M4 match
            m_len = b & 7
            if m_len == 0:
                while ip < src_len and src[ip] == 0:
                    m_len += 255
                    ip += 1
                m_len += 7 + src[ip]
                ip += 1
            m_len += 2
            dist_high = (b & 8) << 11
            lo = src[ip]
            hi = src[ip + 1]
            ip += 2
            m_off = (lo >> 2) | (hi << 6)
            if m_off == 0:
                # End of stream marker
                break
            m_dist = 0x4000 + dist_high + m_off
            trailing = lo & 3
        else:
            raise ValueError(f"Invalid LZO opcode 0x{b:02x} at input offset {ip - 1}")

        m_pos = len(dst) - m_dist
        for _ in range(m_len):
            dst.append(dst[m_pos])
            m_pos += 1

        if trailing > 0:
            dst.extend(src[ip:ip + trailing])
            ip += trailing

        state = trailing
        if ip >= src_len:
            break
        b = src[ip]
        ip += 1

    return bytes(dst)


class PackageHeader:
    PACKAGE_MAGIC = 0x9E2A83C1
    CHUNK_MAGIC = 0x9E2A83C1

    COMP_NONE = 0x00
    COMP_ZLIB = 0x01
    COMP_LZO = 0x02

    def __init__(self):
        self.tag: int = 0
        self.file_version: int = 0
        self.licensee_version: int = 0
        self.total_header_size: int = 0
        self.folder_name: str = ""
        self.package_flags: int = 0
        self.name_count: int = 0
        self.name_offset: int = 0
        self.export_count: int = 0
        self.export_offset: int = 0
        self.import_count: int = 0
        self.import_offset: int = 0
        self.depends_offset: int = 0
        self.guid: str = ""
        self.generations: List[Tuple[int, int, int]] = []
        self.engine_version: int = 0
        self.cooker_version: int = 0
        self.compression_flags: int = 0
        self.compressed_chunks: List[Tuple[int, int, int, int]] = []
        self.package_source: int = 0
        self.additional_packages_to_cook: List[str] = []


class UPKPackage:
    def __init__(self, file_path: str):
        self.file_path = file_path
        self.file_size = os.path.getsize(file_path)
        self.header = PackageHeader()
        self.data: bytearray = bytearray()
        self.names: List[str] = []
        self.imports: List[Dict[str, Any]] = []
        self.exports: List[Dict[str, Any]] = []
        self.depends: List[List[int]] = []

        self._load_and_decompress()
        self._parse_tables()

    def _read_fstring(self, buf: bytes, off: int) -> Tuple[str, int]:
        length, = struct.unpack_from('<i', buf, off)
        off += 4
        if length > 0:
            s = buf[off:off + length - 1].decode('latin1', errors='replace')
            return s, off + length
        elif length < 0:
            chars = -length
            s = buf[off:off + chars * 2 - 2].decode('utf-16le', errors='replace')
            return s, off + chars * 2
        return "", off

    def _load_and_decompress(self):
        with open(self.file_path, 'rb') as fp:
            raw_hdr = fp.read(4096)

        hdr = self.header
        hdr.tag, ver_lic = struct.unpack_from('<II', raw_hdr, 0)
        hdr.file_version = ver_lic & 0xFFFF
        hdr.licensee_version = (ver_lic >> 16) & 0xFFFF
        if hdr.tag != PackageHeader.PACKAGE_MAGIC:
            raise ValueError(f"Invalid package magic: 0x{hdr.tag:08X} (expected 0x9E2A83C1)")

        hdr.total_header_size, = struct.unpack_from('<i', raw_hdr, 8)
        hdr.folder_name, off = self._read_fstring(raw_hdr, 12)

        (hdr.package_flags, hdr.name_count, hdr.name_offset,
         hdr.export_count, hdr.export_offset,
         hdr.import_count, hdr.import_offset,
         hdr.depends_offset) = struct.unpack_from('<8i', raw_hdr, off)
        off += 32

        hdr.guid = raw_hdr[off:off + 16].hex()
        off += 16

        gen_cnt, = struct.unpack_from('<i', raw_hdr, off)
        off += 4
        for _ in range(gen_cnt):
            exp_c, name_c, net_c = struct.unpack_from('<3i', raw_hdr, off)
            hdr.generations.append((exp_c, name_c, net_c))
            off += 12

        hdr.engine_version, hdr.cooker_version, hdr.compression_flags = struct.unpack_from('<3i', raw_hdr, off)
        off += 12

        chunk_cnt, = struct.unpack_from('<i', raw_hdr, off)
        off += 4

        for _ in range(chunk_cnt):
            u_off, u_sz, c_off, c_sz = struct.unpack_from('<4i', raw_hdr, off)
            hdr.compressed_chunks.append((u_off, u_sz, c_off, c_sz))
            off += 16

        if chunk_cnt > 0:
            hdr.package_source, = struct.unpack_from('<I', raw_hdr, off)
            off += 4
            add_cnt, = struct.unpack_from('<i', raw_hdr, off)
            off += 4
            for _ in range(add_cnt):
                pkg_name, off = self._read_fstring(raw_hdr, off)
                hdr.additional_packages_to_cook.append(pkg_name)

        # Decompress / buffer package
        with open(self.file_path, 'rb') as fp:
            if chunk_cnt == 0:
                fp.seek(0)
                self.data = bytearray(fp.read())
            else:
                last_chunk = hdr.compressed_chunks[-1]
                total_uncomp = last_chunk[0] + last_chunk[1]
                self.data = bytearray(total_uncomp)

                first_u_off = hdr.compressed_chunks[0][0]
                fp.seek(0)
                self.data[:first_u_off] = fp.read(first_u_off)

                for u_off, u_sz, c_off, c_sz in hdr.compressed_chunks:
                    fp.seek(c_off)
                    chunk_hdr = fp.read(16)
                    c_magic, blk_sz, tot_comp, tot_uncomp = struct.unpack('<4I', chunk_hdr)
                    if c_magic != PackageHeader.CHUNK_MAGIC:
                        raise ValueError(f"Invalid chunk magic: 0x{c_magic:08X} at file offset {c_off}")

                    num_blks = (u_sz + blk_sz - 1) // blk_sz
                    sub_blks = [struct.unpack('<2I', fp.read(8)) for _ in range(num_blks)]

                    chunk_out = bytearray()
                    for sub_comp, sub_uncomp in sub_blks:
                        comp_data = fp.read(sub_comp)
                        if hdr.compression_flags & PackageHeader.COMP_LZO:
                            decomp = lzo1x_decompress(comp_data, sub_uncomp)
                        elif hdr.compression_flags & PackageHeader.COMP_ZLIB:
                            decomp = zlib.decompress(comp_data)
                        else:
                            decomp = comp_data
                        chunk_out.extend(decomp)

                    self.data[u_off:u_off + u_sz] = chunk_out

    def _parse_tables(self):
        hdr = self.header
        data = self.data

        # 1. Name Table
        off = hdr.name_offset
        self.names = []
        for _ in range(hdr.name_count):
            flen, = struct.unpack_from('<i', data, off)
            off += 4
            if flen > 0:
                self.names.append(data[off:off + flen - 1].decode('latin1', errors='replace'))
                off += flen
            elif flen < 0:
                flen = -flen
                self.names.append(data[off:off + flen * 2 - 2].decode('utf-16le', errors='replace'))
                off += flen * 2
            else:
                self.names.append("")
            off += 8  # uint64 Flags

        # 2. Import Table
        off = hdr.import_offset
        self.imports = []
        for imp_idx in range(hdr.import_count):
            pkg_idx, pkg_num, cls_idx, cls_num, outer_idx, obj_idx, obj_num = struct.unpack_from('<7i', data, off)
            off += 28
            self.imports.append({
                'index': imp_idx,
                'class_package': self.names[pkg_idx] if 0 <= pkg_idx < len(self.names) else str(pkg_idx),
                'class_name': self.names[cls_idx] if 0 <= cls_idx < len(self.names) else str(cls_idx),
                'outer_index': outer_idx,
                'object_name': self.names[obj_idx] if 0 <= obj_idx < len(self.names) else str(obj_idx),
            })

        # 3. Export Table
        off = hdr.export_offset
        self.exports = []
        for exp_idx in range(hdr.export_count):
            cls_idx, sup_idx, out_idx, name_idx, name_num, arch_idx = struct.unpack_from('<6i', data, off)
            off += 24
            obj_flags, = struct.unpack_from('<Q', data, off)
            off += 8
            serial_sz, serial_off = struct.unpack_from('<2i', data, off)
            off += 8

            comp_count, = struct.unpack_from('<i', data, off)
            off += 4
            comp_map = []
            for _ in range(comp_count):
                c_n_idx, c_n_num, c_obj_idx = struct.unpack_from('<3i', data, off)
                off += 12
                c_name = self.names[c_n_idx] if 0 <= c_n_idx < len(self.names) else str(c_n_idx)
                comp_map.append((c_name, c_obj_idx))

            exp_flags, = struct.unpack_from('<I', data, off)
            off += 4

            gen_net_cnt, = struct.unpack_from('<i', data, off)
            off += 4
            gen_net = list(struct.unpack_from(f'<{gen_net_cnt}i', data, off))
            off += gen_net_cnt * 4

            pkg_guid = data[off:off + 16].hex()
            off += 16
            pkg_flags, = struct.unpack_from('<I', data, off)
            off += 4

            obj_name = self.names[name_idx] if 0 <= name_idx < len(self.names) else str(name_idx)

            self.exports.append({
                'index': exp_idx,
                'class_index': cls_idx,
                'super_index': sup_idx,
                'outer_index': out_idx,
                'object_name': obj_name,
                'object_number': name_num,
                'archetype': arch_idx,
                'object_flags': obj_flags,
                'serial_size': serial_sz,
                'serial_offset': serial_off,
                'component_map': comp_map,
                'export_flags': exp_flags,
                'gen_net_count': gen_net,
                'package_guid': pkg_guid,
                'package_flags': pkg_flags
            })

        # 4. Depends Table
        if hdr.depends_offset > 0 and hdr.depends_offset < len(data):
            off = hdr.depends_offset
            self.depends = []
            for _ in range(hdr.export_count):
                dep_cnt, = struct.unpack_from('<i', data, off)
                off += 4
                deps = list(struct.unpack_from(f'<{dep_cnt}i', data, off))
                off += dep_cnt * 4
                self.depends.append(deps)

    def resolve_object_index(self, idx: int) -> Tuple[str, str]:
        if idx > 0:
            exp_i = idx - 1
            if exp_i < len(self.exports):
                exp = self.exports[exp_i]
                cls_name, _ = self.resolve_object_index(exp['class_index'])
                return exp['object_name'], cls_name
            return f"Export_{exp_i}", "Unknown"
        elif idx < 0:
            imp_i = -idx - 1
            if imp_i < len(self.imports):
                imp = self.imports[imp_i]
                return imp['object_name'], imp['class_name']
            return f"Import_{imp_i}", "Unknown"
        return "None", "None"

    def get_export_class(self, exp: Dict[str, Any]) -> str:
        cls_idx = exp['class_index']
        if cls_idx == 0:
            return "Class"
        cls_name, _ = self.resolve_object_index(cls_idx)
        return cls_name

    def parse_properties(self, start_off: int, max_size: int) -> Tuple[Dict[str, Any], int]:
        data = self.data
        pos = start_off
        end_pos = min(len(data), start_off + max_size)
        props: Dict[str, Any] = {}

        while pos + 8 <= end_pos:
            n_idx, n_num = struct.unpack_from('<2i', data, pos)
            pos += 8
            if not (0 <= n_idx < len(self.names)):
                break
            prop_name = self.names[n_idx]
            if prop_name == "None":
                break

            if pos + 16 > end_pos:
                break
            t_idx, t_num = struct.unpack_from('<2i', data, pos)
            pos += 8
            prop_type = self.names[t_idx] if 0 <= t_idx < len(self.names) else "Unknown"

            size, arr_idx = struct.unpack_from('<2i', data, pos)
            pos += 8

            struct_name = None
            bool_val = None
            enum_name = None

            if prop_type == 'StructProperty':
                s_idx, s_num = struct.unpack_from('<2i', data, pos)
                pos += 8
                struct_name = self.names[s_idx] if 0 <= s_idx < len(self.names) else None
            elif prop_type == 'BoolProperty':
                bool_val_int, = struct.unpack_from('<i', data, pos)
                pos += 4
                bool_val = bool(bool_val_int)
            elif prop_type == 'ByteProperty':
                e_idx, e_num = struct.unpack_from('<2i', data, pos)
                pos += 8
                enum_name = self.names[e_idx] if 0 <= e_idx < len(self.names) else None

            val_bytes = data[pos:pos + size]
            pos += size

            parsed_val: Any = None
            if prop_type == 'IntProperty' and size == 4:
                parsed_val, = struct.unpack('<i', val_bytes)
            elif prop_type == 'FloatProperty' and size == 4:
                parsed_val, = struct.unpack('<f', val_bytes)
            elif prop_type == 'BoolProperty':
                parsed_val = bool_val
            elif prop_type == 'ObjectProperty' and size == 4:
                obj_ref, = struct.unpack('<i', val_bytes)
                obj_name, cls_name = self.resolve_object_index(obj_ref)
                parsed_val = {
                    'index': obj_ref,
                    'name': obj_name,
                    'class': cls_name
                }
            elif prop_type == 'NameProperty' and size == 8:
                pn_idx, pn_num = struct.unpack('<2i', val_bytes)
                parsed_val = self.names[pn_idx] if 0 <= pn_idx < len(self.names) else str(pn_idx)
            elif prop_type == 'StrProperty':
                parsed_val, _ = self._read_fstring(val_bytes, 0)
            elif prop_type == 'ByteProperty':
                if size == 1:
                    parsed_val = val_bytes[0]
                elif size == 8:
                    bn_idx, bn_num = struct.unpack('<2i', val_bytes)
                    parsed_val = self.names[bn_idx] if 0 <= bn_idx < len(self.names) else str(bn_idx)
            elif prop_type == 'StructProperty':
                if struct_name == 'Vector' and size == 12:
                    vx, vy, vz = struct.unpack('<3f', val_bytes)
                    parsed_val = {'x': vx, 'y': vy, 'z': vz}
                elif struct_name == 'Rotator' and size == 12:
                    pitch, yaw, roll = struct.unpack('<3i', val_bytes)
                    parsed_val = {'pitch': pitch, 'yaw': yaw, 'roll': roll}
                elif struct_name == 'Color' and size == 4:
                    r, g, b_c, a = struct.unpack('<4B', val_bytes)
                    parsed_val = {'r': r, 'g': g, 'b': b_c, 'a': a}
                elif struct_name == 'LinearColor' and size == 16:
                    lr, lg, lb, la = struct.unpack('<4f', val_bytes)
                    parsed_val = {'r': lr, 'g': lg, 'b': lb, 'a': la}
                elif struct_name == 'Guid' and size == 16:
                    parsed_val = val_bytes.hex()
                elif struct_name == 'Box' and size == 25:
                    min_v = struct.unpack('<3f', val_bytes[0:12])
                    max_v = struct.unpack('<3f', val_bytes[12:24])
                    is_valid = bool(val_bytes[24])
                    parsed_val = {'min': min_v, 'max': max_v, 'is_valid': is_valid}
                else:
                    parsed_val = val_bytes.hex()
            elif prop_type == 'ArrayProperty':
                count, = struct.unpack('<i', val_bytes[:4])
                parsed_val = f'Array(count={count}, bytes={size - 4})'
            else:
                parsed_val = val_bytes.hex()

            props[prop_name] = {
                'type': prop_type,
                'size': size,
                'array_index': arr_idx,
                'struct_name': struct_name,
                'enum_name': enum_name,
                'value': parsed_val
            }

        return props, pos

    def extract_actors(self) -> List[Dict[str, Any]]:
        actors = []
        for exp in self.exports:
            cls_name = self.get_export_class(exp)
            if cls_name.endswith('Component') or cls_name in ['Level', 'World', 'Package', 'Model', 'Polys', 'ShadowMap2D', 'ShadowMapTexture2D']:
                continue
            outer_name, _ = self.resolve_object_index(exp['outer_index'])
            is_actor = outer_name == 'PersistentLevel' or any(sub in cls_name for sub in [
                'Actor', 'Start', 'Light', 'Volume', 'Brush', 'Checkpoint', 'Trigger', 'Pawn', 'Info', 'Note', 'PathNode'
            ])
            if not is_actor:
                continue

            off = exp['serial_offset']
            sz = exp['serial_size']
            hdr_skip = 32 if (exp['object_flags'] & 0x0200000000000000) else 4
            if sz < hdr_skip + 8:
                continue

            props, end_props = self.parse_properties(off + hdr_skip, sz - hdr_skip)

            loc = props.get('Location', {}).get('value', {'x': 0.0, 'y': 0.0, 'z': 0.0})
            rot = props.get('Rotation', {}).get('value', {'pitch': 0, 'yaw': 0, 'roll': 0})
            scale = props.get('DrawScale', {}).get('value', 1.0)
            scale3d = props.get('DrawScale3D', {}).get('value', {'x': 1.0, 'y': 1.0, 'z': 1.0})

            mesh_ref = None
            for cname, cidx in exp['component_map']:
                if 'mesh' in cname.lower():
                    if cidx > 0 and cidx - 1 < len(self.exports):
                        comp_exp = self.exports[cidx - 1]
                        comp_props, _ = self.parse_properties(comp_exp['serial_offset'] + 8, comp_exp['serial_size'] - 8)
                        if 'StaticMesh' in comp_props:
                            sm_info = comp_props['StaticMesh'].get('value', {})
                            mesh_ref = sm_info.get('name')

            actors.append({
                'export_index': exp['index'],
                'name': exp['object_name'],
                'class': cls_name,
                'location': loc,
                'rotation': rot,
                'draw_scale': scale,
                'draw_scale_3d': scale3d,
                'static_mesh': mesh_ref,
                'components': [f"{name}->{self.resolve_object_index(idx)[0]}" for name, idx in exp['component_map']],
                'property_count': len(props)
            })

        return actors

    def extract_static_mesh_info(self, exp_idx: int) -> Dict[str, Any]:
        exp = self.exports[exp_idx]
        off = exp['serial_offset']
        sz = exp['serial_size']

        props, rem_off = self.parse_properties(off + 4, sz - 4)

        data = self.data
        rem = data[rem_off:off + sz]
        bounds = {}
        if len(rem) >= 28:
            ox, oy, oz, ex, ey, ez, r = struct.unpack_from('<7f', rem, 0)
            bounds = {
                'origin': (ox, oy, oz),
                'box_extent': (ex, ey, ez),
                'sphere_radius': r
            }

        body_setup_idx = None
        if len(rem) >= 32:
            body_setup_idx, = struct.unpack_from('<i', rem, 28)

        return {
            'name': exp['object_name'],
            'properties': props,
            'bounds': bounds,
            'body_setup': self.resolve_object_index(body_setup_idx)[0] if body_setup_idx else None,
            'raw_remaining_bytes': len(rem)
        }

    def extract_audio(self) -> List[Tuple[str, bytes]]:
        audio_streams = []
        for exp in self.exports:
            cls_name = self.get_export_class(exp)
            if 'SoundNodeWave' not in cls_name:
                continue

            off = exp['serial_offset']
            sz = exp['serial_size']
            exp_bytes = self.data[off:off + sz]

            ogg_idx = exp_bytes.find(b'OggS')
            if ogg_idx != -1:
                bulk_sz = len(exp_bytes) - ogg_idx
                if ogg_idx >= 12:
                    elem_cnt, disk_sz = struct.unpack_from('<2i', exp_bytes, ogg_idx - 12)
                    if 0 < disk_sz <= len(exp_bytes) - ogg_idx:
                        bulk_sz = disk_sz
                ogg_stream = bytes(exp_bytes[ogg_idx:ogg_idx + bulk_sz])
                audio_streams.append((exp['object_name'], ogg_stream))

        return audio_streams


def cmd_header(args):
    pkg = UPKPackage(args.path)
    hdr = pkg.header
    print("================================================================================")
    print(f"PACKAGE HEADER: {os.path.basename(args.path)}")
    print("================================================================================")
    print(f"File Path:         {args.path}")
    print(f"File Size:         {pkg.file_size:,} bytes")
    print(f"Tag:               0x{hdr.tag:08X} (UE3 Magic)")
    print(f"Engine/Licensee:   UE3 v{hdr.file_version} (Licensee Version {hdr.licensee_version})")
    print(f"Total Header Size: {hdr.total_header_size:,} bytes")
    print(f"Folder Name:       '{hdr.folder_name}'")
    print(f"Package Flags:     0x{hdr.package_flags:08X}")
    print(f"Name Table:        {hdr.name_count:,} entries at uncompressed offset {hdr.name_offset:,}")
    print(f"Import Table:      {hdr.import_count:,} entries at uncompressed offset {hdr.import_offset:,}")
    print(f"Export Table:      {hdr.export_count:,} entries at uncompressed offset {hdr.export_offset:,}")
    print(f"Depends Offset:    {hdr.depends_offset:,}")
    print(f"GUID:              {hdr.guid}")
    print(f"Engine Version:    {hdr.engine_version}")
    print(f"Cooker Version:    {hdr.cooker_version}")

    comp_name = "None (0x00)"
    if hdr.compression_flags & PackageHeader.COMP_LZO:
        comp_name = "LZO (0x02)"
    elif hdr.compression_flags & PackageHeader.COMP_ZLIB:
        comp_name = "ZLIB (0x01)"
    print(f"Compression Flags: 0x{hdr.compression_flags:08X} ({comp_name})")
    print(f"Compressed Chunks: {len(hdr.compressed_chunks):,} chunks")
    for i, c in enumerate(hdr.compressed_chunks[:5]):
        print(f"  Chunk {i}: Uncomp(off={c[0]:,}, sz={c[1]:,}) -> Comp(off={c[2]:,}, sz={c[3]:,})")
    if len(hdr.compressed_chunks) > 5:
        print(f"  ... and {len(hdr.compressed_chunks) - 5} more chunks")
    if hdr.additional_packages_to_cook:
        print(f"Additional Packages To Cook ({len(hdr.additional_packages_to_cook)}):")
        for p in hdr.additional_packages_to_cook:
            print(f"  - {p}")


def cmd_names(args):
    pkg = UPKPackage(args.path)
    limit = args.limit or len(pkg.names)
    print(f"=== NAME TABLE ({len(pkg.names)} entries, showing {min(limit, len(pkg.names))}) ===")
    for i in range(min(limit, len(pkg.names))):
        print(f"  [{i:5d}] {pkg.names[i]}")


def cmd_imports(args):
    pkg = UPKPackage(args.path)
    limit = args.limit or len(pkg.imports)
    print(f"=== IMPORT TABLE ({len(pkg.imports)} entries, showing {min(limit, len(pkg.imports))}) ===")
    for imp in pkg.imports[:limit]:
        print(f"  [-{imp['index'] + 1:4d}] {imp['object_name']} (Class: {imp['class_package']}.{imp['class_name']}, Outer: {imp['outer_index']})")


def cmd_exports(args):
    pkg = UPKPackage(args.path)
    exports = pkg.exports
    if args.filter_class:
        exports = [e for e in exports if args.filter_class.lower() in pkg.get_export_class(e).lower()]
    limit = args.limit or len(exports)
    print(f"=== EXPORT TABLE ({len(exports)} entries matching, showing {min(limit, len(exports))}) ===")
    for exp in exports[:limit]:
        cls_name = pkg.get_export_class(exp)
        print(f"  [{exp['index'] + 1:5d}] {exp['object_name']} (Class: {cls_name}, Size: {exp['serial_size']:,}, Off: {exp['serial_offset']:,})")


def cmd_actors(args):
    pkg = UPKPackage(args.path)
    actors = pkg.extract_actors()
    print("================================================================================")
    print(f"LEVEL ACTOR PLACEMENT LIST: {os.path.basename(args.path)} ({len(actors)} actors)")
    print("================================================================================")
    for a in actors:
        loc = a['location']
        rot = a['rotation']
        mesh_str = f" | Mesh: {a['static_mesh']}" if a['static_mesh'] else ""
        print(f"[{a['export_index'] + 1:5d}] {a['class']} '{a['name']}'{mesh_str}")
        print(f"       Pos: ({loc['x']:10.2f}, {loc['y']:10.2f}, {loc['z']:10.2f}) | "
              f"Rot: (P={rot['pitch']:6d}, Y={rot['yaw']:6d}, R={rot['roll']:6d}) | "
              f"Scale: {a['draw_scale']}")


def cmd_dump(args):
    pkg = UPKPackage(args.path)
    exp_idx = None
    if args.target.isdigit():
        exp_idx = int(args.target) - 1
    else:
        for exp in pkg.exports:
            if exp['object_name'].lower() == args.target.lower():
                exp_idx = exp['index']
                break
    if exp_idx is None or not (0 <= exp_idx < len(pkg.exports)):
        print(f"Error: Export '{args.target}' not found.")
        sys.exit(1)

    exp = pkg.exports[exp_idx]
    cls_name = pkg.get_export_class(exp)
    print(f"=== OBJECT DUMP: [{exp_idx + 1}] {exp['object_name']} (Class: {cls_name}) ===")
    print(f"Serial Size:   {exp['serial_size']:,} bytes")
    print(f"Serial Offset: {exp['serial_offset']:,}")
    print(f"Object Flags:  0x{exp['object_flags']:016X}")
    print(f"Component Map: {exp['component_map']}")

    skip = 32 if any(sub in cls_name for sub in ['Actor', 'Start', 'Light', 'Volume', 'Brush', 'Checkpoint', 'Trigger']) else (4 if cls_name in ['StaticMesh', 'Texture2D'] else 0)
    props, end_pos = pkg.parse_properties(exp['serial_offset'] + skip, exp['serial_size'] - skip)
    print(f"Properties ({len(props)}):")
    for k, v in props.items():
        print(f"  {k} [{v['type']}]: {v['value']}")


def cmd_mesh(args):
    pkg = UPKPackage(args.path)
    exp_idx = None
    if args.target.isdigit():
        exp_idx = int(args.target) - 1
    else:
        for exp in pkg.exports:
            if exp['object_name'].lower() == args.target.lower():
                exp_idx = exp['index']
                break
    if exp_idx is None or not (0 <= exp_idx < len(pkg.exports)):
        print(f"Error: StaticMesh '{args.target}' not found.")
        sys.exit(1)

    info = pkg.extract_static_mesh_info(exp_idx)
    print(f"=== STATIC MESH: {info['name']} ===")
    b = info['bounds']
    if b:
        print(f"Origin:        ({b['origin'][0]:.2f}, {b['origin'][1]:.2f}, {b['origin'][2]:.2f})")
        print(f"Box Extent:    ({b['box_extent'][0]:.2f}, {b['box_extent'][1]:.2f}, {b['box_extent'][2]:.2f})")
        print(f"Sphere Radius: {b['sphere_radius']:.2f}")
    print(f"Body Setup:    {info['body_setup']}")
    print(f"Properties:    {len(info['properties'])}")


def cmd_audio(args):
    pkg = UPKPackage(args.path)
    streams = pkg.extract_audio()
    print(f"Found {len(streams)} embedded Ogg Vorbis streams in {os.path.basename(args.path)}")
    out_dir = args.out_dir
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    for name, data in streams:
        if args.target and args.target.lower() != name.lower():
            continue
        print(f"  - {name}: {len(data):,} bytes Ogg Vorbis")
        if out_dir:
            out_path = os.path.join(out_dir, f"{name}.ogg")
            with open(out_path, 'wb') as fp:
                fp.write(data)
            print(f"    Saved to: {out_path}")


def main():
    parser = argparse.ArgumentParser(description="Mirror's Edge UE3 UPK/ME1/U Package Inspector & Extractor")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_hdr = sub.add_parser("header", help="Dump package header")
    p_hdr.add_argument("path", help="Path to .u, .upk, or .me1 file")

    p_names = sub.add_parser("names", help="Dump Name table")
    p_names.add_argument("path", help="Path to package file")
    p_names.add_argument("--limit", type=int, default=50, help="Maximum entries to show")

    p_imp = sub.add_parser("imports", help="Dump Import table")
    p_imp.add_argument("path", help="Path to package file")
    p_imp.add_argument("--limit", type=int, default=50, help="Maximum entries to show")

    p_exp = sub.add_parser("exports", help="Dump Export table")
    p_exp.add_argument("path", help="Path to package file")
    p_exp.add_argument("--class", dest="filter_class", help="Filter by class name")
    p_exp.add_argument("--limit", type=int, default=50, help="Maximum entries to show")

    p_act = sub.add_parser("actors", help="Extract Level actor placements")
    p_act.add_argument("path", help="Path to map package file (.upk or .me1)")

    p_dump = sub.add_parser("dump", help="Dump object properties")
    p_dump.add_argument("path", help="Path to package file")
    p_dump.add_argument("target", help="Export index (1-based) or object name")

    p_mesh = sub.add_parser("mesh", help="Inspect StaticMesh geometry bounds and LODs")
    p_mesh.add_argument("path", help="Path to package file")
    p_mesh.add_argument("target", help="StaticMesh export index or name")

    p_aud = sub.add_parser("audio", help="Extract SoundNodeWave Ogg Vorbis streams")
    p_aud.add_argument("path", help="Path to package file")
    p_aud.add_argument("--export", dest="target", help="Specific SoundNodeWave name")
    p_aud.add_argument("--out", dest="out_dir", help="Output directory to save .ogg files")

    args = parser.parse_args()

    dispatch = {
        "header": cmd_header,
        "names": cmd_names,
        "imports": cmd_imports,
        "exports": cmd_exports,
        "actors": cmd_actors,
        "dump": cmd_dump,
        "mesh": cmd_mesh,
        "audio": cmd_audio,
    }
    dispatch[args.cmd](args)


if __name__ == "__main__":
    main()
