#include "texture.hpp"

#include <cstring>

using romeo::Arena;

namespace {

// Points every level of t into one block of kTextureTexels texels.
void bind_levels(Texture &t, uint32_t *block) {
    t.size = kTextureSize;
    size_t offset = 0;
    for (int k = 0; k < kTextureLevels; ++k) {
        t.level[k] = block + offset;
        size_t s = size_t(kTextureSize >> k);
        offset += s * s;
    }
}

// Procedural checkerboard with grid lines; the colour comes from (id, variant).
void fill_level0(Texture &t, int id, int variant) {
    uint32_t *px = t.level[0];
    uint32_t seed = uint32_t(id * 7919 + variant * 104729);
    uint8_t r = 80 + (seed * 37) % 160, g = 80 + (seed * 59) % 160, b = 80 + (seed * 83) % 160;
    for (int y = 0; y < t.size; ++y)
        for (int x = 0; x < t.size; ++x) {
            bool check = ((x / 16) ^ (y / 16)) & 1;
            bool line = (x % 64) < 2 || (y % 64) < 2;
            uint8_t k = line ? 30 : check ? 255 : 150;
            px[size_t(y) * t.size + x] = uint32_t(r * k / 255) | uint32_t(g * k / 255) << 8 | uint32_t(b * k / 255) << 16 | 0xff000000u;
        }
}

bool make_texture(Texture &t, Arena &arena, int id, int variant) {
    auto *block = static_cast<uint32_t *>(arena.alloc_bytes(kTextureBytes, kTextureAlign));
    if (!block) return false;
    bind_levels(t, block);
    fill_level0(t, id, variant);
    build_mips(t);
    return true;
}

} // namespace

void build_mips(Texture &t) {
    int level = 0;
    for (int s = t.size; s > 1; s /= 2, ++level) {
        const uint32_t *src = t.level[level];
        uint32_t *dst = t.level[level + 1];
        for (int y = 0; y < s / 2; ++y)
            for (int x = 0; x < s / 2; ++x) {
                uint32_t acc[4] = {};
                for (int k = 0; k < 4; ++k) {
                    uint32_t p = src[size_t(2 * y + k / 2) * s + 2 * x + k % 2];
                    for (int c = 0; c < 4; ++c) acc[c] += (p >> (8 * c)) & 255;
                }
                dst[size_t(y) * (s / 2) + x] = (acc[0] / 4) | (acc[1] / 4) << 8 | (acc[2] / 4) << 16 | (acc[3] / 4) << 24;
            }
    }
}

size_t texture_arena_bytes(int views) {
    constexpr int kBlocks = TextureCache::kTextures + TextureCache::kVariants * TextureCache::kTextures;
    return kBlocks * Arena::budget_bytes(kTextureBytes, kTextureAlign) +
           Arena::budget<TextureCache::LeakSlot>(size_t(views) * TextureCache::kTextures);
}

bool init_textures(TextureCache &cache, Arena &arena, int views, bool leak, Arena copies) {
    // Same build order as before the split: every variant of the pool, then the base set.
    for (int v = 0; v < TextureCache::kVariants; ++v)
        for (int t = 0; t < TextureCache::kTextures; ++t)
            if (!make_texture(cache.pool[v * TextureCache::kTextures + t], arena, t, v)) return false;
    for (int t = 0; t < TextureCache::kTextures; ++t)
        if (!make_texture(cache.base[t], arena, t, 0)) return false;

    cache.leak = leak;
    cache.copies = copies;
    cache.leaked = arena.alloc<TextureCache::LeakSlot>(size_t(views) * TextureCache::kTextures);
    cache.leaked_count = 0;
    return cache.leaked != nullptr;
}

const Texture *TextureCache::get(int view, int id, int variant) {
    const Texture &shared = pool[(variant % kVariants) * kTextures + id];
    if (!leak) return &shared;

    LeakSlot &slot = leaked[view * kTextures + id];
    if (slot.copy && slot.variant == variant) return slot.copy;

    // A new key: copy the shared texture and never release it. A failed copy leaves the arena cursor where it was.
    const size_t mark = copies.mark();
    Texture *copy = copies.alloc<Texture>();
    auto *block = static_cast<uint32_t *>(copies.alloc_bytes(kTextureBytes, kTextureAlign));
    if (!copy || !block) {
        copies.rewind(mark);
        return nullptr;
    }
    std::memcpy(block, shared.level[0], kTextureBytes);
    bind_levels(*copy, block);
    slot = {variant, copy};
    ++leaked_count;
    return copy;
}
