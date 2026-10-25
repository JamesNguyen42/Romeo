// Operational logging through quill: stdout and <data>/romeo.log. Case events are not logs; they go to the WAL.
//
// The reactor thread only encodes arguments into its quill queue. The queue is bounded (reserved once, 4 MiB) and
// drops messages when full, so logging never blocks or grows the reactor. quill's backend thread formats and writes;
// its own allocation stays on that thread.
#pragma once

#include <string_view>

#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"

namespace romeo {

struct LogOptions : quill::FrontendOptions {
    static constexpr quill::QueueType queue_type = quill::QueueType::BoundedDropping;
    static constexpr size_t initial_queue_capacity = 4u << 20;
};
using LogFrontend = quill::FrontendImpl<LogOptions>;
using Logger = quill::LoggerImpl<LogOptions>;

extern Logger *g_log;

// Starts the quill backend thread and the two sinks. Start-up only.
bool log_start(std::string_view file_path);
// Blocks until everything logged so far is written. Used before abort().
void log_flush();

} // namespace romeo

#define RLOG_INFO(...) LOG_INFO(::romeo::g_log, __VA_ARGS__)
#define RLOG_WARN(...) LOG_WARNING(::romeo::g_log, __VA_ARGS__)
#define RLOG_ERROR(...) LOG_ERROR(::romeo::g_log, __VA_ARGS__)
