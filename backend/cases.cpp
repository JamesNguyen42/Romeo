#include "cases.hpp"

#include "evidence.hpp"
#include "log.hpp"

namespace romeo {

// ---------------------------------------------------------------- lookups

bool CaseDef::withholds(std::string_view tool_name) const {
    for (const auto &w : withhold)
        if (w == tool_name) return true;
    return false;
}

bool Case::is_done(std::string_view kind) const {
    for (const auto &d : done)
        if (d == kind) return true;
    return false;
}

const Pending *Case::find_pending(std::string_view action_id) const {
    for (const Pending &p : pending)
        if (p.action_id == action_id) return &p;
    return nullptr;
}

Case *find_case(CaseStore &store, std::string_view id) {
    for (Case &c : store.cases)
        if (c.def.id == id) return &c;
    return nullptr;
}

const CaseDef *find_def(const CaseStore &store, std::string_view id) {
    for (const CaseDef &d : store.defs)
        if (d.id == id) return &d;
    return nullptr;
}

Case *new_case(CaseStore &store) {
    Case *slot = store.cases.push();
    if (!slot) return nullptr;
    Case *c = new (slot) Case();
    c->generation = store.next_generation++;
    return c;
}

void clear_cases(CaseStore &store) {
    store.cases.clear();
    ++store.next_generation;
}

int active_cases(const CaseStore &store) {
    int n = 0;
    for (const Case &c : store.cases) n += c.status != Status::done && c.status != Status::stopped;
    return n;
}

int running_jobs(const CaseStore &store) {
    int n = 0;
    for (const Case &c : store.cases)
        for (const Pending &p : c.pending) n += p.job;
    return n;
}

bool has_run(const Case &c, bool baseline) {
    for (const Run &r : c.runs)
        if (r.baseline == baseline && r.setup == c.def.setup) return true;
    return false;
}

bool requirements_met(const Case &c, const Requirements &r) {
    if (r.any_run && c.runs.count == 0) return false;
    if (r.baseline_run) {
        bool any = false;
        for (const Run &run : c.runs) any |= run.baseline;
        if (!any) return false;
    }
    if (r.both_runs && !(has_run(c, true) && has_run(c, false))) return false;
    if (r.conclusion && !c.concluded) return false;
    if (r.fix && c.fix_rev.empty()) return false;
    if (c.facts.count() < r.min_facts) return false;
    return true;
}

// ---------------------------------------------------------------- tool merge

static const ScriptTool *find_script(const ScriptTools &scripts, std::string_view name) {
    for (const ScriptTool &t : scripts.tools)
        if (t.name == name) return &t;
    return nullptr;
}

static ResolvedTool resolved_script(const ScriptTool &t) {
    return ResolvedTool{t.name.view(), t.criteria.view(), t.label.view(), nullptr, &t, t.requires_, t.once};
}

bool resolve_tool(const ScriptTools &scripts, std::string_view name, ResolvedTool &out) {
    if (const ScriptTool *t = find_script(scripts, name)) {
        out = resolved_script(*t);
        return true;
    }
    const ToolInfo *b = find_builtin(name);
    if (!b || !b->offered) return false;
    out = ResolvedTool{b->name, b->criteria, b->label, b, nullptr, b->requires_, true};
    return true;
}

void candidates(const Case &c, const ScriptTools &scripts, Candidates &out) {
    out.items.clear();
    auto offer = [&](const ResolvedTool &t) {
        if (t.once && c.is_done(t.name)) return;
        if (!requirements_met(c, t.requires_)) return;
        out.items.push(t);
    };
    for (const ToolInfo &b : kTools) {
        if (!b.offered) continue;
        if (const ScriptTool *t = find_script(scripts, b.name)) offer(resolved_script(*t));
        else offer(ResolvedTool{b.name, b.criteria, b.label, &b, nullptr, b.requires_, true});
    }
    for (const ScriptTool &t : scripts.tools)
        if (!find_builtin(t.name.view())) offer(resolved_script(t));
}

// ---------------------------------------------------------------- loading definitions

static bool assign_checked(auto &dst, std::string_view v) { return dst.assign(v); }

uint32_t load_defs(Service &s) {
    CaseStore &store = *s.store;
    store.defs.clear();
    ArenaScope scope(s.request);
    FileData f = read_file(s.request, s.cfg.cases_file.c_str());
    if (f.status != ReadStatus::ok) {
        RLOG_WARN("cases: cannot read {}; no demo cases", s.cfg.cases_file.view());
        return 0;
    }
    yyjson_doc *doc = parse_json(s.request, f.bytes);
    yyjson_val *arr = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(arr)) {
        RLOG_WARN("cases: {} is not a JSON array; no demo cases", s.cfg.cases_file.view());
        return 0;
    }

    size_t i, n;
    yyjson_val *v;
    yyjson_arr_foreach(arr, i, n, v) {
        CaseDef d;
        bool fits = assign_checked(d.id, json_str(v, "id")) && assign_checked(d.title, json_str(v, "title")) &&
                    assign_checked(d.report, json_str(v, "report")) && assign_checked(d.bugs, json_str(v, "bugs")) &&
                    assign_checked(d.difficulty, json_str(v, "difficulty")) && assign_checked(d.rev, json_str(v, "rev"));
        std::string_view good = json_str(v, "rev_good");
        fits = fits && d.rev_good.assign(good.empty() ? std::string_view("main") : good);
        d.rev_good_given = !good.empty();
        std::string_view setup = json_str(v, "setup");
        d.setup = setup.empty() ? 'A' : setup[0];
        size_t wi, wn;
        yyjson_val *w;
        yyjson_arr_foreach(yyjson_obj_get(v, "withhold"), wi, wn, w) {
            auto *slot = d.withhold.push();
            fits = fits && slot && slot->assign(std::string_view(yyjson_get_str(w) ? yyjson_get_str(w) : ""));
        }
        if (!fits) {
            RLOG_WARN("cases: definition {} exceeds a field capacity; refused", json_str(v, "id"));
            continue;
        }

        // Same id twice: the later definition wins, as with the original map.
        uint32_t at = 0;
        while (at < store.defs.count && store.defs[at].id.view() < d.id.view()) ++at;
        if (at < store.defs.count && store.defs[at].id == d.id.view()) {
            store.defs[at] = d;
            continue;
        }
        if (store.defs.full()) {
            RLOG_WARN("cases: more than {} definitions; {} refused", kMaxCaseDefs, d.id.view());
            continue;
        }
        store.defs.push();
        for (uint32_t k = store.defs.count - 1; k > at; --k) store.defs[k] = store.defs[k - 1];
        store.defs[at] = d;
    }
    return store.defs.count;
}

static bool valid_tool_name(std::string_view name) {
    if (name.empty() || name.size() >= kNameBytes) return false;
    for (char ch : name)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
    return name[0] != '-';
}

static bool parse_requirement(std::string_view r, Requirements &out) {
    if (r == "any_run") return out.any_run = true;
    if (r == "baseline_run") return out.baseline_run = true;
    if (r == "conclusion") return out.conclusion = true;
    if (r == "fix") return out.fix = true;
    if (r.substr(0, 7) == "facts>=") {
        long long n = parse_leading_int(r.substr(7));
        if (n < 0 || n > 255) return false;
        out.min_facts = uint8_t(n);
        return true;
    }
    return false;
}

CatalogLoad load_script_tools(Service &s) {
    ScriptTools &table = *s.scripts;
    PathText path;
    cat(path, s.cfg.tools, "/catalog.json");

    ArenaScope scope(s.request);
    FileData f = read_file(s.request, path.c_str());
    if (f.status == ReadStatus::missing) {
        table.tools.clear();
        table.refused = 0;
        RLOG_INFO("tools: {} not found; only built-in tools are available", path.view());
        return {true, 0, 0};
    }
    yyjson_doc *doc = f.status == ReadStatus::ok ? parse_json(s.request, f.bytes) : nullptr;
    yyjson_val *arr = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(arr)) {
        RLOG_WARN("tools: {} is not a JSON array; keeping the previous {} script tools", path.view(), table.tools.count);
        return {false, table.tools.count, 0};
    }

    table.tools.clear();
    table.refused = 0;
    size_t i, n;
    yyjson_val *v;
    yyjson_arr_foreach(arr, i, n, v) {
        std::string_view name = json_str(v, "name");
        const ToolInfo *b = find_builtin(name);
        auto refuse = [&](std::string_view why) {
            ++table.refused;
            RLOG_WARN("tools: entry {} ({}) refused: {}", i, name, why);
        };
        if (!valid_tool_name(name)) { refuse("name must be 1-63 characters of [A-Za-z0-9_-]"); continue; }
        if (b && !builtin_overridable(b->id)) { refuse("this built-in cannot be replaced"); continue; }
        if (find_script(table, name)) { refuse("duplicate name"); continue; }
        if (table.tools.full()) { refuse("the tool table holds 32 tools"); continue; }

        ScriptTool t;
        t.name.assign(name);
        std::string_view criteria = json_str(v, "criteria");
        if (criteria.empty() || !t.criteria.assign(criteria)) { refuse("criteria must be 1-511 bytes"); continue; }
        std::string_view label = json_str(v, "label");
        if (!t.label.assign(label.empty() ? name : label)) { refuse("label is longer than 159 bytes"); continue; }
        yyjson_val *once = yyjson_obj_get(v, "once");
        t.once = once ? yyjson_get_bool(once) : true;
        bool reqs_ok = true;
        size_t ri, rn;
        yyjson_val *r;
        yyjson_arr_foreach(yyjson_obj_get(v, "requires"), ri, rn, r) {
            const char *rs = yyjson_get_str(r);
            reqs_ok = reqs_ok && rs && parse_requirement(rs, t.requires_);
        }
        if (!reqs_ok) { refuse("unknown requirement"); continue; }
        table.tools.push(t);
    }
    RLOG_INFO("tools: {} script tools loaded from {}, {} refused", table.tools.count, path.view(), table.refused);
    return {true, table.tools.count, table.refused};
}

// ---------------------------------------------------------------- state transitions

static std::string_view str_of(yyjson_val *obj, const char *key) { return json_str(obj, key); }

bool can_apply(const Case &c, yyjson_val *ev, std::string_view &reason) {
    std::string_view type = str_of(ev, "type");
    yyjson_val *d = yyjson_obj_get(ev, "data");
    if (c.events.full()) { reason = "the case event index is full"; return false; }
    if (type == "fact" && !c.facts.fits(str_of(d, "text").size())) { reason = "the case fact store is full"; return false; }
    if (type == "action_requested" && !c.find_pending(str_of(d, "action_id")) && c.pending.full()) {
        reason = "the case has too many pending actions";
        return false;
    }
    if (type == "action_result") {
        const Pending *p = c.find_pending(str_of(d, "action_id"));
        if (p && p->job && !str_of(d, "run_dir").empty() && c.runs.full()) { reason = "the case run list is full"; return false; }
    }
    return true;
}

static void assign_logged(auto &dst, std::string_view v, const Case &c, const char *field) {
    if (!dst.assign(v)) RLOG_WARN("case {}: {} truncated to {} bytes in memory (the event keeps the full text)", c.def.id.view(), field, dst.len);
}

// Derived state comes only from events, so replay after a restart gives the same state.
void apply(Case &c, yyjson_val *ev) {
    std::string_view type = str_of(ev, "type");
    yyjson_val *d = yyjson_obj_get(ev, "data");
    c.seq = int(yyjson_get_int(yyjson_obj_get(ev, "seq")));
    c.updated_ms = yyjson_get_sint(yyjson_obj_get(ev, "t_ms"));

    if (type == "case_opened") {
        c.status = Status::investigating;
        if (yyjson_get_str(yyjson_obj_get(d, "title"))) assign_logged(c.def.title, str_of(d, "title"), c, "title");
        if (yyjson_get_str(yyjson_obj_get(d, "setup"))) c.def.setup = str_of(d, "setup").empty() ? 'A' : str_of(d, "setup")[0];
        if (yyjson_get_str(yyjson_obj_get(d, "report"))) assign_logged(c.def.report, str_of(d, "report"), c, "report");
        if (yyjson_get_str(yyjson_obj_get(d, "difficulty"))) assign_logged(c.def.difficulty, str_of(d, "difficulty"), c, "difficulty");
        if (yyjson_get_str(yyjson_obj_get(d, "rev"))) assign_logged(c.def.rev, str_of(d, "rev"), c, "rev");
        if (yyjson_get_str(yyjson_obj_get(d, "rev_good"))) assign_logged(c.def.rev_good, str_of(d, "rev_good"), c, "rev_good");
        if (yyjson_get_str(yyjson_obj_get(d, "scenario_id"))) assign_logged(c.def.scenario_id, str_of(d, "scenario_id"), c, "scenario_id");
        if (yyjson_get_str(yyjson_obj_get(d, "mode"))) c.def.comparison = str_of(d, "mode") == "comparison";
        if (yyjson_obj_get(d, "view_seed")) c.def.view_seed = uint32_t(yyjson_get_uint(yyjson_obj_get(d, "view_seed")));
        // Execution metadata belongs to the run. Replaying a fresh run must not depend on a
        // template with the same id, or on a subsequently edited scenarios file.
        if (yyjson_val *execution = yyjson_obj_get(d, "execution")) {
            assign_logged(c.def.bugs, str_of(execution, "bugs"), c, "bugs");
            assign_logged(c.def.rev, str_of(execution, "rev"), c, "rev");
            assign_logged(c.def.rev_good, str_of(execution, "rev_good"), c, "rev_good");
            c.def.rev_good_given = yyjson_get_bool(yyjson_obj_get(execution, "rev_good_given"));
            c.def.withhold.clear();
            size_t i, n;
            yyjson_val *value;
            yyjson_arr_foreach(yyjson_obj_get(execution, "withhold"), i, n, value) {
                if (auto *slot = c.def.withhold.push()) slot->assign(yyjson_get_str(value) ? yyjson_get_str(value) : "");
            }
        }
    } else if (type == "action_requested") {
        std::string_view id = str_of(d, "action_id");
        Pending *p = const_cast<Pending *>(c.find_pending(id));
        if (!p) p = c.pending.push();
        if (p) {
            p->action_id.assign(id);
            p->kind.assign(str_of(d, "kind"));
            assign_logged(p->label, str_of(d, "label"), c, "label");
            p->dir.assign(str_of(d, "dir"));
            p->job = yyjson_get_bool(yyjson_obj_get(d, "job"));
            p->script = str_of(d, "runner") == "script";
            p->requested_ms = c.updated_ms;
        } else {
            RLOG_WARN("case {}: pending list full; {} not recorded", c.def.id.view(), id);
        }
        c.status = Status::waiting;
        assign_logged(c.waiting_for, str_of(d, "label"), c, "waiting_for");
    } else if (type == "action_result") {
        std::string_view id = str_of(d, "action_id");
        for (uint32_t i = 0; i < c.pending.count; ++i) {
            Pending &p = c.pending[i];
            if (!(p.action_id == id)) continue;
            std::string_view run_dir = str_of(d, "run_dir");
            if (p.job && !run_dir.empty()) {
                if (Run *r = c.runs.push()) {
                    r->action_id = p.action_id;
                    r->kind = p.kind;
                    r->dir.assign(run_dir);
                    r->label = p.label;
                    std::string_view setup = str_of(d, "setup");
                    r->setup = setup.empty() ? c.def.setup : setup[0];
                    r->baseline = yyjson_get_bool(yyjson_obj_get(d, "baseline"));
                } else {
                    RLOG_WARN("case {}: run list full; run {} not recorded", c.def.id.view(), id);
                }
            }
            std::string_view fix = str_of(yyjson_obj_get(d, "data"), "fix_rev");
            if (!fix.empty()) assign_logged(c.fix_rev, fix, c, "fix_rev");
            if (!c.is_done(p.kind.view())) c.done.push(p.kind);
            // Erase while keeping the remaining pending actions in request order.
            for (uint32_t k = i + 1; k < c.pending.count; ++k) c.pending[k - 1] = c.pending[k];
            --c.pending.count;
            break;
        }
        if (c.pending.count == 0 && c.status == Status::waiting) {
            c.status = Status::investigating;
            c.waiting_for.assign("");
        }
    } else if (type == "fact") {
        if (!c.facts.push(str_of(d, "text"))) RLOG_WARN("case {}: fact store full; fact not recorded", c.def.id.view());
    } else if (type == "conclusion") {
        c.concluded = true;
        assign_logged(c.diagnosis, str_of(d, "diagnosis"), c, "diagnosis");
    } else if (type == "stopped") {
        c.status = Status::stopped;
    } else if (type == "done" || type == "comparison_complete") {
        c.status = Status::done;
    } else if (type == "message") {
        Message &m = c.messages[c.message_count % kRecentMessages];
        assign_logged(m.text, str_of(d, "text"), c, "message text");
        m.channel.assign(str_of(d, "channel"));
        m.user.assign(str_of(d, "user"));
        ++c.message_count;
    }
}

} // namespace romeo
