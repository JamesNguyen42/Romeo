// Run outputs: the one place that formats text and writes files.
//
// Files are written with open()/write()/close(). Text is formatted with FixedStringBuilder into fixed storage;
// FileWriter batches it into a caller-provided buffer. Formats are fixed because the case service parses them:
// log.jsonl, frames.csv, memory.csv, trace.json (Chrome trace), summary.json, image.png, image.rgba and the
// tmp/ thumbnails.
#pragma once

#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "fixed.hpp"

using Clock = std::chrono::steady_clock;

inline double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// File paths are built in fixed storage of the platform's path limit.
using Path = romeo::FixedStringBuilder<PATH_MAX>;

inline Path join_path(std::string_view dir, std::string_view name) {
    Path p;
    p.add(dir).add('/').add(name);
    return p;
}

// ---------------------------------------------------------------- files

// Buffered writer over a caller-owned buffer. The buffer size only sets how many bytes one write() carries; a
// failed write is remembered and reported by close().
struct FileWriter {
    int fd = -1;
    uint8_t *buffer = nullptr;
    size_t capacity = 0, used = 0;
    bool failed = false;

    bool open(const char *path, uint8_t *buffer, size_t capacity);
    void add(std::string_view text);
    void flush();
    bool close();
};

// One FileWriter buffer. 16 KiB carries over a thousand frames.csv rows per write(); the size only changes how
// often write() runs, never what is written.
constexpr size_t kWriterBufferBytes = 16 << 10;

// Writes `bytes` to a new or truncated file. Returns false if the file cannot be opened or fully written.
bool write_file(const char *path, const void *data, size_t bytes);

// mkdir -p. Returns false if a component cannot be created.
bool make_directories(const char *path);

// Sum of regular-file sizes in a directory (0 if it cannot be opened).
uint64_t directory_bytes(const char *path);

// Resident set size of this process, in bytes (0 if the platform query fails).
uint64_t resident_bytes();

// ---------------------------------------------------------------- log.jsonl

// Structured log: one JSON object per line, each line one write() so a crash keeps every line written before it.
// Used by the main thread only.
struct Log {
    int fd = -1;
    Clock::time_point t0;

    // `extra` is a pre-formatted list of JSON members without braces ("" for none).
    void line(std::string_view level, std::string_view msg, std::string_view extra = {}) const;
};

Log open_log(const char *path);

// ---------------------------------------------------------------- trace.json

enum class TraceName : uint8_t { frame, wait_asset_lock, raster_tiles, asset_read_parse };

struct TraceEvent {
    double start_ms, dur_ms;
    TraceName name;
};

// One thread's fixed-capacity event buffer; written only by its owning thread, read by the main thread after
// that thread has been joined. A full buffer drops the event and counts it.
struct TraceBuffer {
    TraceEvent *events = nullptr;
    size_t count = 0, capacity = 0, dropped = 0;

    void span(TraceName name, double start_ms, double dur_ms) {
        if (count == capacity) { ++dropped; return; }
        events[count++] = {start_ms, dur_ms, name};
    }
};

// Buffer i is written with Chrome-trace tid i + 1 (main 1, asset loader 2, worker w at w + 3).
bool write_trace(const char *path, const TraceBuffer *buffers, uint32_t buffer_count, uint8_t *io_buffer, size_t io_bytes);

// ---------------------------------------------------------------- frames.csv and memory.csv

void add_frame_row(FileWriter &w, int frame, double ms);
void add_memory_row(FileWriter &w, double t_ms, uint64_t resident);

// ---------------------------------------------------------------- images

// Per-view thumbnail for the compositor: every 8th pixel of every 8th row, raw RGBA8. Gathered in `scratch`
// (rewound before returning) and written with one write().
size_t thumbnail_bytes(int width, int height);
// tmp_dir/viewVV.rgba, or tmp_dir/viewVV-frameFFFFF.rgba when every frame keeps its own file.
Path thumbnail_path(std::string_view tmp_dir, int view, int frame, bool per_frame);
void write_thumbnail(const char *path, const uint32_t *color, int width, int height, romeo::Arena &scratch);

// PNG through stb_image_write; every stb allocation is bump-allocated from `scratch`, which needs
// png_scratch_bytes() of space.
size_t png_scratch_bytes(int width, int height);
bool write_png(const char *path, const uint32_t *rgba, int width, int height, romeo::Arena &scratch);

// ---------------------------------------------------------------- summary.json

struct RunSummary {
    char setup;
    int views, width, height, workers, frames, tile;
    double p50_ms, p95_ms, p99_ms, max_ms;
    int spike_frames;
    uint64_t resident_end_bytes;
    size_t cache_entries;
    uint64_t tmp_bytes;
    double total_ms;
};

bool write_summary(const char *path, const RunSummary &s);
