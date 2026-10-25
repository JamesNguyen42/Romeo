// romeo-render: a small tile-based CPU rasterizer used to produce real evidence for the demo cases.
//
// One run renders N frames of a textured scene from V views. It writes measured frame times, resident memory,
// structured logs, an optional Chrome trace and the last frame as PNG + raw RGBA. Each planted bug is behind a
// --bug flag, so the baseline and the regression come from the same binary.
//
// Threads: the main thread owns the frame loop, transforms and binning. W persistent workers rasterize tiles;
// worker w owns tiles where (tile % W == w), so tile buffers and framebuffer regions are never shared. A barrier
// hands each binned view to the workers and collects them when every tile is done. The asset loader thread owns
// asset reads and publishes finished assets under asset_lock.
//
// Memory: everything comes from arenas reserved once at start-up and sized from the setup (plan_arenas below).
// The main arena holds the scene, views, textures, the asset buffer, worker tiles, trace and writer buffers and the
// per-frame scratch arena. The PNG arena serves stb_image_write once at exit and is then released. cache_leak adds
// a 2 GiB texture-copy arena. An exhausted arena writes a fatal log line and ends the process with a non-zero
// status; nothing falls back to the heap.

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fixed.hpp"
#include "output.hpp"
#include "raster.hpp"
#include "texture.hpp"

using romeo::Arena;
using romeo::ArenaScope;
using romeo::FixedArray;
using romeo::FixedStringBuilder;

namespace {

// ---------------------------------------------------------------- setups and bugs

struct Setup {
    char name;
    int views, width, height, workers;
};

constexpr Setup kSetups[] = {
    {'A', 1, 1920, 1080, 4},
    {'B', 4, 1920, 1080, 8},
    {'C', 1, 3840, 2160, 8},
    {'D', 8, 2560, 1440, 12},
    {'E', 12, 1920, 1080, 16},
};

constexpr int max_workers() {
    int m = 0;
    for (const Setup &s : kSetups) m = s.workers > m ? s.workers : m;
    return m;
}
constexpr int kMaxWorkers = max_workers();
constexpr int kTraceBuffers = kMaxWorkers + 2; // [0] main, [1] asset loader, [2..] workers

struct Bugs {
    bool cache_leak = false;    // texture cache never evicts streamed variants
    bool loader_lock = false;   // asset loader reads and parses files while holding asset_lock
    bool tile_size = false;     // 512-pixel tiles instead of 64
    bool view_setup = false;    // per-view setup (mip rebuild) runs every frame instead of once
    bool view_overflow = false; // view state array sized for 8 views
    bool disk_leak = false;     // per-view intermediate frames are never deleted
    bool mip_bias = false;      // texture LOD bias changed from 0 to +2.5
};

constexpr std::string_view kUsage = "usage: romeo-render --setup A..E [--frames N] [--bug a,b] [--trace] [--out dir]\n";

constexpr size_t kAssetBytes = 64u << 20;            // the streamed asset file
constexpr size_t kTextureCopyArenaBytes = 2ull << 30; // cache_leak copies, reserved address space
constexpr size_t kBlockAlign = 128;                   // large buffers start on their own cache line
constexpr size_t kWarmup = 40; // first frames include thread start-up and first-touch page faults

// Leaves room for "/tmp/viewVV-frameFFFFFFFFFF.rgba" and the other file names below the platform path limit.
constexpr size_t kMaxOutDir = PATH_MAX - 64;

// ---------------------------------------------------------------- arena plan

// Trace capacity per thread. The main thread records one frame span and at most one wait_asset_lock span per
// frame, and each worker one raster_tiles span per view per frame, so neither buffer can fill. The loader records
// one span per 3 s load; `frames` covers that whenever frames average under 3 s, beyond that loads are dropped
// and counted.
struct TracePlan {
    size_t main_events, loader_events, worker_events;
};

TracePlan plan_trace(const Setup &s, int frames, bool trace) {
    if (!trace) return {0, 0, 0};
    return {2 * size_t(frames), size_t(frames), size_t(s.views) * size_t(frames)};
}

// Every byte the run allocates, derived from the setup. Each term matches one allocation in main() or in the
// init function it names; a mismatch shows up as a fatal arena_exhausted line at start-up, never as heap use.
struct ArenaPlan {
    size_t main_bytes;    // one reservation for everything below except the PNG and texture-copy arenas
    size_t scratch_bytes; // per-frame scratch carved from the main arena: thumbnails, and the sort at exit
    size_t png_bytes;     // stb_image_write working set for image.png
    size_t copy_bytes;    // cache_leak texture copies
};

ArenaPlan plan_arenas(const Setup &s, const Bugs &bugs, int frames, bool trace) {
    const int tile = bugs.tile_size ? 512 : 64;
    const TracePlan tp = plan_trace(s, frames, trace);

    ArenaPlan p{};
    p.scratch_bytes = std::max(Arena::budget_bytes(thumbnail_bytes(s.width, s.height), alignof(uint32_t)), Arena::budget<double>(size_t(frames)));
    p.main_bytes = raster_arena_bytes(s.width, s.height, tile, s.views)       // scene, framebuffers, triangles, bins
                   + texture_arena_bytes(s.views)                             // 80 textures with mips, leak slots
                   + Arena::budget_bytes(kAssetBytes, kBlockAlign)            // asset generation and loader reads
                   + size_t(s.workers) * worker_tile_bytes(tile)              // per-worker depth and colour tile
                   + Arena::budget<double>(size_t(frames))                    // frame times for the summary
                   + Arena::budget<TraceEvent>(tp.main_events) + Arena::budget<TraceEvent>(tp.loader_events) +
                   size_t(s.workers) * Arena::budget<TraceEvent>(tp.worker_events)
                   + 3 * Arena::budget_bytes(kWriterBufferBytes)              // frames.csv, memory.csv, trace.json
                   + Arena::budget_bytes(p.scratch_bytes, kBlockAlign);
    p.png_bytes = png_scratch_bytes(s.width, s.height);
    p.copy_bytes = bugs.cache_leak ? kTextureCopyArenaBytes : 0;
    return p;
}

// Zeroed, lazily committed address space: pages count as resident only once touched.
Arena reserve_arena(size_t bytes) {
    if (bytes == 0) return Arena{};
#if defined(MAP_NORESERVE)
    constexpr int kFlags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#else
    constexpr int kFlags = MAP_PRIVATE | MAP_ANON;
#endif
    void *p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, kFlags, -1, 0);
    return p == MAP_FAILED ? Arena{} : Arena(p, bytes);
}

void release_arena(Arena &a) {
    if (a.base) ::munmap(a.base, a.capacity);
    a = Arena{};
}

// ---------------------------------------------------------------- fatal exits

void write_stderr(std::string_view text) { (void)!::write(STDERR_FILENO, text.data(), text.size()); }

// Start-up failure (before any thread exists): fatal log line, stderr line, exit status 1.
[[noreturn]] void fatal_setup(const Log &log, std::string_view msg, std::string_view extra) {
    log.line("fatal", msg, extra);
    FixedStringBuilder<256> e;
    e.add("fatal: ").add(msg).add('\n');
    write_stderr(e.view());
    std::_Exit(1);
}

[[noreturn]] void fatal_arena(const Log &log, std::string_view name, const Arena &a, size_t planned) {
    FixedStringBuilder<256> x;
    x.add("\"arena\":").str(name).add(",\"used_bytes\":").num(a.used).add(",\"capacity_bytes\":").num(a.capacity);
    x.add(",\"planned_bytes\":").num(planned);
    fatal_setup(log, a.base ? "arena_exhausted" : "arena_reserve_failed", x.view());
}

// ---------------------------------------------------------------- asset loader thread

struct AssetLoader {
    const char *path = nullptr;
    uint8_t *buffer = nullptr; // kAssetBytes, owned by the loader thread once it starts
    std::mutex *asset_lock = nullptr;
    uint64_t *published_asset = nullptr; // guarded by asset_lock
    const std::atomic<bool> *stop = nullptr;
    TraceBuffer *trace = nullptr;        // owned by the loader thread
    Clock::time_point t0;
    bool tracing = false;
    bool hold_lock_while_reading = false; // loader_lock bug
};

uint64_t read_and_parse(const AssetLoader &a) {
    size_t n = 0;
    int fd = ::open(a.path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        while (n < kAssetBytes) {
            ssize_t r = ::read(fd, a.buffer + n, kAssetBytes - n);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            n += size_t(r);
        }
        ::close(fd);
    }
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ a.buffer[i]) * 1099511628211ull;
    return h;
}

// Every ~3 s: read and parse the 64 MiB asset file and publish its checksum.
void asset_loader_main(AssetLoader *a) {
    auto next = Clock::now() + std::chrono::milliseconds(3000);
    while (!a->stop->load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (Clock::now() < next) continue;
        next += std::chrono::milliseconds(3000);
        double t = ms_since(a->t0);
        if (a->hold_lock_while_reading) {
            std::lock_guard g(*a->asset_lock);
            *a->published_asset = read_and_parse(*a);
        } else {
            uint64_t h = read_and_parse(*a);
            std::lock_guard g(*a->asset_lock);
            *a->published_asset = h;
        }
        if (a->tracing) a->trace->span(TraceName::asset_read_parse, t, ms_since(a->t0) - t);
    }
}

// ---------------------------------------------------------------- worker threads

struct WorkerContext {
    RasterState *raster = nullptr; // bins and triangles read-only while the worker runs; own tiles written
    WorkerTile tile;               // owned by this worker
    std::barrier<> *sync = nullptr;
    const std::atomic<bool> *stop = nullptr;
    const int *frame_view = nullptr; // written by the main thread before the start barrier
    TraceBuffer *trace = nullptr;    // owned by this worker
    Clock::time_point t0;
    bool tracing = false;
    int index = 0, count = 0;
};

// The first barrier releases all workers for one view, the second collects them when every tile is done.
void worker_main(WorkerContext *w) {
    for (;;) {
        w->sync->arrive_and_wait();
        if (w->stop->load(std::memory_order_relaxed)) return;
        double t = ms_since(w->t0);
        raster_tiles(*w->raster, *w->frame_view, w->tile, w->index, w->count);
        if (w->tracing) w->trace->span(TraceName::raster_tiles, t, ms_since(w->t0) - t);
        w->sync->arrive_and_wait();
    }
}

} // namespace

// ---------------------------------------------------------------- main

int main(int argc, char **argv) {
    // Command line.
    char setup_name = 'A';
    int frames = 240;
    bool trace = false;
    Bugs bugs;
    std::string_view out = "runs/local";
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
            write_stderr(kUsage);
            return 2;
        }
    }
    const Setup *setup = nullptr;
    for (const Setup &s : kSetups)
        if (s.name == setup_name) setup = &s;
    if (!setup) {
        FixedStringBuilder<32> e;
        e.add("unknown setup ").add(setup_name).add('\n');
        write_stderr(e.view());
        return 2;
    }
    if (out.size() > kMaxOutDir) {
        write_stderr("output path too long\n");
        return 2;
    }

    // Output directory and structured log.
    if (!make_directories(Path().add(out).data)) {
        write_stderr("cannot create output directory\n");
        return 1;
    }
    const Log log = open_log(join_path(out, "log.jsonl").data);
    {
        FixedStringBuilder<256> x;
        x.add("\"setup\":\"").add(setup->name).add("\",\"views\":").num(setup->views).add(",\"width\":").num(setup->width);
        x.add(",\"height\":").num(setup->height).add(",\"workers\":").num(setup->workers).add(",\"frames\":").num(frames);
        log.line("info", "run_start", x.view());
    }

    // The fixed view table. The view_overflow bug restores the old 8-view limit.
    const int view_limit = bugs.view_overflow ? 8 : kMaxViews;
    if (setup->views > view_limit) {
        FixedStringBuilder<256> x;
        x.add("\"check\":\"view_count <= kMaxViews\",\"view_count\":").num(setup->views).add(",\"kMaxViews\":").num(view_limit);
        x.add(",\"file\":\"renderer/main.cpp\",\"function\":\"init_views\"");
        log.line("fatal", "assertion_failed", x.view());
        FixedStringBuilder<128> e;
        e.add("assertion failed: view_count <= kMaxViews (").num(setup->views).add(" > ").num(view_limit).add(") in init_views\n");
        write_stderr(e.view());
        std::abort();
    }

    const int W = setup->width, H = setup->height, V = setup->views, workers = setup->workers;
    const int tile = bugs.tile_size ? 512 : 64;
    const float lod_bias = bugs.mip_bias ? 2.5f : 0.0f;

    // Reserve every arena. All allocation happens in this block and the next ones, before any thread starts.
    const ArenaPlan plan = plan_arenas(*setup, bugs, frames, trace);
    Arena arena = reserve_arena(plan.main_bytes);
    if (!arena.base) fatal_arena(log, "main", arena, plan.main_bytes);
    Arena png_arena = reserve_arena(plan.png_bytes);
    if (!png_arena.base) fatal_arena(log, "png", png_arena, plan.png_bytes);
    Arena copy_arena = reserve_arena(plan.copy_bytes);
    if (plan.copy_bytes && !copy_arena.base) fatal_arena(log, "texture_copies", copy_arena, plan.copy_bytes);

    // Scene, per-view state and textures.
    RasterState raster;
    if (!init_raster(raster, arena, W, H, tile, lod_bias, V)) fatal_arena(log, "main", arena, plan.main_bytes);
    TextureCache textures;
    if (!init_textures(textures, arena, V, bugs.cache_leak, copy_arena)) fatal_arena(log, "main", arena, plan.main_bytes);

    // Run bookkeeping: frame times, per-thread trace buffers, writer buffers and the per-frame scratch arena.
    double *frame_ms = arena.alloc<double>(size_t(frames));
    const TracePlan tp = plan_trace(*setup, frames, trace);
    TraceBuffer traces[kTraceBuffers];
    const int trace_count = workers + 2;
    for (int i = 0; i < trace_count; ++i) {
        const size_t capacity = i == 0 ? tp.main_events : i == 1 ? tp.loader_events : tp.worker_events;
        traces[i].events = arena.alloc<TraceEvent>(capacity);
        traces[i].capacity = traces[i].events ? capacity : 0;
    }
    uint8_t *writer_buffers[3];
    for (uint8_t *&b : writer_buffers) b = static_cast<uint8_t *>(arena.alloc_bytes(kWriterBufferBytes));
    Arena scratch(arena.alloc_bytes(plan.scratch_bytes, kBlockAlign), plan.scratch_bytes);
    uint8_t *asset = static_cast<uint8_t *>(arena.alloc_bytes(kAssetBytes, kBlockAlign));
    if (!frame_ms || !writer_buffers[0] || !writer_buffers[1] || !writer_buffers[2] || !scratch.base || !asset)
        fatal_arena(log, "main", arena, plan.main_bytes);

    FixedArray<WorkerContext, kMaxWorkers> worker_ctx;
    for (int w = 0; w < workers; ++w) {
        WorkerContext *c = worker_ctx.push();
        if (!c || !init_worker_tile(c->tile, arena, tile)) fatal_arena(log, "main", arena, plan.main_bytes);
    }

    // Output files that stay open through the frame loop.
    const Path tmp_dir = join_path(out, "tmp");
    if (!make_directories(tmp_dir.data)) fatal_setup(log, "output_open_failed", "\"file\":\"tmp\"");
    FileWriter frames_csv, memory_csv;
    if (!frames_csv.open(join_path(out, "frames.csv").data, writer_buffers[0], kWriterBufferBytes))
        fatal_setup(log, "output_open_failed", "\"file\":\"frames.csv\"");
    if (!memory_csv.open(join_path(out, "memory.csv").data, writer_buffers[1], kWriterBufferBytes))
        fatal_setup(log, "output_open_failed", "\"file\":\"memory.csv\"");
    frames_csv.add("frame,ms\n");
    memory_csv.add("t_ms,resident_bytes\n");

    // Asset loader: every ~3 s it reads and parses a 64 MiB asset file and publishes the checksum.
    const Path asset_path = join_path(out, "asset.bin");
    for (size_t i = 0; i < kAssetBytes; ++i) asset[i] = uint8_t(i * 2654435761u >> 24);
    if (!write_file(asset_path.data, asset, kAssetBytes)) fatal_setup(log, "output_open_failed", "\"file\":\"asset.bin\"");
    std::mutex asset_lock;
    uint64_t published_asset = 0;
    std::atomic<bool> stop{false};
    AssetLoader loader_ctx;
    loader_ctx.path = asset_path.data;
    loader_ctx.buffer = asset;
    loader_ctx.asset_lock = &asset_lock;
    loader_ctx.published_asset = &published_asset;
    loader_ctx.stop = &stop;
    loader_ctx.trace = &traces[1];
    loader_ctx.t0 = log.t0;
    loader_ctx.tracing = trace;
    loader_ctx.hold_lock_while_reading = bugs.loader_lock;
    std::thread loader(asset_loader_main, &loader_ctx);

    // Worker pool.
    int frame_view = 0;
    std::barrier<> sync(workers + 1);
    FixedArray<std::thread, kMaxWorkers> pool;
    for (int w = 0; w < workers; ++w) {
        WorkerContext &c = worker_ctx[uint32_t(w)];
        c.raster = &raster;
        c.sync = &sync;
        c.stop = &stop;
        c.frame_view = &frame_view;
        c.trace = &traces[w + 2];
        c.t0 = log.t0;
        c.tracing = trace;
        c.index = w;
        c.count = workers;
        *pool.push() = std::thread(worker_main, &c);
    }

    // Frame loop.
    for (int f = 0; f < frames; ++f) {
        auto t0 = Clock::now();
        double tf = ms_since(log.t0);

        // Pick up newly published assets; with loader_lock this waits for the whole file read.
        {
            double tw = ms_since(log.t0);
            std::lock_guard g(asset_lock);
            (void)published_asset;
            double waited = ms_since(log.t0) - tw;
            if (trace && waited > 0.5) traces[0].span(TraceName::wait_asset_lock, tw, waited);
        }

        for (int v = 0; v < V; ++v) {
            if (bugs.view_setup)
                for (Texture &t : textures.base) build_mips(t); // should run once at load, not per view per frame

            if (!bin_view(raster, textures, v, f)) {
                // cache_leak ran the texture-copy arena dry: the realistic end of an unbounded leak.
                FixedStringBuilder<512> x;
                x.add("\"arena\":\"texture_copies\",\"used_bytes\":").num(textures.copies.used);
                x.add(",\"capacity_bytes\":").num(textures.copies.capacity).add(",\"request_bytes\":").num(TextureCache::copy_bytes());
                x.add(",\"cache_entries\":").num(textures.entries()).add(",\"frame\":").num(f).add(",\"view\":").num(v);
                log.line("fatal", "texture_arena_exhausted", x.view());
                frames_csv.close(); // keep the frame and memory rows measured up to this point
                memory_csv.close();
                ::unlink(asset_path.data);
                FixedStringBuilder<160> e;
                e.add("fatal: texture_arena_exhausted (").num(textures.copies.used).add(" of ").num(textures.copies.capacity).add(" bytes used)\n");
                write_stderr(e.view());
                std::abort();
            }

            frame_view = v;
            sync.arrive_and_wait(); // start workers on this view
            sync.arrive_and_wait(); // wait for all tiles

            // Per-view intermediate thumbnail for the compositor. The fix overwrites one file per view.
            const Path thumb = thumbnail_path(tmp_dir.view(), v, f, bugs.disk_leak);
            write_thumbnail(thumb.data, raster.views[uint32_t(v)].color, W, H, scratch);
        }

        double ms = ms_since(t0);
        frame_ms[f] = ms;
        add_frame_row(frames_csv, f, ms);
        if (f % 5 == 0) add_memory_row(memory_csv, ms_since(log.t0), resident_bytes());
        if (trace) traces[0].span(TraceName::frame, tf, ms);
    }

    // Shut down the threads; after the joins the main thread owns every buffer again.
    stop.store(true);
    sync.arrive_and_wait();
    for (std::thread &t : pool) t.join();
    loader.join();
    frames_csv.close();
    memory_csv.close();

    // Last frame of view 0: PNG for people, raw RGBA for exact comparison. The PNG arena is used once, so its
    // pages go back before resident memory is measured.
    const uint32_t *view0 = raster.views[0].color;
    write_png(join_path(out, "image.png").data, view0, W, H, png_arena);
    release_arena(png_arena);
    write_file(join_path(out, "image.rgba").data, view0, size_t(W) * size_t(H) * sizeof(uint32_t));

    if (trace) {
        write_trace(join_path(out, "trace.json").data, traces, uint32_t(trace_count), writer_buffers[2], kWriterBufferBytes);
        size_t dropped = 0;
        for (int i = 0; i < trace_count; ++i) dropped += traces[i].dropped;
        if (dropped > 0) {
            FixedStringBuilder<64> x;
            x.add("\"dropped\":").num(dropped);
            log.line("warn", "trace_events_dropped", x.view());
        }
    }

    // Summary. The first kWarmup frames include thread start-up and first-touch page faults; statistics skip them.
    const uint64_t tmp_bytes = directory_bytes(tmp_dir.data);
    {
        ArenaScope scope(scratch);
        const size_t skip = std::min(kWarmup, size_t(frames) - 1);
        const size_t n = size_t(frames) - skip;
        double *sorted = static_cast<double *>(scratch.alloc_bytes(n * sizeof(double), alignof(double)));
        std::memcpy(sorted, frame_ms + skip, n * sizeof(double));
        std::sort(sorted, sorted + n);
        auto pct = [&](double p) { return sorted[std::min(n - 1, size_t(p * double(n)))]; };
        int spikes = 0;
        for (size_t i = 0; i < n; ++i) spikes += sorted[i] > 2.0 * pct(0.5);

        RunSummary s{};
        s.setup = setup->name;
        s.views = V;
        s.width = W;
        s.height = H;
        s.workers = workers;
        s.frames = frames;
        s.tile = tile;
        s.p50_ms = pct(0.5);
        s.p95_ms = pct(0.95);
        s.p99_ms = pct(0.99);
        s.max_ms = sorted[n - 1];
        s.spike_frames = spikes;
        s.resident_end_bytes = resident_bytes();
        s.cache_entries = textures.entries();
        s.tmp_bytes = tmp_bytes;
        s.total_ms = ms_since(log.t0);
        write_summary(join_path(out, "summary.json").data, s);
    }
    log.line("info", "run_end");
    ::unlink(asset_path.data);
    return 0;
}
