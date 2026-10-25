// Evidence from disk: file helpers (POSIX only), JSON parsing over arena memory, and the analyses that turn a
// finished run's files into fact text (summary.json, CSVs, trace.json, the log store, image diff, code search).
//
// All temporary bytes come from Service::evidence, rewound by the caller after each analysis.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>

#include "cases.hpp"
#include "yyjson.h"

namespace romeo {

using PathText = FixedStringBuilder<kPathBytes>;
using EvidenceText = FixedStringBuilder<8192>;

// ---------------------------------------------------------------- files

enum class ReadStatus : uint8_t { ok, missing, too_large };
struct FileData {
    ReadStatus status;
    std::string_view bytes; // in the arena; followed by a NUL byte
};

// Reads a whole file into the arena. Files longer than limit are cut at limit, like the original read_file().
// too_large means the arena could not hold the file.
FileData read_file(Arena &arena, const char *path, size_t limit = size_t(64) << 20);
bool file_exists(const char *path);
bool regular_file_size(const char *path, uint64_t &size);
bool make_dirs(std::string_view path); // mkdir -p
bool write_file(const char *path, std::string_view bytes, bool sync);
bool copy_file(const char *from, const char *to); // overwrites
int count_dir_entries(const char *path);          // excluding . and ..; 0 if missing

// Parses JSON with a yyjson pool allocator over arena memory, so parsing never calls malloc.
yyjson_doc *parse_json(Arena &arena, std::string_view text);
inline std::string_view json_str(yyjson_val *v, const char *key) {
    const char *s = yyjson_get_str(yyjson_obj_get(v, key));
    return s ? std::string_view(s, yyjson_get_len(yyjson_obj_get(v, key))) : std::string_view();
}

// <data>/jobs/<action_id>
template <uint32_t N>
void job_dir(FixedStringBuilder<N> &b, const Service &s, std::string_view action_id) {
    cat(b, s.cfg.data, "/jobs/", action_id);
}

// ["<action_id>/<name>", ...] for each name that exists in the job directory.
template <uint32_t N>
void write_artifacts(FixedStringBuilder<N> &b, const Service &s, std::string_view action_id, std::initializer_list<std::string_view> names) {
    b.add('[');
    bool first = true;
    for (std::string_view name : names) {
        PathText p;
        job_dir(p, s, action_id);
        cat(p, '/', name);
        if (p.overflowed() || !file_exists(p.c_str())) continue;
        if (!first) b.add(',');
        first = false;
        b.add('"').escaped(action_id).add('/').escaped(name).add('"');
    }
    b.add(']');
}

// ---------------------------------------------------------------- analyses

struct Summary {
    double p50 = 0, p95 = 0, p99 = 0, max = 0, total_ms = 0;
    int spikes = 0, views = 0, frames = 0;
    double resident_mb = 0, tmp_mb = 0;
    size_t cache = 0;
    bool ok = false;
};

Summary read_summary(Service &s, std::string_view dir);
int read_exit_code(Service &s, std::string_view dir);

// "romeo-logs/setup-X/<case>/<action_id>/log.jsonl"
template <uint32_t N>
void store_key(FixedStringBuilder<N> &b, const Case &c, std::string_view action_id) {
    cat(b, "romeo-logs/setup-", c.def.setup, '/', c.def.id, '/', action_id, "/log.jsonl");
}
// Uploads a finished run's log into the local object store, laid out like an S3 bucket.
void upload_run_log(Service &s, const Case &c, std::string_view action_id, std::string_view dir);

// Fact text for a finished renderer run. label is "Current build, setup B" and similar.
void describe_memory_run(Service &s, std::string_view dir, std::string_view label, const Summary &sum, EvidenceText &text);
void describe_trace_run(Service &s, std::string_view dir, std::string_view label, const Summary &sum, EvidenceText &text);
void describe_frame_run(Service &s, std::string_view dir, std::string_view label, const Summary &sum, EvidenceText &text);

// Instant evidence tools.
void fetch_logs(Service &s, const Case &c, const Run &last, EvidenceText &text);
void check_disk(Service &s, const Run &last, EvidenceText &text);
void compare_images(Service &s, const Case &c, std::string_view action_id, EvidenceText &text, FixedStringBuilder<1024> &arts);
void search_code(Service &s, const Case &c, EvidenceText &text);

} // namespace romeo
