// HTTP: fixed connection slots, request parsing, routes, and the Server-Sent-Event stream.
//
// Every connection is non-blocking and owned by the reactor. A slot holds one request (headers plus a body of at
// most 256 KiB) and one output buffer: a response, a file being streamed, or a stream subscriber's backlog.
// Responses are written only in the flush phase, after the iteration's WAL commit, so no client sees state that is
// not durable. All connections close after one response (Connection: close), except stream subscribers.
#pragma once

#include <cstdint>
#include <string_view>

#include <curl/curl.h>
#include <sys/types.h>

#include "models.hpp"
#include "wal.hpp"

namespace romeo {

enum class ConnState : uint8_t { free, reading, awaiting_model, writing, stream };

// A chat routing request waiting for Clef.
struct RouteState {
    FixedString<kMessageBytes> text;
    FixedString<kNameBytes> channel, user, case_id, intent;
    FixedStringBuilder<512> probabilities; // {"new_report":0.7,...}
    int64_t deadline_ms = 0;
};

struct Conn {
    int fd = -1;
    ConnState state = ConnState::free;
    uint32_t generation = 0;
    FixedStringBuilder<kConnInBytes> in;
    FixedStringBuilder<kConnOutBytes> out;
    uint32_t out_sent = 0;
    int file_fd = -1;
    uint64_t file_left = 0;
    uint64_t stream_from_lsn = 0;
    RouteState route;
};

// Last system sample, written by the 1 s sampler and read by GET /api/system without blocking.
struct Sampler {
    double gpu_util = 0, gpu_power = 0, gpu_temp = 0, gpu_clock = 0;
    long mem_total_kb = 0, mem_avail_kb = 0;
    FixedString<kPathBytes> nvidia_smi; // resolved at start-up; empty when absent
    pid_t pid = -1;
    int pipe_fd = -1;
    char line[256] = {};
    uint32_t line_len = 0;
    int64_t next_ms = 0, started_ms = 0;
};

struct Http {
    int listen_fd = -1;
    ArenaArray<Conn> conns;
    uint32_t next_generation = 1;
    uint32_t poll_conn[kConnSlots]; // conn index of each polled fd after the listen socket
    uint32_t poll_count = 0;
};

bool http_init(Service &s, Arena &slots);

// Poll registration and the read phase. fds[0] is the listen socket.
uint32_t http_poll_fds(Service &s, curl_waitfd *fds, uint32_t capacity);
void http_on_poll(Service &s, const curl_waitfd *fds, uint32_t count);
// Write phase: runs after the WAL commit.
void http_flush(Service &s);
void http_check_deadlines(Service &s, int64_t now);

// Streams one durable event to every subscriber that connected before it was appended.
void broadcast_committed(Service &s, const WalRecord &record);

// Completion of a chat routing call. Returns true when the slot was reused for the second question.
bool on_route_call_done(Service &s, ModelCall &call, long http, CURLcode rc);

} // namespace romeo
