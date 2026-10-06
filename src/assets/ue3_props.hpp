#pragma once

// -----------------------------------------------------------------------------
// Generic UE3 (v536 / Mirror's Edge) tagged-property tree parser.
//
// Unlike UPKPackage::parse_properties (flat name->value map used by the actor
// extractor), this parser keeps every property (including array indices),
// recursively decodes tagged structs, arrays of tagged structs and arrays of
// object references. It is the foundation of the material system.
//
// Format notes (verified against retail CookedPC data):
//   FPropertyTag = FName Name; [if Name != None] FName Type; int32 Size; int32 ArrayIndex;
//                  StructProperty: + FName StructName
//                  BoolProperty:   + int32 Value (Size == 0)
//                  ByteProperty:   NO enum name in v536 (added in v633). Value is an
//                                  FName (Size 8) or a raw byte (Size 1).
//   "Immutable" structs (Vector, LinearColor, Guid, Color, ...) are serialized natively.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

namespace me {

class UPKPackage;

struct UProperty {
    std::string name;
    std::string type;         // e.g. "IntProperty"
    std::string struct_name;  // StructProperty only
    int32_t array_index = 0;
    int32_t size = 0;
    size_t value_offset = 0;  // absolute offset of the value payload in package data

    // Decoded payloads
    int32_t i = 0;            // Int / Object index / Byte (size 1)
    float f = 0.0f;           // Float
    bool b = false;           // Bool
    std::string s;            // Name / Str / Byte enum value name
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // Vector / Vector2D / LinearColor / Color (RGBA 0..1) / Plane / Quat
    int32_t vi[3] = {0, 0, 0};              // Rotator / IntPoint
    bool immutable_struct = false;

    std::vector<UProperty> fields;                 // tagged struct members
    int32_t array_count = -1;                      // ArrayProperty element count
    std::vector<std::vector<UProperty>> elements;  // ArrayProperty of tagged structs
    std::vector<int32_t> ints;                     // ArrayProperty of 4-byte values (object refs / ints)
    std::vector<std::string> names;                // ArrayProperty of names
};

using UPropertyList = std::vector<UProperty>;

// Parses a tagged property stream at absolute offset `offset` (bounded by `end`).
// Returns the absolute offset just past the terminating 'None' (or where parsing stopped).
size_t parse_property_tree(const UPKPackage& pkg, size_t offset, size_t end, UPropertyList& out, int depth = 0);

// Parses the tagged properties of an export (auto-detects the property start).
// Returns the absolute offset of the first byte after the property block (native tail).
size_t parse_export_properties(const UPKPackage& pkg, int32_t export_index_1based, UPropertyList& out);

// Lookup helpers
const UProperty* find_prop(const UPropertyList& props, const std::string& name, int32_t array_index = 0);
int32_t prop_int(const UPropertyList& props, const std::string& name, int32_t def = 0);
float prop_float(const UPropertyList& props, const std::string& name, float def = 0.0f);
bool prop_bool(const UPropertyList& props, const std::string& name, bool def = false);
int32_t prop_object(const UPropertyList& props, const std::string& name);
std::string prop_name(const UPropertyList& props, const std::string& name, const std::string& def = "");

// Object path helpers (Outer chain), e.g. "B_BD_Commercial.BD_Commercial_01.M_BD_15_01"
//
// object_full_path() is relative to the package file: objects cooked into a level from other
// packages are rooted at an EF_ForcedExport package export (so the path starts with the source
// package), but objects owned by the file itself (e.g. everything inside M_GenericCubemaps.upk)
// have no package prefix. object_canonical_path() always prefixes those with the file's package
// name, giving the globally unique UE3 path name used by imports in other packages.
std::string object_full_path(const UPKPackage& pkg, int32_t index);
std::string object_canonical_path(const UPKPackage& pkg, int32_t index);
std::string object_outermost_name(const UPKPackage& pkg, int32_t index);  // package of the canonical path
std::string package_name_of(const UPKPackage& pkg);                       // file stem, e.g. "M_GenericCubemaps"
std::string object_class_name(const UPKPackage& pkg, int32_t index);
std::string export_object_name(const UPKPackage& pkg, int32_t export_index_1based);

std::string to_lower(std::string s);

}  // namespace me
