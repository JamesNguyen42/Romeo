#include "raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using romeo::Arena;
using romeo::ArenaArray;

namespace {

// Framebuffers and worker tiles start on a 128-byte boundary (the cache-line size of this arm64 target). Tile
// columns are multiples of 64 pixels (256 bytes) and every setup's row stride is a multiple of 128 bytes, so tiles
// owned by different workers never share a cache line.
constexpr size_t kLineAlign = 128;

// ---------------------------------------------------------------- small math

struct V4 { float x, y, z, w; };

V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 norm(V3 a) { float l = std::sqrt(dot(a, a)); return {a.x / l, a.y / l, a.z / l}; }

M4 mul(const M4 &a, const M4 &b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
    return r;
}
V4 xform(const M4 &m, V3 p) {
    return {m.m[0] * p.x + m.m[1] * p.y + m.m[2] * p.z + m.m[3], m.m[4] * p.x + m.m[5] * p.y + m.m[6] * p.z + m.m[7],
            m.m[8] * p.x + m.m[9] * p.y + m.m[10] * p.z + m.m[11], m.m[12] * p.x + m.m[13] * p.y + m.m[14] * p.z + m.m[15]};
}
M4 look_at(V3 eye, V3 at) {
    V3 f = norm(sub(at, eye)), s = norm(cross(f, {0, 1, 0})), u = cross(s, f);
    return {{s.x, s.y, s.z, -dot(s, eye), u.x, u.y, u.z, -dot(u, eye), -f.x, -f.y, -f.z, dot(f, eye), 0, 0, 0, 1}};
}
M4 perspective(float fovy, float aspect, float n, float f) {
    float t = 1.0f / std::tan(fovy * 0.5f);
    return {{t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, (f + n) / (n - f), 2 * f * n / (n - f), 0, 0, -1, 0}};
}

// Color conversion tables, built once: 8-bit texel -> linear, and 12-bit linear -> 8-bit sRGB.
struct ColorTables {
    float to_linear[256];
    uint8_t to_srgb[4096];
    ColorTables() {
        for (int i = 0; i < 256; ++i) to_linear[i] = std::pow(i / 255.0f, 2.2f);
        for (int i = 0; i < 4096; ++i) {
            float c = i / 4095.0f;
            float s = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            to_srgb[i] = uint8_t(s * 255.0f + 0.5f);
        }
    }
    uint8_t encode(float linear01) const { return to_srgb[int(std::clamp(linear01, 0.0f, 1.0f) * 4095.0f + 0.5f)]; }
};
const ColorTables kColor;

// ---------------------------------------------------------------- scene

void push_quad(ArenaArray<Tri> &tris, V3 p0, V3 p1, V3 p2, V3 p3, int tex, float uvs) {
    V3 n = norm(cross(sub(p1, p0), sub(p3, p0)));
    float shade = 0.35f + 0.65f * std::max(0.0f, dot(n, norm({0.4f, 0.8f, 0.3f})));
    tris.push({{p0, 0, 0}, {p1, uvs, 0}, {p2, uvs, uvs}, tex, shade});
    tris.push({{p0, 0, 0}, {p2, uvs, uvs}, {p3, 0, uvs}, tex, shade});
}

// A standing figure around the y axis: radius profile from the feet to the top of the head, in metres. Quads go
// lower-ring -> upper-ring so normals face outward; the pole rings are degenerate and skipped by binning.
void push_figure(ArenaArray<Tri> &tris, int tex) {
    constexpr float kProfile[kFigureBands + 1][2] = {{0.00f, 0.00f}, {0.04f, 0.14f}, {0.30f, 0.21f}, {0.90f, 0.23f}, {1.25f, 0.21f}, {1.40f, 0.13f},
                                                      {1.47f, 0.07f}, {1.52f, 0.11f}, {1.60f, 0.135f}, {1.68f, 0.12f}, {1.74f, 0.07f}, {1.76f, 0.00f}};
    for (int b = 0; b < kFigureBands; ++b)
        for (int k = 0; k < kFigureSegments; ++k) {
            const float a0 = 6.2831853f * float(k) / kFigureSegments, a1 = 6.2831853f * float(k + 1) / kFigureSegments;
            const float y0 = kProfile[b][0], r0 = kProfile[b][1], y1 = kProfile[b + 1][0], r1 = kProfile[b + 1][1];
            push_quad(tris, {r0 * std::cos(a0), y0, r0 * std::sin(a0)}, {r1 * std::cos(a0), y1, r1 * std::sin(a0)},
                      {r1 * std::cos(a1), y1, r1 * std::sin(a1)}, {r0 * std::cos(a1), y0, r0 * std::sin(a1)}, tex, 1);
        }
}

// One box's five faces (no bottom) between (x0, 0, z0) and (x1, h, z1), in the order make_scene() uses.
void push_box(ArenaArray<Tri> &tris, float x0, float z0, float x1, float z1, float h, int tex) {
    V3 c[8] = {{x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}, {x0, h, z0}, {x1, h, z0}, {x1, h, z1}, {x0, h, z1}};
    push_quad(tris, c[4], c[5], c[6], c[7], tex, 1);
    push_quad(tris, c[0], c[1], c[5], c[4], tex, 1);
    push_quad(tris, c[1], c[2], c[6], c[5], tex, 1);
    push_quad(tris, c[2], c[3], c[7], c[6], tex, 1);
    push_quad(tris, c[3], c[0], c[4], c[7], tex, 1);
}

void make_scene(ArenaArray<Tri> &tris) {
    push_quad(tris, {-40, 0, -40}, {40, 0, -40}, {40, 0, 40}, {-40, 0, 40}, 0, 24);
    for (int gz = -5; gz <= 5; ++gz)
        for (int gx = -5; gx <= 5; ++gx) {
            float x = gx * 6.0f, z = gz * 6.0f, h = 1.0f + float((gx * 7 + gz * 13) & 3);
            int tex = 1 + ((gx + 5) * 11 + gz + 5) % 15;
            push_box(tris, x - 1, z - 1, x + 1, z + 1, h, tex);
        }
}

// ---------------------------------------------------------------- moving agents and the ego rig

// Agents in model space: push_figure's 1.76 m figure, feet centred on the origin. The model transform is a translation.

// The seeded path table. Box columns and rows sit at multiples of 6 m with 1 m half width, so corridors are 4 m
// wide around 6k + 3, and the grid ends at z = -31. Even agents cross in front of the rig along x, in the open strip
// z = -34 or the corridors z = -27, -21, -15, within x in [-14, 20]. Odd agents walk along z within z in [-30, 10],
// either side of the ego lane (x = 1.5, 4.5) or in the corridors x = -3, 9. Paths are 4 to 10 m long, speeds 1.2
// to 1.8 m/s.
void make_agent_paths(AgentPath *paths) {
    uint32_t state = 0x5eed1234u;
    auto next = [&]() { state = state * 1664525u + 1013904223u; return float(state >> 8) * (1.0f / 16777216.0f); };
    constexpr float kXLanes[4] = {-34, -27, -21, -15}, kZLanes[4] = {1.5f, 4.5f, -3, 9};
    for (int a = 0; a < kAgents; ++a) {
        AgentPath &p = paths[a];
        p.length = 4.0f + 6.0f * next();
        p.speed = 1.2f + 0.6f * next();
        p.phase = 2.0f * p.length * next();
        const float along = next();
        const int lane = int(next() * 4.0f) & 3;
        if (a % 2 == 0) {
            p.start = {-14.0f + (34.0f - p.length) * along, 0, kXLanes[lane]};
            p.dir = {1, 0, 0};
        } else {
            p.start = {kZLanes[lane], 0, -30.0f + (40.0f - p.length) * along};
            p.dir = {0, 0, 1};
        }
    }
}

// Back and forth: the agent covers 2 * length per cycle, going out along dir and back.
V3 agent_position(const AgentPath &p, int frame) {
    const float cycle = 2.0f * p.length;
    float u = std::fmod(p.phase + p.speed * (float(frame) * kFrameDt), cycle);
    if (u < 0) u += cycle;
    const float d = u < p.length ? u : cycle - u;
    return {p.start.x + p.dir.x * d, 0, p.start.z + p.dir.z * d};
}

M4 translate(V3 t) { return {{1, 0, 0, t.x, 0, 1, 0, t.y, 0, 0, 1, t.z, 0, 0, 0, 1}}; }

// Ego rig: four cameras at 1.6 m in the x = 3 corridor facing +z, +x, -z and -x, tilted slightly down. Starts at
// z = -38 and drives along +z when the ego moves.
constexpr V3 kRigDirs[4] = {{0, 0, 1}, {1, 0, 0}, {0, 0, -1}, {-1, 0, 0}};

// Per-object transforms of one view in a motion setup. vs.prev (view 0) takes the previous-frame ones.
struct ObjectTransforms {
    M4 prev_vp;
    M4 agent[kAgents], agent_prev[kAgents];
};

int tile_count_for(int width, int height, int tile) { return ((width + tile - 1) / tile) * ((height + tile - 1) / tile); }

void init_setup(RasterSetup &s, int width, int height, int tile, float lod_bias, const M4 &proj) {
    s.width = width;
    s.height = height;
    s.tile = tile;
    s.tiles_x = (width + tile - 1) / tile;
    s.tiles_y = (height + tile - 1) / tile;
    s.tile_count = s.tiles_x * s.tiles_y;
    s.lod_bias = lod_bias;
    s.proj = proj;
}

// Bytes of one view's triangles and tile bins.
size_t bin_bytes(int width, int height, int tile, int triangles) {
    const size_t tiles = size_t(tile_count_for(width, height, tile));
    return Arena::budget<ScreenTri>(size_t(triangles)) + Arena::budget<ArenaArray<uint16_t>>(tiles) + tiles * Arena::budget<uint16_t>(size_t(triangles));
}

bool init_bins(ViewState &vs, Arena &arena, int tile_count, int triangles) {
    if (!vs.tris.init(arena, uint32_t(triangles))) return false;
    vs.bins = arena.alloc<ArenaArray<uint16_t>>(size_t(tile_count));
    if (!vs.bins) return false;
    for (int t = 0; t < tile_count; ++t)
        if (!vs.bins[t].init(arena, uint32_t(triangles))) return false;
    return true;
}

// A depth buffer starts at the far plane, the value an uncovered pixel keeps.
float *alloc_depth(Arena &arena, int width, int height) {
    const size_t pixels = size_t(width) * size_t(height);
    auto *d = static_cast<float *>(arena.alloc_bytes(pixels * sizeof(float), kLineAlign));
    if (d) std::fill(d, d + pixels, 1.0f);
    return d;
}

// The lidar mount: the rig origin.
// Roof height of a vehicle rig: above the centre box, so returns come from the surrounding scene.
constexpr V3 kLidarMount = {0, 2, 0};
// Face axes in view-table order.
constexpr V3 kLidarAxes[kLidarViews] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
// Azimuth sector (0: around +x, 1: +z, 2: -x, 3: -z, following the azimuth direction) to view-table face.
constexpr int kSectorFace[4] = {0, 2, 1, 3};

// Transforms the scene with `mvp` and rebuilds the view's triangles and bins. Textured views resolve each
// triangle's texture level; depth-only views leave texels null and never read the cache. Returns false only when
// the cache cannot make a copy. With `ot` (motion setups) agent triangles use their own transforms and a view
// with vs.prev also records each vertex's previous-frame clip position; without it only the static scene is binned.
template <bool kTextured>
bool bin_triangles(const RasterSetup &s, const ArenaArray<Tri> &scene, ViewState &vs, const M4 &mvp, TextureCache *cache, int v, int f,
                   const ObjectTransforms *ot = nullptr) {
    const int W = s.width, H = s.height, tile = s.tile;

    vs.tris.clear();
    for (int t = 0; t < s.tile_count; ++t) vs.bins[t].clear();

    // Motion setups skip the single ground quad (triangles 0 and 1); its tiles follow the agents.
    const uint32_t triangles = ot ? scene.count : uint32_t(kSceneTriangles);
    for (uint32_t i = ot ? 2 : 0; i < triangles; ++i) {
        const Tri &tri = scene[i];
        const int agent = agent_of(int(i));
        const M4 &m = agent < 0 ? mvp : ot->agent[agent];
        const Vertex *vx[3] = {&tri.a, &tri.b, &tri.c};
        ScreenTri st{};
        bool visible = true;
        for (int k = 0; k < 3; ++k) {
            V4 c = xform(m, vx[k]->p);
            if (c.w <= 0.5f) { visible = false; break; }
            float iw = 1.0f / c.w;
            st.x[k] = (c.x * iw * 0.5f + 0.5f) * W;
            st.y[k] = (1.0f - (c.y * iw * 0.5f + 0.5f)) * H;
            st.z[k] = c.z * iw * 0.5f + 0.5f;
            st.inv_w[k] = iw;
            st.u_w[k] = vx[k]->u * iw;
            st.v_w[k] = vx[k]->v * iw;
        }
        if (!visible) continue;
        float area = std::fabs((st.x[1] - st.x[0]) * (st.y[2] - st.y[0]) - (st.x[2] - st.x[0]) * (st.y[1] - st.y[0])) * 0.5f;
        if (area < 0.5f) continue;

        if constexpr (kTextured) {
            // Streamed variant: textures of each view rotate every 60 frames.
            const Texture *tex = tri.texture == 0 ? &cache->base[0] : cache->get(v, tri.texture, f / 60);
            if (!tex) return false;
            float uv_area = 0.5f * std::fabs((tri.b.u - tri.a.u) * (tri.c.v - tri.a.v) - (tri.c.u - tri.a.u) * (tri.b.v - tri.a.v)) * float(tex->size) * float(tex->size);
            int lod = int(std::floor(0.5f * std::log2(std::max(1.0f, uv_area / area)) + s.lod_bias));
            TextureLevel level = select_level(*tex, lod);
            st.texels = level.texels;
            st.size = level.size;
            st.shade = tri.shade;
        }

        // Capacity: each scene triangle adds at most one entry to tris and at most one index to any bin, and every
        // array holds kSceneTriangles, so neither write can overflow.
        const uint16_t idx = uint16_t(vs.tris.count);
        if (vs.prev) {
            const M4 &pm = agent < 0 ? ot->prev_vp : ot->agent_prev[agent];
            PrevClip &pc = vs.prev[idx];
            for (int k = 0; k < 3; ++k) {
                const V4 c = xform(pm, vx[k]->p);
                pc.x_w[k] = c.x * st.inv_w[k];
                pc.y_w[k] = c.y * st.inv_w[k];
                pc.w_w[k] = c.w * st.inv_w[k];
            }
            pc.src = uint16_t(i);
        }
        vs.tris.items[vs.tris.count++] = st;
        int tx0 = std::max(0, int(std::min({st.x[0], st.x[1], st.x[2]})) / tile), tx1 = std::min(s.tiles_x - 1, int(std::max({st.x[0], st.x[1], st.x[2]})) / tile);
        int ty0 = std::max(0, int(std::min({st.y[0], st.y[1], st.y[2]})) / tile), ty1 = std::min(s.tiles_y - 1, int(std::max({st.y[0], st.y[1], st.y[2]})) / tile);
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) {
                ArenaArray<uint16_t> &bin = vs.bins[ty * s.tiles_x + tx];
                bin.items[bin.count++] = idx;
            }
    }
    return true;
}

// Rasterizes the worker's tiles of one view. kShade adds texture sampling and colour; the depth-only path runs the
// same coverage and depth test. A view with a depth buffer receives each finished tile's depth. kMotion (view 0 of
// a motion setup) writes each visible pixel's motion vector and triangle id straight into the view's buffers; the
// worker owns those rectangles exactly as it owns the colour rectangles.
template <bool kShade, bool kMotion>
void raster_view_tiles(const RasterSetup &s, ViewState &vs, WorkerTile &wt, int worker, int workers) {
    const int W = s.width, H = s.height, tile = s.tile;
    float *depth = wt.depth;
    uint32_t *tile_color = wt.color;

    for (int ti = worker; ti < s.tile_count; ti += workers) {
        int x0 = (ti % s.tiles_x) * tile, y0 = (ti / s.tiles_x) * tile;
        int x1 = std::min(x0 + tile, W), y1 = std::min(y0 + tile, H);
        std::fill(depth, depth + size_t(tile) * tile, 1.0f);
        if constexpr (kShade) std::fill(tile_color, tile_color + size_t(tile) * tile, 0xff3a2a1eu);
        if constexpr (kMotion)
            for (int y = y0; y < y1; ++y) {
                std::fill(&vs.motion[2 * (size_t(y) * W + x0)], &vs.motion[2 * (size_t(y) * W + x1)], 0.0f);
                std::fill(&vs.tri_id[size_t(y) * W + x0], &vs.tri_id[size_t(y) * W + x1], kNoTri);
            }

        for (uint16_t idx : vs.bins[ti]) {
            const ScreenTri &st = vs.tris[idx];
            float area = (st.x[1] - st.x[0]) * (st.y[2] - st.y[0]) - (st.x[2] - st.x[0]) * (st.y[1] - st.y[0]);
            if (std::fabs(area) < 1e-6f) continue;
            const float inv_area = 1.0f / area;
            int bx0 = std::max(x0, int(std::floor(std::min({st.x[0], st.x[1], st.x[2]})))),
                bx1 = std::min(x1 - 1, int(std::ceil(std::max({st.x[0], st.x[1], st.x[2]})))),
                by0 = std::max(y0, int(std::floor(std::min({st.y[0], st.y[1], st.y[2]})))),
                by1 = std::min(y1 - 1, int(std::ceil(std::max({st.y[0], st.y[1], st.y[2]}))));
            for (int py = by0; py <= by1; ++py)
                for (int px = bx0; px <= bx1; ++px) {
                    float fx = px + 0.5f, fy = py + 0.5f;
                    float w0 = ((st.x[1] - fx) * (st.y[2] - fy) - (st.x[2] - fx) * (st.y[1] - fy)) * inv_area;
                    float w1 = ((st.x[2] - fx) * (st.y[0] - fy) - (st.x[0] - fx) * (st.y[2] - fy)) * inv_area;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    float z = w0 * st.z[0] + w1 * st.z[1] + w2 * st.z[2];
                    float &d = depth[size_t(py - y0) * tile + (px - x0)];
                    if (z >= d) continue;
                    d = z;
                    if constexpr (kShade) {
                        float iw = w0 * st.inv_w[0] + w1 * st.inv_w[1] + w2 * st.inv_w[2];
                        float u = (w0 * st.u_w[0] + w1 * st.u_w[1] + w2 * st.u_w[2]) / iw;
                        float v = (w0 * st.v_w[0] + w1 * st.v_w[1] + w2 * st.v_w[2]) / iw;
                        uint32_t texel = sample_bilinear(st.texels, st.size, u, v);
                        uint32_t rgb = 0;
                        for (int c = 0; c < 3; ++c)
                            rgb |= uint32_t(kColor.encode(kColor.to_linear[(texel >> (8 * c)) & 255] * st.shade)) << (8 * c);
                        tile_color[size_t(py - y0) * tile + (px - x0)] = rgb | 0xff000000u;
                    }
                    if constexpr (kMotion) {
                        // Perspective-correct: clip x, y and w were each multiplied by the vertex's inv_w, so their
                        // screen-space interpolations share one factor that the divide by w cancels.
                        const PrevClip &pc = vs.prev[idx];
                        const float pw = std::max(w0 * pc.w_w[0] + w1 * pc.w_w[1] + w2 * pc.w_w[2], 1e-6f);
                        const float pxn = (w0 * pc.x_w[0] + w1 * pc.x_w[1] + w2 * pc.x_w[2]) / pw;
                        const float pyn = (w0 * pc.y_w[0] + w1 * pc.y_w[1] + w2 * pc.y_w[2]) / pw;
                        const size_t o = size_t(py) * W + px;
                        vs.motion[2 * o] = fx - (pxn * 0.5f + 0.5f) * W;
                        vs.motion[2 * o + 1] = fy - (1.0f - (pyn * 0.5f + 0.5f)) * H;
                        vs.tri_id[o] = idx;
                    }
                }
        }

        if constexpr (kShade)
            for (int y = y0; y < y1; ++y)
                std::memcpy(&vs.color[size_t(y) * W + x0], &tile_color[size_t(y - y0) * tile], size_t(x1 - x0) * 4);
        if (vs.depth)
            for (int y = y0; y < y1; ++y)
                std::memcpy(&vs.depth[size_t(y) * W + x0], &depth[size_t(y - y0) * tile], size_t(x1 - x0) * sizeof(float));
    }
}

} // namespace

// ---------------------------------------------------------------- start-up

size_t raster_arena_bytes(int width, int height, int tile, int cameras, bool depth, bool lidar, bool motion) {
    const int triangles = motion ? kMotionSceneTriangles : kSceneTriangles;
    const size_t pixels = size_t(width) * size_t(height);
    const size_t per_view = Arena::budget_bytes(pixels * sizeof(uint32_t), kLineAlign) + bin_bytes(width, height, tile, triangles);
    const size_t depth_bytes = depth ? Arena::budget_bytes(pixels * sizeof(float), kLineAlign) : 0;
    const size_t per_face = Arena::budget_bytes(size_t(kLidarWidth) * kLidarHeight * sizeof(float), kLineAlign) +
                            bin_bytes(kLidarWidth, kLidarHeight, tile, triangles);
    // Agent state, view 0's previous clip positions, motion vectors and triangle ids, and the blurred image.
    const size_t motion_bytes = Arena::budget<MotionState>() + Arena::budget<PrevClip>(size_t(triangles)) +
                                Arena::budget_bytes(pixels * 2 * sizeof(float), kLineAlign) +
                                Arena::budget_bytes(pixels * sizeof(uint16_t), kLineAlign) +
                                Arena::budget_bytes(pixels * sizeof(uint32_t), kLineAlign);
    return Arena::budget<Tri>(size_t(triangles)) + size_t(cameras) * per_view + depth_bytes +
           (lidar ? Arena::budget<LidarRig>() + kLidarViews * per_face : 0) + (motion ? motion_bytes : 0);
}

bool init_raster(RasterState &rs, Arena &arena, int width, int height, int tile, float lod_bias, int cameras, bool depth, bool lidar,
                 bool motion, bool ego_moving) {
    RasterSetup &s = rs.setup;
    init_setup(s, width, height, tile, lod_bias, perspective(1.0f, float(width) / float(height), kNear, kFar));
    rs.cameras = cameras;
    rs.triangles = motion ? kMotionSceneTriangles : kSceneTriangles;

    if (!rs.scene.init(arena, uint32_t(rs.triangles))) return false;
    make_scene(rs.scene);
    if (rs.scene.count != uint32_t(kSceneTriangles)) return false; // kSceneTriangles must match make_scene
    if (motion) {
        if (!(rs.motion = arena.alloc<MotionState>())) return false;
        MotionState &m = *rs.motion;
        make_agent_paths(m.paths);
        m.ego_moving = ego_moving;
        m.frame = 0;
        // Textures 1..15 like the boxes, so agents add no texture-cache entries.
        for (int a = 0; a < kAgents; ++a) push_figure(rs.scene, 1 + a % 15);
        constexpr float kCell = 80.0f / kGroundCells;
        for (int gz = 0; gz < kGroundCells; ++gz)
            for (int gx = 0; gx < kGroundCells; ++gx) {
                const float x0 = -40.0f + kCell * gx, z0 = -40.0f + kCell * gz, x1 = x0 + kCell, z1 = z0 + kCell;
                push_quad(rs.scene, {x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}, 0, 1);
            }
        if (rs.scene.count != uint32_t(kMotionSceneTriangles)) return false; // kAgentTris and kGroundCells must match
        if (!(m.blurred = static_cast<uint32_t *>(arena.alloc_bytes(size_t(width) * size_t(height) * sizeof(uint32_t), kLineAlign)))) return false;
    }

    for (int v = 0; v < cameras; ++v) {
        ViewState *vs = rs.views.push();
        if (!vs) return false;
        const size_t color_bytes = size_t(width) * size_t(height) * sizeof(uint32_t);
        vs->color = static_cast<uint32_t *>(arena.alloc_bytes(color_bytes, kLineAlign));
        if (!vs->color) return false;
        std::memset(vs->color, 0, color_bytes);
        if (!init_bins(*vs, arena, s.tile_count, rs.triangles)) return false;
    }
    if (depth && cameras > 0 && !(rs.views[0].depth = alloc_depth(arena, width, height))) return false;
    if (motion && cameras > 0) {
        ViewState &v0 = rs.views[0];
        const size_t pixels = size_t(width) * size_t(height);
        v0.prev = arena.alloc<PrevClip>(size_t(rs.triangles));
        v0.motion = static_cast<float *>(arena.alloc_bytes(pixels * 2 * sizeof(float), kLineAlign));
        v0.tri_id = static_cast<uint16_t *>(arena.alloc_bytes(pixels * sizeof(uint16_t), kLineAlign));
        if (!v0.prev || !v0.motion || !v0.tri_id) return false;
    }
    if (!lidar) return true;

    // 90 degree horizontal field of view: perspective() scales x by tan(fovy / 2) * aspect, which is 1 when
    // tan(fovy / 2) = height / width.
    if (!(rs.rig = arena.alloc<LidarRig>())) return false;
    LidarRig &rig = *rs.rig;
    const float aspect = float(kLidarWidth) / float(kLidarHeight);
    init_setup(rig.setup, kLidarWidth, kLidarHeight, tile, 0.0f, perspective(2.0f * std::atan(1.0f / aspect), aspect, kNear, kFar));
    for (int k = 0; k < kLidarViews; ++k) {
        const V3 axis = kLidarAxes[k];
        rig.mvp[k] = mul(rig.setup.proj, look_at(kLidarMount, {kLidarMount.x + axis.x, kLidarMount.y + axis.y, kLidarMount.z + axis.z}));
        ViewState *vs = rs.views.push();
        if (!vs) return false;
        if (!(vs->depth = alloc_depth(arena, kLidarWidth, kLidarHeight))) return false;
        if (!init_bins(*vs, arena, rig.setup.tile_count, rs.triangles)) return false;
    }
    constexpr float kDeg = 3.14159265358979f / 180.0f;
    for (int r = 0; r < kLidarRings; ++r) {
        const float el = (kLidarMinElevationDeg + (kLidarMaxElevationDeg - kLidarMinElevationDeg) * float(r) / float(kLidarRings - 1)) * kDeg;
        rig.cos_el[r] = std::cos(el);
        rig.sin_el[r] = std::sin(el);
    }
    for (int a = 0; a < kLidarSteps; ++a) {
        const float az = float(a) * (2.0f * 3.14159265358979f / float(kLidarSteps));
        rig.cos_az[a] = std::cos(az);
        rig.sin_az[a] = std::sin(az);
    }
    return true;
}

size_t worker_tile_bytes(int tile) {
    const size_t pixels = size_t(tile) * size_t(tile);
    return Arena::budget_bytes(pixels * sizeof(float), kLineAlign) + Arena::budget_bytes(pixels * sizeof(uint32_t), kLineAlign);
}

bool init_worker_tile(WorkerTile &wt, Arena &arena, int tile) {
    const size_t pixels = size_t(tile) * size_t(tile);
    wt.depth = static_cast<float *>(arena.alloc_bytes(pixels * sizeof(float), kLineAlign));
    wt.color = static_cast<uint32_t *>(arena.alloc_bytes(pixels * sizeof(uint32_t), kLineAlign));
    return wt.depth && wt.color;
}

// ---------------------------------------------------------------- transforms and binning (main thread)

float view_camera_angle(int view, int views, int frame, uint32_t view_seed) {
    // Integer multiplication gives consecutive seeds well-separated, deterministic orbit positions.
    const uint32_t orbit = view_seed * 2654435761u;
    const float offset = float(orbit >> 8) * (6.2831853f / 16777216.0f);
    return float(view) * 6.2831853f / float(views) + float(frame) * 0.01f + offset;
}

namespace {

// View-projection of camera view v at frame f: the orbiting cameras, or the ego rig in motion setups (which
// ignore the view seed).
M4 camera_vp(const RasterState &rs, int v, int f, uint32_t view_seed) {
    if (!rs.motion) {
        float angle = view_camera_angle(v, rs.cameras, f, view_seed);
        return mul(rs.setup.proj, look_at({std::cos(angle) * 34, 14, std::sin(angle) * 34}, {0, 0, 0}));
    }
    const float z = -38.0f + (rs.motion->ego_moving ? kEgoSpeed * (float(f) * kFrameDt) : 0.0f);
    const V3 eye = {3.0f, 1.6f, z}, d = kRigDirs[v % 4];
    return mul(rs.setup.proj, look_at(eye, {eye.x + d.x, eye.y - 0.12f, eye.z + d.z}));
}

} // namespace

bool bin_view(RasterState &rs, TextureCache &cache, int v, int f, uint32_t view_seed) {
    const M4 mvp = camera_vp(rs, v, f, view_seed);
    if (!rs.motion) return bin_triangles<true>(rs.setup, rs.scene, rs.views[uint32_t(v)], mvp, &cache, v, f);
    const MotionState &m = *rs.motion;
    ObjectTransforms ot;
    ot.prev_vp = camera_vp(rs, v, f - 1, view_seed);
    for (int a = 0; a < kAgents; ++a) {
        ot.agent[a] = mul(mvp, translate(m.agent_pos[a]));
        ot.agent_prev[a] = mul(ot.prev_vp, translate(m.agent_prev_pos[a]));
    }
    return bin_triangles<true>(rs.setup, rs.scene, rs.views[uint32_t(v)], mvp, &cache, v, f, &ot);
}

void step_motion(RasterState &rs, int frame, bool stale_agent_prev) {
    MotionState &m = *rs.motion;
    for (int a = 0; a < kAgents; ++a) {
        if (frame == 0) m.agent_prev_pos[a] = agent_position(m.paths[a], -1);
        else if (!stale_agent_prev) m.agent_prev_pos[a] = m.agent_pos[a];
        m.agent_pos[a] = agent_position(m.paths[a], frame);
    }
    m.frame = frame;
}

void bin_lidar_face(RasterState &rs, int face) {
    bin_triangles<false>(rs.rig->setup, rs.scene, rs.views[uint32_t(rs.cameras + face)], rs.rig->mvp[face], nullptr, 0, 0);
}

// ---------------------------------------------------------------- tile rasterization (worker threads)

void raster_tiles(RasterState &rs, int view, WorkerTile &wt, int worker, int workers) {
    ViewState &vs = rs.views[uint32_t(view)];
    if (vs.motion) raster_view_tiles<true, true>(rs.setup, vs, wt, worker, workers);
    else if (view < rs.cameras) raster_view_tiles<true, false>(rs.setup, vs, wt, worker, workers);
    else raster_view_tiles<false, false>(rs.rig->setup, vs, wt, worker, workers);
}

// ---------------------------------------------------------------- motion blur (worker threads)

// Reads view 0's colour and motion (written in the previous barrier phase), writes only this worker's tiles of
// the blurred image. Samples sit at the pixel centre plus (k + 0.5) / 8 - 0.5 of the motion vector, k = 0..7,
// clamped to the image, and are averaged in linear light.
void blur_tiles(RasterState &rs, int worker, int workers) {
    const RasterSetup &s = rs.setup;
    const ViewState &vs = rs.views[0];
    uint32_t *out = rs.motion->blurred;
    const int W = s.width, H = s.height, tile = s.tile;
    constexpr int kSamples = 8;

    for (int ti = worker; ti < s.tile_count; ti += workers) {
        const int x0 = (ti % s.tiles_x) * tile, y0 = (ti / s.tiles_x) * tile;
        const int x1 = std::min(x0 + tile, W), y1 = std::min(y0 + tile, H);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const size_t o = size_t(y) * W + x;
                const float mx = vs.motion[2 * o], my = vs.motion[2 * o + 1];
                if (mx == 0.0f && my == 0.0f) { out[o] = vs.color[o]; continue; }
                float sum[3] = {0, 0, 0};
                for (int k = 0; k < kSamples; ++k) {
                    const float t = (float(k) + 0.5f) / float(kSamples) - 0.5f;
                    const int sx = int(std::clamp(float(x) + 0.5f + mx * t, 0.0f, float(W) - 0.5f));
                    const int sy = int(std::clamp(float(y) + 0.5f + my * t, 0.0f, float(H) - 0.5f));
                    const uint32_t c = vs.color[size_t(sy) * W + sx];
                    for (int ch = 0; ch < 3; ++ch) sum[ch] += kColor.to_linear[(c >> (8 * ch)) & 255];
                }
                uint32_t rgb = 0;
                for (int ch = 0; ch < 3; ++ch) rgb |= uint32_t(kColor.encode(sum[ch] * (1.0f / kSamples))) << (8 * ch);
                out[o] = rgb | 0xff000000u;
            }
    }
}

// ---------------------------------------------------------------- motion ground truth and images (main thread)

namespace {

// Luminance of an sRGB-encoded pixel in 8-bit units, Rec. 709 weights on the encoded values.
float luma(uint32_t c) { return 0.2126f * float(c & 255) + 0.7152f * float((c >> 8) & 255) + 0.0722f * float((c >> 16) & 255); }

} // namespace

MotionMetrics motion_metrics(const RasterState &rs) {
    const RasterSetup &s = rs.setup;
    const ViewState &vs = rs.views[0];
    const MotionState &m = *rs.motion;
    const int W = s.width, H = s.height, f = m.frame;
    const uint32_t *img = m.blurred;

    // The true previous transforms, from the path table and the rig, independent of the stored ones.
    const M4 prev_vp = camera_vp(rs, 0, f - 1, 0);
    V3 true_prev[kAgents];
    for (int a = 0; a < kAgents; ++a) true_prev[a] = agent_position(m.paths[a], f - 1);

    // Pixels in the last row or column have no forward neighbour and are skipped by all three metrics.
    double err = 0, agent_e = 0, static_e = 0;
    size_t agent_n = 0, static_n = 0;
    for (int y = 0; y + 1 < H; ++y)
        for (int x = 0; x + 1 < W; ++x) {
            const size_t o = size_t(y) * W + x;
            const uint16_t id = vs.tri_id[o];
            if (id == kNoTri) continue;
            const float l = luma(img[o]);
            const double e = std::fabs(luma(img[o + 1]) - l) + std::fabs(luma(img[o + W]) - l);
            const int src = vs.prev[id].src, agent = agent_of(src);
            if (agent < 0) { static_e += e; ++static_n; continue; }
            agent_e += e;
            ++agent_n;

            // The pixel centre's surface point in model space: perspective-correct barycentrics of the current
            // screen triangle, from the same edge functions the rasterizer uses.
            const ScreenTri &st = vs.tris[id];
            const float fx = x + 0.5f, fy = y + 0.5f;
            const float inv_area = 1.0f / ((st.x[1] - st.x[0]) * (st.y[2] - st.y[0]) - (st.x[2] - st.x[0]) * (st.y[1] - st.y[0]));
            const float w0 = ((st.x[1] - fx) * (st.y[2] - fy) - (st.x[2] - fx) * (st.y[1] - fy)) * inv_area;
            const float w1 = ((st.x[2] - fx) * (st.y[0] - fy) - (st.x[0] - fx) * (st.y[2] - fy)) * inv_area;
            const float w2 = 1.0f - w0 - w1;
            const float b[3] = {w0 * st.inv_w[0], w1 * st.inv_w[1], w2 * st.inv_w[2]};
            const float bs = 1.0f / (b[0] + b[1] + b[2]);
            const Tri &tri = rs.scene[uint32_t(src)];
            const V3 p[3] = {tri.a.p, tri.b.p, tri.c.p};
            V3 world = true_prev[agent];
            for (int k = 0; k < 3; ++k) {
                world.x += b[k] * bs * p[k].x;
                world.y += b[k] * bs * p[k].y;
                world.z += b[k] * bs * p[k].z;
            }
            const V4 c = xform(prev_vp, world);
            const float iw = 1.0f / c.w;
            const float ax = fx - (c.x * iw * 0.5f + 0.5f) * W, ay = fy - (1.0f - (c.y * iw * 0.5f + 0.5f)) * H;
            err += std::hypot(double(vs.motion[2 * o] - ax), double(vs.motion[2 * o + 1] - ay));
        }
    MotionMetrics r{};
    r.mv_error_px = agent_n ? err / double(agent_n) : 0.0;
    r.agent_edge_energy = agent_n ? agent_e / double(agent_n) : 0.0;
    r.static_edge_energy = static_n ? static_e / double(static_n) : 0.0;
    return r;
}

void motion_image(const RasterState &rs, uint32_t *rgba) {
    const ViewState &vs = rs.views[0];
    const size_t pixels = size_t(rs.setup.width) * size_t(rs.setup.height);
    for (size_t o = 0; o < pixels; ++o) {
        const float mx = vs.motion[2 * o], my = vs.motion[2 * o + 1];
        const float v = std::min(std::sqrt(mx * mx + my * my) / 16.0f, 1.0f);
        // HSV at full saturation: hue from the direction angle, value from the magnitude.
        const float h = (std::atan2(my, mx) + 3.14159265f) * (6.0f / 6.2831853f); // [0, 6]
        const int i = int(h) % 6;
        const float fr = h - std::floor(h);
        const float q = v * (1.0f - fr), t = v * fr;
        float r, g, b;
        switch (i) {
        case 0: r = v, g = t, b = 0; break;
        case 1: r = q, g = v, b = 0; break;
        case 2: r = 0, g = v, b = t; break;
        case 3: r = 0, g = q, b = v; break;
        case 4: r = t, g = 0, b = v; break;
        default: r = v, g = 0, b = q; break;
        }
        rgba[o] = uint32_t(r * 255.0f + 0.5f) | uint32_t(g * 255.0f + 0.5f) << 8 | uint32_t(b * 255.0f + 0.5f) << 16 | 0xff000000u;
    }
}

// ---------------------------------------------------------------- lidar sampling (main thread)

uint32_t sample_lidar(const RasterState &rs, LidarPoint *points) {
    const LidarRig &rig = *rs.rig;
    const int W = rig.setup.width, H = rig.setup.height;
    constexpr int kSector = kLidarSteps / 4, kHalfSector = kLidarSteps / 8;
    uint32_t n = 0;

    for (int r = 0; r < kLidarRings; ++r)
        for (int a = 0; a < kLidarSteps; ++a) {
            // Each face covers the steps within 45 degrees of its axis, so the unit ray's eye depth c.w is at least
            // cos(45) * cos(15) = 0.68.
            const int face = kSectorFace[((a + kHalfSector) / kSector) % 4];
            const V3 dir = {rig.cos_el[r] * rig.cos_az[a], rig.sin_el[r], rig.cos_el[r] * rig.sin_az[a]};
            const V4 c = xform(rig.mvp[face], {kLidarMount.x + dir.x, kLidarMount.y + dir.y, kLidarMount.z + dir.z});
            const float iw = 1.0f / c.w;

            // The pixel the ray passes through, mapped exactly as bin_triangles() maps vertices. Only rounding at
            // a face edge (azimuth exactly 45 degrees off the axis) can land one pixel outside.
            const int px = std::clamp(int((c.x * iw * 0.5f + 0.5f) * W), 0, W - 1);
            const int py = std::clamp(int((1.0f - (c.y * iw * 0.5f + 0.5f)) * H), 0, H - 1);
            const float depth = linear_depth(rs.views[uint32_t(rs.cameras + face)].depth[size_t(py) * W + px]);
            if (!(depth < kFar)) continue;

            // Eye depth grows linearly along the ray: the hit lies depth / c.w unit lengths from the mount.
            const float t = depth * iw;
            points[n++] = {dir.x * t, dir.y * t, dir.z * t, 1.0f - depth / kFar};
        }
    return n;
}
