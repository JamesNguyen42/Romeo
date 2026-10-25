// Shared declarations of the case service: configuration, capacities, and the one Service context that the reactor
// thread owns. Every subsystem function takes Service& and runs on the reactor thread.
#pragma once

#include <cstdint>

#include "fixed.hpp"

namespace romeo {

// ---------------------------------------------------------------- capacities
//
// Each capacity is a property of the workload, sized once at start-up. Exhaustion is explicit: an HTTP 413/503 for
// a request, or a logged refusal for internal work. Nothing falls back to the heap.

// Paths: PATH_MAX on macOS is 1024. Configured roots are validated at start-up so every derived path fits.
inline constexpr uint32_t kPathBytes = 1024;
inline constexpr uint32_t kUrlBytes = 512;

// Case store. The demo has 10 cases; chat and the web form open more. 64 is six times the demo set.
inline constexpr uint32_t kMaxCases = 64;
inline constexpr uint32_t kMaxCaseDefs = 32;
// One investigation step is about 5 events (decision, action_requested, action_result, fact, thought) and a case
// stops after 9 completed actions, so a full case is about 60 events. Messages and restarts add a few more.
inline constexpr uint32_t kMaxEventsPerCase = 512;
// Largest single event: a case_opened with a 4 KiB report escaped, or a thought with 700 tokens of reasoning.
inline constexpr uint32_t kMaxEventBytes = 64u << 10;
inline constexpr uint32_t kTitleBytes = 256;
inline constexpr uint32_t kReportBytes = 4096;
inline constexpr uint32_t kMessageBytes = 4096;
inline constexpr uint32_t kNameBytes = 64;   // channel, user, tool name, action id
inline constexpr uint32_t kLabelBytes = 160;
inline constexpr uint32_t kDiagnosisBytes = 4096;
// Facts: about 12 per complete case at up to 1.5 KiB each (code search hits are the longest).
inline constexpr uint32_t kMaxFactsPerCase = 128;
inline constexpr uint32_t kFactBytesPerCase = 48u << 10;
inline constexpr uint32_t kMaxPendingPerCase = 8;
inline constexpr uint32_t kMaxRunsPerCase = 16;
inline constexpr uint32_t kRecentMessages = 3;

// Script tools from tools/catalog.json.
inline constexpr uint32_t kMaxScriptTools = 32;
inline constexpr uint32_t kCriteriaBytes = 512;

// Connection slots: three viewers (board, wall display, dashboard) with one stream each and a few requests in
// flight, plus three chat bots, doubled.
inline constexpr uint32_t kConnSlots = 32;
inline constexpr uint32_t kMaxHeaderBytes = 16384;      // the original request-header limit
inline constexpr uint32_t kMaxBodyBytes = 256u << 10;   // the original request-body limit
inline constexpr uint32_t kConnInBytes = kMaxHeaderBytes + 4 + kMaxBodyBytes + 1;
// One response or one stream backlog. A complete case (about 60 events at 1-4 KiB) is under 200 KiB.
inline constexpr uint32_t kConnOutBytes = 512u << 10;

// Model call slots: at most one decide/conclude/stop call per case (the busy flag), plus bounded thought calls and
// chat routing calls.
inline constexpr uint32_t kThoughtSlots = 8;
inline constexpr uint32_t kRouteSlots = 8;
inline constexpr uint32_t kModelSlots = kMaxCases + kThoughtSlots + kRouteSlots;
// Decide body: report (4 KiB) + facts (48 KiB) + messages (12 KiB) + criteria (~20 KiB with script tools), escaped.
// Clef refuses bodies over 256 KiB.
inline constexpr uint32_t kModelBodyBytes = 192u << 10;
// Chat responses carry at most 700 tokens of content and reasoning.
inline constexpr uint32_t kModelResponseBytes = 64u << 10;
// The vLLM /metrics page measured 54,574 bytes; 256 KiB is 4.8 times that.
inline constexpr uint32_t kMetricsBytes = 256u << 10;

// WAL write buffer: one reactor iteration appends at most a few dozen events.
inline constexpr uint32_t kWalBufferBytes = 1u << 20;

// Per-request scratch: a 256 KiB body plus its yyjson pool (13 bytes per input byte, in situ), or one case's events
// (bounded by kConnOutBytes) plus a per-event pool.
inline constexpr size_t kRequestArenaBytes = 4u << 20;

// Evidence scratch. The largest analysis is compare_images on two 3840x2160 RGBA images:
//   diff image                     33,177,600
//   two input images               66,355,200  (rewound before the PNG encode)
//   stb PNG encode, worst case:    filter rows 33,179,760 + zlib hash chains 6,029,312
//                                  + zlib output growth (in place at the arena top) 2 x 37,327,000
//                                  + PNG assembly 37,327,000                         = about 151,190,000
// Peak = diff + max(inputs, encode) = about 184.4 MB. 256 MiB (268,435,456) keeps 45% headroom for the worst case.
// trace.json analysis uses file bytes + 12 x file bytes of yyjson pool, so it handles traces up to about 20 MB.
inline constexpr size_t kEvidenceArenaBytes = 256u << 20;
inline constexpr size_t kMaxImageBytes = size_t(3840) * 2160 * 4;

// ---------------------------------------------------------------- configuration

struct Config {
    int port = 8790;
    FixedString<kPathBytes> data, render, cases_file, repo_dir, ui, dashboard, tools;
    FixedString<kUrlBytes> ornith_metrics, clef, ornith, memory;
    bool think = true;
    int frames = 300;
    int max_active = 0;   // ROMEO_MAX_ACTIVE: cases in progress at once in demo mode (0 = no limit)
    FixedString<kPathBytes> render_abs, tools_abs; // resolved once at start-up, like fs::absolute
};

// ---------------------------------------------------------------- service context

struct CaseStore;
struct Wal;
struct Models;
struct Http;
struct Sampler;
struct SpawnQueue;
struct ScriptTools;

struct Service {
    Config cfg;
    Arena request;  // per-request scratch, rewound after each request or completion
    Arena evidence; // evidence scratch, rewound after each analysis
    CaseStore *store = nullptr;
    Wal *wal = nullptr;
    Models *models = nullptr;
    Http *http = nullptr;
    Sampler *sampler = nullptr;
    SpawnQueue *spawns = nullptr;
    ScriptTools *scripts = nullptr;
};

int64_t now_ms();

} // namespace romeo
