#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include "http.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agent.hpp"
#include "evidence.hpp"
#include "log.hpp"

namespace romeo {

using Body = FixedStringBuilder<kConnOutBytes>;

struct Request {
    std::string_view method, path, body;
};

// ---------------------------------------------------------------- slots

static void set_nonblocking_cloexec(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

bool http_init(Service &s, Arena &slots) {
    Http &h = *s.http;
    if (!h.conns.init(slots, kConnSlots)) return false;
    h.conns.count = kConnSlots; // every slot exists from start-up; state marks use

    h.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(h.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(s.cfg.port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(h.listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 || listen(h.listen_fd, 64) != 0) {
        RLOG_ERROR("http: cannot listen on port {}: {}", s.cfg.port, std::string_view(std::strerror(errno)));
        return false;
    }
    set_nonblocking_cloexec(h.listen_fd);
    return true;
}

static void close_conn(Http &h, Conn &c) {
    if (c.fd >= 0) close(c.fd);
    if (c.file_fd >= 0) close(c.file_fd);
    c.fd = -1;
    c.file_fd = -1;
    c.file_left = 0;
    c.state = ConnState::free;
    c.generation = h.next_generation++; // late completions for this slot are dropped
}

// ---------------------------------------------------------------- responses

static std::string_view status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 404: return "Not Found";
    case 400: return "Bad Request";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Error";
    }
}

static void respond_head(Conn &c, int status, std::string_view type, uint64_t length) {
    c.out.clear();
    c.out_sent = 0;
    cat(c.out, "HTTP/1.1 ", status, ' ', status_text(status), "\r\nContent-Type: ", type, "\r\nContent-Length: ", length,
        "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
    c.state = ConnState::writing;
}

static void respond(Conn &c, int status, std::string_view type, std::string_view body) {
    respond_head(c, status, type, body.size());
    c.out.add(body);
    if (c.out.overflowed()) {
        respond_head(c, 413, "application/json", 39);
        c.out.add("{\"error\":\"response exceeds the buffer\"}");
    }
}

static void respond_json(Conn &c, int status, std::string_view body) { respond(c, status, "application/json", body); }
static void respond_too_large(Conn &c) { respond_json(c, 413, "{\"error\":\"response too large\"}"); }

static std::string_view mime(std::string_view path) {
    std::string_view name = path.substr(path.rfind('/') == std::string_view::npos ? 0 : path.rfind('/') + 1);
    size_t dot = name.rfind('.');
    std::string_view e = dot == std::string_view::npos || dot == 0 ? std::string_view() : name.substr(dot);
    return e == ".html" ? "text/html" : e == ".js" ? "text/javascript" : e == ".css" ? "text/css" : e == ".png" ? "image/png"
           : e == ".json" ? "application/json" : e == ".svg" ? "image/svg+xml" : "text/plain";
}

// Streams a regular file. Files longer than limit are cut at limit, like the original read_file().
static bool respond_file(Conn &c, const char *path, uint64_t limit) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return false;
    }
    const uint64_t length = std::min<uint64_t>(uint64_t(st.st_size), limit);
    respond_head(c, 200, mime(path), length);
    c.file_fd = fd;
    c.file_left = length;
    return true;
}

// ---------------------------------------------------------------- JSON pieces

static void write_case_summary(Body &b, const Case &c, bool close_object = true) {
    cat(b, "{\"id\":", json_string(c.def.id.view()), ",\"title\":", json_string(c.def.title.view()), ",\"setup\":\"", c.def.setup, "\",\"status\":",
        json_string(status_name(c.status)), ",\"waiting_for\":", json_string(c.waiting_for.view()), ",\"updated_ms\":", c.updated_ms,
        ",\"event_count\":", c.events.count);
    if (close_object) b.add('}');
}

// Copies one event payload from the WAL into the body.
static bool append_event(const Service &s, Body &b, const EventRef &e) {
    if (b.len + e.length + 2 >= kConnOutBytes) return false;
    if (!wal_read(*s.wal, e.payload_offset, e.length, b.data + b.len)) return false;
    b.len += e.length;
    b.data[b.len] = 0;
    return true;
}

static Body *body_in_arena(Service &s) { return s.request.alloc<Body>(); }

// Live renderer and tool processes, read from /proc (Linux only): the dashboard's proof that real work runs.
static void processes_json(Body &b) {
    b.add("[");
#if defined(__linux__)
    DIR *d = opendir("/proc");
    long ticks = sysconf(_SC_CLK_TCK), page_kb = sysconf(_SC_PAGESIZE) / 1024;
    double uptime = 0;
    if (FILE *u = fopen("/proc/uptime", "r")) { char t[64] = {}; if (fgets(t, sizeof t, u)) uptime = parse_leading_double(t); fclose(u); }
    int shown = 0;
    while (struct dirent *e = d ? readdir(d) : nullptr) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9' || shown >= 12) continue;
        char path[64], cmd[512] = {}, stat[1024] = {}, statm[128] = {};
        FixedStringBuilder<64> pb;
        auto slurp = [&](const char *name, char *out, size_t cap) {
            pb.clear();
            cat(pb, "/proc/", std::string_view(e->d_name), "/", std::string_view(name));
            int fd = open(pb.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) return ssize_t(0);
            ssize_t n = read(fd, out, cap - 1);
            close(fd);
            return n < 0 ? ssize_t(0) : n;
        };
        (void)path;
        ssize_t n = slurp("cmdline", cmd, sizeof cmd);
        if (n <= 0) continue;
        for (ssize_t i = 0; i + 1 < n; ++i) if (!cmd[i]) cmd[i] = ' ';
        std::string_view line(cmd, size_t(n) - (cmd[n - 1] ? 0 : 1));
        bool render = line.find("romeo-render") != std::string_view::npos && line.find("sh -c") == std::string_view::npos;
        bool tool = line.find("/tools/") != std::string_view::npos && line.find("python3") != std::string_view::npos;
        if (!render && !tool) continue;
        slurp("stat", stat, sizeof stat);
        slurp("statm", statm, sizeof statm);
        // stat field 22 is the start time in clock ticks; fields are counted after the ")" that ends the command name.
        const char *p = strrchr(stat, ')');
        double start = 0;
        for (int field = 2; p && *p; ++p)
            if (*p == ' ' && ++field == 22) { start = parse_leading_double(p + 1); break; }
        const char *rss = strchr(statm, ' ');
        double rss_mb = rss ? parse_leading_double(rss + 1) * double(page_kb) / 1024.0 : 0;
        cat(b, shown ? "," : "", "{\"pid\":", std::string_view(e->d_name), ",\"cmd\":", json_string(line.substr(0, 300)),
            ",\"elapsed_s\":", decimal(ticks > 0 ? uptime - start / double(ticks) : 0, 1), ",\"rss_mb\":", decimal(rss_mb, 0), "}");
        ++shown;
    }
    if (d) closedir(d);
#endif
    b.add("]");
}

static void system_json(Service &s, Body &b) {
    const Sampler &m = *s.sampler;
    const CaseStore &store = *s.store;
    int waiting = 0, investigating = 0, done = 0, stopped = 0;
    for (const Case &c : store.cases) {
        waiting += c.status == Status::waiting;
        investigating += c.status == Status::investigating;
        done += c.status == Status::done;
        stopped += c.status == Status::stopped;
    }
    cat(b, "{\"gpu_util_pct\":", decimal(m.gpu_util, 0), ",\"gpu_power_w\":", decimal(m.gpu_power, 1), ",\"gpu_temp_c\":", decimal(m.gpu_temp, 0),
        ",\"gpu_clock_mhz\":", decimal(m.gpu_clock, 0), ",\"mem_total_gb\":", decimal(double(m.mem_total_kb) / 1e6, 1), ",\"mem_used_gb\":",
        decimal(double(m.mem_total_kb - m.mem_avail_kb) / 1e6, 1), ",\"cases\":{\"investigating\":", investigating, ",\"waiting\":", waiting,
        ",\"done\":", done, ",\"stopped\":", stopped, "},\"running_jobs\":", running_jobs(store), ",\"clef\":{\"decisions\":", store.decisions_total,
        ",\"mean_ms\":", decimal(store.decisions_total ? store.decision_ms_total / store.decisions_total : 0, 1), "},\"ornith\":");
    const OrnithSnapshot &o = s.models->ornith;
    if (!o.up) b.add("{\"up\":false}");
    else
        cat(b, "{\"up\":true,\"generation_tokens_total\":", decimal(o.generation_tokens, 0), ",\"prompt_tokens_total\":", decimal(o.prompt_tokens, 0),
            ",\"requests_running\":", decimal(o.requests_running, 0), ",\"spec_draft_tokens_total\":", decimal(o.spec_draft, 0),
            ",\"spec_accepted_tokens_total\":", decimal(o.spec_accepted, 0), ",\"kv_cache_usage\":", decimal(o.kv_cache_usage, 3), "}");
    cat(b, ",\"jobs\":{\"running\":", job_processes_running(s), ",\"queued\":", s.spawns->items.count, "}");
    cat(b, ",\"wal\":{\"lsn\":", s.wal->durable_lsn, ",\"bytes\":", s.wal->durable_end, ",\"fsyncs\":", s.wal->syncs, "}");
    b.add(",\"processes\":");
    processes_json(b);
    cat(b, ",\"t_ms\":", now_ms(), "}");
}

// ---------------------------------------------------------------- case views built from WAL events

// Reads each event of the case into a reusable buffer, parses it in its own scope, and calls f(event, raw).
template <typename F>
static bool for_each_event(Service &s, const Case &c, F &&f) {
    char *buf = static_cast<char *>(s.request.alloc_bytes(kMaxEventBytes + 1, 16));
    if (!buf) return false;
    for (const EventRef &e : c.events) {
        if (!wal_read(*s.wal, e.payload_offset, e.length, buf)) return false;
        ArenaScope scope(s.request);
        std::string_view raw(buf, e.length);
        yyjson_doc *doc = parse_json(s.request, raw);
        if (!doc) return false;
        if (!f(yyjson_doc_get_root(doc), raw)) return false;
    }
    return true;
}

// Chrome trace of the case's agent activity: tid 1 Clef decisions, 2 Ornith calls, 3 renderer and script jobs,
// 4 evidence tools. Timestamps are microseconds after case_opened.
static bool case_trace(Service &s, const Case &c, Body &b) {
    struct OpenSpan {
        FixedString<kNameBytes> action_id, kind;
        int64_t start;
        bool job;
    };
    FixedArray<OpenSpan, kMaxPendingPerCase * 4> open;
    FixedArray<int64_t, 16> thought_starts; // job results waiting for their thought, oldest first
    int64_t t0 = -1, prev = 0, last = 0, conclude_start = -1, stop_start = -1;
    bool first = true;
    auto us = [&](double t) { return decimal((t - double(t0)) * 1000.0, 1); };
    auto sep = [&] {
        if (!first) b.add(",\n");
        first = false;
    };
    auto span = [&](std::string_view name, int tid, double start, double dur_us) {
        sep();
        cat(b, "{\"name\":", json_string(name), ",\"ph\":\"X\",\"pid\":1,\"tid\":", tid, ",\"ts\":", us(start), ",\"dur\":", decimal(dur_us, 1), ",\"args\":");
    };
    auto instant = [&](std::string_view name, int tid, double t, bool process) {
        sep();
        cat(b, "{\"name\":", json_string(name), ",\"ph\":\"i\",\"s\":\"", process ? 'p' : 't', "\",\"pid\":1,\"tid\":", tid, ",\"ts\":", us(t), ",\"args\":");
    };

    b.add("{\"traceEvents\":[");
    static constexpr std::string_view kTracks[] = {"Clef decisions", "Ornith", "renderer jobs", "evidence tools"};
    for (int tid = 1; tid <= 4; ++tid) {
        sep();
        cat(b, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":", tid, ",\"args\":{\"name\":", json_string(kTracks[tid - 1]), "}}");
    }
    sep();
    cat(b, "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"tid\":1,\"args\":{\"name\":", json_string(c.def.id.view()), "}}");

    bool ok = for_each_event(s, c, [&](yyjson_val *ev, std::string_view) {
        std::string_view type = json_str(ev, "type");
        yyjson_val *d = yyjson_obj_get(ev, "data");
        const int64_t t = yyjson_get_sint(yyjson_obj_get(ev, "t_ms"));
        if (t0 < 0) t0 = t;
        last = t;

        if (type == "decision") {
            const double ms = yyjson_get_num(yyjson_obj_get(d, "inference_ms"));
            std::string_view chosen = json_str(d, "chosen");
            span("decision", 1, double(t) - ms, ms * 1000.0); // the decision event marks the end of the span
            cat(b, "{\"chosen\":", json_string(chosen), ",\"p\":{");
            size_t i, n;
            yyjson_val *cand;
            yyjson_arr_foreach(yyjson_obj_get(d, "candidates"), i, n, cand)
                cat(b, i ? "," : "", json_string(json_str(cand, "action")), ':', decimal(yyjson_get_num(yyjson_obj_get(cand, "p")), 4));
            b.add("}}}");
            if (chosen == "conclude") conclude_start = t;
            if (chosen == "stop_need_more") stop_start = t;
        } else if (type == "action_requested") {
            if (OpenSpan *o = open.push()) {
                o->action_id.assign(json_str(d, "action_id"));
                o->kind.assign(json_str(d, "kind"));
                o->start = t;
                o->job = yyjson_get_bool(yyjson_obj_get(d, "job"));
            }
        } else if (type == "action_result") {
            std::string_view id = json_str(d, "action_id");
            for (uint32_t k = 0; k < open.count; ++k) {
                if (!(open[k].action_id == id)) continue;
                const OpenSpan o = open[k];
                open.remove_swap(k);
                span(o.kind.view(), o.job ? 3 : 4, double(o.start), double(t - o.start) * 1000.0);
                cat(b, "{\"action_id\":", json_string(id), ",\"source\":", json_string(json_str(d, "source")), ",\"summary\":",
                    json_string(json_str(d, "summary")), "}}");
                if (o.job && !(o.kind == "verify_fix") && !thought_starts.full()) thought_starts.push(t);
                break;
            }
        } else if (type == "thought") {
            int64_t start = prev;
            if (thought_starts.count) {
                start = thought_starts[0];
                for (uint32_t k = 1; k < thought_starts.count; ++k) thought_starts[k - 1] = thought_starts[k];
                --thought_starts.count;
            }
            span("thought", 2, double(start), double(t - start) * 1000.0);
            cat(b, "{\"text\":", json_string(json_str(d, "text")), "}}");
        } else if (type == "conclusion") {
            const int64_t start = conclude_start >= 0 ? conclude_start : prev;
            span("conclusion", 2, double(start), double(t - start) * 1000.0);
            cat(b, "{\"diagnosis\":", json_string(json_str(d, "diagnosis")), ",\"fix\":", json_string(json_str(d, "fix")), "}}");
        } else if (type == "stopped") {
            const int64_t start = stop_start >= 0 ? stop_start : prev;
            span("stop_note", 2, double(start), double(t - start) * 1000.0);
            cat(b, "{\"missing\":", json_string(json_str(d, "missing")), "}}");
            instant("stopped", 1, double(t), true);
            cat(b, "{\"reason\":", json_string(json_str(d, "reason")), ",\"missing\":", json_string(json_str(d, "missing")), "}}");
        } else if (type == "fact") {
            instant("fact", 4, double(t), false);
            cat(b, "{\"text\":", json_string(json_str(d, "text")), "}}");
        } else if (type == "restarted") {
            instant("restarted", 1, double(t), true);
            b.add("{\"pending\":[");
            size_t i, n;
            yyjson_val *p;
            yyjson_arr_foreach(yyjson_obj_get(d, "pending"), i, n, p) cat(b, i ? "," : "", json_string(yyjson_get_str(p) ? yyjson_get_str(p) : ""));
            b.add("]}}");
        } else if (type == "done") {
            instant("done", 1, double(t), true);
            cat(b, "{\"summary\":", json_string(json_str(d, "summary")), ",\"verified_with\":", json_string(json_str(d, "verified_with")), "}}");
        }
        prev = t;
        return !b.overflowed();
    });
    for (const OpenSpan &o : open) {
        span(o.kind.view(), o.job ? 3 : 4, double(o.start), double(last - o.start) * 1000.0);
        cat(b, "{\"action_id\":", json_string(o.action_id.view()), ",\"pending\":true}}");
    }
    b.add("]}\n");
    return ok && !b.overflowed();
}

// {"case": summary, "events": [...], "artifacts": [unique artifact paths referenced by any event]}
static bool case_export(Service &s, const Case &c, Body &b) {
    constexpr uint32_t kMaxArtifacts = 1024, kArtifactBytes = 64u << 10;
    auto *names = static_cast<std::string_view *>(s.request.alloc_bytes(sizeof(std::string_view) * kMaxArtifacts, alignof(std::string_view)));
    char *pool = static_cast<char *>(s.request.alloc_bytes(kArtifactBytes, 1));
    if (!names || !pool) return false;
    uint32_t count = 0, pool_used = 0;
    bool fits = true;
    auto remember = [&](yyjson_val *arr) {
        size_t i, n;
        yyjson_val *v;
        yyjson_arr_foreach(arr, i, n, v) {
            const char *p = yyjson_get_str(v);
            if (!p) continue;
            std::string_view name(p, yyjson_get_len(v));
            if (std::find(names, names + count, name) != names + count) continue;
            if (count == kMaxArtifacts || pool_used + name.size() > kArtifactBytes) {
                fits = false;
                return;
            }
            std::memcpy(pool + pool_used, name.data(), name.size());
            names[count++] = std::string_view(pool + pool_used, name.size());
            pool_used += uint32_t(name.size());
        }
    };

    b.add("{\"case\":");
    write_case_summary(b, c);
    b.add(",\"events\":[");
    bool first = true;
    bool ok = for_each_event(s, c, [&](yyjson_val *ev, std::string_view raw) {
        if (!first) b.add(',');
        first = false;
        b.add(raw);
        yyjson_val *d = yyjson_obj_get(ev, "data");
        remember(yyjson_obj_get(d, "artifacts"));
        remember(yyjson_obj_get(d, "evidence"));
        return fits && !b.overflowed();
    });
    b.add("],\"artifacts\":[");
    for (uint32_t i = 0; i < count; ++i) cat(b, i ? "," : "", json_string(names[i]));
    b.add("]}");
    return ok && fits && !b.overflowed();
}

// ---------------------------------------------------------------- chat routing

static bool is_open(const Case &c) { return c.status == Status::investigating || c.status == Status::waiting; }

template <uint32_t N>
static void route_state(FixedStringBuilder<N> &b, Service &s, const RouteState &r) {
    cat(b, "Message from ", r.user.empty() ? std::string_view("someone") : r.user.view());
    if (!r.channel.empty()) cat(b, " (", r.channel, ")");
    cat(b, ": ", r.text, "\nOpen cases:\n");
    uint32_t n = 0;
    for (const Case &c : s.store->cases)
        if (is_open(c)) {
            cat(b, "- ", c.def.title, " (", c.def.id, ")\n");
            ++n;
        }
    if (n == 0) b.add("(none)\n");
}

static constexpr std::string_view kIntents[][2] = {
    {"new_report", "Describes a new problem"},
    {"case_question", "Asks about an existing case"},
    {"status", "Asks what is going on overall"},
    {"other", "Anything else"},
};

static void route_respond(Conn &conn) {
    const RouteState &r = conn.route;
    std::string_view action = r.intent == "new_report" ? "open_case" : r.intent == "case_question" ? "append_message" : r.intent == "status" ? "summarize_status" : "none";
    FixedStringBuilder<1024> b;
    cat(b, "{\"intent\":", json_string(r.intent.view()), ",\"p\":", r.probabilities);
    if (!r.case_id.empty()) cat(b, ",\"case_id\":", json_string(r.case_id.view()));
    cat(b, ",\"action\":", json_string(action), "}");
    respond_json(conn, 200, b.view());
}

static bool post_route_question(Service &s, ModelCall &call, Conn &conn, bool case_question) {
    call.options.clear();
    call.body.clear();
    call.response.clear();
    call.response_overflow = false;
    auto *state = s.request.alloc<FixedStringBuilder<kModelBodyBytes>>();
    if (!state) return false;
    route_state(*state, s, conn.route);
    cat(call.body, "{\"model\":\"clef-flash\",\"state\":", json_string(state->view()), ",\"questions\":{");
    if (!case_question) {
        call.kind = CallKind::route_intent;
        call.body.add("\"intent\":{\"type\":\"choice\",\"instructions\":\"What does this chat message ask for?\",\"criteria\":{");
        for (uint32_t i = 0; i < std::size(kIntents); ++i) {
            cat(call.body, i ? "," : "", json_string(kIntents[i][0]), ':', json_string(kIntents[i][1]));
            call.options.push(FixedString<kNameBytes>(kIntents[i][0]));
        }
    } else {
        call.kind = CallKind::route_case;
        call.body.add("\"case\":{\"type\":\"choice\",\"instructions\":\"Which open case is this message about?\",\"criteria\":{");
        for (const Case &c : s.store->cases)
            if (is_open(c) && call.options.push(c.def.id))
                cat(call.body, call.options.count > 1 ? "," : "", json_string(c.def.id.view()), ':', json_string(c.def.title.view()));
    }
    call.body.add("}}}}");
    if (state->overflowed() || call.body.overflowed()) return false;
    call.conn_index = uint32_t(&conn - s.http->conns.items);
    call.conn_generation = conn.generation;
    post_call(s, call, s.cfg.clef.view(), 25);
    return true;
}

bool on_route_call_done(Service &s, ModelCall &call, long http, CURLcode rc) {
    Http &h = *s.http;
    if (call.conn_index >= h.conns.count) return false;
    Conn &conn = h.conns[call.conn_index];
    if (conn.generation != call.conn_generation || conn.state != ConnState::awaiting_model) return false; // client left or timed out

    yyjson_doc *doc = parse_json(s.request, call.response.view());
    if (rc != CURLE_OK || http != 200 || !doc) {
        FixedStringBuilder<512> b;
        b.add("{\"error\":\"Clef call failed (");
        if (rc != CURLE_OK) b.escaped(curl_easy_strerror(rc));
        else cat(b, "HTTP ", http);
        b.add(")\"}");
        respond_json(conn, 503, b.view());
        return false;
    }
    const bool intent_question = call.kind == CallKind::route_intent;
    yyjson_val *answer = yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc), "answers"), intent_question ? "intent" : "case");
    std::string_view choice = json_str(answer, "choice");
    if (intent_question) {
        RouteState &r = conn.route;
        r.intent.assign(choice);
        r.probabilities.clear();
        yyjson_val *probs = yyjson_obj_get(answer, "probabilities");
        r.probabilities.add('{');
        for (uint32_t i = 0; i < call.options.count; ++i)
            cat(r.probabilities, i ? "," : "", json_string(call.options[i].view()), ':', decimal(yyjson_get_num(yyjson_obj_get(probs, call.options[i].c_str())), 4));
        r.probabilities.add('}');
        bool any_open = false;
        for (const Case &c : s.store->cases) any_open |= is_open(c);
        if (r.intent == "case_question" && r.case_id.empty() && any_open) {
            if (post_route_question(s, call, conn, true)) return true;
            respond_json(conn, 503, "{\"error\":\"routing request too large\"}");
            return false;
        }
    } else {
        conn.route.case_id.assign(choice);
    }
    route_respond(conn);
    return false;
}

void http_check_deadlines(Service &s, int64_t now) {
    for (Conn &c : s.http->conns)
        if (c.state == ConnState::awaiting_model && now > c.route.deadline_ms) {
            ++c.generation; // the completion that may still arrive is dropped
            respond_json(c, 504, "{\"error\":\"routing timed out\"}");
        }
}

// ---------------------------------------------------------------- routes

static bool body_fields_fit(yyjson_val *b, std::initializer_list<std::pair<const char *, uint32_t>> limits) {
    for (auto [key, cap] : limits)
        if (json_str(b, key).size() >= cap) return false;
    return true;
}

static void handle(Service &s, Conn &conn, const Request &r) {
    CaseStore &store = *s.store;
    const std::string_view path = r.path;
    const bool get = r.method == "GET", post = r.method == "POST";

    if (r.method == "OPTIONS") {
        conn.out.clear();
        conn.out_sent = 0;
        conn.out.add("HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST\r\nAccess-Control-Allow-Headers: "
                     "Content-Type\r\nConnection: close\r\n\r\n");
        conn.state = ConnState::writing;
        return;
    }
    if (get && path == "/api/stream") {
        conn.out.clear();
        conn.out_sent = 0;
        conn.out.add("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n\r\n: connected\n\n");
        conn.state = ConnState::stream;
        conn.stream_from_lsn = s.wal->next_lsn;
        return;
    }
    if (get && path == "/api/cases") {
        Body *b = body_in_arena(s);
        if (!b) return respond_too_large(conn);
        b->add('[');
        for (uint32_t i = 0; i < store.cases.count; ++i) {
            if (i) b->add(',');
            write_case_summary(*b, store.cases[i]);
        }
        b->add(']');
        return b->overflowed() ? respond_too_large(conn) : respond_json(conn, 200, b->view());
    }
    if (get && path == "/api/system") {
        Body *b = body_in_arena(s);
        if (!b) return respond_too_large(conn);
        system_json(s, *b);
        return respond_json(conn, 200, b->view());
    }
    if (get && path.substr(0, 11) == "/api/cases/") {
        std::string_view rest = path.substr(11);
        size_t slash = rest.find('/');
        std::string_view view = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
        if (view == "trace" || view == "export") {
            Case *c = find_case(store, rest.substr(0, slash));
            if (!c) return respond_json(conn, 404, "{\"error\":\"no such case\"}");
            Body *b = body_in_arena(s);
            if (!b) return respond_too_large(conn);
            bool ok = view == "trace" ? case_trace(s, *c, *b) : case_export(s, *c, *b);
            return ok ? respond_json(conn, 200, b->view()) : respond_too_large(conn);
        }
        Case *c = find_case(store, rest);
        if (!c) return respond_json(conn, 404, "{\"error\":\"no such case\"}");
        Body *b = body_in_arena(s);
        if (!b) return respond_too_large(conn);
        write_case_summary(*b, *c, false);
        b->add(",\"events\":[");
        for (uint32_t i = 0; i < c->events.count; ++i) {
            if (i) b->add(',');
            if (!append_event(s, *b, c->events[i])) return respond_too_large(conn);
        }
        b->add("]}");
        return b->overflowed() ? respond_too_large(conn) : respond_json(conn, 200, b->view());
    }
    if (get && path.substr(0, 15) == "/api/artifacts/") {
        // /api/artifacts/<case>/<action_id>/<file>; only files inside the jobs directory are served.
        std::string_view rest = path.substr(15);
        size_t slash = rest.find('/');
        std::string_view rel = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
        if (rel.find("..") != std::string_view::npos || rel.empty()) return respond_json(conn, 400, "{\"error\":\"bad path\"}");
        PathText p;
        cat(p, s.cfg.data, "/jobs/", rel);
        if (p.overflowed() || !respond_file(conn, p.c_str(), uint64_t(256) << 20)) return respond_json(conn, 404, "{\"error\":\"no such artifact\"}");
        return;
    }
    if (post && path == "/api/demo/load") {
        // With ROMEO_MAX_ACTIVE set, only that many cases open now; open_next_demo_case() opens the rest one by one.
        s.store->demo_rolling = s.cfg.max_active > 0;
        int opened = 0;
        for (const CaseDef &d : store.defs) {
            if (find_case(store, d.id.view())) continue;
            if (s.cfg.max_active > 0 && active_cases(store) >= s.cfg.max_active) break;
            if (!open_case(s, d, "", "")) {
                RLOG_WARN("demo: case {} could not be opened (case store full or event refused)", d.id.view());
                break;
            }
            ++opened;
        }
        FixedStringBuilder<64> b;
        cat(b, "{\"opened\":", opened, "}");
        return respond_json(conn, 200, b.view());
    }
    if (post && path == "/api/reset") {
        // Archive the data directory (WAL, outbox, jobs, store); memory starts empty. Nothing is deleted.
        FixedString<kPathBytes> archived;
        std::string_view reason;
        if (!reset_data(s, archived, reason)) return respond_json(conn, 400, "{\"error\":\"jobs are still running\"}");
        FixedStringBuilder<kPathBytes + 32> b;
        cat(b, "{\"archived_to\":", json_string(archived.view()), "}");
        return respond_json(conn, 200, b.view());
    }
    if (post && path == "/api/tools/reload") {
        CatalogLoad l = load_script_tools(s);
        if (!l.ok) return respond_json(conn, 400, "{\"error\":\"catalog.json is not a valid tool catalog\"}");
        FixedStringBuilder<64> b;
        cat(b, "{\"tools\":", l.loaded, ",\"refused\":", l.refused, "}");
        return respond_json(conn, 200, b.view());
    }
    if (post && path == "/api/route") {
        yyjson_val *b = yyjson_doc_get_root(parse_json(s.request, r.body));
        std::string_view text = json_str(b, "text"), case_id = json_str(b, "case_id");
        if (text.empty()) return respond_json(conn, 400, "{\"error\":\"text is required\"}");
        if (!body_fields_fit(b, {{"text", kMessageBytes}, {"channel", kNameBytes}, {"user", kNameBytes}, {"case_id", kNameBytes}}))
            return respond_json(conn, 413, "{\"error\":\"request too large\"}");
        if (!case_id.empty() && !find_case(store, case_id)) return respond_json(conn, 404, "{\"error\":\"no such case\"}");
        ModelCall *call = acquire_call(s, CallKind::route_intent);
        if (!call) return respond_json(conn, 503, "{\"error\":\"routing is busy\"}");
        RouteState &rs = conn.route;
        rs.text.assign(text);
        rs.channel.assign(json_str(b, "channel"));
        rs.user.assign(json_str(b, "user"));
        rs.case_id.assign(case_id);
        rs.intent.assign("");
        rs.deadline_ms = now_ms() + 30000;
        conn.state = ConnState::awaiting_model;
        if (!post_route_question(s, *call, conn, false)) {
            release_call(s, *call);
            return respond_json(conn, 413, "{\"error\":\"routing request too large\"}");
        }
        return;
    }
    if (post && path.substr(0, 11) == "/api/cases/" && path.size() > 20 && path.substr(path.size() - 9) == "/messages") {
        Case *c = find_case(store, path.substr(11, path.size() - 20));
        if (!c) return respond_json(conn, 404, "{\"error\":\"no such case\"}");
        yyjson_val *b = yyjson_doc_get_root(parse_json(s.request, r.body));
        std::string_view text = json_str(b, "text");
        if (text.empty()) return respond_json(conn, 400, "{\"error\":\"text is required\"}");
        if (!body_fields_fit(b, {{"text", kMessageBytes}, {"channel", kNameBytes}, {"user", kNameBytes}}))
            return respond_json(conn, 413, "{\"error\":\"request too large\"}");
        bool ok = emit(s, *c, "message", [&](EventLine &e) {
            cat(e, "{\"text\":", json_string(text), ",\"channel\":", json_string(json_str(b, "channel")), ",\"user\":", json_string(json_str(b, "user")), "}");
        });
        if (!ok) return respond_json(conn, 503, "{\"error\":\"the case cannot take more events\"}");
        Body *out = body_in_arena(s);
        if (!out) return respond_too_large(conn);
        write_case_summary(*out, *c);
        return respond_json(conn, 200, out->view());
    }
    if (post && path == "/api/cases") {
        yyjson_val *b = yyjson_doc_get_root(parse_json(s.request, r.body));
        const char *title = yyjson_get_str(yyjson_obj_get(b, "title")), *report = yyjson_get_str(yyjson_obj_get(b, "report"));
        std::string_view setup = json_str(b, "setup");
        if (!title || !report) return respond_json(conn, 400, "{\"error\":\"title and report are required\"}");
        if (!body_fields_fit(b, {{"title", kTitleBytes}, {"report", kReportBytes}, {"channel", kNameBytes}, {"user", kNameBytes}}))
            return respond_json(conn, 413, "{\"error\":\"title, report, channel or user too long\"}");
        CaseDef d;
        FixedStringBuilder<kNameBytes> id;
        cat(id, "case-", 100 + store.cases.count);
        d.id.assign(id.view());
        d.title.assign(json_str(b, "title"));
        d.report.assign(json_str(b, "report"));
        d.rev_good.assign("main");
        d.setup = !setup.empty() && setup[0] >= 'A' && setup[0] <= 'E' ? setup[0] : 'A';
        Case *c = open_case(s, d, json_str(b, "channel"), json_str(b, "user"));
        if (!c) return respond_json(conn, 503, "{\"error\":\"the case store is full\"}");
        Body *out = body_in_arena(s);
        if (!out) return respond_too_large(conn);
        write_case_summary(*out, *c);
        return respond_json(conn, 200, out->view());
    }
    if (get && (path == "/dashboard" || path.substr(0, 11) == "/dashboard/")) {
        std::string_view rel = path.size() <= 11 ? std::string_view("index.html") : path.substr(11);
        PathText p;
        cat(p, s.cfg.dashboard, '/', rel);
        if (rel.find("..") == std::string_view::npos && !p.overflowed() && respond_file(conn, p.c_str(), uint64_t(64) << 20)) return;
    }
    if (get) {
        PathText p;
        cat(p, s.cfg.ui, '/', path == "/" ? std::string_view("index.html") : path.substr(1));
        if (path.find("..") == std::string_view::npos && !p.overflowed() && respond_file(conn, p.c_str(), uint64_t(64) << 20)) return;
    }
    respond_json(conn, 404, "{\"error\":\"not found\"}");
}

// ---------------------------------------------------------------- request parsing

// Returns 1 when a complete request is parsed, 0 when more bytes are needed, -1 when it is too large.
static int parse(Conn &c, Request &r) {
    std::string_view buf = c.in.view();
    size_t end = buf.find("\r\n\r\n");
    if (end == std::string_view::npos) return buf.size() > kMaxHeaderBytes ? -1 : 0;
    if (end > kMaxHeaderBytes) return -1;
    std::string_view head = buf.substr(0, end);
    size_t len = 0;
    for (size_t p = head.find("\r\n"); p != std::string_view::npos; p = head.find("\r\n", p + 2)) {
        std::string_view h = head.substr(p + 2, head.find("\r\n", p + 2) == std::string_view::npos ? std::string_view::npos : head.find("\r\n", p + 2) - p - 2);
        if (h.size() >= 15 && strncasecmp(h.data(), "Content-Length:", 15) == 0) len = size_t(std::max(0LL, parse_leading_int(h.substr(15))));
    }
    if (len > kMaxBodyBytes) return -1;
    if (buf.size() < end + 4 + len) return 0;

    std::string_view line = head.substr(0, head.find("\r\n"));
    auto word = [&](std::string_view &rest) {
        size_t b = rest.find_first_not_of(" \t");
        if (b == std::string_view::npos) return rest = {}, std::string_view();
        rest = rest.substr(b);
        size_t e = rest.find_first_of(" \t");
        std::string_view w = rest.substr(0, e);
        rest = e == std::string_view::npos ? std::string_view() : rest.substr(e);
        return w;
    };
    r.method = word(line);
    r.path = word(line);
    if (size_t q = r.path.find('?'); q != std::string_view::npos) r.path = r.path.substr(0, q);
    r.body = buf.substr(end + 4, len);
    return 1;
}

// ---------------------------------------------------------------- poll phases

uint32_t http_poll_fds(Service &s, curl_waitfd *fds, uint32_t capacity) {
    Http &h = *s.http;
    uint32_t n = 0;
    fds[n++] = {h.listen_fd, CURL_WAIT_POLLIN, 0};
    h.poll_count = 0;
    for (uint32_t i = 0; i < h.conns.count && n < capacity; ++i) {
        Conn &c = h.conns[i];
        if (c.state == ConnState::free) continue;
        short events = 0;
        if (c.state == ConnState::reading || c.state == ConnState::awaiting_model || c.state == ConnState::stream) events |= CURL_WAIT_POLLIN;
        if ((c.state == ConnState::writing || c.state == ConnState::stream) && (c.out_sent < c.out.len || c.file_left > 0)) events |= CURL_WAIT_POLLOUT;
        fds[n++] = {c.fd, events, 0};
        h.poll_conn[h.poll_count++] = i;
    }
    return n;
}

static void accept_all(Service &s) {
    Http &h = *s.http;
    for (;;) {
        int fd = accept(h.listen_fd, nullptr, nullptr);
        if (fd < 0) return;
        set_nonblocking_cloexec(fd);
#if defined(SO_NOSIGPIPE)
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        Conn *slot = nullptr;
        for (Conn &c : h.conns)
            if (c.state == ConnState::free) {
                slot = &c;
                break;
            }
        if (!slot) {
            static constexpr std::string_view kBusy =
                "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: 39\r\nAccess-Control-Allow-Origin: *\r\nConnection: "
                "close\r\n\r\n{\"error\":\"all connection slots in use\"}";
            ssize_t ignored = send(fd, kBusy.data(), kBusy.size(), 0);
            (void)ignored;
            close(fd);
            RLOG_WARN("http: all {} connection slots in use; connection refused with 503", kConnSlots);
            continue;
        }
        slot->fd = fd;
        slot->state = ConnState::reading;
        slot->in.clear();
        slot->out.clear();
        slot->out_sent = 0;
        slot->generation = h.next_generation++;
    }
}

static void read_conn(Service &s, Conn &c) {
    Http &h = *s.http;
    char *dst = c.in.data + c.in.len;
    const size_t room = kConnInBytes - 1 - c.in.len;
    ssize_t got = room ? recv(c.fd, dst, room, 0) : 0;
    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    if (got <= 0 && room) {
        close_conn(h, c); // peer closed or failed
        return;
    }
    if (c.state != ConnState::reading) return; // stream and awaiting slots read only to notice a closed peer
    c.in.len += uint32_t(got);
    c.in.data[c.in.len] = 0;

    Request r;
    int parsed = parse(c, r);
    if (parsed < 0 || (parsed == 0 && c.in.len + 1 >= kConnInBytes)) return respond_json(c, 413, "{\"error\":\"request too large\"}");
    if (parsed == 0) return;
    ArenaScope scope(s.request);
    handle(s, c, r);
}

void http_on_poll(Service &s, const curl_waitfd *fds, uint32_t count) {
    Http &h = *s.http;
    if (count > 0 && (fds[0].revents & CURL_WAIT_POLLIN)) accept_all(s);
    for (uint32_t k = 1; k < count && k - 1 < h.poll_count; ++k) {
        if (!(fds[k].revents & CURL_WAIT_POLLIN)) continue;
        Conn &c = h.conns[h.poll_conn[k - 1]];
        if (c.fd != fds[k].fd || c.state == ConnState::free) continue;
        if (c.state == ConnState::reading || c.state == ConnState::awaiting_model || c.state == ConnState::stream) {
            if (c.state == ConnState::reading) {
                read_conn(s, c);
                continue;
            }
            char probe[512];
            ssize_t got = recv(c.fd, probe, sizeof probe, 0);
            if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) close_conn(h, c);
        }
    }
}

void broadcast_committed(Service &s, const WalRecord &record) {
    Http &h = *s.http;
    for (Conn &c : h.conns) {
        if (c.state != ConnState::stream || record.lsn < c.stream_from_lsn) continue;
        const size_t need = 6 + record.payload.size() + 2;
        if (c.out.len + need >= kConnOutBytes && c.out_sent > 0) {
            // Compact: drop what the subscriber already received.
            std::memmove(c.out.data, c.out.data + c.out_sent, c.out.len - c.out_sent);
            c.out.len -= c.out_sent;
            c.out_sent = 0;
        }
        cat(c.out, "data: ", record.payload, "\n\n");
        if (c.out.overflowed()) {
            RLOG_WARN("http: stream subscriber is {} bytes behind; disconnected", c.out.len - c.out_sent);
            close_conn(h, c);
        }
    }
}

void http_flush(Service &s) {
    Http &h = *s.http;
    for (Conn &c : h.conns) {
        if (c.state != ConnState::writing && c.state != ConnState::stream) continue;
        for (;;) {
            if (c.out_sent < c.out.len) {
                ssize_t n = send(c.fd, c.out.data + c.out_sent, c.out.len - c.out_sent, 0);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (n <= 0) {
                    close_conn(h, c);
                    break;
                }
                c.out_sent += uint32_t(n);
                continue;
            }
            if (c.state == ConnState::stream) {
                c.out.clear();
                c.out_sent = 0;
                break;
            }
            if (c.file_left > 0) {
                const size_t chunk = size_t(std::min<uint64_t>(c.file_left, kConnOutBytes - 1));
                ssize_t n = read(c.file_fd, c.out.data, chunk);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                    close_conn(h, c); // the file shrank: the response cannot be completed
                    break;
                }
                c.out.len = uint32_t(n);
                c.out_sent = 0;
                c.file_left -= uint64_t(n);
                continue;
            }
            close_conn(h, c);
            break;
        }
    }
}

} // namespace romeo
