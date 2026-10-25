// Textures: storage, mip construction, bilinear sampling and the streamed-variant cache.
//
// Every texture is 512 x 512 RGBA8 (linear colour) with its full mip chain in one contiguous block. All blocks are
// bump-allocated from the main arena at start-up and never move, so the main thread can hand level pointers to the
// workers inside each binned triangle.
//
// The cache_leak bug copies a texture per (view, texture, variant) into a dedicated texture-copy arena and never
// releases the copy. When that arena is full, get() returns nullptr and the caller decides what to do.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "fixed.hpp"

constexpr int kTextureSize = 512;
constexpr int kTextureLevels = 10; // 512, 256, ..., 1

constexpr size_t texture_texels() {
    size_t n = 0;
    for (int s = kTextureSize; s >= 1; s /= 2) n += size_t(s) * size_t(s);
    return n;
}
constexpr size_t kTextureTexels = texture_texels(); // 349,525
constexpr size_t kTextureBytes = kTextureTexels * sizeof(uint32_t); // 1,398,100
constexpr size_t kTextureAlign = 128; // one block starts on its own cache line (128 bytes on this arm64 target)

struct Texture {
    int size = 0;                          // level-0 width == height, power of two
    uint32_t *level[kTextureLevels] = {};  // level[k] holds (size >> k)^2 texels inside one block
};

// The mip level a triangle samples, resolved once per triangle at binning time.
struct TextureLevel {
    const uint32_t *texels;
    int size;
};

inline TextureLevel select_level(const Texture &t, int lod) {
    lod = std::clamp(lod, 0, kTextureLevels - 1);
    return {t.level[lod], std::max(1, t.size >> lod)};
}

// Wrapping bilinear filter on one mip level of size s x s (s is a power of two).
inline uint32_t sample_bilinear(const uint32_t *px, int s, float u, float v) {
    float fx = (u - std::floor(u)) * s - 0.5f, fy = (v - std::floor(v)) * s - 0.5f;
    int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    float ax = fx - x0, ay = fy - y0;
    auto at = [&](int x, int y) { return px[size_t((y & (s - 1)) * s + (x & (s - 1)))]; };
    uint32_t q[4] = {at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1)};
    uint32_t out = 0;
    for (int c = 0; c < 3; ++c) {
        auto ch = [&](uint32_t p) { return float((p >> (8 * c)) & 255); };
        float top = ch(q[0]) + (ch(q[1]) - ch(q[0])) * ax, bot = ch(q[2]) + (ch(q[3]) - ch(q[2])) * ax;
        out |= uint32_t(top + (bot - top) * ay + 0.5f) << (8 * c);
    }
    return out;
}

// Rebuilds levels 1.. of t in place from level 0.
void build_mips(Texture &t);

// Streamed texture variants. All variants are built once at start-up into a fixed pool; the cache hands out
// pointers into the pool. The cache_leak bug keys copies by view and never frees them.
//
// Owned and mutated by the main thread only (binning and view_setup run there while the workers wait).
struct TextureCache {
    static constexpr int kTextures = 16, kVariants = 4;

    Texture base[kTextures];               // variant 0; texture 0 is the ground, view_setup rebuilds their mips
    Texture pool[kVariants * kTextures];   // every streamed variant, built once

    // cache_leak only. Variants only grow (frame / 60), so once a view moves to a newer variant it never asks for
    // an older key again: one slot per (view, texture) finds the live copy, and every superseded copy stays
    // allocated in `copies` forever, exactly like the never-evicting map it models.
    struct LeakSlot {
        int variant = -1;
        const Texture *copy = nullptr;
    };
    bool leak = false;
    romeo::Arena copies;         // texture-copy arena, bump only, never rewound
    LeakSlot *leaked = nullptr;  // views * kTextures slots
    uint32_t leaked_count = 0;

    // Bytes one leaked copy takes from `copies`, including worst-case alignment padding.
    static constexpr size_t copy_bytes() { return romeo::Arena::budget<Texture>() + romeo::Arena::budget_bytes(kTextureBytes, kTextureAlign); }

    // nullptr only when cache_leak needs a new copy and the texture-copy arena is full.
    const Texture *get(int view, int id, int variant);
    size_t entries() const { return leak ? leaked_count : size_t(kVariants * kTextures); }
};

// Main-arena bytes init_textures() takes for this view count.
size_t texture_arena_bytes(int views);

// Builds every pool and base texture from `arena`. `copies` is the texture-copy arena (empty unless leak).
// Returns false when `arena` is too small.
bool init_textures(TextureCache &cache, romeo::Arena &arena, int views, bool leak, romeo::Arena copies);
