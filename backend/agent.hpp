// The agent step: event emission, decide / execute / ingest / conclude / verify / thought, renderer and script job
// supervision, WAL recovery and reset.
#pragma once

#include <cstdint>
#include <string_view>

#include <curl/curl.h>

#include "cases.hpp"
#include "models.hpp"

namespace romeo {

// ---------------------------------------------------------------- events

using EventLine = FixedStringBuilder<kMaxEventBytes>;

// Validates, appends to the WAL buffer, applies, and indexes one complete event line. Refusals (event too large,
// case capacity) are logged and freeze the case. Visible outside the process only after the next commit.
bool commit_event(Service &s, Case &c, EventLine &line);

// Appends one event: {"seq","case","t_ms","type","data"}. write_data appends the data object to the line.
template <typename WriteData>
    requires std::invocable<WriteData &, EventLine &>
bool emit(Service &s, Case &c, std::string_view type, WriteData &&write_data) {
    EventLine line;
    cat(line, "{\"seq\":", c.seq + 1, ",\"case\":", json_string(c.def.id.view()), ",\"t_ms\":", now_ms(), ",\"type\":", json_string(type),
        ",\"data\":");
    write_data(line);
    line.add('}');
    return commit_event(s, c, line);
}

bool emit_fact(Service &s, Case &c, std::string_view text, std::string_view evidence_json = "[]");

// ---------------------------------------------------------------- job supervision

// A job whose action_requested record waits for the WAL commit. Jobs start only after the commit.
struct SpawnRequest {
    uint32_t case_index = 0, generation = 0;
    FixedString<kNameBytes> action_id, kind;
    FixedString<64> args;
    char setup = 'A';
    bool baseline = false;
    bool script = false;
};

struct SpawnQueue {
    FixedArray<SpawnRequest, kModelSlots + 8> items;
};

// Makes the WAL buffer durable, streams the committed events, then starts the jobs whose records are now durable.
void commit_iteration(Service &s);

// Started job processes that have not written their exit file yet.
int job_processes_running(const Service &s);

// ---------------------------------------------------------------- lifecycle

// Rebuilds the case store from the WAL, then marks every case with pending actions as restarted.
bool recover(Service &s);
Case *open_case(Service &s, const CaseDef &def, std::string_view channel, std::string_view user);
// Archives the data directory and starts empty. False (with a reason) while jobs run.
bool reset_data(Service &s, FixedString<kPathBytes> &archived_to, std::string_view &reason);

// Finished jobs, timeouts, and next decisions. Runs every 250 ms.
void agent_tick(Service &s);
void on_case_call_done(Service &s, ModelCall &call, long http, CURLcode rc);

} // namespace romeo
