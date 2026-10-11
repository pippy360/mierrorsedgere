#pragma once

// -----------------------------------------------------------------------------
// BC1 / BC2 / BC3 (DXT1 / DXT3 / DXT5) block decoding to RGBA8 on the CPU.
//
// Most Android GPUs have no S3TC, so the OpenGL ES renderer decodes the level's
// DXT textures here and uploads them as RGBA8 / SRGB8_ALPHA8 (every mip level).
// The decoding follows the S3TC extension spec exactly: BC1 blocks whose first
// colour is not greater than the second use the three-colour mode with the fourth
// index transparent black (GL_COMPRESSED_RGBA_S3TC_DXT1), BC2 carries explicit
// 4-bit alpha, BC3 an interpolated 8-bit alpha ramp. Partial blocks at the right
// and bottom edges of textures whose size is not a multiple of 4 are clipped.
//
// ME_DXT_CPU=1 makes the renderer use this path even where the driver has S3TC,
// so it can be checked against the GPU's decoding on the desktop.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <vector>

namespace me {

enum class BcFormat { BC1, BC2, BC3 };

namespace bc_detail {

inline uint16_t read_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Expands a 5:6:5 colour to 8 bits a channel, replicating the top bits as the GPUs do.
inline void expand_565(uint16_t c, uint8_t out[4]) {
    const unsigned r = (c >> 11) & 0x1Fu;
    const unsigned g = (c >> 5) & 0x3Fu;
    const unsigned b = c & 0x1Fu;
    out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
    out[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
    out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
    out[3] = 255;
}

// Decodes one 8-byte colour block into `px` (16 RGBA texels, row by row). `opaque` is set for
// BC2 / BC3, whose colour block is always decoded in four-colour mode.
inline void decode_color_block(const uint8_t* block, bool opaque, uint8_t px[16][4]) {
    const uint16_t c0 = read_u16(block);
    const uint16_t c1 = read_u16(block + 2);
    uint8_t palette[4][4];
    expand_565(c0, palette[0]);
    expand_565(c1, palette[1]);
    if (opaque || c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            palette[2][k] = static_cast<uint8_t>((2 * palette[0][k] + palette[1][k] + 1) / 3);
            palette[3][k] = static_cast<uint8_t>((palette[0][k] + 2 * palette[1][k] + 1) / 3);
        }
        palette[2][3] = palette[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) {
            palette[2][k] = static_cast<uint8_t>((palette[0][k] + palette[1][k]) / 2);
            palette[3][k] = 0;
        }
        palette[2][3] = 255;
        palette[3][3] = 0;  // transparent black
    }
    const uint32_t bits = read_u32(block + 4);
    for (int i = 0; i < 16; ++i) {
        const unsigned idx = (bits >> (2 * i)) & 3u;
        px[i][0] = palette[idx][0];
        px[i][1] = palette[idx][1];
        px[i][2] = palette[idx][2];
        px[i][3] = palette[idx][3];
    }
}

// BC2: 16 explicit 4-bit alpha values.
inline void decode_alpha_explicit(const uint8_t* block, uint8_t alpha[16]) {
    for (int row = 0; row < 4; ++row) {
        const uint16_t v = read_u16(block + row * 2);
        for (int col = 0; col < 4; ++col) {
            const unsigned a = (v >> (4 * col)) & 0xFu;
            alpha[row * 4 + col] = static_cast<uint8_t>(a | (a << 4));
        }
    }
}

// BC3: two alpha end points and 16 3-bit indices into a ramp between them.
inline void decode_alpha_interpolated(const uint8_t* block, uint8_t alpha[16]) {
    const unsigned a0 = block[0];
    const unsigned a1 = block[1];
    unsigned ramp[8];
    ramp[0] = a0;
    ramp[1] = a1;
    if (a0 > a1) {
        for (unsigned i = 1; i < 7; ++i) ramp[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
    } else {
        for (unsigned i = 1; i < 5; ++i) ramp[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
        ramp[6] = 0;
        ramp[7] = 255;
    }
    // 48 bits of indices, little endian.
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) alpha[i] = static_cast<uint8_t>(ramp[(bits >> (3 * i)) & 7u]);
}

}  // namespace bc_detail

inline size_t bc_block_bytes(BcFormat f) { return f == BcFormat::BC1 ? 8 : 16; }

// Decodes a `width` x `height` image of `format` blocks (`blocks`, `size` bytes, rows of
// ceil(width / 4) blocks) into `out` as RGBA8, row 0 first. Returns false if `size` is short.
inline bool bc_decode_rgba8(BcFormat format, int width, int height, const uint8_t* blocks, size_t size,
                            std::vector<uint8_t>& out) {
    if (width <= 0 || height <= 0 || !blocks) return false;
    const int bw = (width + 3) / 4;
    const int bh = (height + 3) / 4;
    const size_t block_bytes = bc_block_bytes(format);
    if (size < static_cast<size_t>(bw) * static_cast<size_t>(bh) * block_bytes) return false;
    out.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    const uint8_t* b = blocks;
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx, b += block_bytes) {
            uint8_t px[16][4];
            uint8_t alpha[16];
            switch (format) {
                case BcFormat::BC1:
                    bc_detail::decode_color_block(b, false, px);
                    break;
                case BcFormat::BC2:
                    bc_detail::decode_alpha_explicit(b, alpha);
                    bc_detail::decode_color_block(b + 8, true, px);
                    for (int i = 0; i < 16; ++i) px[i][3] = alpha[i];
                    break;
                case BcFormat::BC3:
                    bc_detail::decode_alpha_interpolated(b, alpha);
                    bc_detail::decode_color_block(b + 8, true, px);
                    for (int i = 0; i < 16; ++i) px[i][3] = alpha[i];
                    break;
            }
            for (int row = 0; row < 4; ++row) {
                const int y = by * 4 + row;
                if (y >= height) break;
                for (int col = 0; col < 4; ++col) {
                    const int x = bx * 4 + col;
                    if (x >= width) break;
                    uint8_t* dst = &out[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4];
                    dst[0] = px[row * 4 + col][0];
                    dst[1] = px[row * 4 + col][1];
                    dst[2] = px[row * 4 + col][2];
                    dst[3] = px[row * 4 + col][3];
                }
            }
        }
    }
    return true;
}

}  // namespace me
