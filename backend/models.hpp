// Model calls on the reactor's curl multi handle: Clef decisions, Ornith chat calls, chat routing questions, and the
// 1 s vLLM /metrics scrape.
//
// Every call owns one fixed slot (request body and response buffer) reserved at start-up. The easy handles are also
// created once and reused. libcurl allocates internally (connection cache, DNS, TLS state); that allocation is
// accepted at this library boundary and stays out of the case store and the request path.
#pragma once

#include <cstdint>
#include <string_view>

#include <curl/curl.h>

#include "cases.hpp"

namespace romeo {

enum class CallKind : uint8_t { decide, think, conclude, stop_note, route_intent, route_case };

struct ModelCall {
    CURL *easy = nullptr;
    bool busy = false;
    bool response_overflow = false;
    CallKind kind = CallKind::decide;
    uint32_t case_index = 0, case_generation = 0; // case calls
    uint32_t conn_index = 0, conn_generation = 0; // routing calls
    FixedArray<FixedString<kNameBytes>, kToolCount + kMaxScriptTools> options; // choice options, in request order
    FixedStringBuilder<kModelBodyBytes> body;
    FixedStringBuilder<kModelResponseBytes> response;
};

struct OrnithSnapshot {
    bool up = false;
    double generation_tokens = 0, prompt_tokens = 0, requests_running = 0, spec_draft = 0, spec_accepted = 0, kv_cache_usage = 0;
};

struct Models {
    CURLM *multi = nullptr;
    curl_slist *json_header = nullptr; // one shared "Content-Type: application/json" list
    ArenaArray<ModelCall> calls;
    uint32_t thoughts_in_flight = 0, routes_in_flight = 0;

    CURL *metrics_easy = nullptr;
    bool metrics_busy = false, metrics_overflow = false;
    FixedStringBuilder<kMetricsBytes> *metrics_text = nullptr;
    OrnithSnapshot ornith;
    int64_t next_metrics_ms = 0;
};

// Start-up: easy handles and slots. slots must hold kModelSlots calls plus the metrics buffer.
bool models_init(Service &s, Arena &slots);

// A free slot for this kind, or null (logged) when the kind's share of the slots is in use.
ModelCall *acquire_call(Service &s, CallKind kind);
void release_call(Service &s, ModelCall &call);
// Starts the POST of call.body. The slot stays busy until its completion is dispatched.
void post_call(Service &s, ModelCall &call, std::string_view url, long timeout_s);

// {"model":"ornith-35b",...,"messages":[system,user]} into call.body.
void write_chat_body(ModelCall &call, std::string_view system, std::string_view user, int max_tokens, bool think);

// Starts the metrics scrape when it is due and not in flight.
void scrape_metrics_if_due(Service &s, int64_t now);
// Drives transfers and dispatches every finished call.
void models_poll(Service &s);

} // namespace romeo
