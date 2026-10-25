#include "agent.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "evidence.hpp"
#include "http.hpp"
#include "log.hpp"
#include "wal.hpp"

namespace romeo {

static constexpr int64_t kScriptTimeoutMs = 20 * 60 * 1000;

static uint32_t case_index(const Service &s, const Case &c) { return uint32_t(&c - s.store->cases.items); }

// ---------------------------------------------------------------- events

bool commit_event(Service &s, Case &c, EventLine &line) {
    if (c.frozen) return false;
    if (line.overflowed()) {
        RLOG_WARN("case {}: event exceeds {} bytes; refused, case frozen", c.def.id.view(), kMaxEventBytes);
        c.frozen = true;
        return false;
    }
    ArenaScope scope(s.request);
    yyjson_doc *doc = parse_json(s.request, line.view());
    yyjson_val *ev = yyjson_doc_get_root(doc);
    std::string_view reason = "the event could not be parsed";
    if (!ev || !can_apply(c, ev, reason)) {
        RLOG_WARN("case {}: {} event refused: {}; case frozen", c.def.id.view(), json_str(ev, "type"), reason);
        c.frozen = true;
        return false;
    }
    WalRecord rec{};
    if (!wal_append(*s.wal, line.view(), rec, [&](const WalRecord &r) { broadcast_committed(s, r); })) {
        RLOG_WARN("case {}: WAL append refused; case frozen", c.def.id.view());
        c.frozen = true;
        return false;
    }
    apply(c, ev);
    c.events.push(EventRef{rec.payload_offset, uint32_t(rec.payload.size())});
    return true;
}

bool emit_fact(Service &s, Case &c, std::string_view text, std::string_view evidence_json) {
    return emit(s, c, "fact", [&](EventLine &b) { cat(b, "{\"text\":", json_string(text), ",\"evidence\":", evidence_json, "}"); });
}

static void action_id_for(FixedString<kNameBytes> &out, const Case &c) {
    FixedStringBuilder<kNameBytes> b;
    cat(b, c.def.id, '-', c.seq + 1);
    out.assign(b.view());
}

// ---------------------------------------------------------------- job start

// Shell single-quoting for a revision name inside sh -c.
template <uint32_t N>
static void shell_quoted(FixedStringBuilder<N> &b, std::string_view v) {
    b.add('\'');
    for (char ch : v) {
        if (ch == '\'') b.add("'\\''");
        else b.add(ch);
    }
    b.add('\'');
}

// Writes <job_dir>/input.json for a script tool.
static bool write_tool_input(Service &s, const Case &c, std::string_view action_id, std::string_view dir) {
    ArenaScope scope(s.request);
    auto *state = s.request.alloc<FixedStringBuilder<kModelBodyBytes>>();
    auto *json = s.request.alloc<FixedStringBuilder<kModelBodyBytes>>();
    if (!state || !json) return false;
    write_clef_state(*state, c);
    FixedStringBuilder<4> setup;
    setup.add(c.def.setup);
    cat(*json, "{\"case_id\":", json_string(c.def.id.view()), ",\"action_id\":", json_string(action_id), ",\"setup\":", json_string(setup.view()),
        ",\"args\":{\"setup\":", json_string(setup.view()), "},\"case_state\":", json_string(state->view()), ",\"facts\":[");
    for (uint32_t i = 0; i < c.facts.count(); ++i) cat(*json, i ? "," : "", json_string(c.facts[i]));
    cat(*json, "],\"title\":", json_string(c.def.title.view()), ",\"report\":", json_string(c.def.report.view()));
    if (!c.def.rev.empty()) cat(*json, ",\"rev\":", json_string(c.def.rev.view()));
    if (!c.def.rev.empty() || c.def.rev_good_given) cat(*json, ",\"rev_good\":", json_string(c.def.rev_good.view()));
    json->add('}');
    if (state->overflowed() || json->overflowed()) {
        RLOG_WARN("case {}: input.json for {} exceeds {} bytes; refused", c.def.id.view(), action_id, kModelBodyBytes);
        return false;
    }
    PathText p;
    cat(p, dir, "/input.json");
    return write_file(p.c_str(), json->view(), true);
}

// The revision a renderer job builds, or empty for the --bug path.
static std::string_view job_revision(const Case &c, const SpawnRequest &r) {
    if (c.def.comparison) return {}; // Explicit comparison uses the local renderer's planted bug flags.
    const bool verify = r.kind == "verify_fix";
    if (verify && !c.fix_rev.empty()) return c.fix_rev.view();
    if (c.def.rev.empty()) return {};
    if (verify || r.baseline) return c.def.rev_good.view();
    return c.def.rev.view();
}

// Starts a detached job. The outbox file is created first with O_EXCL: a second start of the same action fails
// here, so no action runs twice. The caller guarantees that the action_requested record is already durable.
static bool start_job(Service &s, Case &c, const SpawnRequest &r) {
    PathText outbox, dir;
    cat(outbox, s.cfg.data, "/outbox/", r.action_id, ".json");
    job_dir(dir, s, r.action_id.view());
    if (outbox.overflowed() || dir.overflowed()) return false;
    int fd = open(outbox.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    make_dirs(dir.view());

    FixedStringBuilder<4096> cmd;
    if (r.script) {
        cat(cmd, "exec ", s.cfg.tools_abs, '/', r.kind, ' ', dir);
    } else if (std::string_view rev = job_revision(c, r); !rev.empty()) {
        cat(cmd, "bin=$(", s.cfg.tools_abs, "/build_rev ");
        shell_quoted(cmd, rev);
        cat(cmd, ") && exec \"$bin\" --setup ", r.setup, " --frames ", s.cfg.frames, ' ', r.args, " --out ", dir);
    } else {
        std::string_view bugs = c.def.bugs.view();
        cat(cmd, "exec ", s.cfg.render_abs, " --setup ", r.setup, " --frames ", s.cfg.frames, ' ', r.args);
        if (!(r.baseline || bugs.empty())) cat(cmd, " --bug ", bugs);
        cat(cmd, " --out ", dir);
    }
    FixedStringBuilder<8192> wrapped;
    cat(wrapped, '(', cmd, ") > ", dir, "/stdout.txt 2>&1; echo $? > ", dir, "/exit");

    EventLine rec;
    cat(rec, "{\"action_id\":", json_string(r.action_id.view()), ",\"kind\":", json_string(r.kind.view()), ",\"command\":", json_string(wrapped.view()),
        ",\"t_ms\":", now_ms(), "}\n");
    bool ok = !cmd.overflowed() && !wrapped.overflowed() && !rec.overflowed();
    size_t done = 0;
    while (ok && done < rec.len) {
        ssize_t n = write(fd, rec.data + done, rec.len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false;
        else done += size_t(n);
    }
    if (ok) fsync(fd);
    close(fd);
    if (!ok) return false;
    if (r.script && !write_tool_input(s, c, r.action_id.view(), dir.view())) return false;

    // Everything the child needs is built before fork: after fork only async-signal-safe calls run.
    struct rlimit lim {};
    getrlimit(RLIMIT_NOFILE, &lim);
    const int max_fd = int(std::min<rlim_t>(lim.rlim_cur, 4096));
    const char *script = wrapped.c_str();
    pid_t pid = fork();
    if (pid == 0) {
        setsid(); // the job survives a restart of this service
        for (int f = 3; f < max_fd; ++f) close(f);
        execl("/bin/sh", "sh", "-c", script, static_cast<char *>(nullptr));
        _exit(127);
    }
    if (pid > 0) RLOG_INFO("case {}: started job {} ({}) as pid {}", c.def.id.view(), r.action_id.view(), r.kind.view(), int(pid));
    return pid > 0;
}

// A job process is running when its outbox file exists (it was started) and its exit file does not.
// which: 0 = all jobs, 1 = renderer jobs only, 2 = script tools only.
static int count_running(const Service &s, int which) {
    int n = 0;
    for (const Case &c : s.store->cases)
        for (const Pending &p : c.pending) {
            if (!p.job || (which == 1 && p.script) || (which == 2 && !p.script)) continue;
            PathText outbox, exit_file;
            cat(outbox, s.cfg.data, "/outbox/", p.action_id, ".json");
            cat(exit_file, p.dir, "/exit");
            n += file_exists(outbox.c_str()) && !file_exists(exit_file.c_str());
        }
    return n;
}

int job_processes_running(const Service &s) { return count_running(s, 0); }

void commit_iteration(Service &s) {
    wal_commit(*s.wal, [&](const WalRecord &r) { broadcast_committed(s, r); });

    // One job process at a time keeps frame-time measurements free of interference. Jobs that cannot start yet
    // stay queued in order; their action_requested records are already durable.
    // Renderer measurements run one at a time so frame timings stay clean. Script tools (checks, fixes, pull
    // requests) do not measure frame time, so up to kScriptSlots of them run alongside: several cases progress at once.
    constexpr int kScriptSlots = 3;
    SpawnQueue &q = *s.spawns;
    int renderers = count_running(s, 1), scripts = count_running(s, 2);
    // A case a person marked as priority goes first: move its first queued job to the front.
    for (uint32_t i = 1; i < q.items.count; ++i) {
        const SpawnRequest &r = q.items[i];
        if (r.case_index < s.store->cases.count && s.store->cases[r.case_index].priority &&
            !(q.items[0].case_index < s.store->cases.count && s.store->cases[q.items[0].case_index].priority)) {
            const SpawnRequest moved = r;
            for (uint32_t k = i; k > 0; --k) q.items[k] = q.items[k - 1];
            q.items[0] = moved;
            break;
        }
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < q.items.count; ++i) {
        const SpawnRequest r = q.items[i];
        if (r.case_index >= s.store->cases.count) continue;
        Case &c = s.store->cases[r.case_index];
        if (c.generation != r.generation) continue;
        const bool slot_free = r.script ? scripts < kScriptSlots : renderers == 0;
        if (!slot_free) {
            q.items[kept++] = r;
            continue;
        }
        if (start_job(s, c, r)) {
            (r.script ? scripts : renderers) += 1;
        } else if (r.kind != "verify_fix") {
            FixedStringBuilder<256> text;
            cat(text, "Job ", r.action_id, " was already started once; not starting it again.");
            emit_fact(s, c, text.view());
        }
    }
    q.items.count = kept;
}

struct JobParams {
    char setup;
    bool baseline;
    FixedStringBuilder<64> args;
};

// The single definition of a built-in renderer job's parameters, derived from its kind.
static JobParams job_params(const Service &s, const Case &c, std::string_view kind) {
    JobParams j{kind == "try_smaller_setup" ? 'A' : c.def.setup, kind == "compare_baseline" || kind == "verify_fix", {}};
    if (c.def.comparison && (kind == "measure_frames" || kind == "compare_baseline"))
        cat(j.args, "--preview-frames 40 --view-seed ", c.def.view_seed);
    else if (kind == "capture_trace") j.args.add("--trace");
    else if (kind == "measure_memory") cat(j.args, "--frames ", s.cfg.frames * 2);
    return j;
}

static void queue_job(Service &s, Case &c, std::string_view action_id, std::string_view kind, char setup, bool baseline, std::string_view args, bool script) {
    SpawnRequest *r = s.spawns->items.push();
    if (!r) {
        RLOG_WARN("case {}: job queue full; job {} not started", c.def.id.view(), action_id);
        return;
    }
    r->case_index = case_index(s, c);
    r->generation = c.generation;
    r->action_id.assign(action_id);
    r->kind.assign(kind);
    r->args.assign(args);
    r->setup = setup;
    r->baseline = baseline;
    r->script = script;
}

// ---------------------------------------------------------------- model requests

static void request_decision(Service &s, Case &c) {
    Candidates opts;
    candidates(c, *s.scripts, opts);
    if (opts.items.count == 0) return;
    ModelCall *call = acquire_call(s, CallKind::decide);
    if (!call) return;

    auto *state = s.request.alloc<FixedStringBuilder<kModelBodyBytes>>();
    if (state) write_clef_state(*state, c);
    cat(call->body, "{\"model\":\"clef-flash\",\"state\":", json_string(state ? state->view() : std::string_view()),
        ",\"questions\":{\"next_action\":{\"type\":\"choice\",\"instructions\":\"Which action gives the most useful new evidence next? Conclude only when "
        "the evidence explains the report.\",\"criteria\":{");
    for (uint32_t i = 0; i < opts.items.count; ++i) {
        const ResolvedTool &t = opts.items[i];
        cat(call->body, i ? "," : "", json_string(t.name), ':', json_string(t.criteria));
        call->options.push(FixedString<kNameBytes>(t.name));
    }
    call->body.add("}}}}");
    if (!state || state->overflowed() || call->body.overflowed()) {
        RLOG_WARN("case {}: decision request exceeds {} bytes; refused, case frozen", c.def.id.view(), kModelBodyBytes);
        c.frozen = true;
        release_call(s, *call);
        return;
    }
    call->case_index = case_index(s, c);
    call->case_generation = c.generation;
    c.busy = true;
    post_call(s, *call, s.cfg.clef.view(), 60);
}

static bool chat_call(Service &s, Case &c, CallKind kind, std::string_view system, int max_tokens, bool think) {
    ModelCall *call = acquire_call(s, kind);
    if (!call) return false;
    auto *state = s.request.alloc<FixedStringBuilder<kModelBodyBytes>>();
    if (state) write_case_state(*state, c);
    if (state) write_chat_body(*call, system, state->view(), max_tokens, think);
    if (!state || state->overflowed() || call->body.overflowed()) {
        RLOG_WARN("case {}: chat request exceeds {} bytes; refused", c.def.id.view(), kModelBodyBytes);
        release_call(s, *call);
        return false;
    }
    call->case_index = case_index(s, c);
    call->case_generation = c.generation;
    post_call(s, *call, s.cfg.ornith.view(), 120);
    return true;
}

static void request_thought(Service &s, Case &c) {
    ArenaScope scope(s.request);
    chat_call(s, c, CallKind::think,
              "You are investigating a rendering problem. In 3 short sentences: what the newest evidence means, which explanations remain, and what "
              "to check next. Use only the evidence given.",
              s.cfg.think ? 700 : 160, s.cfg.think);
}

// ---------------------------------------------------------------- instant tools

// Instant actions read evidence that already exists, so they finish inside the same step.
static void run_instant(Service &s, Case &c, std::string_view action_id, ToolId id) {
    const Run *last = c.runs.count ? &c.runs[c.runs.count - 1] : nullptr;
    EvidenceText text;
    FixedStringBuilder<1024> arts;
    arts.add("[]");
    switch (id) {
    case ToolId::fetch_logs:
        if (last) {
            fetch_logs(s, c, *last, text);
            arts.clear();
            cat(arts, '[', '"', last->action_id, "/log.jsonl\"]");
        }
        break;
    case ToolId::check_disk:
        if (last) check_disk(s, *last, text);
        break;
    case ToolId::compare_images:
        arts.clear();
        compare_images(s, c, action_id, text, arts);
        break;
    case ToolId::search_code: search_code(s, c, text); break;
    case ToolId::recall_similar:
        cat(text, "Case memory: ", s.store->cases.count - 1, " other cases in memory; similar-case search runs when the memory service answers.");
        break;
    default: break;
    }
    if (text.overflowed()) RLOG_WARN("case {}: evidence text for {} truncated at {} bytes", c.def.id.view(), action_id, text.len);
    emit(s, c, "action_result", [&](EventLine &b) {
        cat(b, "{\"action_id\":", json_string(action_id), ",\"source\":\"run\",\"summary\":", json_string(text.view()), ",\"artifacts\":", arts, "}");
    });
    emit_fact(s, c, text.view(), arts.view());
}

// ---------------------------------------------------------------- execute

// Verification runs the agent-written fix when one exists, otherwise the known-good revision (reference fix).
static void start_verification(Service &s, Case &c) {
    FixedString<kNameBytes> action_id;
    action_id_for(action_id, c);
    PathText dir;
    job_dir(dir, s, action_id.view());
    bool ok = emit(s, c, "action_requested", [&](EventLine &b) {
        cat(b, "{\"action_id\":", json_string(action_id.view()), ",\"kind\":\"verify_fix\",\"label\":\"");
        if (c.fix_rev.empty()) b.add("verification run with the reference fix on setup ");
        else b.add("verification run with agent-written fix ").escaped(c.fix_rev.view()).add(" on setup ");
        cat(b, c.def.setup, "\",\"job\":true,\"dir\":", json_string(dir.view()), "}");
    });
    if (ok) queue_job(s, c, action_id.view(), "verify_fix", c.def.setup, true, "", false);
}

static void execute(Service &s, Case &c, std::string_view kind) {
    ResolvedTool t;
    if (!resolve_tool(*s.scripts, kind, t)) return;
    FixedString<kNameBytes> action_id;
    action_id_for(action_id, c);
    const ToolId id = t.builtin ? t.builtin->id : ToolId::count;

    if (id == ToolId::conclude) {
        ArenaScope scope(s.request);
        if (chat_call(s, c, CallKind::conclude,
                      "Answer in JSON only: {\"supported\": true only if the evidence identifies a concrete cause, \"diagnosis\": one sentence "
                      "naming every cause the evidence supports, at most two, \"fix\": one sentence describing the code change, \"missing\": "
                      "what evidence is still needed if supported is false}. Use only the evidence.",
                      400, false))
            c.busy = true;
        return;
    }
    if (id == ToolId::stop_need_more) {
        ArenaScope scope(s.request);
        if (chat_call(s, c, CallKind::stop_note,
                      "The investigation stops here. In one sentence, list the evidence that would be needed to explain the report.", 160, false))
            c.busy = true;
        return;
    }

    // Withheld data: the tool stays visible to Clef, but choosing it records that the data does not exist.
    if (c.def.withholds(kind)) {
        FixedStringBuilder<512> text;
        cat(text, t.label, ": not available for this case (data not kept)");
        emit(s, c, "action_requested", [&](EventLine &b) {
            cat(b, "{\"action_id\":", json_string(action_id.view()), ",\"kind\":", json_string(kind), ",\"label\":", json_string(t.label), ",\"job\":false}");
        });
        emit(s, c, "action_result", [&](EventLine &b) {
            cat(b, "{\"action_id\":", json_string(action_id.view()), ",\"source\":\"unavailable\",\"summary\":", json_string(text.view()), ",\"artifacts\":[]}");
        });
        emit_fact(s, c, text.view());
        return;
    }

    PathText dir;
    job_dir(dir, s, action_id.view());
    FixedStringBuilder<4> setup_text;

    if (t.script) {
        setup_text.add(c.def.setup);
        bool ok = emit(s, c, "action_requested", [&](EventLine &b) {
            cat(b, "{\"action_id\":", json_string(action_id.view()), ",\"kind\":", json_string(kind), ",\"label\":", json_string(t.label),
                ",\"job\":true,\"dir\":", json_string(dir.view()), ",\"runner\":\"script\",\"args\":{\"setup\":", json_string(setup_text.view()), "}}");
        });
        if (ok) queue_job(s, c, action_id.view(), kind, c.def.setup, false, "", true);
        return;
    }

    if (t.builtin->kind == ToolKind::renderer_job) {
        const JobParams j = job_params(s, c, kind);
        const char setup = j.setup;
        setup_text.add(setup);
        bool ok = emit(s, c, "action_requested", [&](EventLine &b) {
            cat(b, "{\"action_id\":", json_string(action_id.view()), ",\"kind\":", json_string(kind), ",\"label\":\"");
            b.escaped(t.label).add(" on setup ").add(setup);
            cat(b, "\",\"job\":true,\"dir\":", json_string(dir.view()), ",\"args\":{\"setup\":", json_string(setup_text.view()), "}}");
        });
        if (ok) queue_job(s, c, action_id.view(), kind, setup, j.baseline, j.args.view(), false);
        return;
    }

    bool ok = emit(s, c, "action_requested", [&](EventLine &b) {
        cat(b, "{\"action_id\":", json_string(action_id.view()), ",\"kind\":", json_string(kind), ",\"label\":", json_string(t.label), ",\"job\":false}");
    });
    if (ok) run_instant(s, c, action_id.view(), id);
}

// ---------------------------------------------------------------- ingest

static void write_metrics(EventLine &b, const Summary &sum) {
    cat(b, "{\"p50_ms\":", decimal(sum.p50, 4), ",\"p95_ms\":", decimal(sum.p95, 4), ",\"p99_ms\":", decimal(sum.p99, 4),
        ",\"max_ms\":", decimal(sum.max, 4), ",\"total_ms\":", decimal(sum.total_ms, 4), ",\"spike_frames\":", sum.spikes,
        ",\"views\":", sum.views, ",\"frames\":", sum.frames, ",\"resident_end_bytes\":", uint64_t(sum.resident_mb * 1048576.0),
        ",\"tmp_bytes\":", uint64_t(sum.tmp_mb * 1048576.0), ",\"cache_entries\":", sum.cache, "}");
}

static void ingest_job(Service &s, Case &c, const Pending &p) {
    std::string_view dir = p.dir.view();
    const int code = read_exit_code(s, dir);
    upload_run_log(s, c, p.action_id.view(), dir);

    const bool baseline = p.kind == "compare_baseline";
    const char setup = p.kind == "try_smaller_setup" ? 'A' : c.def.setup;
    const Summary sum = read_summary(s, dir);
    auto result_head = [&](EventLine &b) {
        cat(b, "{\"action_id\":", json_string(p.action_id.view()), ",\"source\":\"run\",\"run_dir\":", json_string(dir), ",\"setup\":\"", setup,
            "\",\"baseline\":", baseline ? "true" : "false", ",\"exit_code\":", code);
    };

    if (code != 0 || !sum.ok) {
        EvidenceText text;
        cat(text, "Run on setup ", setup, " exited with code ", code, code == 134 ? " (abort, SIGABRT)" : "", " before writing results");
        describe_crashed_run(s, dir, text);
        emit(s, c, "action_result", [&](EventLine &b) {
            result_head(b);
            cat(b, ",\"summary\":", json_string(text.view()), ",\"artifacts\":");
            write_artifacts(b, s, p.action_id.view(), {"log.jsonl", "stdout.txt", "memory.csv", "frames.csv"});
            b.add('}');
        });
        FixedStringBuilder<1024> arts;
        write_artifacts(arts, s, p.action_id.view(), {"log.jsonl", "memory.csv"});
        text.add('.');
        emit_fact(s, c, text.view(), arts.view());
        return;
    }

    FixedStringBuilder<64> label;
    cat(label, c.def.comparison ? (baseline ? "Baseline renderer" : "Renderer with the scenario") : (baseline ? "Previous build" : "Current build"),
        ", setup ", setup);
    EvidenceText text;
    if (p.kind == "measure_memory") describe_memory_run(s, dir, label.view(), sum, text);
    else if (p.kind == "capture_trace") describe_trace_run(s, dir, label.view(), sum, text);
    else describe_frame_run(s, dir, label.view(), sum, text);

    emit(s, c, "action_result", [&](EventLine &b) {
        result_head(b);
        b.add(",\"metrics\":");
        write_metrics(b, sum);
        cat(b, ",\"summary\":", json_string(text.view()), ",\"artifacts\":");
        write_artifacts(b, s, p.action_id.view(), {"summary.json", "frames.csv", "memory.csv", "trace.json", "image.png", "preview.json", "log.jsonl"});
        b.add('}');
    });
    FixedStringBuilder<1024> arts;
    write_artifacts(arts, s, p.action_id.view(), {"frames.csv", "memory.csv", "trace.json", "image.png"});
    emit_fact(s, c, text.view(), arts.view());
}

// A script tool's result.json: {"summary", "facts": [...], "artifacts": [file names in job_dir], "data": {...}}.
static void ingest_script(Service &s, Case &c, const Pending &p) {
    std::string_view dir = p.dir.view();
    const int code = read_exit_code(s, dir);
    ArenaScope scope(s.request);
    PathText path;
    cat(path, dir, "/result.json");
    FileData f = read_file(s.request, path.c_str(), kMaxBodyBytes);
    yyjson_doc *doc = f.status == ReadStatus::ok ? parse_json(s.request, f.bytes) : nullptr;
    yyjson_val *root = yyjson_doc_get_root(doc);

    auto head = [&](EventLine &b) {
        cat(b, "{\"action_id\":", json_string(p.action_id.view()), ",\"source\":\"run\",\"tool\":", json_string(p.kind.view()), ",\"exit_code\":", code);
    };
    if (code != 0 || !yyjson_is_obj(root)) {
        EvidenceText text;
        cat(text, p.label, " exited with code ", code, " before writing results");
        emit(s, c, "action_result", [&](EventLine &b) {
            head(b);
            cat(b, ",\"summary\":", json_string(text.view()), ",\"artifacts\":");
            write_artifacts(b, s, p.action_id.view(), {"stdout.txt"});
            b.add('}');
        });
        text.add('.');
        emit_fact(s, c, text.view());
        return;
    }

    // Artifacts: plain file names that exist in the job directory.
    FixedStringBuilder<4096> arts;
    arts.add('[');
    size_t i, n;
    yyjson_val *v;
    bool first = true;
    yyjson_arr_foreach(yyjson_obj_get(root, "artifacts"), i, n, v) {
        std::string_view name = yyjson_get_str(v) ? std::string_view(yyjson_get_str(v)) : std::string_view();
        if (name.empty() || name.find("..") != std::string_view::npos || name[0] == '/') continue;
        PathText file;
        cat(file, dir, '/', name);
        if (file.overflowed() || !file_exists(file.c_str())) continue;
        cat(arts, first ? "" : ",", '"');
        arts.escaped(p.action_id.view()).add('/').escaped(name).add('"');
        first = false;
    }
    arts.add(']');

    // data is copied verbatim; yyjson writes it into the request arena through a pool allocator.
    std::string_view data = "{}";
    if (yyjson_val *d = yyjson_obj_get(root, "data")) {
        size_t cap = kMaxEventBytes;
        void *mem = s.request.alloc_bytes(cap, 16);
        yyjson_alc alc;
        if (mem && yyjson_alc_pool_init(&alc, mem, cap)) {
            size_t len = 0;
            if (char *json = yyjson_val_write_opts(d, 0, &alc, &len, nullptr)) data = std::string_view(json, len);
        }
    }
    std::string_view summary = json_str(root, "summary");
    emit(s, c, "action_result", [&](EventLine &b) {
        head(b);
        cat(b, ",\"summary\":", json_string(summary), ",\"artifacts\":", arts, ",\"data\":", data, "}");
    });
    uint32_t facts = 0;
    yyjson_arr_foreach(yyjson_obj_get(root, "facts"), i, n, v) {
        if (const char *t = yyjson_get_str(v)) {
            emit_fact(s, c, t, arts.view());
            ++facts;
        }
    }
    if (facts == 0 && !summary.empty()) emit_fact(s, c, summary, arts.view());
}

static void finish_verification(Service &s, Case &c, const Pending &p) {
    const Summary after = read_summary(s, p.dir.view());
    const int code = read_exit_code(s, p.dir.view());
    const Run *before = nullptr;
    for (const Run &r : c.runs)
        if (!r.baseline && r.setup == c.def.setup) before = &r;
    const Summary b = before ? read_summary(s, before->dir.view()) : Summary{};

    FixedStringBuilder<2048> checks, line;
    cat(line, c.fix_rev.empty() ? "Run with the reference fix exits " : "Run with the agent-written fix exits ");
    if (code == 0) line.add("cleanly");
    else cat(line, "with code ", code);
    cat(checks, '[', json_string(line.view()));
    if (before && b.ok && after.ok) {
        line.clear();
        cat(line, "p99 frame ", decimal(b.p99, 1), " ms -> ", decimal(after.p99, 1), " ms; worst ", decimal(b.max, 0), " ms -> ", decimal(after.max, 0), " ms");
        cat(checks, ',', json_string(line.view()));
        line.clear();
        cat(line, "Resident memory ", decimal(b.resident_mb, 0), " MB -> ", decimal(after.resident_mb, 0), " MB; temporary data ", decimal(b.tmp_mb, 1), " MB -> ",
            decimal(after.tmp_mb, 1), " MB");
        cat(checks, ',', json_string(line.view()));
    }
    checks.add(']');

    emit(s, c, "action_result", [&](EventLine &e) {
        cat(e, "{\"action_id\":", json_string(p.action_id.view()), ",\"source\":\"run\",\"run_dir\":", json_string(p.dir.view()),
            ",\"baseline\":true,\"exit_code\":", code, ",\"success\":", code == 0 && after.ok ? "true" : "false",
            ",\"summary\":\"verification run finished\",\"artifacts\":");
        write_artifacts(e, s, p.action_id.view(), {"summary.json", "frames.csv", "image.png", "stdout.txt"});
        e.add('}');
    });
    if (code != 0 || !after.ok) {
        emit(s, c, "stopped", [&](EventLine &e) {
            cat(e, "{\"reason\":\"Verification failed.\",\"missing\":",
                json_string(code != 0 ? "A verification run that exits successfully." : "A complete, valid verification summary."),
                ",\"checks\":", checks, "}");
        });
        return;
    }
    FixedStringBuilder<256> verified;
    if (c.fix_rev.empty()) verified.add("reference fix (planted change removed)");
    else cat(verified, "agent-written fix ", c.fix_rev);
    emit(s, c, "done", [&](EventLine &e) {
        cat(e, "{\"summary\":", json_string(c.diagnosis.view()), ",\"success\":true,\"checks\":", checks, ",\"verified_with\":", json_string(verified.view()), "}");
    });
}

static void record_timeout(Service &s, Case &c, const Pending &p) {
    EvidenceText text;
    cat(text, p.label, ": no result after 20 minutes; the tool run timed out");
    emit(s, c, "action_result", [&](EventLine &b) {
        cat(b, "{\"action_id\":", json_string(p.action_id.view()), ",\"source\":\"timeout\",\"tool\":", json_string(p.kind.view()),
            ",\"summary\":", json_string(text.view()), ",\"artifacts\":[]}");
    });
    emit_fact(s, c, text.view());
}

// ---------------------------------------------------------------- model completions

void on_case_call_done(Service &s, ModelCall &call, long http, CURLcode rc) {
    if (call.case_index >= s.store->cases.count) return;
    Case &c = s.store->cases[call.case_index];
    if (c.generation != call.case_generation) return; // the case was archived by a reset
    // A thought is not tracked by busy; only the call that set busy clears it.
    if (call.kind != CallKind::think) c.busy = false;

    yyjson_doc *doc = parse_json(s.request, call.response.view());
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (rc != CURLE_OK || http != 200 || !doc) {
        if (call.kind != CallKind::think) {
            FixedStringBuilder<512> text;
            text.add(call.kind == CallKind::decide ? "Clef could not answer (" : "Ornith could not answer (");
            if (rc != CURLE_OK) text.add(curl_easy_strerror(rc));
            else cat(text, "HTTP ", http);
            text.add(").");
            emit(s, c, "stopped", [&](EventLine &e) {
                cat(e, "{\"reason\":", json_string(text.view()), ",\"missing\":",
                    json_string(call.kind == CallKind::decide
                        ? "Start the configured Clef service, then start a new investigation. Measured comparisons do not need a model."
                        : "Start the configured Ornith service, then start a new investigation."), "}");
            });
        }
        return;
    }

    if (call.kind == CallKind::decide) {
        yyjson_val *answer = yyjson_obj_get(yyjson_obj_get(root, "answers"), "next_action");
        yyjson_val *probs = yyjson_obj_get(answer, "probabilities");
        std::string_view chosen = json_str(answer, "choice");
        bool chosen_valid = false, probabilities_valid = yyjson_is_obj(probs);
        for (const auto &option : call.options) {
            chosen_valid = chosen_valid || chosen == option.view();
            yyjson_val *value = yyjson_obj_get(probs, option.c_str());
            probabilities_valid = probabilities_valid && yyjson_is_num(value) && yyjson_get_num(value) >= 0 && yyjson_get_num(value) <= 1;
        }
        if (!chosen_valid || !probabilities_valid) {
            emit(s, c, "stopped", [&](EventLine &e) {
                e.add("{\"reason\":\"Clef returned an invalid decision.\",\"missing\":\"A choice from the offered tools and a numeric probability for each tool. No prediction was recorded.\"}");
            });
            return;
        }
        struct Ranked {
            double p;
            std::string_view name;
        };
        FixedArray<Ranked, kToolCount + kMaxScriptTools> ranked;
        for (const auto &k : call.options) ranked.push(Ranked{yyjson_get_num(yyjson_obj_get(probs, k.c_str())), k.view()});
        // Descending by (p, name), like sorting (p, name) pairs in reverse.
        std::sort(ranked.begin(), ranked.end(), [](const Ranked &a, const Ranked &b) { return a.p != b.p ? a.p > b.p : a.name > b.name; });
        const double ms = yyjson_get_num(yyjson_obj_get(yyjson_obj_get(root, "timings"), "inference_ms"));
        const double input_tokens = yyjson_get_num(yyjson_obj_get(yyjson_obj_get(root, "usage"), "input_tokens"));
        ++s.store->decisions_total;
        s.store->decision_ms_total += ms;
        emit(s, c, "decision", [&](EventLine &b) {
            b.add("{\"candidates\":[");
            for (uint32_t i = 0; i < ranked.count; ++i) {
                ResolvedTool t;
                std::string_view label = resolve_tool(*s.scripts, ranked[i].name, t) ? t.label : std::string_view();
                cat(b, i ? "," : "", "{\"action\":", json_string(ranked[i].name), ",\"label\":", json_string(label), ",\"p\":", decimal(ranked[i].p, 4), "}");
            }
            cat(b, "],\"chosen\":", json_string(chosen), ",\"model\":\"clef-flash\",\"inference_ms\":", decimal(ms, 1), ",\"input_tokens\":", decimal(input_tokens, 0), "}");
        });
        ResolvedTool t;
        if (!chosen.empty() && resolve_tool(*s.scripts, chosen, t)) execute(s, c, chosen);
        return;
    }

    yyjson_val *msg = yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(root, "choices"), 0), "message");
    std::string_view content = json_str(msg, "content");
    std::string_view reasoning = json_str(msg, "reasoning_content");
    if (!yyjson_get_str(yyjson_obj_get(msg, "reasoning_content"))) reasoning = json_str(msg, "reasoning");
    // Token counts ride on the event the call produces, so per-case model cost can be summed from the case log.
    yyjson_val *usage = yyjson_obj_get(root, "usage");
    FixedStringBuilder<96> tokens;
    cat(tokens, ",\"prompt_tokens\":", decimal(yyjson_get_num(yyjson_obj_get(usage, "prompt_tokens")), 0), ",\"completion_tokens\":",
        decimal(yyjson_get_num(yyjson_obj_get(usage, "completion_tokens")), 0));

    if (call.kind == CallKind::think) {
        emit(s, c, "thought", [&](EventLine &b) {
            cat(b, "{\"text\":", json_string(content), ",\"reasoning\":", json_string(reasoning), ",\"model\":\"ornith-35b\"", tokens.view(), "}");
        });
    } else if (call.kind == CallKind::conclude) {
        std::string_view diag = content, fix, missing;
        bool supported = true;
        size_t open = content.find('{'), close = content.rfind('}');
        if (open != std::string_view::npos && close != std::string_view::npos && close >= open) {
            if (yyjson_doc *j = parse_json(s.request, content.substr(open, close - open + 1))) {
                yyjson_val *r = yyjson_doc_get_root(j);
                if (yyjson_get_str(yyjson_obj_get(r, "diagnosis"))) diag = json_str(r, "diagnosis");
                fix = json_str(r, "fix");
                missing = json_str(r, "missing");
                if (yyjson_is_bool(yyjson_obj_get(r, "supported"))) supported = yyjson_get_bool(yyjson_obj_get(r, "supported"));
            }
        }
        emit(s, c, "conclusion", [&](EventLine &b) {
            cat(b, "{\"diagnosis\":", json_string(diag), ",\"fix\":", json_string(fix), ",\"supported\":", supported ? "true" : "false",
                ",\"model\":\"ornith-35b\"", tokens.view(), "}");
        });
        // A conclusion without a supported cause stops the case with what is missing; nothing is verified.
        if (!supported) {
            emit(s, c, "stopped", [&](EventLine &b) {
                cat(b, "{\"reason\":", json_string(diag), ",\"missing\":", json_string(missing.empty() ? std::string_view("not stated") : missing), "}");
            });
            return;
        }
        // Delivery after a supported diagnosis is a fixed pipeline, not a model choice:
        // regression check -> fix -> pull request -> verification run.
        ResolvedTool t;
        if (resolve_tool(*s.scripts, "visualize", t)) execute(s, c, "visualize");
        else if (resolve_tool(*s.scripts, "write_test", t)) execute(s, c, "write_test");
        else start_verification(s, c);
    } else if (call.kind == CallKind::stop_note) {
        emit(s, c, "stopped", [&](EventLine &b) {
            cat(b, "{\"reason\":\"The available tools cannot establish more.\",\"missing\":", json_string(content), tokens.view(), "}");
        });
    }
}

// ---------------------------------------------------------------- the agent step

// Demo pacing: with ROMEO_MAX_ACTIVE set and the demo set loaded, open the next unopened demo case whenever fewer
// than that many cases are still in progress.
static void open_next_demo_case(Service &s) {
    CaseStore &store = *s.store;
    if (s.cfg.max_active <= 0 || active_cases(store) >= s.cfg.max_active) return;
    bool demo_loaded = false;
    for (const CaseDef &d : store.defs) demo_loaded = demo_loaded || find_case(store, d.id.view());
    if (!demo_loaded) return;
    for (const CaseDef &d : store.defs)
        if (!find_case(store, d.id.view())) {
            open_case(s, d, "", "");
            return;
        }
}

// An explicit measurement run does not consult a model or execute script tools. Both builds use
// the same scene seed and the renderer jobs remain globally serial, just like investigations.
static void comparison_step(Service &s, Case &c) {
    const bool current_done = c.is_done("measure_frames"), baseline_done = c.is_done("compare_baseline");
    if (!current_done || !baseline_done) {
        const bool baseline = current_done;
        const std::string_view kind = baseline ? "compare_baseline" : "measure_frames";
        FixedString<kNameBytes> action_id;
        action_id_for(action_id, c);
        PathText dir;
        job_dir(dir, s, action_id.view());
        const JobParams j = job_params(s, c, kind);
        const bool ok = emit(s, c, "action_requested", [&](EventLine &b) {
            cat(b, "{\"action_id\":", json_string(action_id.view()), ",\"kind\":", json_string(kind),
                ",\"label\":", json_string(baseline ? "Measuring the baseline" : "Measuring the scenario"), ",\"job\":true,\"dir\":", json_string(dir.view()),
                ",\"args\":{\"setup\":\"", c.def.setup, "\",\"view_seed\":", c.def.view_seed, ",\"preview_frames\":40}}");
        });
        if (ok) queue_job(s, c, action_id.view(), kind, j.setup, j.baseline, j.args.view(), false);
        return;
    }

    const Run *current = nullptr, *baseline = nullptr;
    for (const Run &run : c.runs) {
        if (run.kind == "measure_frames") current = &run;
        if (run.kind == "compare_baseline") baseline = &run;
    }
    const Summary a = current ? read_summary(s, current->dir.view()) : Summary{};
    const Summary b = baseline ? read_summary(s, baseline->dir.view()) : Summary{};
    const bool ok = current && baseline && a.ok && b.ok && read_exit_code(s, current->dir.view()) == 0 && read_exit_code(s, baseline->dir.view()) == 0;
    if (!ok) {
        emit(s, c, "stopped", [&](EventLine &e) {
            e.add("{\"reason\":\"The comparison could not finish.\",\"missing\":\"Both renderer runs must exit successfully and write complete measurement summaries. Check the run output.\"}");
        });
        return;
    }
    emit(s, c, "comparison_complete", [&](EventLine &e) {
        cat(e, "{\"success\":true,\"summary\":\"Both renderer measurements finished. This comparison does not assert a diagnosis or verified fix.\",\"view_seed\":", c.def.view_seed,
            ",\"current_action_id\":", json_string(current->action_id.view()), ",\"baseline_action_id\":", json_string(baseline->action_id.view()), ",\"current\":");
        write_metrics(e, a);
        e.add(",\"baseline\":");
        write_metrics(e, b);
        e.add('}');
    });
}

void agent_tick(Service &s) {
    CaseStore &store = *s.store;
    const int64_t now = now_ms();
    open_next_demo_case(s);
    for (uint32_t ci = 0; ci < store.cases.count; ++ci) {
        Case &c = store.cases[ci];
        if (c.frozen) continue;

        // Finished jobs, including ones that finished while the service was down.
        FixedArray<Pending, kMaxPendingPerCase> ready, expired;
        for (const Pending &p : c.pending) {
            if (!p.job) continue;
            PathText exit_file;
            cat(exit_file, p.dir, "/exit");
            if (file_exists(exit_file.c_str())) ready.push(p);
            else if (p.script && now - p.requested_ms > kScriptTimeoutMs) expired.push(p);
        }
        for (const Pending &p : ready) {
            if (p.kind == "verify_fix") {
                finish_verification(s, c, p);
            } else if (p.script) {
                ingest_script(s, c, p);
                if (!c.def.comparison) request_thought(s, c);
                if (c.concluded && !c.def.comparison) {
                    ResolvedTool t;
                    if (p.kind == "visualize" && resolve_tool(*s.scripts, "write_test", t)) execute(s, c, "write_test");
                    else if (p.kind == "write_test" && resolve_tool(*s.scripts, "write_fix", t)) execute(s, c, "write_fix");
                    else if (p.kind == "write_fix" && !c.fix_rev.empty() && resolve_tool(*s.scripts, "open_pr", t)) execute(s, c, "open_pr");
                    else start_verification(s, c);
                }
            } else {
                ingest_job(s, c, p);
                if (!c.def.comparison) request_thought(s, c);
            }
            s.evidence.reset();
        }
        for (const Pending &p : expired) record_timeout(s, c, p);

        if (c.status != Status::investigating || c.busy || c.pending.count || c.concluded || c.frozen) continue;
        // Jobs are serialized when they start (commit_iteration), so other cases keep deciding while one job runs.
        ArenaScope scope(s.request);
        if (c.def.comparison) {
            comparison_step(s, c);
            continue;
        }
        if (c.done.count >= 9) {
            execute(s, c, "stop_need_more");
            continue;
        }
        request_decision(s, c);
    }
}

// ---------------------------------------------------------------- lifecycle

static void make_data_dirs(const Service &s) {
    PathText p;
    cat(p, s.cfg.data, "/outbox");
    make_dirs(p.view());
    p.clear();
    cat(p, s.cfg.data, "/jobs");
    make_dirs(p.view());
}

static void fill_def(const Service &s, CaseDef &def, std::string_view id) {
    if (const CaseDef *d = find_def(*s.store, id)) def = *d;
    else def.id.assign(id);
}

bool recover(Service &s) {
    make_data_dirs(s);
    PathText path;
    cat(path, s.cfg.data, "/wal.log");
    uint64_t records = 0;
    bool ok = wal_open_and_recover(*s.wal, path.view(), s.request, [&](const WalRecord &r) {
        ArenaScope scope(s.request);
        yyjson_doc *doc = parse_json(s.request, r.payload);
        yyjson_val *ev = yyjson_doc_get_root(doc);
        std::string_view id = json_str(ev, "case");
        if (!ev || id.empty()) {
            RLOG_WARN("wal: record {} is not a case event; skipped", r.lsn);
            return;
        }
        Case *c = find_case(*s.store, id);
        if (!c) {
            c = new_case(*s.store);
            if (!c) {
                RLOG_WARN("wal: case store full ({} cases); record {} of {} skipped", kMaxCases, r.lsn, id);
                return;
            }
            fill_def(s, c->def, id);
        }
        apply(*c, ev);
        if (!c->events.push(EventRef{r.payload_offset, uint32_t(r.payload.size())}))
            RLOG_WARN("case {}: event index full during recovery; record {} not indexed", id, r.lsn);
        ++records;
    });
    if (!ok) return false;
    RLOG_INFO("wal: recovered {} records, {} cases", records, s.store->cases.count);

    // Every case that was waiting gets a visible restart marker; its jobs are not started again.
    for (Case &c : s.store->cases) {
        if (c.pending.count == 0) continue;
        FixedArray<std::string_view, kMaxPendingPerCase> ids;
        for (const Pending &p : c.pending) {
            ids.push(p.action_id.view());
            PathText outbox;
            cat(outbox, s.cfg.data, "/outbox/", p.action_id, ".json");
            if (p.job && !file_exists(outbox.c_str())) {
                // Never started: starting it now is its first and only start (the outbox O_EXCL rule still holds).
                const JobParams j = job_params(s, c, p.kind.view());
                queue_job(s, c, p.action_id.view(), p.kind.view(), j.setup, j.baseline, j.args.view(), p.script);
                RLOG_INFO("case {}: job {} never started before the restart; queued it", c.def.id.view(), p.action_id.view());
            }
        }
        std::sort(ids.begin(), ids.end());
        emit(s, c, "restarted", [&](EventLine &b) {
            b.add("{\"pending\":[");
            for (uint32_t i = 0; i < ids.count; ++i) cat(b, i ? "," : "", json_string(ids[i]));
            b.add("]}");
        });
    }
    return true;
}

Case *open_case(Service &s, const CaseDef &def, std::string_view channel, std::string_view user) {
    Case *c = new_case(*s.store);
    if (!c) return nullptr;
    c->def = def;
    bool ok = emit(s, *c, "case_opened", [&](EventLine &b) {
        cat(b, "{\"title\":", json_string(def.title.view()), ",\"setup\":\"", def.setup, "\",\"report\":", json_string(def.report.view()));
        if (!def.difficulty.empty()) cat(b, ",\"difficulty\":", json_string(def.difficulty.view()));
        if (!def.rev.empty()) cat(b, ",\"rev\":", json_string(def.rev.view()), ",\"rev_good\":", json_string(def.rev_good.view()));
        cat(b, ",\"scenario_id\":", json_string(def.scenario_id.view()), ",\"mode\":", json_string(def.comparison ? "comparison" : "investigation"),
            ",\"view_seed\":", def.view_seed, ",\"execution\":{\"bugs\":", json_string(def.bugs.view()), ",\"rev\":", json_string(def.rev.view()),
            ",\"rev_good\":", json_string(def.rev_good.view()), ",\"rev_good_given\":", def.rev_good_given ? "true" : "false", ",\"withhold\":[");
        for (uint32_t i = 0; i < def.withhold.count; ++i) cat(b, i ? "," : "", json_string(def.withhold[i].view()));
        b.add("]}");
        if (!channel.empty()) cat(b, ",\"channel\":", json_string(channel));
        if (!user.empty()) cat(b, ",\"user\":", json_string(user));
        b.add('}');
    });
    if (!ok) {
        --s.store->cases.count; // the slot was the last one taken
        return nullptr;
    }
    return c;
}

bool reset_data(Service &s, FixedString<kPathBytes> &archived_to, std::string_view &reason) {
    if (running_jobs(*s.store) > 0) {
        reason = "jobs are still running";
        return false;
    }
    // Events appended earlier in this iteration become durable in the directory being archived.
    commit_iteration(s);

    std::string_view data = s.cfg.data.view();
    size_t slash = data.rfind('/');
    FixedStringBuilder<kPathBytes> archive;
    if (slash == std::string_view::npos) cat(archive, "archive-", now_ms());
    else if (slash == 0) cat(archive, "/archive-", now_ms());
    else cat(archive, data.substr(0, slash), "/archive-", now_ms());
    archived_to.assign(archive.view());

    wal_close(*s.wal);
    if (rename(s.cfg.data.c_str(), archive.c_str()) != 0)
        RLOG_WARN("reset: rename {} -> {} failed: {}", data, archive.view(), std::string_view(std::strerror(errno)));
    else RLOG_INFO("reset: archived {} to {}; the operational log of that run stays in the archive", data, archive.view());
    clear_cases(*s.store);
    recover(s);
    return true;
}

} // namespace romeo
