#include "evidence.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.hpp"

// ---------------------------------------------------------------- stb_image_write over the evidence arena
//
// stb allocates its filter rows, zlib hash chains and output through these hooks. Each block carries a 16-byte size
// header so realloc can copy. A block at the top of the arena grows in place, which is the common case for the zlib
// output buffer once the hash chains stop growing. Free is a no-op: the analysis rewinds the arena afterwards.

namespace {
romeo::Arena *g_png_arena = nullptr;

void *png_alloc(size_t n) {
    if (!g_png_arena) return nullptr;
    auto *p = static_cast<uint8_t *>(g_png_arena->alloc_bytes(n + 16, 16));
    if (!p) return nullptr;
    std::memcpy(p, &n, sizeof n);
    return p + 16;
}

void *png_realloc(void *ptr, size_t n) {
    if (!ptr) return png_alloc(n);
    auto *p = static_cast<uint8_t *>(ptr);
    size_t old = 0;
    std::memcpy(&old, p - 16, sizeof old);
    romeo::Arena &a = *g_png_arena;
    if (p + old == a.base + a.used && size_t(p - a.base) + n <= a.capacity) {
        a.used = size_t(p - a.base) + n;
        if (a.used > a.high_water) a.high_water = a.used;
        std::memcpy(p - 16, &n, sizeof n);
        return p;
    }
    void *q = png_alloc(n);
    if (q) std::memcpy(q, p, old < n ? old : n);
    return q;
}

void png_free(void *) {}
} // namespace

#define STBIW_MALLOC(sz) png_alloc(sz)
#define STBIW_REALLOC(p, newsz) png_realloc(p, newsz)
#define STBIW_FREE(p) png_free(p)
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "stb_image_write.h"
#pragma clang diagnostic pop

namespace romeo {

// ---------------------------------------------------------------- files

FileData read_file(Arena &arena, const char *path, size_t limit) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {ReadStatus::missing, {}};
    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return {ReadStatus::missing, {}};
    }
    size_t want = std::min(size_t(st.st_size), limit);
    char *buf = static_cast<char *>(arena.alloc_bytes(want + 1, 16));
    if (!buf) {
        close(fd);
        return {ReadStatus::too_large, {}};
    }
    size_t got = 0;
    while (got < want) {
        ssize_t n = read(fd, buf + got, want - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += size_t(n);
    }
    close(fd);
    buf[got] = 0;
    return {ReadStatus::ok, std::string_view(buf, got)};
}

bool file_exists(const char *path) {
    struct stat st {};
    return stat(path, &st) == 0;
}

bool regular_file_size(const char *path, uint64_t &size) {
    struct stat st {};
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    size = uint64_t(st.st_size);
    return true;
}

bool make_dirs(std::string_view path) {
    FixedString<kPathBytes> p;
    if (!p.assign(path)) return false;
    for (uint32_t i = 1; i <= p.len; ++i) {
        if (i != p.len && p.data[i] != '/') continue;
        char saved = p.data[i];
        p.data[i] = 0;
        if (mkdir(p.data, 0755) != 0 && errno != EEXIST) {
            p.data[i] = saved;
            return false;
        }
        p.data[i] = saved;
    }
    return true;
}

static bool write_all(int fd, const char *data, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, data + done, n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        done += size_t(w);
    }
    return true;
}

bool write_file(const char *path, std::string_view bytes, bool sync) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = write_all(fd, bytes.data(), bytes.size());
    if (ok && sync) ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

bool copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        close(in);
        return false;
    }
    char buf[64 << 10];
    bool ok = true;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) ok = false;
        if (n <= 0) break;
        if (!write_all(out, buf, size_t(n))) {
            ok = false;
            break;
        }
    }
    close(in);
    close(out);
    return ok;
}

int count_dir_entries(const char *path) {
    // opendir allocates its DIR inside libc. check_disk runs once per case, outside any steady-state loop.
    DIR *d = opendir(path);
    if (!d) return 0;
    int n = 0;
    while (dirent *e = readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        ++n;
    }
    closedir(d);
    return n;
}

yyjson_doc *parse_json(Arena &arena, std::string_view text) {
    if (text.empty()) return nullptr;
    size_t want = yyjson_read_max_memory_usage(text.size(), 0);
    size_t start = (arena.used + 15) & ~size_t(15);
    size_t avail = start < arena.capacity ? arena.capacity - start : 0;
    size_t size = want && want < avail ? want : avail;
    void *mem = size >= 256 ? arena.alloc_bytes(size, 16) : nullptr;
    if (!mem) {
        RLOG_WARN("json: {} bytes of input do not fit the scratch arena; refused", text.size());
        return nullptr;
    }
    yyjson_alc alc;
    yyjson_alc_pool_init(&alc, mem, size);
    yyjson_read_err err{};
    yyjson_doc *doc = yyjson_read_opts(const_cast<char *>(text.data()), text.size(), 0, &alc, &err);
    if (!doc && err.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION)
        RLOG_WARN("json: {} bytes of input exceed the scratch arena pool ({} bytes); refused", text.size(), size);
    return doc;
}

// ---------------------------------------------------------------- run files

Summary read_summary(Service &s, std::string_view dir) {
    Summary out;
    ArenaScope scope(s.evidence);
    PathText p;
    cat(p, dir, "/summary.json");
    FileData f = read_file(s.evidence, p.c_str());
    yyjson_doc *doc = f.status == ReadStatus::ok ? parse_json(s.evidence, f.bytes) : nullptr;
    if (!doc) return out;
    yyjson_val *r = yyjson_doc_get_root(doc);
    auto n = [&](const char *k) { return yyjson_get_num(yyjson_obj_get(r, k)); };
    out = {n("p50_ms"), n("p95_ms"), n("p99_ms"), n("max_ms"), n("total_ms"), int(n("spike_frames")), int(n("views")), int(n("frames")),
           n("resident_end_bytes") / 1048576.0, n("tmp_bytes") / 1048576.0, size_t(n("cache_entries")), true};
    return out;
}

int read_exit_code(Service &s, std::string_view dir) {
    ArenaScope scope(s.evidence);
    PathText p;
    cat(p, dir, "/exit");
    FileData f = read_file(s.evidence, p.c_str());
    return f.status == ReadStatus::ok ? int(parse_leading_int(f.bytes)) : 0;
}

void upload_run_log(Service &s, const Case &c, std::string_view action_id, std::string_view dir) {
    PathText store, parent, from;
    cat(store, s.cfg.data, "/store/");
    store_key(store, c, action_id);
    std::string_view sv = store.view();
    parent.add(sv.substr(0, sv.rfind('/')));
    cat(from, dir, "/log.jsonl");
    if (store.overflowed() || from.overflowed()) return;
    make_dirs(parent.view());
    copy_file(from.c_str(), store.c_str());
}

// One CSV column, skipping the header line. Field selection follows std::getline(stream, cell, ','): a line with
// fewer fields yields its last field, a trailing comma adds no field, and an empty line yields 0.
struct Column {
    double *values = nullptr;
    uint32_t count = 0;
    bool refused = false;
    double back() const { return values[count - 1]; }
};

static Column csv_column(Arena &arena, std::string_view path_dir, std::string_view file, int col) {
    Column c;
    PathText p;
    cat(p, path_dir, '/', file);
    FileData f = read_file(arena, p.c_str());
    if (f.status == ReadStatus::too_large) c.refused = true;
    if (f.status != ReadStatus::ok) return c;

    std::string_view text = f.bytes;
    size_t first = text.find('\n');
    if (first == std::string_view::npos) return c;
    text = text.substr(first + 1);

    uint32_t lines = uint32_t(std::count(text.begin(), text.end(), '\n')) + (!text.empty() && text.back() != '\n');
    c.values = static_cast<double *>(arena.alloc_bytes(sizeof(double) * (lines + 1), alignof(double)));
    if (!c.values) {
        c.refused = true;
        return c;
    }
    while (!text.empty()) {
        size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);

        std::string_view cell;
        size_t at = 0;
        for (int i = 0; !line.empty(); ++i) {
            size_t comma = line.find(',', at);
            cell = line.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
            if (i == col || comma == std::string_view::npos) break;
            at = comma + 1;
            if (at >= line.size()) break; // a trailing comma adds no field
        }
        c.values[c.count++] = parse_leading_double(cell);
    }
    return c;
}

void describe_memory_run(Service &s, std::string_view dir, std::string_view label, const Summary &sum, EvidenceText &text) {
    ArenaScope scope(s.evidence);
    Column t = csv_column(s.evidence, dir, "memory.csv", 0);
    Column m = csv_column(s.evidence, dir, "memory.csv", 1);
    if (t.refused || m.refused) RLOG_WARN("evidence: {}/memory.csv does not fit the evidence arena; refused", dir);
    double grow = m.count > 1 ? (m.back() - m.values[m.count / 4]) / 1048576.0 : 0;
    double secs = t.count > 1 ? (t.back() - t.values[t.count / 4]) / 1000.0 : 1;
    cat(text, label, ": resident memory ", decimal(m.count == 0 ? 0 : m.back() / 1048576.0, 0), " MB at the end; ", signed_decimal(grow, 0),
        " MB over the last ", decimal(secs, 0), " s (", signed_decimal(grow / secs, 1), " MB/s); ", sum.cache, " cached texture copies.");
}

void describe_trace_run(Service &s, std::string_view dir, std::string_view label, const Summary &sum, EvidenceText &text) {
    ArenaScope scope(s.evidence);
    PathText p;
    cat(p, dir, "/trace.json");
    FileData f = read_file(s.evidence, p.c_str(), size_t(-1));
    yyjson_doc *doc = f.status == ReadStatus::ok ? parse_json(s.evidence, f.bytes) : nullptr;
    if (f.status == ReadStatus::too_large || (f.status == ReadStatus::ok && !doc && s.evidence.used >= s.evidence.capacity - 256)) {
        RLOG_WARN("evidence: {} does not fit the evidence arena; not analysed", p.view());
        cat(text, label, " trace: trace.json does not fit the evidence buffer and was not analysed. Median frame ", decimal(sum.p50, 1), " ms.");
        return;
    }
    double wait_max = 0, wait_total = 0, read_max = 0;
    int waits = 0, reads = 0;
    size_t i, n;
    yyjson_val *e;
    yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(doc), "traceEvents"), i, n, e) {
        std::string_view name = json_str(e, "name");
        double dur = yyjson_get_num(yyjson_obj_get(e, "dur")) / 1000.0;
        if (name == "wait_asset_lock") {
            ++waits;
            wait_total += dur;
            wait_max = std::max(wait_max, dur);
        }
        if (name == "asset_read_parse") {
            ++reads;
            read_max = std::max(read_max, dur);
        }
    }
    cat(text, label, " trace: the render thread waited on the asset lock ", waits, " times (longest ", decimal(wait_max, 0), " ms, total ",
        decimal(wait_total, 0), " ms); the asset loader ran ", reads, " file reads (longest ", decimal(read_max, 0), " ms). Median frame ",
        decimal(sum.p50, 1), " ms.");
}

void describe_frame_run(Service &s, std::string_view dir, std::string_view label, const Summary &sum, EvidenceText &text) {
    cat(text, label, ": median frame ", decimal(sum.p50, 1), " ms, p99 ", decimal(sum.p99, 1), " ms, worst ", decimal(sum.max, 0), " ms, ", sum.spikes,
        " spike frames (over 2x median); resident memory ", decimal(sum.resident_mb, 0), " MB.");
    ArenaScope scope(s.evidence);
    Column ms = csv_column(s.evidence, dir, "frames.csv", 1);
    if (ms.refused) RLOG_WARN("evidence: {}/frames.csv does not fit the evidence arena; refused", dir);
    uint32_t first = 0, last = 0, spikes = 0;
    for (uint32_t k = 40; k < ms.count; ++k)
        if (ms.values[k] > 2 * sum.p50) {
            if (spikes == 0) first = k;
            last = k;
            ++spikes;
        }
    if (spikes >= 2)
        cat(text, " Spikes repeat about every ", decimal(double(last - first) / double(spikes - 1) * sum.p50 / 1000.0, 1), " s.");
}

// ---------------------------------------------------------------- instant evidence tools

void fetch_logs(Service &s, const Case &c, const Run &last, EvidenceText &text) {
    FixedStringBuilder<kPathBytes> key;
    store_key(key, c, last.action_id.view());
    PathText p;
    cat(p, s.cfg.data, "/store/", key);

    ArenaScope scope(s.evidence);
    FileData f = read_file(s.evidence, p.c_str(), size_t(-1));
    if (f.status == ReadStatus::too_large) {
        RLOG_WARN("evidence: {} does not fit the evidence arena; refused", p.view());
        cat(text, "Fetched s3://", key, " from the local log store, but it does not fit the evidence buffer; not read.");
        return;
    }
    int errors = 0, lines = 0;
    std::string_view worst, rest = f.bytes;
    while (!rest.empty()) {
        size_t nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);
        ++lines;
        if (line.find("\"fatal\"") != std::string_view::npos || line.find("\"error\"") != std::string_view::npos) {
            ++errors;
            worst = line;
        }
    }
    cat(text, "Fetched s3://", key, " from the local log store (", lines, " lines). ");
    if (errors) cat(text, errors, " error/fatal line(s); last: ", worst.substr(0, 300));
    else text.add("No error or fatal lines.");
}

void check_disk(Service &s, const Run &last, EvidenceText &text) {
    Summary sum = read_summary(s, last.dir.view());
    PathText p;
    cat(p, last.dir, "/tmp");
    int files = count_dir_entries(p.c_str());
    cat(text, "Last run left ", files, " temporary files using ", decimal(sum.tmp_mb, 1), " MB for ", sum.frames, " frames.");
}

void compare_images(Service &s, const Case &c, std::string_view action_id, EvidenceText &text, FixedStringBuilder<1024> &arts) {
    const Run *base = nullptr, *cur = nullptr;
    for (const Run &r : c.runs) {
        if (r.baseline) base = &r;
        else if (r.setup == c.def.setup) cur = &r;
    }
    if (!base || !cur) {
        text.add("Image comparison needs a previous-build run and a current-build run.");
        arts.add("[]");
        return;
    }
    {
        FixedStringBuilder<kNameBytes + 16> a, b, d;
        cat(a, base->action_id, "/image.png");
        cat(b, cur->action_id, "/image.png");
        cat(d, action_id, "/diff.png");
        cat(arts, '[', json_string(a.view()), ',', json_string(b.view()), ',', json_string(d.view()), ']');
    }

    PathText pa, pb;
    cat(pa, base->dir, "/image.rgba");
    cat(pb, cur->dir, "/image.rgba");
    uint64_t size_a = 0, size_b = 0;
    regular_file_size(pa.c_str(), size_a);
    regular_file_size(pb.c_str(), size_b);
    if (size_a > kMaxImageBytes || size_b > kMaxImageBytes) {
        RLOG_WARN("evidence: images of {} and {} bytes exceed the 3840x2160 RGBA evidence budget; refused", size_a, size_b);
        text.add("Image comparison refused: the images are larger than 3840x2160 RGBA, the size the evidence buffer holds.");
        return;
    }

    ArenaScope scope(s.evidence);
    const size_t px = size_t(std::min(size_a, size_b)) / 4;
    uint8_t *diff = static_cast<uint8_t *>(s.evidence.alloc_bytes(px * 4 + 1, 16));
    const size_t inputs_mark = s.evidence.mark();
    FileData fa = read_file(s.evidence, pa.c_str(), kMaxImageBytes);
    FileData fb = read_file(s.evidence, pb.c_str(), kMaxImageBytes);
    if (!diff || fa.status == ReadStatus::too_large || fb.status == ReadStatus::too_large) {
        RLOG_WARN("evidence: image comparison does not fit the evidence arena; refused");
        text.add("Image comparison refused: the images do not fit the evidence buffer.");
        return;
    }
    const auto *a = reinterpret_cast<const uint8_t *>(fa.bytes.data());
    const auto *b = reinterpret_cast<const uint8_t *>(fb.bytes.data());
    const size_t n = std::min(std::min(fa.bytes.size(), fb.bytes.size()) / 4, px);

    Summary sb = read_summary(s, base->dir.view());
    size_t differ = 0;
    double sum = 0, edge_a = 0, edge_b = 0;
    const int w = int(sb.views ? std::sqrt(double(n) * 16.0 / 9.0) : 0);
    std::memset(diff, 255, n * 4);
    for (size_t i = 0; i < n; ++i) {
        int d = 0;
        for (int ch = 0; ch < 3; ++ch) d = std::max(d, std::abs(int(a[i * 4 + ch]) - int(b[i * 4 + ch])));
        sum += d;
        differ += d > 8;
        uint8_t v = uint8_t(std::min(255, d * 4));
        diff[i * 4] = v;
        diff[i * 4 + 1] = uint8_t(v / 3);
        diff[i * 4 + 2] = uint8_t(v / 3);
        if (w > 0 && i % size_t(w) + 1 < size_t(w)) {
            edge_a += std::abs(int(a[i * 4 + 1]) - int(a[i * 4 + 5]));
            edge_b += std::abs(int(b[i * 4 + 1]) - int(b[i * 4 + 5]));
        }
    }

    // The inputs are no longer needed: the PNG encoder reuses their space.
    s.evidence.rewind(inputs_mark);
    const int h = w > 0 ? int(n / size_t(w)) : 0;
    if (w > 0) {
        PathText out_dir, out;
        job_dir(out_dir, s, action_id);
        cat(out, out_dir, "/diff.png");
        make_dirs(out_dir.view());
        g_png_arena = &s.evidence;
        int png_len = 0;
        unsigned char *png = stbi_write_png_to_mem(diff, w * 4, w, h, 4, &png_len);
        g_png_arena = nullptr;
        if (png) write_file(out.c_str(), std::string_view(reinterpret_cast<const char *>(png), size_t(png_len)), false);
        else RLOG_WARN("evidence: diff.png encode does not fit the evidence arena; not written");
    }

    const double denom = double(std::max<size_t>(n, 1));
    cat(text, "Image comparison, previous vs current build: ", decimal(100.0 * double(differ) / denom, 2), "% of pixels differ by more than 8/255; mean difference ",
        decimal(sum / denom, 2), "/255; edge detail of the current image is ", decimal(edge_a > 0 ? 100.0 * edge_b / edge_a : 100.0, 0), "% of the previous one.");
}

void search_code(Service &s, const Case &c, EvidenceText &text) {
    ArenaScope scope(s.evidence);
    // Topic chosen from the evidence text; the matching source lines become evidence.
    const size_t ev_cap = c.facts.used + c.facts.count() + 1;
    char *ev_buf = static_cast<char *>(s.evidence.alloc_bytes(ev_cap, 1));
    size_t ev_len = 0;
    for (uint32_t i = 0; ev_buf && i < c.facts.count(); ++i) {
        std::string_view f = c.facts[i];
        std::memcpy(ev_buf + ev_len, f.data(), f.size());
        ev_len += f.size();
        ev_buf[ev_len++] = ' ';
    }
    std::string_view ev(ev_buf ? ev_buf : "", ev_len);
    auto has = [&](std::string_view k) { return ev.find(k) != std::string_view::npos; };
    std::string_view keys[2] = {"frame", "tile"};
    if (has("lock") || has("pike")) keys[0] = "asset_lock", keys[1] = "read_and_parse";
    else if (has("memory")) keys[0] = "leaked", keys[1] = "TextureCache";
    else if (has("temporary")) keys[0] = "tmp", keys[1] = "thumbnail";
    else if (has("abort") || has("exited")) keys[0] = "kMaxViews", keys[1] = "view_limit";
    else if (has("edge detail")) keys[0] = "lod_bias", keys[1] = "lod";

    // Every *.cpp and *.hpp file of the source directory, in name order.
    FixedArray<FixedString<256>, 128> files;
    if (DIR *d = opendir(s.cfg.repo_dir.c_str())) { // opendir allocates its DIR inside libc; once per case
        while (dirent *e = readdir(d)) {
            std::string_view name = e->d_name;
            bool source = name.size() > 4 && (name.substr(name.size() - 4) == ".cpp" || name.substr(name.size() - 4) == ".hpp");
            if (!source) continue;
            if (files.full()) {
                RLOG_WARN("evidence: more than {} source files in {}; the rest are not searched", 128, s.cfg.repo_dir.view());
                break;
            }
            files.push(FixedString<256>(name));
        }
        closedir(d);
    }
    std::sort(files.begin(), files.end(), [](const FixedString<256> &x, const FixedString<256> &y) { return x.view() < y.view(); });

    FixedStringBuilder<4096> hits;
    int count = 0;
    for (const FixedString<256> &name : files) {
        if (count >= 6) break;
        ArenaScope file_scope(s.evidence);
        PathText path;
        cat(path, s.cfg.repo_dir, '/', name);
        FileData f = read_file(s.evidence, path.c_str());
        int ln = 0;
        std::string_view rest = f.status == ReadStatus::ok ? f.bytes : std::string_view();
        while (!rest.empty() && count < 6) {
            size_t nl = rest.find('\n');
            std::string_view line = rest.substr(0, nl);
            rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);
            ++ln;
            for (std::string_view k : keys)
                if (line.find(k) != std::string_view::npos) {
                    size_t b = line.find_first_not_of(' ');
                    cat(hits, "\n", name, ":", ln, "  ", line.substr(b == std::string_view::npos ? 0 : b, 140));
                    ++count;
                    break;
                }
        }
    }
    cat(text, "Code search for ", keys[0], " / ", keys[1], ":");
    if (hits.view().empty()) text.add(" no matches.");
    else text.add(hits.view());
}

} // namespace romeo
