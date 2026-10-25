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

void make_scene(ArenaArray<Tri> &tris) {
    auto quad = [&](V3 p0, V3 p1, V3 p2, V3 p3, int tex, float uvs) {
        V3 n = norm(cross(sub(p1, p0), sub(p3, p0)));
        float shade = 0.35f + 0.65f * std::max(0.0f, dot(n, norm({0.4f, 0.8f, 0.3f})));
        tris.push({{p0, 0, 0}, {p1, uvs, 0}, {p2, uvs, uvs}, tex, shade});
        tris.push({{p0, 0, 0}, {p2, uvs, uvs}, {p3, 0, uvs}, tex, shade});
    };
    quad({-40, 0, -40}, {40, 0, -40}, {40, 0, 40}, {-40, 0, 40}, 0, 24);
    for (int gz = -5; gz <= 5; ++gz)
        for (int gx = -5; gx <= 5; ++gx) {
            float x = gx * 6.0f, z = gz * 6.0f, h = 1.0f + float((gx * 7 + gz * 13) & 3);
            int tex = 1 + ((gx + 5) * 11 + gz + 5) % 15;
            V3 c[8] = {{x - 1, 0, z - 1}, {x + 1, 0, z - 1}, {x + 1, 0, z + 1}, {x - 1, 0, z + 1},
                       {x - 1, h, z - 1}, {x + 1, h, z - 1}, {x + 1, h, z + 1}, {x - 1, h, z + 1}};
            quad(c[4], c[5], c[6], c[7], tex, 1);
            quad(c[0], c[1], c[5], c[4], tex, 1);
            quad(c[1], c[2], c[6], c[5], tex, 1);
            quad(c[2], c[3], c[7], c[6], tex, 1);
            quad(c[3], c[0], c[4], c[7], tex, 1);
        }
}

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
size_t bin_bytes(int width, int height, int tile) {
    const size_t tiles = size_t(tile_count_for(width, height, tile));
    return Arena::budget<ScreenTri>(kSceneTriangles) + Arena::budget<ArenaArray<uint16_t>>(tiles) + tiles * Arena::budget<uint16_t>(kSceneTriangles);
}

bool init_bins(ViewState &vs, Arena &arena, int tile_count) {
    if (!vs.tris.init(arena, kSceneTriangles)) return false;
    vs.bins = arena.alloc<ArenaArray<uint16_t>>(size_t(tile_count));
    if (!vs.bins) return false;
    for (int t = 0; t < tile_count; ++t)
        if (!vs.bins[t].init(arena, kSceneTriangles)) return false;
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
// the cache cannot make a copy.
template <bool kTextured>
bool bin_triangles(const RasterSetup &s, const ArenaArray<Tri> &scene, ViewState &vs, const M4 &mvp, TextureCache *cache, int v, int f) {
    const int W = s.width, H = s.height, tile = s.tile;

    vs.tris.clear();
    for (int t = 0; t < s.tile_count; ++t) vs.bins[t].clear();

    for (const Tri &tri : scene) {
        const Vertex *vx[3] = {&tri.a, &tri.b, &tri.c};
        ScreenTri st{};
        bool visible = true;
        for (int k = 0; k < 3; ++k) {
            V4 c = xform(mvp, vx[k]->p);
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
// same coverage and depth test. A view with a depth buffer receives each finished tile's depth.
template <bool kShade>
void raster_view_tiles(const RasterSetup &s, ViewState &vs, WorkerTile &wt, int worker, int workers) {
    const int W = s.width, H = s.height, tile = s.tile;
    float *depth = wt.depth;
    uint32_t *tile_color = wt.color;

    for (int ti = worker; ti < s.tile_count; ti += workers) {
        int x0 = (ti % s.tiles_x) * tile, y0 = (ti / s.tiles_x) * tile;
        int x1 = std::min(x0 + tile, W), y1 = std::min(y0 + tile, H);
        std::fill(depth, depth + size_t(tile) * tile, 1.0f);
        if constexpr (kShade) std::fill(tile_color, tile_color + size_t(tile) * tile, 0xff3a2a1eu);

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

size_t raster_arena_bytes(int width, int height, int tile, int cameras, bool depth, bool lidar) {
    const size_t per_view = Arena::budget_bytes(size_t(width) * size_t(height) * sizeof(uint32_t), kLineAlign) + bin_bytes(width, height, tile);
    const size_t depth_bytes = depth ? Arena::budget_bytes(size_t(width) * size_t(height) * sizeof(float), kLineAlign) : 0;
    const size_t per_face = Arena::budget_bytes(size_t(kLidarWidth) * kLidarHeight * sizeof(float), kLineAlign) +
                            bin_bytes(kLidarWidth, kLidarHeight, tile);
    return Arena::budget<Tri>(kSceneTriangles) + size_t(cameras) * per_view + depth_bytes +
           (lidar ? Arena::budget<LidarRig>() + kLidarViews * per_face : 0);
}

bool init_raster(RasterState &rs, Arena &arena, int width, int height, int tile, float lod_bias, int cameras, bool depth, bool lidar) {
    RasterSetup &s = rs.setup;
    init_setup(s, width, height, tile, lod_bias, perspective(1.0f, float(width) / float(height), kNear, kFar));
    rs.cameras = cameras;

    if (!rs.scene.init(arena, kSceneTriangles)) return false;
    make_scene(rs.scene);
    if (rs.scene.count != uint32_t(kSceneTriangles)) return false; // kSceneTriangles must match make_scene

    for (int v = 0; v < cameras; ++v) {
        ViewState *vs = rs.views.push();
        if (!vs) return false;
        const size_t color_bytes = size_t(width) * size_t(height) * sizeof(uint32_t);
        vs->color = static_cast<uint32_t *>(arena.alloc_bytes(color_bytes, kLineAlign));
        if (!vs->color) return false;
        std::memset(vs->color, 0, color_bytes);
        if (!init_bins(*vs, arena, s.tile_count)) return false;
    }
    if (depth && cameras > 0 && !(rs.views[0].depth = alloc_depth(arena, width, height))) return false;
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
        if (!init_bins(*vs, arena, rig.setup.tile_count)) return false;
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

bool bin_view(RasterState &rs, TextureCache &cache, int v, int f) {
    float angle = float(v) * 6.2831853f / float(rs.cameras) + float(f) * 0.01f;
    M4 mvp = mul(rs.setup.proj, look_at({std::cos(angle) * 34, 14, std::sin(angle) * 34}, {0, 0, 0}));
    return bin_triangles<true>(rs.setup, rs.scene, rs.views[uint32_t(v)], mvp, &cache, v, f);
}

void bin_lidar_face(RasterState &rs, int face) {
    bin_triangles<false>(rs.rig->setup, rs.scene, rs.views[uint32_t(rs.cameras + face)], rs.rig->mvp[face], nullptr, 0, 0);
}

// ---------------------------------------------------------------- tile rasterization (worker threads)

void raster_tiles(RasterState &rs, int view, WorkerTile &wt, int worker, int workers) {
    ViewState &vs = rs.views[uint32_t(view)];
    if (view < rs.cameras) raster_view_tiles<true>(rs.setup, vs, wt, worker, workers);
    else raster_view_tiles<false>(rs.rig->setup, vs, wt, worker, workers);
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
