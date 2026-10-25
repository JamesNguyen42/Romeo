// Case model: case definitions, derived case state, the tool catalog, and the candidate rules.
//
// Case state is derived only from events. apply() is the single transition function: the live path and WAL recovery
// both parse the event JSON and call it, so a restart rebuilds exactly the state the service had.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "service.hpp"
#include "yyjson.h"

namespace romeo {

// ---------------------------------------------------------------- tool catalog

enum class ToolKind : uint8_t {
    renderer_job,   // detached renderer process; result read from its job directory
    local_evidence, // reads evidence already on disk inside the same step
    model,          // a model call (conclusion, stop note)
};

// Preconditions for offering a tool. Script tools use the same vocabulary ("any_run", "baseline_run", "facts>=N",
// "conclusion"); both_runs is the built-in image-comparison rule.
struct Requirements {
    bool any_run = false;
    bool baseline_run = false;
    bool both_runs = false; // a previous-build run and a current-build run on the case setup
    bool conclusion = false;
    bool fix = false; // an earlier action_result carried data.fix_rev
    uint8_t min_facts = 0;
};

enum class ToolId : uint8_t {
    measure_frames,
    measure_memory,
    capture_trace,
    compare_baseline,
    try_smaller_setup,
    fetch_logs,
    check_disk,
    compare_images,
    search_code,
    recall_similar,
    conclude,
    stop_need_more,
    verify_fix, // started after a conclusion; never offered to Clef
    count,
};

struct ToolInfo {
    ToolId id;
    std::string_view name, criteria, label;
    ToolKind kind;
    Requirements requires_;
    bool offered;
};

// The typed action catalog Clef chooses from. Criteria are what Clef reads; labels are what people read.
inline constexpr ToolInfo kTools[] = {
    {ToolId::measure_frames, "measure_frames", "Run the current build and measure frame times (median, p99, spikes)", "frame-time run", ToolKind::renderer_job, {}, true},
    {ToolId::measure_memory, "measure_memory", "Run the current build longer and record resident memory over time", "long memory run", ToolKind::renderer_job, {}, true},
    {ToolId::capture_trace, "capture_trace", "Run with a timing trace of every thread and find where frames wait", "trace capture", ToolKind::renderer_job, {}, true},
    {ToolId::compare_baseline, "compare_baseline", "Run the previous build on the same setup for a before/after comparison", "previous-build run", ToolKind::renderer_job, {}, true},
    {ToolId::try_smaller_setup, "try_smaller_setup", "Run the current build on setup A to see whether the problem depends on the setup", "setup A run", ToolKind::renderer_job, {}, true},
    {ToolId::fetch_logs, "fetch_logs", "Fetch the run logs from the log store and look for errors and fatal lines", "log fetch", ToolKind::local_evidence, {.any_run = true}, true},
    {ToolId::check_disk, "check_disk", "Check how much temporary data the last run left on disk", "disk check", ToolKind::local_evidence, {.any_run = true}, true},
    {ToolId::compare_images, "compare_images", "Compare the rendered image of the current and previous build pixel by pixel", "image comparison", ToolKind::local_evidence, {.both_runs = true}, true},
    {ToolId::search_code, "search_code", "Search the renderer source for code related to the evidence", "code search", ToolKind::local_evidence, {.min_facts = 2}, true},
    {ToolId::recall_similar, "recall_similar", "Look up similar past cases in case memory", "memory lookup", ToolKind::local_evidence, {}, true},
    {ToolId::conclude, "conclude", "Enough evidence: state the cause and propose a fix", "conclusion", ToolKind::model, {.min_facts = 3}, true},
    {ToolId::stop_need_more, "stop_need_more", "Stop: the available tools cannot establish more", "stop", ToolKind::model, {}, true},
    {ToolId::verify_fix, "verify_fix", "", "verification run", ToolKind::renderer_job, {}, false},
};

inline constexpr size_t kToolCount = size_t(ToolId::count);

constexpr bool tool_table_matches_enum() {
    if (std::size(kTools) != kToolCount) return false;
    for (size_t i = 0; i < std::size(kTools); ++i)
        if (size_t(kTools[i].id) != i) return false;
    return true;
}
constexpr bool tool_names_unique() {
    for (size_t i = 0; i < std::size(kTools); ++i)
        for (size_t j = i + 1; j < std::size(kTools); ++j)
            if (kTools[i].name == kTools[j].name) return false;
    return true;
}
static_assert(tool_table_matches_enum(), "kTools must list every ToolId once, in enum order");
static_assert(tool_names_unique(), "tool names must be unique");

constexpr const ToolInfo &tool(ToolId id) { return kTools[size_t(id)]; }

constexpr const ToolInfo *find_builtin(std::string_view name) {
    for (const ToolInfo &t : kTools)
        if (t.name == name) return &t;
    return nullptr;
}

// Built-ins that a script tool may not replace: the conclusion flow depends on them.
constexpr bool builtin_overridable(ToolId id) { return id != ToolId::conclude && id != ToolId::stop_need_more && id != ToolId::verify_fix; }

// ---------------------------------------------------------------- script tools (tools/catalog.json)

struct ScriptTool {
    FixedString<kNameBytes> name;
    FixedString<kCriteriaBytes> criteria;
    FixedString<kLabelBytes> label;
    Requirements requires_;
    bool once = true;
};

struct ScriptTools {
    FixedArray<ScriptTool, kMaxScriptTools> tools;
    uint32_t refused = 0; // entries refused by the last load
};

struct CatalogLoad {
    bool ok;           // false: catalog.json exists but is not a valid catalog; the previous table is kept
    uint32_t loaded, refused;
};

// Loads <tools>/catalog.json into the fixed table. A missing file gives an empty table.
CatalogLoad load_script_tools(Service &s);

// A tool as the agent sees it after merging: a script tool with a built-in's name replaces that built-in.
struct ResolvedTool {
    std::string_view name, criteria, label;
    const ToolInfo *builtin; // null for a script tool
    const ScriptTool *script;
    Requirements requires_;
    bool once;
};

bool resolve_tool(const ScriptTools &scripts, std::string_view name, ResolvedTool &out);

// ---------------------------------------------------------------- case definitions

struct CaseDef {
    FixedString<kNameBytes> id;
    // Fresh runs retain their source scenario independently of the unique run id.
    FixedString<kNameBytes> scenario_id;
    FixedString<kTitleBytes> title;
    FixedString<kReportBytes> report;
    FixedString<128> bugs, difficulty, rev, rev_good;
    bool rev_good_given = false;
    bool comparison = false;
    uint32_t view_seed = 0;
    FixedArray<FixedString<kNameBytes>, 16> withhold;
    char setup = 'A';

    bool withholds(std::string_view tool_name) const;
};

// ---------------------------------------------------------------- derived case state

enum class Status : uint8_t { investigating, waiting, stopped, done };
inline constexpr std::string_view kStatusNames[] = {"investigating", "waiting", "stopped", "done"};
inline std::string_view status_name(Status s) { return kStatusNames[size_t(s)]; }

struct Pending {
    FixedString<kNameBytes> action_id, kind;
    FixedString<kLabelBytes> label;
    FixedString<kPathBytes> dir;
    int64_t requested_ms = 0; // t_ms of the action_requested event
    bool job = false;
    bool script = false; // a tools/<name> job ("runner":"script")
};

struct Run {
    FixedString<kNameBytes> action_id, kind;
    FixedString<kPathBytes> dir;
    FixedString<kLabelBytes> label;
    char setup = 'A';
    bool baseline = false;
};

struct Message {
    FixedString<kMessageBytes> text;
    FixedString<kNameBytes> channel, user;
};

struct EventRef {
    uint64_t payload_offset;
    uint32_t length;
};

// Ordered text list over one fixed block.
template <uint32_t MaxItems, uint32_t Bytes>
struct TextList {
    struct Ref {
        uint32_t offset, length;
    };
    FixedArray<Ref, MaxItems> refs;
    uint32_t used = 0;
    char text[Bytes];

    bool fits(size_t n) const { return refs.count < MaxItems && used + n <= Bytes; }
    bool push(std::string_view s) {
        if (!fits(s.size())) return false;
        std::memcpy(text + used, s.data(), s.size());
        refs.push(Ref{used, uint32_t(s.size())});
        used += uint32_t(s.size());
        return true;
    }
    uint32_t count() const { return refs.count; }
    std::string_view operator[](uint32_t i) const { return {text + refs[i].offset, refs[i].length}; }
    void clear() { refs.clear(); used = 0; }
};

struct Case {
    CaseDef def;
    Status status = Status::investigating;
    FixedString<kLabelBytes> waiting_for;
    FixedString<kDiagnosisBytes> diagnosis;
    FixedString<128> fix_rev; // data.fix_rev of the latest action_result that carried one
    TextList<kMaxFactsPerCase, kFactBytesPerCase> facts;
    FixedArray<FixedString<kNameBytes>, kToolCount + kMaxScriptTools> done; // completed action kinds
    FixedArray<Pending, kMaxPendingPerCase> pending;
    FixedArray<Run, kMaxRunsPerCase> runs;
    Message messages[kRecentMessages]; // ring of the latest messages
    uint32_t message_count = 0;
    FixedArray<EventRef, kMaxEventsPerCase> events;
    int seq = 0;
    int64_t updated_ms = 0;
    uint32_t generation = 0; // changes when the slot is reused, so late model completions are dropped
    bool busy = false;       // a decide, conclude or stop call for this case is in flight
    bool concluded = false;
    bool frozen = false;     // an event was refused for capacity; the case no longer advances

    bool is_done(std::string_view kind) const;
    const Pending *find_pending(std::string_view action_id) const;
};

struct CaseStore {
    ArenaArray<Case> cases;
    FixedArray<CaseDef, kMaxCaseDefs> defs; // sorted by id, like the original std::map
    uint32_t next_generation = 1;
    uint32_t decisions_total = 0; // Clef decisions since start, for the dashboard
    double decision_ms_total = 0;
};

Case *find_case(CaseStore &store, std::string_view id);
const CaseDef *find_def(const CaseStore &store, std::string_view id);
// Takes a fresh slot for a new case, or null when the store is full.
Case *new_case(CaseStore &store);
void clear_cases(CaseStore &store);

// Reads the case definitions file into store.defs. Returns the number loaded; refusals are logged.
uint32_t load_defs(Service &s);

// The single state transition. ev is a parsed event object.
void apply(Case &c, yyjson_val *ev);
// Capacity check before an event is written: false if apply() could not record it completely.
bool can_apply(const Case &c, yyjson_val *ev, std::string_view &reason);

int running_jobs(const CaseStore &store);
bool has_run(const Case &c, bool baseline);
bool requirements_met(const Case &c, const Requirements &r);

// The tools Clef may choose from next, in catalog order (built-ins, then script tools).
struct Candidates {
    FixedArray<ResolvedTool, kToolCount + kMaxScriptTools> items;
};
void candidates(const Case &c, const ScriptTools &scripts, Candidates &out);

// "Report: ...\nSetup X.\nEvidence so far:\n..." (what Ornith reads).
template <uint32_t N>
void write_case_state(FixedStringBuilder<N> &b, const Case &c) {
    cat(b, "Report: ", c.def.report, "\nSetup ", c.def.setup, ".\nEvidence so far:\n");
    if (c.facts.count() == 0) b.add("(none yet)\n");
    for (uint32_t i = 0; i < c.facts.count(); ++i) cat(b, i + 1, ". ", c.facts[i], "\n");
}

// What Clef reads: the case state, the latest messages from people, and the completed action count.
template <uint32_t N>
void write_clef_state(FixedStringBuilder<N> &b, const Case &c) {
    write_case_state(b, c);
    if (c.message_count > 0) {
        b.add("Messages from people:\n");
        uint32_t n = c.message_count < kRecentMessages ? c.message_count : kRecentMessages;
        for (uint32_t k = 0; k < n; ++k) {
            const Message &m = c.messages[(c.message_count - n + k) % kRecentMessages];
            cat(b, "- ", m.user.empty() ? std::string_view("someone") : m.user.view());
            if (!m.channel.empty()) cat(b, " (", m.channel, ")");
            cat(b, ": ", m.text, "\n");
        }
    }
    cat(b, "\nCompleted actions: ", c.done.count);
}

} // namespace romeo
