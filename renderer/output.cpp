#include "output.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

using romeo::Arena;
using romeo::ArenaScope;
using romeo::FixedStringBuilder;

// ---------------------------------------------------------------- stb_image_write, bound to a bump arena

namespace {

// Set by write_png() for the duration of one stbi_write_png_to_func() call on the main thread.
Arena *g_png_arena = nullptr;

void *png_alloc(size_t bytes) { return g_png_arena->alloc_bytes(bytes); }

// Bump realloc: a new block plus a copy. The old block is released with the whole scope.
void *png_realloc(void *p, size_t old_bytes, size_t new_bytes) {
    void *q = png_alloc(new_bytes);
    if (q && p) std::memcpy(q, p, std::min(old_bytes, new_bytes));
    return q;
}

} // namespace

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STBIW_MALLOC(bytes) png_alloc(bytes)
#define STBIW_REALLOC_SIZED(p, old_bytes, new_bytes) png_realloc(p, old_bytes, new_bytes)
#define STBIW_FREE(p) ((void)(p))
#define STBIW_CRC32(buffer, len) romeo::crc32(buffer, size_t(len))
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

int open_for_write(const char *path) { return ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666); }

bool write_all(int fd, const void *data, size_t bytes) {
    auto *p = static_cast<const uint8_t *>(data);
    while (bytes > 0) {
        ssize_t n = ::write(fd, p, bytes);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        bytes -= size_t(n);
    }
    return true;
}

// printf("%0*d") for non-negative values.
template <uint32_t N>
void add_padded(FixedStringBuilder<N> &b, int value, int width) {
    int digits = 1;
    for (int v = value; v >= 10; v /= 10) ++digits;
    for (int i = digits; i < width; ++i) b.add('0');
    b.num(value);
}

} // namespace

// ---------------------------------------------------------------- files

bool FileWriter::open(const char *path, uint8_t *buf, size_t cap) {
    fd = open_for_write(path);
    buffer = buf;
    capacity = cap;
    used = 0;
    failed = fd < 0;
    return fd >= 0;
}

void FileWriter::flush() {
    if (used > 0 && fd >= 0 && !write_all(fd, buffer, used)) failed = true;
    used = 0;
}

void FileWriter::add(std::string_view text) {
    if (text.size() > capacity - used) flush();
    if (text.size() > capacity) {
        if (fd >= 0 && !write_all(fd, text.data(), text.size())) failed = true;
        return;
    }
    std::memcpy(buffer + used, text.data(), text.size());
    used += text.size();
}

bool FileWriter::close() {
    flush();
    if (fd >= 0 && ::close(fd) != 0) failed = true;
    fd = -1;
    return !failed;
}

bool write_file(const char *path, const void *data, size_t bytes) {
    int fd = open_for_write(path);
    if (fd < 0) return false;
    bool ok = write_all(fd, data, bytes);
    return ::close(fd) == 0 && ok;
}

bool make_directories(const char *path) {
    Path p;
    p.add(path);
    if (p.overflowed() || p.len == 0) return false;
    // Create every prefix that ends before a '/', then the full path; existing directories are fine.
    for (uint32_t i = 1; i <= p.len; ++i) {
        if (i < p.len && p.data[i] != '/') continue;
        const char saved = p.data[i];
        p.data[i] = 0;
        int rc = ::mkdir(p.data, 0777);
        p.data[i] = saved;
        if (rc != 0 && errno != EEXIST) return false;
    }
    struct stat st;
    return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

uint64_t directory_bytes(const char *path) {
    // opendir() allocates its DIR inside libc; this runs once, after the frame loop.
    DIR *d = ::opendir(path);
    if (!d) return 0;
    uint64_t total = 0;
    while (dirent *e = ::readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        struct stat st;
        if (::fstatat(::dirfd(d), e->d_name, &st, 0) == 0 && S_ISREG(st.st_mode)) total += uint64_t(st.st_size);
    }
    ::closedir(d);
    return total;
}

uint64_t resident_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, task_info_t(&info), &count) == KERN_SUCCESS) return info.resident_size;
    return 0;
#else
    // /proc/self/statm: "size resident shared text lib data dt", in pages.
    int fd = ::open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char text[128];
    ssize_t n = ::read(fd, text, sizeof text - 1);
    ::close(fd);
    if (n <= 0) return 0;
    const char *p = text, *end = text + n;
    unsigned long pages = 0, resident = 0;
    auto r = std::from_chars(p, end, pages);
    if (r.ec != std::errc() || r.ptr == end) return 0;
    r = std::from_chars(r.ptr + 1, end, resident);
    if (r.ec != std::errc()) return 0;
    return uint64_t(resident) * uint64_t(sysconf(_SC_PAGESIZE));
#endif
}

// ---------------------------------------------------------------- log.jsonl

Log open_log(const char *path) { return Log{open_for_write(path), Clock::now()}; }

void Log::line(std::string_view level, std::string_view msg, std::string_view extra) const {
    if (fd < 0) return;
    FixedStringBuilder<1024> b;
    b.add("{\"t_ms\":").num(ms_since(t0), 3).add(",\"level\":\"").add(level).add("\",\"msg\":\"").add(msg).add('"');
    if (!extra.empty()) b.add(',').add(extra);
    b.add("}\n");
    write_all(fd, b.data, b.len);
}

// ---------------------------------------------------------------- trace.json

bool write_trace(const char *path, const TraceBuffer *buffers, uint32_t buffer_count, uint8_t *io_buffer, size_t io_bytes) {
    static constexpr std::string_view kNames[] = {"frame", "wait_asset_lock", "raster_tiles", "asset_read_parse"};
    FileWriter w;
    if (!w.open(path, io_buffer, io_bytes)) return false;
    w.add("{\"traceEvents\":[");
    bool first = true;
    for (uint32_t i = 0; i < buffer_count; ++i)
        for (size_t k = 0; k < buffers[i].count; ++k) {
            const TraceEvent &e = buffers[i].events[k];
            FixedStringBuilder<192> b;
            if (!first) b.add(",\n");
            b.add("{\"name\":\"").add(kNames[size_t(e.name)]).add("\",\"ph\":\"X\",\"pid\":1,\"tid\":").num(i + 1);
            b.add(",\"ts\":").num(e.start_ms * 1000.0, 1).add(",\"dur\":").num(e.dur_ms * 1000.0, 1).add('}');
            w.add(b.view());
            first = false;
        }
    w.add("]}\n");
    return w.close();
}

// ---------------------------------------------------------------- frames.csv and memory.csv

void add_frame_row(FileWriter &w, int frame, double ms) {
    FixedStringBuilder<64> b;
    b.num(frame).add(',').num(ms, 3).add('\n');
    w.add(b.view());
}

void add_memory_row(FileWriter &w, double t_ms, uint64_t resident) {
    FixedStringBuilder<64> b;
    b.num(t_ms, 1).add(',').num(resident).add('\n');
    w.add(b.view());
}

// ---------------------------------------------------------------- images

size_t thumbnail_bytes(int width, int height) { return size_t((width + 7) / 8) * size_t((height + 7) / 8) * sizeof(uint32_t); }

Path thumbnail_path(std::string_view tmp_dir, int view, int frame, bool per_frame) {
    Path p;
    p.add(tmp_dir).add("/view");
    add_padded(p, view, 2);
    if (per_frame) {
        p.add("-frame");
        add_padded(p, frame, 5);
    }
    p.add(".rgba");
    return p;
}

void write_thumbnail(const char *path, const uint32_t *color, int width, int height, Arena &scratch) {
    ArenaScope scope(scratch);
    auto *px = static_cast<uint32_t *>(scratch.alloc_bytes(thumbnail_bytes(width, height), alignof(uint32_t)));
    if (!px) return; // sized at start-up from the same thumbnail_bytes(); cannot happen
    size_t n = 0;
    for (int y = 0; y < height; y += 8)
        for (int x = 0; x < width; x += 8) px[n++] = color[size_t(y) * width + x];
    write_file(path, px, n * sizeof(uint32_t));
}

size_t png_scratch_bytes(int width, int height) {
    // Every allocation stbi_write_png_to_func() makes for one 4-channel image. Bump realloc never frees, so the
    // bound is the sum of all requested blocks, each padded to the 16-byte alloc_bytes() alignment.
    constexpr size_t kPad = alignof(std::max_align_t) - 1;
    constexpr size_t kHash = 16384;                         // stbiw__ZHASH chains
    const size_t filtered = (size_t(width) * 4 + 1) * size_t(height);

    // A hash chain holds at most 2 * quality = 16 pointers before stb halves it, so its stretchy buffer grows
    // through capacities 2, 5, 11 and 23, each block carrying an 8-byte header.
    const size_t chains = kHash * ((2 + 5 + 11 + 23) * sizeof(void *) + 4 * (8 + kPad));

    // The fixed-Huffman stream costs at most 9 bits per input byte, plus the 2-byte zlib header, the end code
    // and the 4-byte Adler-32. A stretchy buffer that ends with n bytes has capacity at most 2n + 1, and the
    // capacities it grew through sum to at most twice that; growth runs at most 40 times.
    const size_t stream = filtered + filtered / 8 + 16;
    const size_t stream_blocks = 2 * (2 * stream + 1) + 40 * (8 + kPad);

    return Arena::budget_bytes(filtered) + Arena::budget_bytes(size_t(width) * 4) + Arena::budget_bytes(kHash * sizeof(void *)) +
           chains + stream_blocks + Arena::budget_bytes(stream + 57);
}

bool write_png(const char *path, const uint32_t *rgba, int width, int height, Arena &scratch) {
    int fd = open_for_write(path);
    if (fd < 0) return false;
    struct Sink { int fd; bool ok; } sink{fd, true};
    auto on_bytes = [](void *context, void *data, int size) {
        auto *s = static_cast<Sink *>(context);
        s->ok = s->ok && write_all(s->fd, data, size_t(size));
    };
    ArenaScope scope(scratch);
    g_png_arena = &scratch;
    int ok = stbi_write_png_to_func(on_bytes, &sink, width, height, 4, rgba, width * 4);
    g_png_arena = nullptr;
    return ::close(fd) == 0 && ok != 0 && sink.ok;
}

// ---------------------------------------------------------------- summary.json

bool write_summary(const char *path, const RunSummary &s) {
    FixedStringBuilder<1024> b;
    b.add("{\"setup\":\"").add(s.setup).add("\",\"views\":").num(s.views).add(",\"width\":").num(s.width);
    b.add(",\"height\":").num(s.height).add(",\"workers\":").num(s.workers).add(",\"frames\":").num(s.frames);
    b.add(",\"tile\":").num(s.tile).add(",\"p50_ms\":").num(s.p50_ms, 3).add(",\"p95_ms\":").num(s.p95_ms, 3);
    b.add(",\"p99_ms\":").num(s.p99_ms, 3).add(",\"max_ms\":").num(s.max_ms, 3).add(",\"spike_frames\":").num(s.spike_frames);
    b.add(",\"resident_end_bytes\":").num(s.resident_end_bytes).add(",\"cache_entries\":").num(s.cache_entries);
    b.add(",\"tmp_bytes\":").num(s.tmp_bytes).add(",\"total_ms\":").num(s.total_ms, 1).add("}\n");
    return !b.overflowed() && write_file(path, b.data, b.len);
}
