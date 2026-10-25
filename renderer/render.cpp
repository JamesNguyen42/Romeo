// romeo-render: a small tile-based CPU rasterizer used to produce real evidence for the demo cases.
//
// One run renders N frames of a textured scene from V views. It writes measured frame times, resident memory,
// structured logs, an optional Chrome trace and the last frame as PNG + raw RGBA. Each planted bug is behind a
// --bug flag, so the baseline and the regression come from the same binary.
//
// Threads: the main thread owns the frame loop, transforms and binning. W persistent workers rasterize tiles;
// worker w owns tiles where (tile % W == w), so tile buffers and framebuffer regions are never shared. The asset
// loader thread owns asset reads and publishes finished assets under asset_lock.

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- setups and bugs

struct Setup {
    char name;
    int views, width, height, workers;
};

constexpr std::array<Setup, 5> kSetups{{
    {'A', 1, 1920, 1080, 4},
    {'B', 4, 1920, 1080, 8},
    {'C', 1, 3840, 2160, 8},
    {'D', 8, 2560, 1440, 12},
    {'E', 12, 1920, 1080, 16},
}};

struct Bugs {
    bool cache_leak = false;    // texture cache never evicts streamed variants
    bool loader_lock = false;   // asset loader reads and parses files while holding asset_lock
    bool tile_size = false;     // 512-pixel tiles instead of 64
    bool view_setup = false;    // per-view setup (mip rebuild) runs every frame instead of once
    bool view_overflow = false; // view state array sized for 8 views
    bool disk_leak = false;     // per-view intermediate frames are never deleted
    bool mip_bias = false;      // texture LOD bias changed from 0 to +2.5
};

// ---------------------------------------------------------------- small math

struct V3 { float x, y, z; };
struct V4 { float x, y, z, w; };
struct M4 { float m[16]; };

static V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 norm(V3 a) { float l = std::sqrt(dot(a, a)); return {a.x / l, a.y / l, a.z / l}; }

static M4 mul(const M4 &a, const M4 &b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
    return r;
}
static V4 xform(const M4 &m, V3 p) {
    return {m.m[0] * p.x + m.m[1] * p.y + m.m[2] * p.z + m.m[3], m.m[4] * p.x + m.m[5] * p.y + m.m[6] * p.z + m.m[7],
            m.m[8] * p.x + m.m[9] * p.y + m.m[10] * p.z + m.m[11], m.m[12] * p.x + m.m[13] * p.y + m.m[14] * p.z + m.m[15]};
}
static M4 look_at(V3 eye, V3 at) {
    V3 f = norm(sub(at, eye)), s = norm(cross(f, {0, 1, 0})), u = cross(s, f);
    return {{s.x, s.y, s.z, -dot(s, eye), u.x, u.y, u.z, -dot(u, eye), -f.x, -f.y, -f.z, dot(f, eye), 0, 0, 0, 1}};
}
static M4 perspective(float fovy, float aspect, float n, float f) {
    float t = 1.0f / std::tan(fovy * 0.5f);
    return {{t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, (f + n) / (n - f), 2 * f * n / (n - f), 0, 0, -1, 0}};
}

// ---------------------------------------------------------------- textures

struct Texture {
    int size = 0;                              // level-0 width == height, power of two
    std::vector<std::vector<uint32_t>> levels; // RGBA8, linear color
};

static void build_mips(Texture &t) {
    t.levels.resize(1);
    for (int s = t.size; s > 1; s /= 2) {
        const auto &src = t.levels.back();
        std::vector<uint32_t> dst(size_t(s / 2) * (s / 2));
        for (int y = 0; y < s / 2; ++y)
            for (int x = 0; x < s / 2; ++x) {
                uint32_t acc[4] = {};
                for (int k = 0; k < 4; ++k) {
                    uint32_t p = src[size_t(2 * y + k / 2) * s + 2 * x + k % 2];
                    for (int c = 0; c < 4; ++c) acc[c] += (p >> (8 * c)) & 255;
                }
                dst[size_t(y) * (s / 2) + x] = (acc[0] / 4) | (acc[1] / 4) << 8 | (acc[2] / 4) << 16 | (acc[3] / 4) << 24;
            }
        t.levels.push_back(std::move(dst));
    }
}

static Texture make_texture(int id, int variant) {
    Texture t;
    t.size = 512;
    t.levels.assign(1, std::vector<uint32_t>(size_t(t.size) * t.size));
    uint32_t seed = uint32_t(id * 7919 + variant * 104729);
    uint8_t r = 80 + (seed * 37) % 160, g = 80 + (seed * 59) % 160, b = 80 + (seed * 83) % 160;
    for (int y = 0; y < t.size; ++y)
        for (int x = 0; x < t.size; ++x) {
            bool check = ((x / 16) ^ (y / 16)) & 1;
            bool line = (x % 64) < 2 || (y % 64) < 2;
            uint8_t k = line ? 30 : check ? 255 : 150;
            t.levels[0][size_t(y) * t.size + x] = uint32_t(r * k / 255) | uint32_t(g * k / 255) << 8 | uint32_t(b * k / 255) << 16 | 0xff000000u;
        }
    build_mips(t);
    return t;
}

static uint32_t sample_bilinear(const Texture &t, float u, float v, int level) {
    level = std::clamp(level, 0, int(t.levels.size()) - 1);
    int s = std::max(1, t.size >> level);
    const auto &px = t.levels[size_t(level)];
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

// Streamed texture variants. All variants are built once at startup into a fixed pool; the cache hands out
// pointers into the pool. The cache_leak bug keys copies by view and never frees them.
struct TextureCache {
    static constexpr int kTextures = 16, kVariants = 4;
    std::vector<Texture> pool; // kTextures * kVariants, built once
    std::map<uint64_t, std::unique_ptr<Texture>> leaked;
    bool leak = false;

    void build() {
        pool.reserve(kTextures * kVariants);
        for (int v = 0; v < kVariants; ++v)
            for (int t = 0; t < kTextures; ++t) pool.push_back(make_texture(t, v));
    }
    const Texture &get(int view, int id, int variant) {
        const Texture &shared = pool[size_t((variant % kVariants) * kTextures + id)];
        if (!leak) return shared;
        uint64_t key = uint64_t(view) << 40 | uint64_t(id) << 20 | uint64_t(variant);
        auto &slot = leaked[key];
        if (!slot) slot = std::make_unique<Texture>(shared);
        return *slot;
    }
    size_t entries() const { return leak ? leaked.size() : pool.size(); }
};

// ---------------------------------------------------------------- output helpers

static uint64_t resident_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, task_info_t(&info), &count) == KERN_SUCCESS) return info.resident_size;
    return 0;
#else
    unsigned long pages = 0, resident = 0;
    if (FILE *f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%lu %lu", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    return uint64_t(resident) * uint64_t(sysconf(_SC_PAGESIZE));
#endif
}

static double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// Structured log: one JSON object per line, flushed so a crash keeps every line written before it.
struct Log {
    FILE *f = nullptr;
    Clock::time_point t0;
    void line(const char *level, const char *msg, const char *extra = "") {
        if (!f) return;
        std::fprintf(f, "{\"t_ms\":%.3f,\"level\":\"%s\",\"msg\":\"%s\"%s%s}\n", ms_since(t0), level, msg, *extra ? "," : "", extra);
        std::fflush(f);
    }
};

// Chrome trace events (chrome://tracing, Perfetto). Each thread appends to its own buffer; buffers are joined at exit.
struct TraceBuffer {
    std::vector<std::string> events;
    void span(const char *name, int tid, double start_ms, double dur_ms) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "{\"name\":\"%s\",\"ph\":\"X\",\"pid\":1,\"tid\":%d,\"ts\":%.1f,\"dur\":%.1f}", name, tid, start_ms * 1000.0, dur_ms * 1000.0);
        events.emplace_back(buf);
    }
};

// ---------------------------------------------------------------- scene

struct Vertex { V3 p; float u, v; };
struct Tri { Vertex a, b, c; int texture; float shade; };

static std::vector<Tri> make_scene() {
    std::vector<Tri> tris;
    auto quad = [&](V3 p0, V3 p1, V3 p2, V3 p3, int tex, float uvs) {
        V3 n = norm(cross(sub(p1, p0), sub(p3, p0)));
        float shade = 0.35f + 0.65f * std::max(0.0f, dot(n, norm({0.4f, 0.8f, 0.3f})));
        tris.push_back({{p0, 0, 0}, {p1, uvs, 0}, {p2, uvs, uvs}, tex, shade});
        tris.push_back({{p0, 0, 0}, {p2, uvs, uvs}, {p3, 0, uvs}, tex, shade});
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
    return tris;
}

// Screen-space triangle after transform, ready for rasterization.
struct ScreenTri {
    float x[3], y[3], z[3], inv_w[3], u_w[3], v_w[3];
    const Texture *tex;
    float shade;
    int lod;
};

// ---------------------------------------------------------------- per-view state

constexpr int kMaxViews = 16;

struct ViewState {
    std::vector<uint32_t> color; // RGBA8, sRGB-encoded
    std::vector<ScreenTri> tris;
    std::vector<std::vector<uint32_t>> bins; // triangle indices per tile, capacity reused across frames
};

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
static const ColorTables kColor;

// ---------------------------------------------------------------- main

int main(int argc, char **argv) {
    char setup_name = 'A';
    int frames = 240;
    bool trace = false;
    Bugs bugs;
    fs::path out = "runs/local";
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--setup") setup_name = next()[0];
        else if (a == "--frames") frames = std::max(1, std::atoi(next()));
        else if (a == "--out") out = next();
        else if (a == "--trace") trace = true;
        else if (a == "--bug") {
            std::string_view list = next();
            auto has = [&](std::string_view b) { return list.find(b) != std::string_view::npos; };
            bugs = {has("cache_leak"), has("loader_lock"), has("tile_size"), has("view_setup"), has("view_overflow"), has("disk_leak"), has("mip_bias")};
        } else {
            std::fprintf(stderr, "usage: romeo-render --setup A..E [--frames N] [--bug a,b] [--trace] [--out dir]\n");
            return 2;
        }
    }
    const Setup *setup = nullptr;
    for (const auto &s : kSetups)
        if (s.name == setup_name) setup = &s;
    if (!setup) { std::fprintf(stderr, "unknown setup %c\n", setup_name); return 2; }

    fs::create_directories(out);
    Log log{std::fopen((out / "log.jsonl").c_str(), "w"), Clock::now()};
    char extra[256];
    std::snprintf(extra, sizeof extra, "\"setup\":\"%c\",\"views\":%d,\"width\":%d,\"height\":%d,\"workers\":%d,\"frames\":%d",
                  setup->name, setup->views, setup->width, setup->height, setup->workers, frames);
    log.line("info", "run_start", extra);

    // The fixed view table. The view_overflow bug restores the old 8-view limit.
    const int view_limit = bugs.view_overflow ? 8 : kMaxViews;
    if (setup->views > view_limit) {
        std::snprintf(extra, sizeof extra, "\"check\":\"view_count <= kMaxViews\",\"view_count\":%d,\"kMaxViews\":%d,\"file\":\"renderer/render.cpp\",\"function\":\"init_views\"", setup->views, view_limit);
        log.line("fatal", "assertion_failed", extra);
        std::fprintf(stderr, "assertion failed: view_count <= kMaxViews (%d > %d) in init_views\n", setup->views, view_limit);
        std::abort();
    }

    const int W = setup->width, H = setup->height, V = setup->views, workers = setup->workers;
    const int tile = bugs.tile_size ? 512 : 64;
    const int tiles_x = (W + tile - 1) / tile, tiles_y = (H + tile - 1) / tile, tile_count = tiles_x * tiles_y;
    const float lod_bias = bugs.mip_bias ? 2.5f : 0.0f;

    std::vector<Tri> scene = make_scene();
    std::array<ViewState, kMaxViews> views;
    for (int v = 0; v < V; ++v) {
        views[size_t(v)].color.assign(size_t(W) * H, 0);
        views[size_t(v)].tris.reserve(scene.size());
        views[size_t(v)].bins.resize(size_t(tile_count));
    }
    TextureCache cache;
    cache.leak = bugs.cache_leak;
    cache.build();
    std::vector<Texture> base_textures;
    for (int t = 0; t < 16; ++t) base_textures.push_back(make_texture(t, 0));

    // Asset loader: every ~3 s it reads and parses a 64 MiB asset file and publishes the checksum.
    const fs::path asset_path = out / "asset.bin";
    {
        std::vector<uint8_t> blob(64u << 20);
        for (size_t i = 0; i < blob.size(); ++i) blob[i] = uint8_t(i * 2654435761u >> 24);
        FILE *f = std::fopen(asset_path.c_str(), "wb");
        std::fwrite(blob.data(), 1, blob.size(), f);
        std::fclose(f);
    }
    std::mutex asset_lock;
    uint64_t published_asset = 0;
    std::atomic<bool> stop{false};
    std::vector<TraceBuffer> traces(size_t(workers) + 2); // [0] main, [1] loader, [2..] workers
    std::thread loader([&] {
        std::vector<uint8_t> buf(64u << 20);
        auto next = Clock::now() + std::chrono::milliseconds(3000);
        while (!stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (Clock::now() < next) continue;
            next += std::chrono::milliseconds(3000);
            auto read_and_parse = [&] {
                FILE *f = std::fopen(asset_path.c_str(), "rb");
                size_t n = f ? std::fread(buf.data(), 1, buf.size(), f) : 0;
                if (f) std::fclose(f);
                uint64_t h = 1469598103934665603ull;
                for (size_t i = 0; i < n; ++i) h = (h ^ buf[i]) * 1099511628211ull;
                return h;
            };
            double t = ms_since(log.t0);
            if (bugs.loader_lock) {
                std::lock_guard g(asset_lock);
                published_asset = read_and_parse();
            } else {
                uint64_t h = read_and_parse();
                std::lock_guard g(asset_lock);
                published_asset = h;
            }
            if (trace) traces[1].span("asset_read_parse", 2, t, ms_since(log.t0) - t);
        }
    });

    // Worker pool: the barrier releases all workers for a frame and collects them when every tile is done.
    int frame_view = 0;
    std::barrier sync(workers + 1);
    std::vector<std::thread> pool;
    for (int w = 0; w < workers; ++w)
        pool.emplace_back([&, w] {
            std::vector<float> depth(size_t(tile) * tile);
            std::vector<uint32_t> tile_color(size_t(tile) * tile);
            for (;;) {
                sync.arrive_and_wait();
                if (stop.load(std::memory_order_relaxed)) return;
                ViewState &vs = views[size_t(frame_view)];
                double t = ms_since(log.t0);
                for (int ti = w; ti < tile_count; ti += workers) {
                    int x0 = (ti % tiles_x) * tile, y0 = (ti / tiles_x) * tile;
                    int x1 = std::min(x0 + tile, W), y1 = std::min(y0 + tile, H);
                    std::fill(depth.begin(), depth.end(), 1.0f);
                    std::fill(tile_color.begin(), tile_color.end(), 0xff3a2a1eu);
                    for (uint32_t idx : vs.bins[size_t(ti)]) {
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
                                float iw = w0 * st.inv_w[0] + w1 * st.inv_w[1] + w2 * st.inv_w[2];
                                float u = (w0 * st.u_w[0] + w1 * st.u_w[1] + w2 * st.u_w[2]) / iw;
                                float v = (w0 * st.v_w[0] + w1 * st.v_w[1] + w2 * st.v_w[2]) / iw;
                                uint32_t texel = sample_bilinear(*st.tex, u, v, st.lod);
                                uint32_t rgb = 0;
                                for (int c = 0; c < 3; ++c)
                                    rgb |= uint32_t(kColor.encode(kColor.to_linear[(texel >> (8 * c)) & 255] * st.shade)) << (8 * c);
                                tile_color[size_t(py - y0) * tile + (px - x0)] = rgb | 0xff000000u;
                            }
                    }
                    for (int y = y0; y < y1; ++y)
                        std::memcpy(&vs.color[size_t(y) * W + x0], &tile_color[size_t(y - y0) * tile], size_t(x1 - x0) * 4);
                }
                if (trace) traces[size_t(w) + 2].span("raster_tiles", w + 3, t, ms_since(log.t0) - t);
                sync.arrive_and_wait();
            }
        });

    FILE *frames_csv = std::fopen((out / "frames.csv").c_str(), "w");
    FILE *memory_csv = std::fopen((out / "memory.csv").c_str(), "w");
    std::fprintf(frames_csv, "frame,ms\n");
    std::fprintf(memory_csv, "t_ms,resident_bytes\n");
    std::vector<double> frame_ms;
    frame_ms.reserve(size_t(frames));
    const M4 proj = perspective(1.0f, float(W) / float(H), 0.5f, 200.0f);
    fs::create_directories(out / "tmp");

    for (int f = 0; f < frames; ++f) {
        auto t0 = Clock::now();
        double tf = ms_since(log.t0);

        // Pick up newly published assets; with loader_lock this waits for the whole file read.
        {
            double tw = ms_since(log.t0);
            std::lock_guard g(asset_lock);
            (void)published_asset;
            double waited = ms_since(log.t0) - tw;
            if (trace && waited > 0.5) traces[0].span("wait_asset_lock", 1, tw, waited);
        }

        for (int v = 0; v < V; ++v) {
            ViewState &vs = views[size_t(v)];
            if (bugs.view_setup)
                for (auto &t : base_textures) build_mips(t); // should run once at load, not per view per frame
            float angle = float(v) * 6.2831853f / float(V) + float(f) * 0.01f;
            M4 mvp = mul(proj, look_at({std::cos(angle) * 34, 14, std::sin(angle) * 34}, {0, 0, 0}));

            vs.tris.clear();
            for (auto &b : vs.bins) b.clear();
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
                // Streamed variant: textures of each view rotate every 60 frames.
                st.tex = tri.texture == 0 ? &base_textures[0] : &cache.get(v, tri.texture, f / 60);
                float uv_area = 0.5f * std::fabs((tri.b.u - tri.a.u) * (tri.c.v - tri.a.v) - (tri.c.u - tri.a.u) * (tri.b.v - tri.a.v)) * float(st.tex->size) * float(st.tex->size);
                st.lod = int(std::floor(0.5f * std::log2(std::max(1.0f, uv_area / area)) + lod_bias));
                st.shade = tri.shade;
                uint32_t idx = uint32_t(vs.tris.size());
                vs.tris.push_back(st);
                int tx0 = std::max(0, int(std::min({st.x[0], st.x[1], st.x[2]})) / tile), tx1 = std::min(tiles_x - 1, int(std::max({st.x[0], st.x[1], st.x[2]})) / tile);
                int ty0 = std::max(0, int(std::min({st.y[0], st.y[1], st.y[2]})) / tile), ty1 = std::min(tiles_y - 1, int(std::max({st.y[0], st.y[1], st.y[2]})) / tile);
                for (int ty = ty0; ty <= ty1; ++ty)
                    for (int tx = tx0; tx <= tx1; ++tx) vs.bins[size_t(ty * tiles_x + tx)].push_back(idx);
            }

            frame_view = v;
            sync.arrive_and_wait(); // start workers on this view
            sync.arrive_and_wait(); // wait for all tiles

            // Per-view intermediate thumbnail for the compositor. The fix overwrites one file per view.
            char name[64];
            if (bugs.disk_leak) std::snprintf(name, sizeof name, "view%02d-frame%05d.rgba", v, f);
            else std::snprintf(name, sizeof name, "view%02d.rgba", v);
            if (FILE *t = std::fopen((out / "tmp" / name).c_str(), "wb")) {
                for (int y = 0; y < H; y += 8)
                    for (int x = 0; x < W; x += 8) std::fwrite(&vs.color[size_t(y) * W + x], 4, 1, t);
                std::fclose(t);
            }
        }

        double ms = ms_since(t0);
        frame_ms.push_back(ms);
        std::fprintf(frames_csv, "%d,%.3f\n", f, ms);
        if (f % 5 == 0) std::fprintf(memory_csv, "%.1f,%llu\n", ms_since(log.t0), (unsigned long long)resident_bytes());
        if (trace) traces[0].span("frame", 1, tf, ms);
    }

    stop = true;
    sync.arrive_and_wait();
    for (auto &t : pool) t.join();
    loader.join();
    std::fclose(frames_csv);
    std::fclose(memory_csv);

    // Last frame of view 0: PNG for people, raw RGBA for exact comparison.
    stbi_write_png((out / "image.png").c_str(), W, H, 4, views[0].color.data(), W * 4);
    if (FILE *r = std::fopen((out / "image.rgba").c_str(), "wb")) {
        std::fwrite(views[0].color.data(), 4, views[0].color.size(), r);
        std::fclose(r);
    }
    if (trace) {
        FILE *tf = std::fopen((out / "trace.json").c_str(), "w");
        std::fprintf(tf, "{\"traceEvents\":[");
        bool first = true;
        for (auto &tb : traces)
            for (auto &e : tb.events) { std::fprintf(tf, "%s%s", first ? "" : ",\n", e.c_str()); first = false; }
        std::fprintf(tf, "]}\n");
        std::fclose(tf);
    }

    uint64_t tmp_bytes = 0;
    for (auto &e : fs::directory_iterator(out / "tmp")) tmp_bytes += e.file_size();
    // The first kWarmup frames include thread start-up and first-touch page faults; statistics skip them.
    constexpr size_t kWarmup = 40;
    std::vector<double> sorted(frame_ms.begin() + std::min(kWarmup, frame_ms.size() - 1), frame_ms.end());
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) { return sorted[std::min(sorted.size() - 1, size_t(p * double(sorted.size())))]; };
    int spikes = 0;
    for (double ms : sorted) spikes += ms > 2.0 * pct(0.5);
    FILE *s = std::fopen((out / "summary.json").c_str(), "w");
    std::fprintf(s, "{\"setup\":\"%c\",\"views\":%d,\"width\":%d,\"height\":%d,\"workers\":%d,\"frames\":%d,\"tile\":%d,"
                    "\"p50_ms\":%.3f,\"p95_ms\":%.3f,\"p99_ms\":%.3f,\"max_ms\":%.3f,\"spike_frames\":%d,"
                    "\"resident_end_bytes\":%llu,\"cache_entries\":%zu,\"tmp_bytes\":%llu,\"total_ms\":%.1f}\n",
                 setup->name, V, W, H, workers, frames, tile, pct(0.5), pct(0.95), pct(0.99), sorted.back(), spikes,
                 (unsigned long long)resident_bytes(), cache.entries(), (unsigned long long)tmp_bytes, ms_since(log.t0));
    std::fclose(s);
    log.line("info", "run_end");
    fs::remove(asset_path);
    return 0;
}
