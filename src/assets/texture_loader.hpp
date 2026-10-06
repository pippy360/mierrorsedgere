#pragma once

// -----------------------------------------------------------------------------
// Texture2D / TextureCube loader for Mirror's Edge cooked packages (UE3 v536).
//
// UTexture2D native layout (after tagged properties):
//   FByteBulkData SourceArt            (empty in cooked data)
//   int32         NumMips
//   NumMips x { FByteBulkData Data; int32 SizeX; int32 SizeY; }
//
// FByteBulkData = uint32 Flags; int32 ElementCount; int32 SizeOnDisk; int32 OffsetInFile;
//                 [payload inline when !(Flags & BULKDATA_StoreInSeparateFile)]
// Flags: 0x01 StoreInSeparateFile, 0x02 ZLIB, 0x10 LZO, 0x20 Unused (no payload).
// Separate-file mips live in the package named after the texture's OUTERMOST
// package (e.g. Buildings/B_BD_Commercial.upk) at an absolute file offset,
// stored as UE3 compressed chunks (tag 0x9E2A83C1, block size, sizes, blocks).
// -----------------------------------------------------------------------------

#include "scene_materials.hpp"

#include <cstdint>
#include <string>

namespace me {

class UPKPackage;
class PackageManager;

struct TextureInfo {
    int size_x = 0;
    int size_y = 0;
    TexFormat format = TexFormat::Unknown;
    std::string format_name;
    bool srgb = true;
    TexAddress address_x = TexAddress::Wrap;
    TexAddress address_y = TexAddress::Wrap;
    float unpack_min[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float unpack_max[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    std::string compression_settings;
    std::string lod_group;
    bool is_normal_map = false;
};

// Reads only the tagged properties of a Texture2D export (cheap).
bool read_texture_info(const UPKPackage& pkg, int32_t export_index_1based, TextureInfo& out);

// Loads a Texture2D export including mip data (largest mip <= max_size).
bool load_texture2d(PackageManager& pm, const UPKPackage& pkg, int32_t export_index_1based, int max_size,
                    SceneTexture& out, std::string* error = nullptr);

// Loads a TextureCube export (its six Texture2D faces).
bool load_texture_cube(PackageManager& pm, const UPKPackage& pkg, int32_t export_index_1based, int max_size,
                       SceneTexture& out, std::string* error = nullptr);

// Decompresses a UE3 compressed-chunk payload (LZO or ZLIB) into `out`.
bool decompress_ue3_chunks(const uint8_t* src, size_t src_len, bool lzo, size_t expected_size, std::vector<uint8_t>& out);

// Bytes for a mip of the given format/size.
size_t texture_mip_bytes(TexFormat fmt, int w, int h);

}  // namespace me
