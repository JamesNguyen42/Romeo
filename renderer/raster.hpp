// Scene, per-view transforms and binning (main thread), and tile rasterization (worker threads).
//
// For each view the main thread transforms the scene into screen-space triangles and bins their indices per tile.
// The workers then rasterize disjoint tiles of that view: worker w owns tiles where tile % workers == w, writes
// only its own WorkerTile and its own framebuffer rectangles, and only reads the binned triangles. The caller's
// barrier separates the two phases, so neither side needs a lock.
#pragma once

#include <cstddef>
#include <cstdint>

#include "fixed.hpp"
#include "texture.hpp"

// The fixed view table. Its capacity is part of the view_overflow story: the bug restores an 8-view limit.
constexpr int kMaxViews = 16;

// Ground quad plus an 11 x 11 grid of boxes with 5 faces each (no bottom), two triangles per quad.
constexpr int kSceneTriangles = 2 + 11 * 11 * 5 * 2; // 1,212
static_assert(kSceneTriangles <= 65535, "tile bins store uint16_t triangle indices");

struct V3 { float x, y, z; };
struct M4 { float m[16]; };

struct Vertex { V3 p; float u, v; };
struct Tri { Vertex a, b, c; int texture; float shade; };

// Screen-space triangle after transform, ready for rasterization. The texture level is resolved at binning time.
struct ScreenTri {
    float x[3], y[3], z[3], inv_w[3], u_w[3], v_w[3];
    const uint32_t *texels; // mip level chosen for this triangle (after LOD bias and clamp)
    int size;               // that level's width == height
    float shade;
};

struct RasterSetup {
    int width = 0, height = 0, tile = 0, tiles_x = 0, tiles_y = 0, tile_count = 0;
    float lod_bias = 0;
    M4 proj = {};
};

struct ViewState {
    uint32_t *color = nullptr;                  // width * height RGBA8, sRGB-encoded; each tile written by its owner
    romeo::ArenaArray<ScreenTri> tris;          // capacity kSceneTriangles, rebuilt per frame by the main thread
    romeo::ArenaArray<uint16_t> *bins = nullptr; // tile_count bins of triangle indices, capacity kSceneTriangles each
};

struct RasterState {
    RasterSetup setup;
    romeo::ArenaArray<Tri> scene;                 // kSceneTriangles, built once
    romeo::FixedArray<ViewState, kMaxViews> views;
};

// One worker's tile-sized depth and colour scratch. Owned by that worker for the whole run.
struct WorkerTile {
    float *depth = nullptr;
    uint32_t *color = nullptr;
};

// Main-arena bytes init_raster() takes.
size_t raster_arena_bytes(int width, int height, int tile, int views);
bool init_raster(RasterState &rs, romeo::Arena &arena, int width, int height, int tile, float lod_bias, int views);

// Main-arena bytes init_worker_tile() takes for one worker.
size_t worker_tile_bytes(int tile);
bool init_worker_tile(WorkerTile &wt, romeo::Arena &arena, int tile);

// Main thread: transform the scene for view `view` of frame `frame` and rebuild that view's bins. Returns false
// only when the texture cache cannot make a copy (cache_leak with a full texture-copy arena); the view is then
// partially binned and must not be rasterized.
bool bin_view(RasterState &rs, TextureCache &cache, int view, int frame);

// Worker thread: rasterize every tile of `view` that worker `worker` of `workers` owns.
void raster_tiles(RasterState &rs, int view, WorkerTile &wt, int worker, int workers);
