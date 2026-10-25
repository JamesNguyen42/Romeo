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

// Motion setups append kAgents moving boxes (5 faces each, no bottom) after the static scene, so the static
// triangles keep their indices and order. Agent a owns scene triangles [kSceneTriangles + a * kAgentTris, + kAgentTris).
// The rig cameras stand on the ground, and a triangle with a vertex behind the near plane is dropped, so motion
// setups replace the single ground quad (triangles 0 and 1) with kGroundCells^2 tiles of one texture repeat each,
// appended after the agents.
constexpr int kAgents = 24;
// An agent is a person-shaped surface of revolution (feet, hips, chest, shoulders, neck, head): kFigureBands
// profile bands of kFigureSegments quads each.
constexpr int kFigureSegments = 10, kFigureBands = 11;
constexpr int kAgentTris = kFigureSegments * kFigureBands * 2;
constexpr int kGroundCells = 24; // the ground quad's 24 texture repeats over 80 m
constexpr int kMotionSceneTriangles = kSceneTriangles + kAgents * kAgentTris + kGroundCells * kGroundCells * 2;
static_assert(kMotionSceneTriangles < 65535, "tile bins and the pixel triangle ids store uint16_t indices");

// Agent that owns scene triangle `tri`, or -1 for static geometry.
constexpr int agent_of(int tri) {
    return tri >= kSceneTriangles && tri < kSceneTriangles + kAgents * kAgentTris ? (tri - kSceneTriangles) / kAgentTris : -1;
}

// Fixed simulation step of moving agents and the ego rig: frame f is at time f * kFrameDt seconds.
constexpr float kFrameDt = 1.0f / 30.0f;
constexpr float kEgoSpeed = 8.0f; // m/s along +z when the ego moves

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

// Previous-frame clip position of one ScreenTri's vertices, each multiplied by that vertex's current inv_w so
// screen-space barycentrics interpolate it perspective-correctly. `src` is the scene triangle index.
struct PrevClip {
    float x_w[3], y_w[3], w_w[3];
    uint16_t src;
};

struct ViewState {
    uint32_t *color = nullptr;                  // width * height RGBA8, sRGB-encoded; camera views only
    float *depth = nullptr;                     // width * height window depth of the last pass, or nullptr: view 0
                                                // when the setup has a depth sensor, and every lidar face
    romeo::ArenaArray<ScreenTri> tris;          // capacity = scene triangles, rebuilt per frame by the main thread
    romeo::ArenaArray<uint16_t> *bins = nullptr; // tile_count bins of triangle indices, capacity = scene triangles
    // Motion setups, view 0 only (nullptr otherwise):
    PrevClip *prev = nullptr;                   // parallel to tris
    float *motion = nullptr;                    // width * height float2, pixels per frame, current - previous
    uint16_t *tri_id = nullptr;                 // width * height index into tris of the visible triangle, or kNoTri
};

constexpr uint16_t kNoTri = 0xffff;

// One agent's straight back-and-forth path, from the seeded table built by init_raster().
struct AgentPath {
    V3 start, dir; // dir is a unit axis
    float length, speed, phase; // metres, m/s, metres along the 2 * length cycle at t = 0
};

// Moving agents, ego rig and blur output. From the arena only for motion setups; written by the main thread
// between frames, read by the workers while they rasterize and blur.
struct MotionState {
    AgentPath paths[kAgents];
    V3 agent_pos[kAgents];      // this frame
    V3 agent_prev_pos[kAgents]; // the previous-frame model transform (a translation) used for motion vectors
    bool ego_moving = false;
    int frame = 0;
    uint32_t *blurred = nullptr; // width * height RGBA8: view 0 after motion blur
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
    MotionState *motion = nullptr;                // from the arena, only when the setup has moving agents
    int triangles = kSceneTriangles;              // scene triangle count
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
// after the camera views; `motion` adds the agents, view 0's motion buffers and the blurred image.
size_t raster_arena_bytes(int width, int height, int tile, int cameras, bool depth, bool lidar, bool motion);
bool init_raster(RasterState &rs, romeo::Arena &arena, int width, int height, int tile, float lod_bias, int cameras, bool depth,
                 bool lidar, bool motion, bool ego_moving);

// Main thread, motion setups, before binning frame `frame`: moves the agents. The stored previous-frame agent
// transforms take this frame's old values; `stale_agent_prev` (the bug) stops that update after frame 0.
void step_motion(RasterState &rs, int frame, bool stale_agent_prev);

// Worker thread, motion setups: blurs every tile of view 0 that worker `worker` of `workers` owns into
// MotionState::blurred, 8 samples along each pixel's motion vector.
void blur_tiles(RasterState &rs, int worker, int workers);

// Main thread, motion setups, after the last frame's blur.
struct MotionMetrics {
    double mv_error_px;        // mean |rendered - analytic| motion vector over agent pixels
    double agent_edge_energy;  // mean |dL/dx| + |dL/dy| of the blurred image over agent pixels
    double static_edge_energy; // the same over the other covered pixels
};
MotionMetrics motion_metrics(const RasterState &rs);

// motion.png pixels: hue = direction, value = magnitude / 16 px (clamped).
void motion_image(const RasterState &rs, uint32_t *rgba);

// Main-arena bytes init_worker_tile() takes for one worker.
size_t worker_tile_bytes(int tile);
bool init_worker_tile(WorkerTile &wt, romeo::Arena &arena, int tile);

// Main thread: transform the scene for camera view `view` of frame `frame` and rebuild that view's bins. Returns
// false only when the texture cache cannot make a copy (cache_leak with a full texture-copy arena); the view is then
// partially binned and must not be rasterized.
bool bin_view(RasterState &rs, TextureCache &cache, int view, int frame, uint32_t view_seed = 0);

// A seed changes the orbit's starting angle, not the scene or its textures. Seed 0 preserves the original view.
// Reuse the same seed when comparing builds so every camera position matches.
float view_camera_angle(int view, int views, int frame, uint32_t view_seed);

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
