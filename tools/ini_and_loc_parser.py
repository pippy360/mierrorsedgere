#!/usr/bin/env python3
"""
Mirror's Edge UE3 INI and Localization (.int) Parser & Oracle Generator.

Parses Unreal Engine 3 configuration files (*.ini) and localization files (*.int, *.pol, etc.)
supporting:
- UE3 array directives: '+Key=Val' (append), '-Key=Val' (remove), '.Key=Val' (append)
- Struct literals: '(Field1=Val1, Field2=Val2, ...)' recursively parsed to dicts
- Scalar values: bool, int, float (including trailing 'f'/'F'), quoted strings
- Transparent encoding detection: UTF-16LE/BE with BOM, UTF-8 BOM, ANSI/latin-1
- Exporting structured JSON or Python dict oracle for engine reimplementation
"""

import os
import sys
import glob
import json
import argparse
from typing import Any, Dict, List, Tuple, Union, Optional


def detect_and_decode(file_path: str) -> str:
    """Read file and decode using appropriate encoding (UTF-16LE BOM, UTF-8, latin1)."""
    with open(file_path, 'rb') as f:
        raw = f.read()

    if raw.startswith(b'\xff\xfe'):
        return raw[2:].decode('utf-16le', errors='replace')
    elif raw.startswith(b'\xfe\xff'):
        return raw[2:].decode('utf-16be', errors='replace')
    elif raw.startswith(b'\xef\xbb\xbf'):
        return raw[3:].decode('utf-8', errors='replace')

    # Try utf-8
    try:
        return raw.decode('utf-8')
    except UnicodeDecodeError:
        return raw.decode('latin1', errors='replace')


def split_top_level_comma(s: str) -> List[str]:
    """Split comma-separated elements at depth 0, respecting quotes and parentheses."""
    tokens = []
    depth = 0
    in_quote = False
    quote_char = ''
    current = []

    for char in s:
        if in_quote:
            current.append(char)
            if char == quote_char:
                in_quote = False
        else:
            if char in ('"', "'"):
                in_quote = True
                quote_char = char
                current.append(char)
            elif char in ('(', '[', '{', '<'):
                depth += 1
                current.append(char)
            elif char in (')', ']', '}', '>'):
                depth = max(0, depth - 1)
                current.append(char)
            elif depth == 0 and char == ',':
                tokens.append(''.join(current).strip())
                current = []
            else:
                current.append(char)

    if current:
        tokens.append(''.join(current).strip())

    return tokens


def parse_ue3_struct(s: str) -> Union[Dict[str, Any], List[Any]]:
    """Recursively parse a UE3 struct literal like (A=1, B=2) or list (1, 2, 3)."""
    s = s.strip()
    if s.startswith('(') and s.endswith(')'):
        s = s[1:-1].strip()

    if not s:
        return {}

    tokens = split_top_level_comma(s)
    has_keys = any('=' in t for t in tokens)

    if has_keys:
        result: Dict[str, Any] = {}
        for tok in tokens:
            if not tok:
                continue
            if '=' in tok:
                k, v = tok.split('=', 1)
                k = k.strip()
                v = v.strip()
                result[k] = parse_ue3_value(v)
            else:
                result[tok] = True
        return result
    else:
        return [parse_ue3_value(t) for t in tokens if t]


def parse_ue3_value(v: str) -> Any:
    """Parse scalar value, struct, array, or string."""
    v = v.strip()
    if v.endswith(';'):
        v = v[:-1].strip()

    # Quoted string
    if (v.startswith('"') and v.endswith('"')) or (v.startswith("'") and v.endswith("'")):
        return v[1:-1]

    # Struct literal
    if v.startswith('(') and v.endswith(')'):
        return parse_ue3_struct(v)

    # Boolean
    v_lower = v.lower()
    if v_lower in ('true', 'yes', 'on'):
        return True
    if v_lower in ('false', 'no', 'off'):
        return False

    # Numeric
    clean_v = v.rstrip('fF')
    try:
        if '.' in clean_v:
            return float(clean_v)
        return int(clean_v, 0)
    except ValueError:
        pass

    return v


def parse_ue3_text(text: str) -> Dict[str, Dict[str, Any]]:
    """Parse text formatted as UE3 INI or INT file into a structured dictionary."""
    sections: Dict[str, Dict[str, Any]] = {}
    current_section_name: Optional[str] = None
    current_section: Dict[str, Any] = {}

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith(';') or line.startswith('#') or line.startswith('//'):
            continue

        # Section header
        if line.startswith('[') and line.endswith(']'):
            if current_section_name is not None:
                sections[current_section_name] = current_section
            current_section_name = line[1:-1].strip()
            current_section = sections.get(current_section_name, {})
            continue

        if current_section_name is None:
            # Lines before first section header
            continue

        # Strip inline comments if not in quotes
        comment_pos = -1
        in_quote = False
        for idx, ch in enumerate(line):
            if ch == '"':
                in_quote = not in_quote
            elif not in_quote and ch in (';', '#'):
                comment_pos = idx
                break
        if comment_pos != -1:
            line = line[:comment_pos].strip()
            if not line:
                continue

        # Key=Value
        if '=' not in line:
            continue

        prefix = ''
        if line[0] in ('+', '-', '.'):
            prefix = line[0]
            line = line[1:].strip()

        key, val_str = line.split('=', 1)
        key = key.strip()
        val = parse_ue3_value(val_str)

        if prefix == '-':
            # Remove directive
            if key in current_section:
                if isinstance(current_section[key], list):
                    if val in current_section[key]:
                        current_section[key].remove(val)
                elif current_section[key] == val:
                    del current_section[key]
        elif prefix in ('+', '.'):
            # Explicit append directive
            if key not in current_section:
                current_section[key] = [val]
            elif isinstance(current_section[key], list):
                current_section[key].append(val)
            else:
                current_section[key] = [current_section[key], val]
        else:
            # Standard assignment
            if key in current_section:
                # If key was already defined, promote to list
                if isinstance(current_section[key], list):
                    current_section[key].append(val)
                else:
                    current_section[key] = [current_section[key], val]
            else:
                current_section[key] = val

    if current_section_name is not None:
        sections[current_section_name] = current_section

    return sections


def parse_ini_file(file_path: str) -> Dict[str, Dict[str, Any]]:
    """Parse a single UE3 .ini configuration file."""
    text = detect_and_decode(file_path)
    return parse_ue3_text(text)


def parse_loc_file(file_path: str) -> Dict[str, Dict[str, Any]]:
    """Parse a single UE3 localization file (.int, .pol, .deu, etc.)."""
    text = detect_and_decode(file_path)
    return parse_ue3_text(text)


def parse_config_directory(config_dir: str) -> Dict[str, Dict[str, Dict[str, Any]]]:
    """Parse all .ini files in a configuration directory."""
    result: Dict[str, Dict[str, Dict[str, Any]]] = {}
    pattern = os.path.join(config_dir, '*.ini')
    for filepath in sorted(glob.glob(pattern)):
        basename = os.path.basename(filepath)
        result[basename] = parse_ini_file(filepath)
    return result


def parse_localization_directory(loc_dir: str, lang: str = 'INT') -> Dict[str, Dict[str, Dict[str, Any]]]:
    """Parse all localization files in a given language folder (default INT)."""
    lang_dir = os.path.join(loc_dir, lang.upper())
    if not os.path.exists(lang_dir):
        lang_dir = loc_dir  # Assume direct directory if language subdir doesn't exist

    result: Dict[str, Dict[str, Dict[str, Any]]] = {}
    for filepath in sorted(glob.glob(os.path.join(lang_dir, '*.*'))):
        basename = os.path.basename(filepath)
        result[basename] = parse_loc_file(filepath)
    return result


def generate_full_oracle(tdgame_dir: str) -> Dict[str, Any]:
    """Generate complete game configuration & localization oracle from TdGame path."""
    config_dir = os.path.join(tdgame_dir, 'Config')
    loc_dir = os.path.join(tdgame_dir, 'Localization', 'INT')

    oracle: Dict[str, Any] = {
        'configs': parse_config_directory(config_dir) if os.path.isdir(config_dir) else {},
        'localization_int': parse_localization_directory(loc_dir, 'INT') if os.path.isdir(loc_dir) else {},
    }
    return oracle


def main():
    parser = argparse.ArgumentParser(description="Mirror's Edge UE3 INI & Localization Parser")
    parser.add_argument('--file', help='Path to single INI or INT file to parse')
    parser.add_argument('--config-dir', help='Path to TdGame/Config directory')
    parser.add_argument('--loc-dir', help='Path to TdGame/Localization directory')
    parser.add_argument('--tdgame', help='Path to TdGame directory to generate full oracle')
    parser.add_argument('--lang', default='INT', help='Language subfolder for localization (default: INT)')
    parser.add_argument('--out', help='Output JSON file path (prints to stdout if omitted)')
    parser.add_argument('--summary', action='store_true', help='Print summary statistics')

    args = parser.parse_args()

    data: Any = None

    if args.file:
        data = parse_ini_file(args.file)
    elif args.config_dir:
        data = parse_config_directory(args.config_dir)
    elif args.loc_dir:
        data = parse_localization_directory(args.loc_dir, args.lang)
    elif args.tdgame:
        data = generate_full_oracle(args.tdgame)
    else:
        # Default test using /Users/tomnom/mirrorsedge/TdGame
        default_tdgame = '/Users/tomnom/mirrorsedge/TdGame'
        if os.path.isdir(default_tdgame):
            data = generate_full_oracle(default_tdgame)
        else:
            parser.print_help()
            sys.exit(1)

    if args.summary:
        if isinstance(data, dict):
            print(f"Parsed top-level entries: {len(data)}")
            if 'configs' in data:
                print(f"  Config files: {len(data['configs'])}")
                total_sections = sum(len(sec) for sec in data['configs'].values())
                print(f"  Total config sections: {total_sections}")
            if 'localization_int' in data:
                print(f"  Localization files: {len(data['localization_int'])}")
                total_loc_sections = sum(len(sec) for sec in data['localization_int'].values())
                print(f"  Total localization sections: {total_loc_sections}")
            elif not ('configs' in data):
                total_sections = sum(len(sec) if isinstance(sec, dict) else 1 for sec in data.values())
                print(f"  Total sections/entries: {total_sections}")

    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        print(f"Successfully exported oracle to {args.out}")
    elif not args.summary:
        print(json.dumps(data, indent=2, ensure_ascii=False))


if __name__ == '__main__':
    main()
