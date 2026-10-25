// Scene, per-view transforms and binning (main thread), and tile rasterization (worker threads).
//
// For each view the main thread transforms the scene into screen-space triangles and bins their indices per tile.
// The workers then rasterize disjoint tiles of that view: worker w owns tiles where tile % workers == w, writes
// only its own WorkerTile and its own framebuffer rectangles, and only reads the binned triangles. The caller's
// barrier separates the two phases, so neither side needs a lock.
//
// The view table holds the camera views first, then, when the setup has a lidar, kLidarViews depth-only lidar
// faces. Camera views are shaded and textured; lidar faces run the same tiles and workers but only write depth.
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

// Near and far planes of every view. The depth test stores window depth in [0, 1]; 1 is the far plane and also the
// value of a pixel no triangle covers.
constexpr float kNear = 0.5f, kFar = 200.0f;

// Eye-space depth in metres along the view axis for window depth d, the inverse of the perspective() depth mapping.
inline float linear_depth(float d) {
    const float z_ndc = 2.0f * d - 1.0f;
    return 2.0f * kFar * kNear / (kFar + kNear - z_ndc * (kFar - kNear));
}

// Lidar at the rig origin: kLidarViews depth-only faces with a 90 degree horizontal field of view, facing +x, -x, +z
// and -z in that table order. One sweep samples kLidarRings elevations from -15 to +15 degrees (both included) at
// kLidarSteps azimuth steps around the full circle.
constexpr int kLidarViews = 4;
constexpr int kLidarRings = 32, kLidarSteps = 1024;
constexpr float kLidarMinElevationDeg = -15.0f, kLidarMaxElevationDeg = 15.0f;
constexpr int kLidarPoints = kLidarRings * kLidarSteps; // most returns one sweep can write
static_assert(kLidarSteps % 8 == 0, "every face covers kLidarSteps / 4 steps centred on its axis");

// Lidar face resolution, derived from the sweep pattern. In the image plane of a 90 degree face the azimuth steps
// (2 pi / 1024 = 0.00614 rad) are at least 0.00614 apart, and a pixel is 2 / 384 = 0.00521 wide, so every step
// samples its own column. The highest ring reaches tan(15) / cos(45) = 0.379 above the axis at the face edge, which
// needs 0.379 * 384 = 145.5 rows above and below the centre line together; pixels are square.
constexpr int kLidarWidth = 384, kLidarHeight = 146;

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
    uint32_t *color = nullptr;                  // width * height RGBA8, sRGB-encoded; camera views only
    float *depth = nullptr;                     // width * height window depth of the last pass, or nullptr: view 0
                                                // when the setup has a depth sensor, and every lidar face
    romeo::ArenaArray<ScreenTri> tris;          // capacity kSceneTriangles, rebuilt per frame by the main thread
    romeo::ArenaArray<uint16_t> *bins = nullptr; // tile_count bins of triangle indices, capacity kSceneTriangles each
};

// The lidar faces' fixed geometry and the sweep's direction tables, built once by init_raster().
struct LidarRig {
    RasterSetup setup;                    // kLidarWidth x kLidarHeight, 90 degree horizontal field of view
    M4 mvp[kLidarViews];                  // one per face, in view-table order (+x, -x, +z, -z)
    float cos_el[kLidarRings], sin_el[kLidarRings];
    float cos_az[kLidarSteps], sin_az[kLidarSteps]; // azimuth from +x towards +z
};

struct RasterState {
    RasterSetup setup;                            // camera views
    romeo::ArenaArray<Tri> scene;                 // kSceneTriangles, built once
    romeo::FixedArray<ViewState, kMaxViews> views; // [0, cameras) camera views, then the lidar faces if any
    int cameras = 0;
    LidarRig *rig = nullptr;                      // from the arena, only when the setup has a lidar
};

// One lidar return in the lidar frame: origin at the mount, axes parallel to the world axes (y up), metres.
// intensity = 1 - depth / kFar, where depth is the face's eye-space depth of the hit.
struct LidarPoint {
    float x, y, z, intensity;
};

// One worker's tile-sized depth and colour scratch. Owned by that worker for the whole run.
struct WorkerTile {
    float *depth = nullptr;
    uint32_t *color = nullptr;
};

// Main-arena bytes init_raster() takes. `depth` adds view 0's depth buffer; `lidar` adds kLidarViews lidar faces
// after the camera views.
size_t raster_arena_bytes(int width, int height, int tile, int cameras, bool depth, bool lidar);
bool init_raster(RasterState &rs, romeo::Arena &arena, int width, int height, int tile, float lod_bias, int cameras, bool depth,
                 bool lidar);

// Main-arena bytes init_worker_tile() takes for one worker.
size_t worker_tile_bytes(int tile);
bool init_worker_tile(WorkerTile &wt, romeo::Arena &arena, int tile);

// Main thread: transform the scene for camera view `view` of frame `frame` and rebuild that view's bins. Returns
// false only when the texture cache cannot make a copy (cache_leak with a full texture-copy arena); the view is then
// partially binned and must not be rasterized.
bool bin_view(RasterState &rs, TextureCache &cache, int view, int frame);

// Main thread: transform the scene for lidar face `face` (0 .. kLidarViews - 1) and rebuild its bins. Depth-only
// triangles carry no texture, so the texture cache is never touched.
void bin_lidar_face(RasterState &rs, int face);

// Worker thread: rasterize every tile of `view` that worker `worker` of `workers` owns. Camera views are shaded;
// lidar faces only run the depth test. Each view with a depth buffer receives its tiles' depth.
void raster_tiles(RasterState &rs, int view, WorkerTile &wt, int worker, int workers);

// Main thread, after every lidar face of this sweep has been rasterized: samples the sweep pattern from the faces'
// depth and writes one point per hit closer than kFar into `points` (capacity kLidarPoints), ring by ring from
// -15 degrees, each ring from azimuth 0 (+x) towards +z. Returns the number of points written.
uint32_t sample_lidar(const RasterState &rs, LidarPoint *points);
