// romeo: the always-on case service.
//
// One thread owns everything: the HTTP listener, the live event stream, model calls (libcurl multi, polled in the
// same poll() as the sockets) and renderer jobs. Cases are append-only JSONL logs; all case state is rebuilt from
// those logs on start, so a restart resumes every case where it stopped.
//
// Send-once rule: a job action first writes outbox/<action_id>.json with O_EXCL, then starts a detached renderer
// process that writes <job>/exit when it finishes. After a restart the service never starts the job again; it waits
// for the exit file of the job that is already running or finished.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "yyjson.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- configuration

static std::string env(const char *name, const char *fallback) {
    const char *v = std::getenv(name);
    return v && *v ? v : fallback;
}

struct Config {
    int port = std::atoi(env("ROMEO_PORT", "8790").c_str());
    fs::path data = env("ROMEO_DATA", "runs/data");
    fs::path render = env("ROMEO_RENDER", "build/romeo-render");
    fs::path cases_file = env("ROMEO_CASES", "cases/cases.json");
    fs::path repo_file = env("ROMEO_REPO_FILE", "renderer/render.cpp");
    fs::path ui = env("ROMEO_UI", "ui/dist");
    fs::path dashboard = env("ROMEO_DASHBOARD", "dashboard");
    std::string ornith_metrics = env("ORNITH_METRICS", "http://127.0.0.1:8000/metrics");
    std::string clef = env("CLEF_URL", "http://127.0.0.1:8001/v1/systemone");
    std::string ornith = env("ORNITH_URL", "http://127.0.0.1:8000/v1/chat/completions");
    std::string memory = env("MEMORY_URL", "http://127.0.0.1:8791/similar");
    bool think = env("ROMEO_THINK", "1") == "1";
    int frames = std::atoi(env("ROMEO_FRAMES", "300").c_str());
} cfg;

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string read_file(const fs::path &p, size_t limit = 64u << 20) {
    std::ifstream f(p, std::ios::binary);
    std::string s;
    if (!f) return s;
    s.assign(std::istreambuf_iterator<char>(f), {});
    if (s.size() > limit) s.resize(limit);
    return s;
}

static std::string json_escape(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += char(c);
        }
    }
    return o;
}
static std::string q(std::string_view s) { return "\"" + json_escape(s) + "\""; }

// ---------------------------------------------------------------- case model

struct CaseDef {
    std::string id, title, report, bugs;
    char setup = 'A';
};

struct Pending {
    std::string action_id, kind, label, dir;
    bool job = false;
};

struct Run {
    std::string action_id, kind, dir, label;
    char setup;
    bool baseline;
};

struct Case {
    CaseDef def;
    std::vector<std::string> events; // raw JSON lines, in order
    std::vector<std::string> facts;
    std::set<std::string> done;      // completed action kinds
    std::map<std::string, Pending> pending;
    std::vector<Run> runs;
    std::string status = "investigating", waiting_for, diagnosis;
    int seq = 0;
    int64_t updated_ms = 0;
    bool busy = false;  // a model call for this case is in flight
    bool concluded = false;
};

static std::vector<Case> cases;
static std::map<std::string, CaseDef> defs;
static std::vector<int> sse_clients;
static int decisions_total = 0;      // Clef decisions since start, for the dashboard
static double decision_ms_total = 0;

static Case *find_case(std::string_view id) {
    for (auto &c : cases)
        if (c.def.id == id) return &c;
    return nullptr;
}

// ---------------------------------------------------------------- event log

static void broadcast(const std::string &line) {
    std::string msg = "data: " + line + "\n\n";
    for (size_t i = 0; i < sse_clients.size();) {
        ssize_t n = send(sse_clients[i], msg.data(), msg.size(), 0);
        if (n != ssize_t(msg.size())) { close(sse_clients[i]); sse_clients.erase(sse_clients.begin() + long(i)); }
        else ++i;
    }
}

// Derived state comes only from events, so replay after a restart gives the same state.
static void apply(Case &c, yyjson_val *ev) {
    std::string type = yyjson_get_str(yyjson_obj_get(ev, "type")) ?: "";
    yyjson_val *d = yyjson_obj_get(ev, "data");
    auto s = [&](const char *k) { const char *v = yyjson_get_str(yyjson_obj_get(d, k)); return std::string(v ? v : ""); };
    c.seq = int(yyjson_get_int(yyjson_obj_get(ev, "seq")));
    c.updated_ms = yyjson_get_sint(yyjson_obj_get(ev, "t_ms"));
    if (type == "case_opened") {
        c.status = "investigating";
    } else if (type == "action_requested") {
        Pending p{s("action_id"), s("kind"), s("label"), s("dir"), yyjson_get_bool(yyjson_obj_get(d, "job"))};
        c.pending[p.action_id] = p;
        c.status = "waiting";
        c.waiting_for = p.label;
    } else if (type == "action_result") {
        auto it = c.pending.find(s("action_id"));
        if (it != c.pending.end()) {
            if (it->second.job && !s("run_dir").empty())
                c.runs.push_back({it->second.action_id, it->second.kind, s("run_dir"), it->second.label, s("setup").empty() ? c.def.setup : s("setup")[0], yyjson_get_bool(yyjson_obj_get(d, "baseline"))});
            c.done.insert(it->second.kind);
            c.pending.erase(it);
        }
        if (c.pending.empty() && c.status == "waiting") { c.status = "investigating"; c.waiting_for.clear(); }
    } else if (type == "fact") {
        c.facts.push_back(s("text"));
    } else if (type == "conclusion") {
        c.concluded = true;
        c.diagnosis = s("diagnosis");
    } else if (type == "stopped") {
        c.status = "stopped";
    } else if (type == "done") {
        c.status = "done";
    }
}

static fs::path case_log(const Case &c) { return cfg.data / "cases" / (c.def.id + ".jsonl"); }

// Append one event: write the line, flush it to disk, apply it, then stream it.
static void emit(Case &c, const char *type, const std::string &data_json) {
    int seq = c.seq + 1;
    std::string line = "{\"seq\":" + std::to_string(seq) + ",\"case\":" + q(c.def.id) + ",\"t_ms\":" + std::to_string(now_ms()) +
                       ",\"type\":" + q(type) + ",\"data\":" + data_json + "}";
    int fd = open(case_log(c).c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    std::string out = line + "\n";
    if (fd < 0 || write(fd, out.data(), out.size()) != ssize_t(out.size())) {
        std::fprintf(stderr, "case log write failed for %s: %s\n", c.def.id.c_str(), std::strerror(errno));
        std::abort(); // an event that cannot be made durable must not be acted on
    }
    fsync(fd);
    close(fd);
    yyjson_doc *doc = yyjson_read(line.data(), line.size(), 0);
    apply(c, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    c.events.push_back(line);
    broadcast(line);
}

static void fact(Case &c, const std::string &text, const std::string &evidence_json = "[]") {
    emit(c, "fact", "{\"text\":" + q(text) + ",\"evidence\":" + evidence_json + "}");
}

static void load_defs() {
    std::string raw = read_file(cfg.cases_file);
    yyjson_doc *doc = yyjson_read(raw.data(), raw.size(), 0);
    yyjson_val *arr = yyjson_doc_get_root(doc);
    size_t i, n;
    yyjson_val *v;
    yyjson_arr_foreach(arr, i, n, v) {
        auto s = [&](const char *k) { const char *x = yyjson_get_str(yyjson_obj_get(v, k)); return std::string(x ? x : ""); };
        CaseDef d{s("id"), s("title"), s("report"), s("bugs"), s("setup").empty() ? 'A' : s("setup")[0]};
        defs[d.id] = d;
    }
    yyjson_doc_free(doc);
}

static void load_cases() {
    fs::create_directories(cfg.data / "cases");
    fs::create_directories(cfg.data / "outbox");
    fs::create_directories(cfg.data / "jobs");
    std::vector<fs::path> logs;
    for (auto &e : fs::directory_iterator(cfg.data / "cases"))
        if (e.path().extension() == ".jsonl") logs.push_back(e.path());
    std::sort(logs.begin(), logs.end());
    for (auto &p : logs) {
        Case c;
        std::ifstream f(p);
        std::string line;
        while (std::getline(f, line)) {
            yyjson_doc *doc = yyjson_read(line.data(), line.size(), 0);
            if (!doc) break; // a torn final line from a crash mid-write is ignored
            yyjson_val *d = yyjson_obj_get(yyjson_doc_get_root(doc), "data");
            if (c.events.empty()) {
                const char *id = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "case"));
                c.def = defs.count(id ? id : "") ? defs[id] : CaseDef{id ? id : "", "", "", "", 'A'};
                if (const char *t = yyjson_get_str(yyjson_obj_get(d, "title"))) c.def.title = t;
                if (const char *s = yyjson_get_str(yyjson_obj_get(d, "setup"))) c.def.setup = s[0];
                if (const char *r = yyjson_get_str(yyjson_obj_get(d, "report"))) c.def.report = r;
            }
            apply(c, yyjson_doc_get_root(doc));
            yyjson_doc_free(doc);
            c.events.push_back(line);
        }
        cases.push_back(std::move(c));
    }
    // Every case that was waiting gets a visible restart marker; its jobs are not started again.
    for (auto &c : cases)
        if (!c.pending.empty()) {
            std::string ids = "[";
            for (auto &[id, p] : c.pending) ids += (ids.size() > 1 ? "," : "") + q(id);
            emit(c, "restarted", "{\"pending\":" + ids + "]}");
        }
}

static Case &open_case(const CaseDef &d) {
    cases.push_back(Case{});
    Case &c = cases.back();
    c.def = d;
    emit(c, "case_opened", "{\"title\":" + q(d.title) + ",\"setup\":\"" + std::string(1, d.setup) + "\",\"report\":" + q(d.report) + "}");
    return c;
}

// ---------------------------------------------------------------- model calls (libcurl multi)

enum class CallKind { decide, think, conclude, stop_note, memory };

struct Call {
    std::string case_id, body, response;
    CallKind kind;
    std::vector<std::string> options;
    curl_slist *headers = nullptr;
};

static CURLM *multi;
static std::map<CURL *, Call *> calls;

static size_t on_data(char *p, size_t s, size_t n, void *u) {
    static_cast<Call *>(u)->response.append(p, s * n);
    return s * n;
}

static void post(const std::string &url, Call *call, long timeout_s) {
    CURL *h = curl_easy_init();
    call->headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, call->headers);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, call->body.c_str());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, call);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_multi_add_handle(multi, h);
    calls[h] = call;
}

static std::string chat_body(const std::string &system, const std::string &user, int max_tokens, bool think) {
    return "{\"model\":\"ornith-35b\",\"temperature\":0,\"max_tokens\":" + std::to_string(max_tokens) +
           ",\"chat_template_kwargs\":{\"enable_thinking\":" + (think ? "true" : "false") + "},\"messages\":[{\"role\":\"system\",\"content\":" + q(system) +
           "},{\"role\":\"user\",\"content\":" + q(user) + "}]}";
}

static std::string case_state(const Case &c) {
    std::string s = "Report: " + c.def.report + "\nSetup " + std::string(1, c.def.setup) + ".\nEvidence so far:\n";
    if (c.facts.empty()) s += "(none yet)\n";
    for (size_t i = 0; i < c.facts.size(); ++i) s += std::to_string(i + 1) + ". " + c.facts[i] + "\n";
    return s;
}

// ---------------------------------------------------------------- actions

struct ActionInfo {
    const char *kind, *criteria, *label;
    bool job;
};

// The typed action catalog Clef chooses from. Criteria are what Clef reads; labels are what people read.
static const ActionInfo kActions[] = {
    {"measure_frames", "Run the current build and measure frame times (median, p99, spikes)", "frame-time run", true},
    {"measure_memory", "Run the current build longer and record resident memory over time", "long memory run", true},
    {"capture_trace", "Run with a timing trace of every thread and find where frames wait", "trace capture", true},
    {"compare_baseline", "Run the previous build on the same setup for a before/after comparison", "previous-build run", true},
    {"try_smaller_setup", "Run the current build on setup A to see whether the problem depends on the setup", "setup A run", true},
    {"fetch_logs", "Fetch the run logs from the log store and look for errors and fatal lines", "log fetch", false},
    {"check_disk", "Check how much temporary data the last run left on disk", "disk check", false},
    {"compare_images", "Compare the rendered image of the current and previous build pixel by pixel", "image comparison", false},
    {"search_code", "Search the renderer source for code related to the evidence", "code search", false},
    {"recall_similar", "Look up similar past cases in case memory", "memory lookup", false},
    {"conclude", "Enough evidence: state the cause and propose a fix", "conclusion", false},
    {"stop_need_more", "Stop: the available tools cannot establish more", "stop", false},
};

static const ActionInfo *action(std::string_view kind) {
    for (auto &a : kActions)
        if (kind == a.kind) return &a;
    return nullptr;
}

static bool has_run(const Case &c, bool baseline) {
    for (auto &r : c.runs) if (r.baseline == baseline && r.setup == c.def.setup) return true;
    return false;
}

static std::vector<std::string> candidates(const Case &c) {
    std::vector<std::string> out;
    bool any_run = !c.runs.empty();
    for (auto &a : kActions) {
        std::string k = a.kind;
        if (c.done.count(k)) continue;
        if ((k == "fetch_logs" || k == "check_disk") && !any_run) continue;
        if (k == "compare_images" && !(has_run(c, true) && has_run(c, false))) continue;
        if ((k == "conclude") && c.facts.size() < 3) continue;
        if (k == "search_code" && c.facts.size() < 2) continue;
        out.push_back(k);
    }
    return out;
}

static std::string job_dir_for(const std::string &action_id) { return (cfg.data / "jobs" / action_id).string(); }

static int running_jobs() {
    int n = 0;
    for (auto &c : cases) for (auto &[id, p] : c.pending) n += p.job;
    return n;
}

// Start a detached renderer job. The outbox file is created first with O_EXCL: a second start of the same action
// fails here, so no action runs twice.
static bool start_job(Case &c, const std::string &action_id, const std::string &kind, char setup, bool baseline, const std::string &args) {
    fs::path outbox = cfg.data / "outbox" / (action_id + ".json");
    int fd = open(outbox.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return false;
    std::string dir = job_dir_for(action_id);
    fs::create_directories(dir);
    std::string cmd = "exec " + fs::absolute(cfg.render).string() + " --setup " + std::string(1, setup) + " --frames " + std::to_string(cfg.frames) + " " + args +
                      (baseline || defs[c.def.id].bugs.empty() ? "" : " --bug " + defs[c.def.id].bugs) + " --out " + dir;
    std::string wrapped = "(" + cmd + ") > " + dir + "/stdout.txt 2>&1; echo $? > " + dir + "/exit";
    std::string rec = "{\"action_id\":" + q(action_id) + ",\"kind\":" + q(kind) + ",\"command\":" + q(wrapped) + ",\"t_ms\":" + std::to_string(now_ms()) + "}\n";
    if (write(fd, rec.data(), rec.size()) != ssize_t(rec.size())) { close(fd); return false; }
    fsync(fd);
    close(fd);
    pid_t pid = fork();
    if (pid == 0) {
        setsid(); // the job survives a restart of this service
        execl("/bin/sh", "sh", "-c", wrapped.c_str(), (char *)nullptr);
        _exit(127);
    }
    return pid > 0;
}

// ---------------------------------------------------------------- evidence from runs

struct Summary {
    double p50 = 0, p95 = 0, p99 = 0, max = 0, total_ms = 0;
    int spikes = 0, views = 0, frames = 0;
    double resident_mb = 0, tmp_mb = 0;
    size_t cache = 0;
    bool ok = false;
};

static Summary summary(const std::string &dir) {
    Summary s;
    std::string raw = read_file(fs::path(dir) / "summary.json");
    yyjson_doc *doc = yyjson_read(raw.data(), raw.size(), 0);
    if (!doc) return s;
    yyjson_val *r = yyjson_doc_get_root(doc);
    auto n = [&](const char *k) { return yyjson_get_num(yyjson_obj_get(r, k)); };
    s = {n("p50_ms"), n("p95_ms"), n("p99_ms"), n("max_ms"), n("total_ms"), int(n("spike_frames")), int(n("views")), int(n("frames")),
         n("resident_end_bytes") / 1048576.0, n("tmp_bytes") / 1048576.0, size_t(n("cache_entries")), true};
    yyjson_doc_free(doc);
    return s;
}

static std::vector<double> csv_column(const std::string &path, int col) {
    std::vector<double> v;
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string cell;
        for (int i = 0; i <= col && std::getline(ss, cell, ','); ++i) {}
        v.push_back(std::atof(cell.c_str()));
    }
    return v;
}

static std::string fmt(const char *f, double a) { char b[64]; std::snprintf(b, sizeof b, f, a); return b; }

static std::string artifacts(const std::string &action_id, std::initializer_list<const char *> names) {
    std::string a = "[";
    for (auto *n : names)
        if (fs::exists(fs::path(job_dir_for(action_id)) / n)) a += (a.size() > 1 ? "," : "") + q(action_id + "/" + n);
    return a + "]";
}

// Upload a finished run's log into the local object store, laid out like an S3 bucket.
static std::string store_key(const Case &c, const std::string &action_id) {
    return "romeo-logs/setup-" + std::string(1, c.def.setup) + "/" + c.def.id + "/" + action_id + "/log.jsonl";
}

static void ingest_job(Case &c, const Pending &p) {
    std::string dir = p.dir;
    int code = std::atoi(read_file(fs::path(dir) / "exit").c_str());
    fs::path store = cfg.data / "store" / store_key(c, p.action_id);
    fs::create_directories(store.parent_path());
    std::error_code ec;
    fs::copy_file(fs::path(dir) / "log.jsonl", store, fs::copy_options::overwrite_existing, ec);

    bool baseline = p.kind == "compare_baseline";
    char setup = p.kind == "try_smaller_setup" ? 'A' : c.def.setup;
    Summary s = summary(dir);
    std::string res = "{\"action_id\":" + q(p.action_id) + ",\"source\":\"run\",\"run_dir\":" + q(dir) + ",\"setup\":\"" + std::string(1, setup) + "\",\"baseline\":" + (baseline ? "true" : "false") + ",\"exit_code\":" + std::to_string(code);
    if (code != 0 || !s.ok) {
        std::string summary_text = "Run on setup " + std::string(1, setup) + " exited with code " + std::to_string(code) + (code == 134 ? " (abort)" : "") + " before writing results";
        emit(c, "action_result", res + ",\"summary\":" + q(summary_text) + ",\"artifacts\":" + artifacts(p.action_id, {"log.jsonl", "stdout.txt"}) + "}");
        fact(c, summary_text + ".", artifacts(p.action_id, {"log.jsonl"}));
        return;
    }
    std::string label = std::string(baseline ? "Previous build" : "Current build") + ", setup " + std::string(1, setup);
    std::string text;
    if (p.kind == "measure_memory") {
        auto t = csv_column(dir + "/memory.csv", 0), m = csv_column(dir + "/memory.csv", 1);
        double grow = m.size() > 1 ? (m.back() - m[m.size() / 4]) / 1048576.0 : 0, secs = t.size() > 1 ? (t.back() - t[t.size() / 4]) / 1000.0 : 1;
        text = label + ": resident memory " + fmt("%.0f MB", m.empty() ? 0 : m.back() / 1048576.0) + " at the end; " + fmt("%+.0f MB", grow) + " over the last " + fmt("%.0f s", secs) +
               " (" + fmt("%+.1f MB/s", grow / secs) + "); " + std::to_string(s.cache) + " cached texture copies.";
    } else if (p.kind == "capture_trace") {
        std::string raw = read_file(dir + "/trace.json");
        yyjson_doc *doc = yyjson_read(raw.data(), raw.size(), 0);
        double wait_max = 0, wait_total = 0, read_max = 0;
        int waits = 0, reads = 0;
        size_t i, n;
        yyjson_val *e;
        yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(doc), "traceEvents"), i, n, e) {
            std::string name = yyjson_get_str(yyjson_obj_get(e, "name")) ?: "";
            double dur = yyjson_get_num(yyjson_obj_get(e, "dur")) / 1000.0;
            if (name == "wait_asset_lock") { ++waits; wait_total += dur; wait_max = std::max(wait_max, dur); }
            if (name == "asset_read_parse") { ++reads; read_max = std::max(read_max, dur); }
        }
        yyjson_doc_free(doc);
        text = label + " trace: the render thread waited on the asset lock " + std::to_string(waits) + " times (longest " + fmt("%.0f ms", wait_max) + ", total " + fmt("%.0f ms", wait_total) +
               "); the asset loader ran " + std::to_string(reads) + " file reads (longest " + fmt("%.0f ms", read_max) + "). Median frame " + fmt("%.1f ms", s.p50) + ".";
    } else {
        text = label + ": median frame " + fmt("%.1f ms", s.p50) + ", p99 " + fmt("%.1f ms", s.p99) + ", worst " + fmt("%.0f ms", s.max) + ", " + std::to_string(s.spikes) +
               " spike frames (over 2x median); resident memory " + fmt("%.0f MB", s.resident_mb) + ".";
        auto ms = csv_column(dir + "/frames.csv", 1);
        std::vector<int> spike_idx;
        for (size_t k = 40; k < ms.size(); ++k) if (ms[k] > 2 * s.p50) spike_idx.push_back(int(k));
        if (spike_idx.size() >= 2) text += " Spikes repeat about every " + fmt("%.1f s", double(spike_idx.back() - spike_idx.front()) / double(spike_idx.size() - 1) * s.p50 / 1000.0) + ".";
    }
    emit(c, "action_result", res + ",\"summary\":" + q(text) + ",\"artifacts\":" + artifacts(p.action_id, {"frames.csv", "memory.csv", "trace.json", "image.png", "log.jsonl"}) + "}");
    fact(c, text, artifacts(p.action_id, {"frames.csv", "memory.csv", "trace.json", "image.png"}));
}

// Instant actions read evidence that already exists, so they finish inside the same step.
static void run_instant(Case &c, const std::string &action_id, const std::string &kind) {
    const Run *last = c.runs.empty() ? nullptr : &c.runs.back();
    std::string res = "{\"action_id\":" + q(action_id) + ",\"source\":\"run\"";
    std::string text, arts = "[]";
    if (kind == "fetch_logs" && last) {
        std::string key = store_key(c, last->action_id);
        std::ifstream f(cfg.data / "store" / key);
        std::string line, worst;
        int errors = 0, lines = 0;
        while (std::getline(f, line)) {
            ++lines;
            if (line.find("\"fatal\"") != std::string::npos || line.find("\"error\"") != std::string::npos) { ++errors; worst = line; }
        }
        text = "Fetched s3://" + key + " from the local log store (" + std::to_string(lines) + " lines). " +
               (errors ? std::to_string(errors) + " error/fatal line(s); last: " + worst.substr(0, 300) : "No error or fatal lines.");
        arts = "[" + q(last->action_id + "/log.jsonl") + "]";
    } else if (kind == "check_disk" && last) {
        Summary s = summary(last->dir);
        size_t files = 0;
        std::error_code ec;
        for (auto &e : fs::directory_iterator(fs::path(last->dir) / "tmp", ec)) { (void)e; ++files; }
        text = "Last run left " + std::to_string(files) + " temporary files using " + fmt("%.1f MB", s.tmp_mb) + " for " + std::to_string(s.frames) + " frames.";
    } else if (kind == "compare_images") {
        const Run *base = nullptr, *cur = nullptr;
        for (auto &r : c.runs) { if (r.baseline) base = &r; else if (r.setup == c.def.setup) cur = &r; }
        std::string a = read_file(fs::path(base->dir) / "image.rgba", 1u << 30), b = read_file(fs::path(cur->dir) / "image.rgba", 1u << 30);
        Summary sb = summary(base->dir);
        size_t px = std::min(a.size(), b.size()) / 4, differ = 0;
        double sum = 0, edge_a = 0, edge_b = 0;
        int w = int(sb.views ? std::sqrt(double(px) * 16.0 / 9.0) : 0);
        std::vector<uint8_t> diff(px * 4, 255);
        for (size_t i = 0; i < px; ++i) {
            int d = 0;
            for (int ch = 0; ch < 3; ++ch) d = std::max(d, std::abs(int(uint8_t(a[i * 4 + ch])) - int(uint8_t(b[i * 4 + ch]))));
            sum += d;
            differ += d > 8;
            uint8_t v = uint8_t(std::min(255, d * 4));
            diff[i * 4] = v; diff[i * 4 + 1] = uint8_t(v / 3); diff[i * 4 + 2] = uint8_t(v / 3);
            if (w > 0 && i % size_t(w) + 1 < size_t(w)) {
                edge_a += std::abs(int(uint8_t(a[i * 4 + 1])) - int(uint8_t(a[i * 4 + 5])));
                edge_b += std::abs(int(uint8_t(b[i * 4 + 1])) - int(uint8_t(b[i * 4 + 5])));
            }
        }
        int h = w > 0 ? int(px / size_t(w)) : 0;
        fs::path out = fs::path(job_dir_for(action_id));
        fs::create_directories(out);
        if (w > 0) stbi_write_png((out / "diff.png").c_str(), w, h, 4, diff.data(), w * 4);
        text = "Image comparison, previous vs current build: " + fmt("%.2f%%", 100.0 * double(differ) / double(std::max<size_t>(px, 1))) + " of pixels differ by more than 8/255; mean difference " +
               fmt("%.2f/255", sum / double(std::max<size_t>(px, 1))) + "; edge detail of the current image is " + fmt("%.0f%%", edge_a > 0 ? 100.0 * edge_b / edge_a : 100.0) + " of the previous one.";
        arts = "[" + q(base->action_id + "/image.png") + "," + q(cur->action_id + "/image.png") + "," + q(action_id + "/diff.png") + "]";
    } else if (kind == "search_code") {
        // Topic chosen from the evidence text; the matching source lines become evidence.
        std::string ev;
        for (auto &f : c.facts) ev += f + " ";
        std::vector<std::string> keys;
        if (ev.find("lock") != std::string::npos || ev.find("pike") != std::string::npos) keys = {"asset_lock", "read_and_parse"};
        else if (ev.find("memory") != std::string::npos) keys = {"leaked", "TextureCache"};
        else if (ev.find("temporary") != std::string::npos) keys = {"tmp", "fopen"};
        else if (ev.find("abort") != std::string::npos || ev.find("exited") != std::string::npos) keys = {"kMaxViews", "view_limit"};
        else if (ev.find("edge detail") != std::string::npos) keys = {"lod_bias", "lod"};
        else keys = {"frame", "tile"};
        std::ifstream f(cfg.repo_file);
        std::string line, hits;
        int ln = 0, count = 0;
        while (std::getline(f, line) && count < 6) {
            ++ln;
            for (auto &k : keys)
                if (line.find(k) != std::string::npos) {
                    auto b = line.find_first_not_of(' ');
                    hits += "\n" + cfg.repo_file.filename().string() + ":" + std::to_string(ln) + "  " + line.substr(b == std::string::npos ? 0 : b, 140);
                    ++count;
                    break;
                }
        }
        text = "Code search for " + keys[0] + " / " + keys[1] + ":" + (hits.empty() ? " no matches." : hits);
    } else if (kind == "recall_similar") {
        text = "Case memory: " + std::to_string(cases.size() - 1) + " other cases in memory; similar-case search runs when the memory service answers.";
    }
    emit(c, "action_result", res + ",\"summary\":" + q(text) + ",\"artifacts\":" + arts + "}");
    fact(c, text, arts);
}

// ---------------------------------------------------------------- the agent step

static void request_decision(Case &c) {
    auto opts = candidates(c);
    if (opts.empty()) return;
    std::string criteria;
    for (auto &k : opts) criteria += (criteria.empty() ? "" : ",") + q(k) + ":" + q(action(k)->criteria);
    auto *call = new Call{c.def.id, "", "", CallKind::decide, opts};
    call->body = "{\"model\":\"clef-flash\",\"state\":" + q(case_state(c) + "\nCompleted actions: " + std::to_string(c.done.size())) +
                 ",\"questions\":{\"next_action\":{\"type\":\"choice\",\"instructions\":\"Which action gives the most useful new evidence next? Conclude only when the evidence explains the report.\",\"criteria\":{" + criteria + "}}}}";
    c.busy = true;
    post(cfg.clef, call, 60);
}

static void request_thought(Case &c) {
    auto *call = new Call{c.def.id, chat_body("You are investigating a rendering problem. In 3 short sentences: what the newest evidence means, which explanations remain, and what to check next. Use only the evidence given.",
                                              case_state(c), cfg.think ? 700 : 160, cfg.think), "", CallKind::think, {}};
    post(cfg.ornith, call, 120);
}

static void execute(Case &c, const std::string &kind) {
    std::string action_id = c.def.id + "-" + std::to_string(c.seq + 1);
    const ActionInfo *a = action(kind);
    if (kind == "conclude") {
        auto *call = new Call{c.def.id, chat_body("Answer in JSON only: {\"diagnosis\": one sentence naming the cause, \"fix\": one sentence describing the code change}. Use only the evidence.",
                                                  case_state(c), 400, false), "", CallKind::conclude, {}};
        c.busy = true;
        post(cfg.ornith, call, 120);
        return;
    }
    if (kind == "stop_need_more") {
        auto *call = new Call{c.def.id, chat_body("The investigation stops here. In one sentence, list the evidence that would be needed to explain the report.", case_state(c), 160, false), "", CallKind::stop_note, {}};
        c.busy = true;
        post(cfg.ornith, call, 120);
        return;
    }
    if (a->job) {
        bool baseline = kind == "compare_baseline";
        char setup = kind == "try_smaller_setup" ? 'A' : c.def.setup;
        std::string args = kind == "capture_trace" ? "--trace" : kind == "measure_memory" ? "--frames " + std::to_string(cfg.frames * 2) : "";
        emit(c, "action_requested", "{\"action_id\":" + q(action_id) + ",\"kind\":" + q(kind) + ",\"label\":" + q(std::string(a->label) + " on setup " + std::string(1, setup)) +
                                        ",\"job\":true,\"dir\":" + q(job_dir_for(action_id)) + ",\"args\":{\"setup\":\"" + std::string(1, setup) + "\"}}");
        if (!start_job(c, action_id, kind, setup, baseline, args)) fact(c, "Job " + action_id + " was already started once; not starting it again.");
        return;
    }
    emit(c, "action_requested", "{\"action_id\":" + q(action_id) + ",\"kind\":" + q(kind) + ",\"label\":" + q(a->label) + ",\"job\":false}");
    run_instant(c, action_id, kind);
}

static void on_call_done(Call *call, long http, CURLcode rc) {
    Case *c = find_case(call->case_id);
    if (!c) return;
    c->busy = false;
    yyjson_doc *doc = yyjson_read(call->response.data(), call->response.size(), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (rc != CURLE_OK || http != 200 || !doc) {
        if (call->kind != CallKind::think)
            fact(*c, std::string("Model call failed (") + (rc != CURLE_OK ? curl_easy_strerror(rc) : "HTTP " + std::to_string(http)) + "); will retry on the next step.");
        yyjson_doc_free(doc);
        return;
    }
    if (call->kind == CallKind::decide) {
        yyjson_val *probs = yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(root, "answers"), "next_action"), "probabilities");
        const char *chosen = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(root, "answers"), "next_action"), "choice"));
        std::vector<std::pair<double, std::string>> ranked;
        for (auto &k : call->options) ranked.push_back({yyjson_get_num(yyjson_obj_get(probs, k.c_str())), k});
        std::sort(ranked.rbegin(), ranked.rend());
        std::string cand = "[";
        for (auto &[p, k] : ranked) cand += (cand.size() > 1 ? "," : "") + std::string("{\"action\":") + q(k) + ",\"label\":" + q(action(k)->label) + ",\"p\":" + fmt("%.4f", p) + "}";
        double ms = yyjson_get_num(yyjson_obj_get(yyjson_obj_get(root, "timings"), "inference_ms"));
        ++decisions_total;
        decision_ms_total += ms;
        emit(*c, "decision", "{\"candidates\":" + cand + "],\"chosen\":" + q(chosen ? chosen : "") + ",\"model\":\"clef-flash\",\"inference_ms\":" + fmt("%.1f", ms) + "}");
        if (chosen && action(chosen)) execute(*c, chosen);
    } else {
        yyjson_val *msg = yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(root, "choices"), 0), "message");
        std::string content = yyjson_get_str(yyjson_obj_get(msg, "content")) ?: "";
        std::string reasoning = yyjson_get_str(yyjson_obj_get(msg, "reasoning_content")) ?: (yyjson_get_str(yyjson_obj_get(msg, "reasoning")) ?: "");
        if (call->kind == CallKind::think) {
            emit(*c, "thought", "{\"text\":" + q(content) + ",\"reasoning\":" + q(reasoning) + ",\"model\":\"ornith-35b\"}");
        } else if (call->kind == CallKind::conclude) {
            auto b = content.find('{'), e = content.rfind('}');
            std::string diag = content, fix;
            if (b != std::string::npos && e != std::string::npos) {
                yyjson_doc *j = yyjson_read(content.data() + b, e - b + 1, 0);
                if (j) {
                    diag = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(j), "diagnosis")) ?: content;
                    fix = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(j), "fix")) ?: "";
                    yyjson_doc_free(j);
                }
            }
            emit(*c, "conclusion", "{\"diagnosis\":" + q(diag) + ",\"fix\":" + q(fix) + ",\"model\":\"ornith-35b\"}");
            // Verification runs the build without the planted change: the reference fix.
            std::string action_id = c->def.id + "-" + std::to_string(c->seq + 1);
            emit(*c, "action_requested", "{\"action_id\":" + q(action_id) + ",\"kind\":\"verify_fix\",\"label\":" + q("verification run with the reference fix on setup " + std::string(1, c->def.setup)) +
                                             ",\"job\":true,\"dir\":" + q(job_dir_for(action_id)) + "}");
            start_job(*c, action_id, "verify_fix", c->def.setup, true, "");
        } else if (call->kind == CallKind::stop_note) {
            emit(*c, "stopped", "{\"reason\":\"The available tools cannot establish more.\",\"missing\":" + q(content) + "}");
        }
    }
    yyjson_doc_free(doc);
}

static void finish_verification(Case &c, const Pending &p) {
    Summary after = summary(p.dir);
    int code = std::atoi(read_file(fs::path(p.dir) / "exit").c_str());
    const Run *before = nullptr;
    for (auto &r : c.runs) if (!r.baseline && r.setup == c.def.setup) before = &r;
    Summary b = before ? summary(before->dir) : Summary{};
    std::string checks = "[" + q("Run with the reference fix exits " + std::string(code == 0 ? "cleanly" : "with code " + std::to_string(code))) +
                         (before && b.ok && after.ok ? "," + q("p99 frame " + fmt("%.1f ms", b.p99) + " -> " + fmt("%.1f ms", after.p99) + "; worst " + fmt("%.0f ms", b.max) + " -> " + fmt("%.0f ms", after.max)) +
                                                    "," + q("Resident memory " + fmt("%.0f MB", b.resident_mb) + " -> " + fmt("%.0f MB", after.resident_mb) + "; temporary data " + fmt("%.1f MB", b.tmp_mb) + " -> " + fmt("%.1f MB", after.tmp_mb))
                                              : "") + "]";
    emit(c, "action_result", "{\"action_id\":" + q(p.action_id) + ",\"source\":\"run\",\"run_dir\":" + q(p.dir) + ",\"baseline\":true,\"summary\":\"verification run finished\",\"artifacts\":" +
                                 artifacts(p.action_id, {"frames.csv", "image.png"}) + "}");
    emit(c, "done", "{\"summary\":" + q(c.diagnosis) + ",\"checks\":" + checks + ",\"verified_with\":\"reference fix (planted change removed)\"}");
}

static void tick() {
    for (auto &c : cases) {
        // Finished jobs, including ones that finished while the service was down.
        std::vector<Pending> ready;
        for (auto &[id, p] : c.pending)
            if (p.job && fs::exists(fs::path(p.dir) / "exit")) ready.push_back(p);
        for (auto &p : ready) {
            if (p.kind == "verify_fix") finish_verification(c, p);
            else { ingest_job(c, p); request_thought(c); }
        }
        if (c.status != "investigating" || c.busy || !c.pending.empty() || c.concluded) continue;
        // One renderer job at a time keeps frame-time measurements free of interference.
        if (running_jobs() > 0) continue;
        if (c.done.size() >= 9) { execute(c, "stop_need_more"); continue; }
        request_decision(c);
    }
}

// ---------------------------------------------------------------- HTTP

struct Request { std::string method, path, body; };

static void send_all(int fd, const std::string &s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) { if (errno == EINTR) continue; return; }
        off += size_t(n);
    }
}

static void respond(int fd, int status, const std::string &type, const std::string &body) {
    const char *text = status == 200 ? "OK" : status == 404 ? "Not Found" : status == 400 ? "Bad Request" : status == 413 ? "Payload Too Large" : "Error";
    send_all(fd, "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                     "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n" + body);
    close(fd);
}

static std::string case_summary(const Case &c) {
    return "{\"id\":" + q(c.def.id) + ",\"title\":" + q(c.def.title) + ",\"setup\":\"" + std::string(1, c.def.setup) + "\",\"status\":" + q(c.status) +
           ",\"waiting_for\":" + q(c.waiting_for) + ",\"updated_ms\":" + std::to_string(c.updated_ms) + ",\"event_count\":" + std::to_string(c.events.size()) + "}";
}

static size_t on_text(char *p, size_t s, size_t n, void *u) { static_cast<std::string *>(u)->append(p, s * n); return s * n; }

static double metric(const std::string &text, const char *name) {
    double total = 0;
    for (size_t p = text.find(name); p != std::string::npos; p = text.find(name, p + 1)) {
        if (p > 0 && text[p - 1] != '\n') continue;
        auto sp = text.find(' ', p);
        if (sp != std::string::npos) total += std::atof(text.c_str() + sp + 1);
    }
    return total;
}

static std::string model_json() {
    std::string text;
    CURL *h = curl_easy_init();
    curl_easy_setopt(h, CURLOPT_URL, cfg.ornith_metrics.c_str());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_text);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &text);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 300L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    bool ok = curl_easy_perform(h) == CURLE_OK;
    curl_easy_cleanup(h);
    if (!ok) return "{\"up\":false}";
    return "{\"up\":true,\"generation_tokens_total\":" + fmt("%.0f", metric(text, "vllm:generation_tokens_total")) +
           ",\"prompt_tokens_total\":" + fmt("%.0f", metric(text, "vllm:prompt_tokens_total")) +
           ",\"requests_running\":" + fmt("%.0f", metric(text, "vllm:num_requests_running")) +
           ",\"spec_draft_tokens_total\":" + fmt("%.0f", metric(text, "vllm:spec_decode_num_draft_tokens_total")) +
           ",\"spec_accepted_tokens_total\":" + fmt("%.0f", metric(text, "vllm:spec_decode_num_accepted_tokens_total")) +
           ",\"kv_cache_usage\":" + fmt("%.3f", metric(text, "vllm:kv_cache_usage_perc")) + "}";
}

static std::string system_json() {
    std::string gpu;
    if (FILE *p = popen("nvidia-smi --query-gpu=utilization.gpu,power.draw,temperature.gpu,clocks.sm --format=csv,noheader,nounits 2>/dev/null", "r")) {
        char b[256] = {};
        if (fgets(b, sizeof b, p)) gpu = b;
        pclose(p);
    }
    double util = 0, power = 0, temp = 0, clock = 0;
    std::sscanf(gpu.c_str(), "%lf, %lf, %lf, %lf", &util, &power, &temp, &clock);
    long total_kb = 0, avail_kb = 0;
    std::ifstream mi("/proc/meminfo");
    std::string k;
    long v;
    std::string unit;
    while (mi >> k >> v >> unit) { if (k == "MemTotal:") total_kb = v; if (k == "MemAvailable:") avail_kb = v; }
    int waiting = 0, investigating = 0, done = 0, stopped = 0;
    for (auto &c : cases) { waiting += c.status == "waiting"; investigating += c.status == "investigating"; done += c.status == "done"; stopped += c.status == "stopped"; }
    return "{\"gpu_util_pct\":" + fmt("%.0f", util) + ",\"gpu_power_w\":" + fmt("%.1f", power) + ",\"gpu_temp_c\":" + fmt("%.0f", temp) + ",\"gpu_clock_mhz\":" + fmt("%.0f", clock) +
           ",\"mem_total_gb\":" + fmt("%.1f", total_kb / 1e6) + ",\"mem_used_gb\":" + fmt("%.1f", (total_kb - avail_kb) / 1e6) + ",\"cases\":{\"investigating\":" + std::to_string(investigating) +
           ",\"waiting\":" + std::to_string(waiting) + ",\"done\":" + std::to_string(done) + ",\"stopped\":" + std::to_string(stopped) + "},\"running_jobs\":" + std::to_string(running_jobs()) +
           ",\"clef\":{\"decisions\":" + std::to_string(decisions_total) + ",\"mean_ms\":" + fmt("%.1f", decisions_total ? decision_ms_total / decisions_total : 0) + "}" +
           ",\"ornith\":" + model_json() + ",\"t_ms\":" + std::to_string(now_ms()) + "}";
}

static const char *mime(const fs::path &p) {
    auto e = p.extension().string();
    return e == ".html" ? "text/html" : e == ".js" ? "text/javascript" : e == ".css" ? "text/css" : e == ".png" ? "image/png" : e == ".json" ? "application/json" : e == ".svg" ? "image/svg+xml" : "text/plain";
}

static void handle(int fd, const Request &r) {
    if (r.method == "OPTIONS") {
        send_all(fd, "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST\r\nAccess-Control-Allow-Headers: Content-Type\r\nConnection: close\r\n\r\n");
        close(fd);
        return;
    }
    if (r.method == "GET" && r.path == "/api/stream") {
        send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n\r\n: connected\n\n");
        sse_clients.push_back(fd);
        return;
    }
    if (r.method == "GET" && r.path == "/api/cases") {
        std::string out = "[";
        for (auto &c : cases) out += (out.size() > 1 ? "," : "") + case_summary(c);
        return respond(fd, 200, "application/json", out + "]");
    }
    if (r.method == "GET" && r.path == "/api/system") return respond(fd, 200, "application/json", system_json());
    if (r.method == "GET" && r.path.rfind("/api/cases/", 0) == 0) {
        Case *c = find_case(r.path.substr(11));
        if (!c) return respond(fd, 404, "application/json", "{\"error\":\"no such case\"}");
        std::string s = case_summary(*c);
        s.pop_back();
        s += ",\"events\":[";
        for (size_t i = 0; i < c->events.size(); ++i) s += (i ? "," : "") + c->events[i];
        return respond(fd, 200, "application/json", s + "]}");
    }
    if (r.method == "GET" && r.path.rfind("/api/artifacts/", 0) == 0) {
        // /api/artifacts/<case>/<action_id>/<file>; only files inside the jobs directory are served.
        std::string rest = r.path.substr(15);
        auto slash = rest.find('/');
        std::string rel = slash == std::string::npos ? "" : rest.substr(slash + 1);
        if (rel.find("..") != std::string::npos || rel.empty()) return respond(fd, 400, "application/json", "{\"error\":\"bad path\"}");
        fs::path p = cfg.data / "jobs" / rel;
        if (!fs::is_regular_file(p)) return respond(fd, 404, "application/json", "{\"error\":\"no such artifact\"}");
        return respond(fd, 200, mime(p), read_file(p, 256u << 20));
    }
    if (r.method == "POST" && r.path == "/api/demo/load") {
        int opened = 0;
        for (auto &[id, d] : defs) if (!find_case(id)) { open_case(d); ++opened; }
        return respond(fd, 200, "application/json", "{\"opened\":" + std::to_string(opened) + "}");
    }
    if (r.method == "POST" && r.path == "/api/reset") {
        // Archive every case log and job; memory starts empty. Nothing is deleted.
        if (running_jobs() > 0) return respond(fd, 400, "application/json", "{\"error\":\"jobs are still running\"}");
        fs::path archive = cfg.data.parent_path() / ("archive-" + std::to_string(now_ms()));
        std::error_code ec;
        fs::rename(cfg.data, archive, ec);
        cases.clear();
        load_cases();
        return respond(fd, 200, "application/json", "{\"archived_to\":" + q(archive.string()) + "}");
    }
    if (r.method == "POST" && r.path == "/api/cases") {
        yyjson_doc *doc = yyjson_read(r.body.data(), r.body.size(), 0);
        yyjson_val *b = yyjson_doc_get_root(doc);
        const char *title = yyjson_get_str(yyjson_obj_get(b, "title")), *report = yyjson_get_str(yyjson_obj_get(b, "report")), *setup = yyjson_get_str(yyjson_obj_get(b, "setup"));
        if (!title || !report) { yyjson_doc_free(doc); return respond(fd, 400, "application/json", "{\"error\":\"title and report are required\"}"); }
        CaseDef d{"case-" + std::to_string(100 + cases.size()), title, report, "", setup && *setup >= 'A' && *setup <= 'E' ? *setup : 'A'};
        yyjson_doc_free(doc);
        return respond(fd, 200, "application/json", case_summary(open_case(d)));
    }
    if (r.method == "GET" && (r.path == "/dashboard" || r.path.rfind("/dashboard/", 0) == 0)) {
        std::string rel = r.path.size() <= 11 ? "index.html" : r.path.substr(11);
        fs::path p = cfg.dashboard / rel;
        if (rel.find("..") == std::string::npos && fs::is_regular_file(p)) return respond(fd, 200, mime(p), read_file(p));
    }
    if (r.method == "GET") {
        fs::path p = cfg.ui / (r.path == "/" ? "index.html" : r.path.substr(1));
        if (r.path.find("..") == std::string::npos && fs::is_regular_file(p)) return respond(fd, 200, mime(p), read_file(p));
    }
    respond(fd, 404, "application/json", "{\"error\":\"not found\"}");
}

// Reads one complete request (headers plus Content-Length body) from a client socket. Bodies over 256 KiB are refused.
struct Conn { int fd; std::string buf; };

static bool parse(Conn &cn, Request &r, bool &too_big) {
    auto end = cn.buf.find("\r\n\r\n");
    if (end == std::string::npos) { too_big = cn.buf.size() > 16384; return false; }
    std::string head = cn.buf.substr(0, end);
    size_t len = 0;
    for (size_t p = head.find("\r\n"); p != std::string::npos; p = head.find("\r\n", p + 2)) {
        std::string h = head.substr(p + 2, head.find("\r\n", p + 2) - p - 2);
        if (strncasecmp(h.c_str(), "Content-Length:", 15) == 0) len = size_t(std::atol(h.c_str() + 15));
    }
    if (len > (256u << 10)) { too_big = true; return false; }
    if (cn.buf.size() < end + 4 + len) return false;
    std::istringstream line(head.substr(0, head.find("\r\n")));
    line >> r.method >> r.path;
    if (auto qm = r.path.find('?'); qm != std::string::npos) r.path.resize(qm);
    r.body = cn.buf.substr(end + 4, len);
    return true;
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN); // detached jobs are reaped automatically; completion is read from their exit files
    curl_global_init(CURL_GLOBAL_DEFAULT);
    multi = curl_multi_init();
    load_defs();
    load_cases();

    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(cfg.port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (sockaddr *)&addr, sizeof addr) != 0 || listen(ls, 64) != 0) { std::perror("bind/listen"); return 1; }
    std::fprintf(stderr, "romeo: %zu cases loaded, listening on :%d\n", cases.size(), cfg.port);

    std::vector<Conn> conns;
    auto last_tick = Clock::now();
    for (;;) {
        std::vector<curl_waitfd> fds;
        fds.push_back({ls, CURL_WAIT_POLLIN, 0});
        for (auto &c : conns) fds.push_back({c.fd, CURL_WAIT_POLLIN, 0});
        int n = 0;
        curl_multi_poll(multi, fds.data(), unsigned(fds.size()), 250, &n);

        if (fds[0].revents & CURL_WAIT_POLLIN) {
            int fd = accept(ls, nullptr, nullptr);
            if (fd >= 0) conns.push_back({fd, {}});
        }
        for (size_t i = 1; i < fds.size(); ++i) {
            if (!(fds[i].revents & CURL_WAIT_POLLIN)) continue;
            Conn &cn = conns[i - 1];
            char buf[16384];
            ssize_t got = recv(cn.fd, buf, sizeof buf, 0);
            if (got <= 0) { close(cn.fd); cn.fd = -1; continue; }
            cn.buf.append(buf, size_t(got));
            Request r;
            bool too_big = false;
            if (parse(cn, r, too_big)) { int fd = cn.fd; cn.fd = -1; handle(fd, r); }
            else if (too_big) { respond(cn.fd, 413, "application/json", "{\"error\":\"request too large\"}"); cn.fd = -1; }
        }
        conns.erase(std::remove_if(conns.begin(), conns.end(), [](const Conn &c) { return c.fd < 0; }), conns.end());

        int running = 0;
        curl_multi_perform(multi, &running);
        int left = 0;
        while (CURLMsg *m = curl_multi_info_read(multi, &left)) {
            if (m->msg != CURLMSG_DONE) continue;
            long http = 0;
            curl_easy_getinfo(m->easy_handle, CURLINFO_RESPONSE_CODE, &http);
            Call *call = calls[m->easy_handle];
            calls.erase(m->easy_handle);
            CURLcode rc = m->data.result;
            curl_multi_remove_handle(multi, m->easy_handle);
            curl_easy_cleanup(m->easy_handle);
            on_call_done(call, http, rc);
            curl_slist_free_all(call->headers);
            delete call;
        }

        if (Clock::now() - last_tick > std::chrono::milliseconds(250)) {
            last_tick = Clock::now();
            tick();
        }
    }
}
