// romeo: the always-on case service.
//
// Threading model
// ---------------
// One reactor thread owns everything: the listen socket, the fixed connection slots, the Server-Sent-Event
// subscribers, the curl multi handle (model calls and the 1 s vLLM /metrics scrape), the WAL, the case store,
// renderer and script job supervision, and the 1 s system sampler (nvidia-smi through fork + a non-blocking pipe,
// /proc/meminfo through open/read). All waiting happens in one curl_multi_poll over the curl sockets plus our own
// fds. GET /api/system returns the last snapshot and never blocks.
//
// quill runs its own backend thread for operational logging. Renderer and script jobs are separate detached
// processes (fork, setsid, exec) that survive a restart of this service. There are no other threads, no locks, and
// no atomics in this code.
//
// Reactor iteration
// -----------------
//   1. poll       curl_multi_poll over curl sockets, the listen socket, connections and the sampler pipe
//   2. read       accept, read requests, run routes (routes may append events)
//   3. models     finished model calls (may append events and queue jobs)
//   4. sample     sampler pipe, metrics scrape, routing deadlines
//   5. tick       every 250 ms: finished jobs, timeouts, next decisions
//   6. commit     WAL write + sync once when dirty, then stream committed events, then start queued jobs
//   7. flush      write responses and stream backlogs
// Nothing leaves the process before step 6 makes the events it depends on durable.
//
// Memory
// ------
// One anonymous mapping is reserved at start-up and split into fixed regions: case store, per-request scratch,
// evidence scratch, model call slots, connection slots, and the WAL write buffer. Capacity exhaustion is an HTTP
// 413/503 or a logged refusal, never a heap fallback.
//
// Send-once rule: a job action first commits its action_requested record, then creates outbox/<action_id>.json with
// O_EXCL, then starts a detached process that writes <job>/exit when it finishes. After a restart the service never
// starts the job again; it waits for the exit file of the job that is already running or finished.

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "quill/Backend.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/FileSink.h"

#include "agent.hpp"
#include "evidence.hpp"
#include "http.hpp"
#include "log.hpp"
#include "models.hpp"
#include "wal.hpp"

namespace romeo {

Logger *g_log = nullptr;

int64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool log_start(std::string_view file_path) {
    quill::BackendOptions backend;
    quill::Backend::start(backend);
    quill::ConsoleSinkConfig console;
    console.set_stream("stdout");
    console.set_colour_mode(quill::ConsoleSinkConfig::ColourMode::Never);
    auto out = LogFrontend::create_or_get_sink<quill::ConsoleSink>("stdout", console);
    quill::FileSinkConfig file_cfg;
    file_cfg.set_open_mode('a');
    auto file = LogFrontend::create_or_get_sink<quill::FileSink>(std::string(file_path), file_cfg);
    g_log = LogFrontend::create_or_get_logger("romeo", {out, file}, quill::PatternFormatterOptions{"%(time) %(log_level:<7) %(message)"});
    return g_log != nullptr;
}

void log_flush() {
    if (g_log) g_log->flush_log();
}

namespace {

// ---------------------------------------------------------------- owned state

CaseStore g_store;
Wal g_wal;
Models g_models;
Http g_http;
Sampler g_sampler;
SpawnQueue g_spawns;
ScriptTools g_scripts;
Service g_service;

// ---------------------------------------------------------------- configuration

template <uint32_t N>
bool env(FixedString<N> &out, const char *name, std::string_view fallback) {
    const char *v = std::getenv(name);
    std::string_view value = v && *v ? std::string_view(v) : fallback;
    while (value.size() > 1 && value.back() == '/') value.remove_suffix(1); // "a/" / "b" is "a/b"
    return out.assign(value);
}

int env_int(const char *name, int fallback) {
    const char *v = std::getenv(name);
    return v && *v ? int(parse_leading_int(v)) : fallback;
}

// Like fs::absolute: a relative path is joined to the working directory without normalisation.
bool absolute(FixedString<kPathBytes> &out, const FixedString<kPathBytes> &path) {
    if (path.view().substr(0, 1) == "/") return out.assign(path.view());
    char cwd[kPathBytes];
    if (!getcwd(cwd, sizeof cwd)) return false;
    FixedStringBuilder<kPathBytes> b;
    cat(b, std::string_view(cwd), '/', path);
    return !b.overflowed() && out.assign(b.view());
}

bool load_config(Config &c) {
    // Derived paths add at most "/jobs/<action id>/<file name>" (about 200 bytes) to the data directory.
    constexpr size_t kMaxRoot = kPathBytes - 256;
    bool ok = env(c.data, "ROMEO_DATA", "runs/data") && c.data.len < kMaxRoot;
    ok = ok && env(c.render, "ROMEO_RENDER", "build/romeo-render") && env(c.cases_file, "ROMEO_CASES", "cases/cases.json");
    ok = ok && env(c.repo_dir, "ROMEO_REPO_DIR", "renderer") && c.repo_dir.len < kMaxRoot;
    ok = ok && env(c.ui, "ROMEO_UI", "ui/dist") && c.ui.len < kMaxRoot && env(c.dashboard, "ROMEO_DASHBOARD", "dashboard") && c.dashboard.len < kMaxRoot;
    ok = ok && env(c.tools, "ROMEO_TOOLS", "tools") && c.tools.len < kMaxRoot;
    ok = ok && env(c.ornith_metrics, "ORNITH_METRICS", "http://127.0.0.1:8000/metrics") && env(c.clef, "CLEF_URL", "http://127.0.0.1:8001/v1/systemone");
    ok = ok && env(c.ornith, "ORNITH_URL", "http://127.0.0.1:8000/v1/chat/completions") && env(c.memory, "MEMORY_URL", "http://127.0.0.1:8791/similar");
    const char *think = std::getenv("ROMEO_THINK");
    c.think = std::string_view(think && *think ? think : "1") == "1";
    c.port = env_int("ROMEO_PORT", 8790);
    c.frames = env_int("ROMEO_FRAMES", 300);
    ok = ok && absolute(c.render_abs, c.render) && absolute(c.tools_abs, c.tools) && c.tools_abs.len < kMaxRoot;
    return ok;
}

// ---------------------------------------------------------------- start-up reservation

struct Regions {
    size_t case_store, request, evidence, model_slots, conn_slots, wal_buffer;
    size_t total() const { return case_store + request + evidence + model_slots + conn_slots + wal_buffer + 6 * 4096; }
};

constexpr Regions kRegions{
    Arena::budget<Case>(kMaxCases),
    kRequestArenaBytes,
    kEvidenceArenaBytes,
    Arena::budget<ModelCall>(kModelSlots) + Arena::budget<FixedStringBuilder<kMetricsBytes>>(),
    Arena::budget<Conn>(kConnSlots),
    kWalBufferBytes,
};

bool reserve(Service &s, Arena &model_slots, Arena &conn_slots) {
    const size_t total = kRegions.total();
    void *base = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) {
        RLOG_ERROR("start-up: cannot reserve {} bytes: {}", total, std::string_view(std::strerror(errno)));
        return false;
    }
    Arena all(base, total);
    auto region = [&](size_t bytes) { return Arena(all.alloc_bytes(bytes, 4096), bytes); };
    Arena case_store = region(kRegions.case_store);
    s.request = region(kRegions.request);
    s.evidence = region(kRegions.evidence);
    model_slots = region(kRegions.model_slots);
    conn_slots = region(kRegions.conn_slots);
    Arena wal = region(kRegions.wal_buffer);
    if (!case_store.base || !s.request.base || !s.evidence.base || !model_slots.base || !conn_slots.base || !wal.base) return false;
    if (!s.store->cases.init(case_store, kMaxCases)) return false;
    wal_init(*s.wal, wal.base, uint32_t(kRegions.wal_buffer));

    RLOG_INFO("start-up: reserved {} bytes: case store {} ({} cases of {} bytes), request {}, evidence {}, model slots {} ({} x {}), "
              "connection slots {} ({} x {}), WAL buffer {}",
              total, kRegions.case_store, kMaxCases, sizeof(Case), kRegions.request, kRegions.evidence, kRegions.model_slots, kModelSlots,
              sizeof(ModelCall), kRegions.conn_slots, kConnSlots, sizeof(Conn), kRegions.wal_buffer);
    return true;
}

// ---------------------------------------------------------------- system sampler

const char *const kSmiArgs[] = {"nvidia-smi", "--query-gpu=utilization.gpu,power.draw,temperature.gpu,clocks.sm", "--format=csv,noheader,nounits",
                                nullptr};

void sampler_init(Sampler &m) {
    const char *path = std::getenv("PATH");
    std::string_view rest = path ? path : "/usr/bin:/bin";
    while (!rest.empty()) {
        size_t colon = rest.find(':');
        std::string_view dir = rest.substr(0, colon);
        rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
        FixedStringBuilder<kPathBytes> candidate;
        cat(candidate, dir, "/nvidia-smi");
        if (!dir.empty() && !candidate.overflowed() && access(candidate.c_str(), X_OK) == 0) {
            m.nvidia_smi.assign(candidate.view());
            break;
        }
    }
    if (m.nvidia_smi.empty()) RLOG_INFO("sampler: nvidia-smi not found on PATH; GPU fields stay 0");
}

void read_meminfo(Sampler &m) {
    m.mem_total_kb = 0;
    m.mem_avail_kb = 0;
    int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    std::string_view text(buf, size_t(n));
    auto field = [&](std::string_view key) -> long {
        size_t p = text.find(key);
        while (p != std::string_view::npos && p > 0 && text[p - 1] != '\n') p = text.find(key, p + 1);
        return p == std::string_view::npos ? 0 : long(parse_leading_int(text.substr(p + key.size())));
    };
    m.mem_total_kb = field("MemTotal:");
    m.mem_avail_kb = field("MemAvailable:");
}

// sscanf("%lf, %lf, %lf, %lf") semantics: values parsed before the first mismatch are kept, the rest stay 0.
void parse_gpu_line(Sampler &m) {
    double v[4] = {0, 0, 0, 0};
    int parsed = 0;
    std::string_view rest(m.line, m.line_len);
    for (int k = 0; k < 4; ++k) {
        size_t b = rest.find_first_not_of(" \t\r\n");
        if (b == std::string_view::npos) break;
        rest = rest.substr(b);
        double x = 0;
        auto r = std::from_chars(rest.data(), rest.data() + rest.size(), x);
        if (r.ec != std::errc()) break;
        v[k] = x;
        ++parsed;
        rest = rest.substr(size_t(r.ptr - rest.data()));
        if (k < 3) {
            if (rest.empty() || rest[0] != ',') break;
            rest = rest.substr(1);
        }
    }
    m.gpu_util = v[0];
    m.gpu_power = v[1];
    m.gpu_temp = v[2];
    m.gpu_clock = v[3];
    m.gpu_available = parsed == 4;
    if (m.gpu_available) m.gpu_sample_ms = now_ms();
}

void sampler_finish(Sampler &m) {
    if (m.pipe_fd >= 0) close(m.pipe_fd);
    m.pipe_fd = -1;
    m.pid = -1;
    parse_gpu_line(m);
}

void sampler_tick(Sampler &m, int64_t now) {
    if (m.pipe_fd >= 0) {
        if (now - m.started_ms > 5000) { // Keep the last values for inspection, but mark them unavailable.
            kill(m.pid, SIGKILL);
            m.gpu_available = false;
            close(m.pipe_fd);
            m.pipe_fd = -1;
            m.pid = -1;
            RLOG_WARN("sampler: nvidia-smi did not answer within 5 s; killed");
        }
        return;
    }
    if (now < m.next_ms) return;
    m.next_ms = now + 1000;
    read_meminfo(m);
    if (m.nvidia_smi.empty()) return;

    int fds[2];
    if (pipe(fds) != 0) return;
    int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    const char *path = m.nvidia_smi.c_str();
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        if (devnull >= 0) dup2(devnull, 2);
        for (int f = 3; f < 1024; ++f) close(f);
        execv(path, const_cast<char *const *>(kSmiArgs));
        _exit(127);
    }
    close(fds[1]);
    if (devnull >= 0) close(devnull);
    if (pid < 0) {
        close(fds[0]);
        return;
    }
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    m.pid = pid;
    m.pipe_fd = fds[0];
    m.line_len = 0;
    m.started_ms = now;
}

// Keeps the first line (at most 255 bytes, like fgets into a 256-byte buffer) and finishes at end of file.
void sampler_on_readable(Sampler &m) {
    char buf[1024];
    for (;;) {
        ssize_t n = read(m.pipe_fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return; // EAGAIN: more later
        if (n == 0) {
            sampler_finish(m);
            return;
        }
        for (ssize_t i = 0; i < n && m.line_len < sizeof m.line - 1; ++i) {
            if (memchr(m.line, '\n', m.line_len)) break;
            m.line[m.line_len++] = buf[i];
        }
    }
}

// ---------------------------------------------------------------- reactor

int run() {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN); // detached jobs are reaped automatically; completion is read from their exit files

    Service &s = g_service;
    s.store = &g_store;
    s.wal = &g_wal;
    s.models = &g_models;
    s.http = &g_http;
    s.sampler = &g_sampler;
    s.spawns = &g_spawns;
    s.scripts = &g_scripts;

    if (!load_config(s.cfg)) {
        static constexpr char kMsg[] = "romeo: configuration paths or URLs are too long\n";
        ssize_t ignored = write(2, kMsg, sizeof kMsg - 1);
        (void)ignored;
        return 1;
    }
    make_dirs(s.cfg.data.view());
    FixedStringBuilder<kPathBytes> log_path;
    cat(log_path, s.cfg.data, "/romeo.log");
    if (!log_start(log_path.view())) return 1;

    Arena model_slots, conn_slots;
    if (!reserve(s, model_slots, conn_slots)) return 1;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (!models_init(s, model_slots) || !http_init(s, conn_slots)) {
        log_flush();
        return 1;
    }
    sampler_init(*s.sampler);

    RLOG_INFO("cases: {} definitions loaded from {}", load_defs(s), s.cfg.cases_file.view());
    load_script_tools(s);
    if (!recover(s)) {
        log_flush();
        return 1;
    }
    commit_iteration(s); // restart markers become durable before anything else happens
    RLOG_INFO("romeo: {} cases loaded, listening on :{}", s.store->cases.count, s.cfg.port);

    curl_waitfd fds[kConnSlots + 2];
    int64_t next_tick = now_ms() + 250;
    for (;;) {
        // 1. poll
        int64_t now = now_ms();
        int64_t wait = next_tick - now;
        if (s.wal->dirty() || s.spawns->items.count) wait = 0;
        int timeout = int(wait < 0 ? 0 : wait > 250 ? 250 : wait);
        uint32_t n = http_poll_fds(s, fds, kConnSlots + 1);
        const uint32_t sampler_index = n;
        if (s.sampler->pipe_fd >= 0) fds[n++] = {s.sampler->pipe_fd, CURL_WAIT_POLLIN, 0};
        curl_multi_poll(s.models->multi, fds, n, timeout, nullptr);

        // 2. read
        http_on_poll(s, fds, sampler_index);

        // 3. models
        models_poll(s);

        // 4. sample
        if (sampler_index < n && (fds[sampler_index].revents & CURL_WAIT_POLLIN)) sampler_on_readable(*s.sampler);
        now = now_ms();
        scrape_metrics_if_due(s, now);
        sampler_tick(*s.sampler, now);
        http_check_deadlines(s, now);

        // 5. tick
        if (now >= next_tick) {
            agent_tick(s);
            next_tick = now_ms() + 250;
        }

        // 6. commit, 7. flush
        commit_iteration(s);
        http_flush(s);
        s.request.reset();
    }
}

} // namespace
} // namespace romeo

int main() { return romeo::run(); }
