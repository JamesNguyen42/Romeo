#include "models.hpp"

#include "agent.hpp"
#include "http.hpp"
#include "log.hpp"

namespace romeo {

// ---------------------------------------------------------------- slots

static size_t on_response(char *p, size_t size, size_t n, void *user) {
    auto *call = static_cast<ModelCall *>(user);
    call->response.add(std::string_view(p, size * n));
    if (call->response.overflowed()) {
        call->response_overflow = true;
        return 0; // aborts the transfer with CURLE_WRITE_ERROR
    }
    return size * n;
}

static size_t on_metrics(char *p, size_t size, size_t n, void *user) {
    auto *m = static_cast<Models *>(user);
    m->metrics_text->add(std::string_view(p, size * n));
    if (m->metrics_text->overflowed()) {
        m->metrics_overflow = true;
        return 0;
    }
    return size * n;
}

bool models_init(Service &s, Arena &slots) {
    Models &m = *s.models;
    m.multi = curl_multi_init();
    m.json_header = curl_slist_append(nullptr, "Content-Type: application/json");
    if (!m.multi || !m.json_header || !m.calls.init(slots, kModelSlots)) return false;
    m.calls.count = kModelSlots; // every slot exists from start-up; busy marks use
    for (uint32_t i = 0; i < kModelSlots; ++i) {
        m.calls[i].easy = curl_easy_init();
        if (!m.calls[i].easy) return false;
    }
    m.metrics_text = slots.alloc<FixedStringBuilder<kMetricsBytes>>();
    m.metrics_easy = curl_easy_init();
    return m.metrics_text && m.metrics_easy;
}

static bool is_route(CallKind k) { return k == CallKind::route_intent || k == CallKind::route_case; }

ModelCall *acquire_call(Service &s, CallKind kind) {
    Models &m = *s.models;
    // Case calls are bounded by the busy flag (one per case); thoughts and routing calls have their own shares, so
    // a case decision always finds a slot.
    if (kind == CallKind::think && m.thoughts_in_flight >= kThoughtSlots) {
        RLOG_WARN("models: {} thought calls in flight; thought refused", kThoughtSlots);
        return nullptr;
    }
    if (is_route(kind) && m.routes_in_flight >= kRouteSlots) {
        RLOG_WARN("models: {} routing calls in flight; routing call refused", kRouteSlots);
        return nullptr;
    }
    for (ModelCall &c : m.calls) {
        if (c.busy) continue;
        c.busy = true;
        c.response_overflow = false;
        c.kind = kind;
        c.options.clear();
        c.body.clear();
        c.response.clear();
        if (kind == CallKind::think) ++m.thoughts_in_flight;
        if (is_route(kind)) ++m.routes_in_flight;
        return &c;
    }
    RLOG_WARN("models: all {} call slots in use; call refused", kModelSlots);
    return nullptr;
}

void release_call(Service &s, ModelCall &call) {
    Models &m = *s.models;
    if (call.kind == CallKind::think) --m.thoughts_in_flight;
    if (is_route(call.kind)) --m.routes_in_flight;
    call.busy = false;
}

void post_call(Service &s, ModelCall &call, std::string_view url, long timeout_s) {
    FixedString<kUrlBytes> u(url);
    CURL *h = call.easy;
    curl_easy_reset(h);
    curl_easy_setopt(h, CURLOPT_URL, u.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, s.models->json_header);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, call.body.data);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, long(call.body.len));
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_response);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &call);
    curl_easy_setopt(h, CURLOPT_PRIVATE, &call);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_multi_add_handle(s.models->multi, h);
}

void write_chat_body(ModelCall &call, std::string_view system, std::string_view user, int max_tokens, bool think) {
    cat(call.body, "{\"model\":\"ornith-35b\",\"temperature\":0,\"max_tokens\":", max_tokens, ",\"chat_template_kwargs\":{\"enable_thinking\":",
        think ? "true" : "false", "},\"messages\":[{\"role\":\"system\",\"content\":", json_string(system),
        "},{\"role\":\"user\",\"content\":", json_string(user), "}]}");
}

// ---------------------------------------------------------------- metrics

// Sum of every sample line that starts with name; the value follows the first space.
static double metric(std::string_view text, std::string_view name) {
    double total = 0;
    for (size_t p = text.find(name); p != std::string_view::npos; p = text.find(name, p + 1)) {
        if (p > 0 && text[p - 1] != '\n') continue;
        size_t sp = text.find(' ', p);
        if (sp != std::string_view::npos) total += parse_leading_double(text.substr(sp + 1));
    }
    return total;
}

void scrape_metrics_if_due(Service &s, int64_t now) {
    Models &m = *s.models;
    if (m.metrics_busy || now < m.next_metrics_ms) return;
    m.next_metrics_ms = now + 1000;
    m.metrics_busy = true;
    m.metrics_overflow = false;
    m.metrics_text->clear();
    CURL *h = m.metrics_easy;
    curl_easy_reset(h);
    curl_easy_setopt(h, CURLOPT_URL, s.cfg.ornith_metrics.c_str());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_metrics);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(h, CURLOPT_PRIVATE, nullptr);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 300L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_multi_add_handle(m.multi, h);
}

static void metrics_done(Models &m, CURLcode rc) {
    m.metrics_busy = false;
    if (m.metrics_overflow) RLOG_WARN("models: /metrics page exceeds {} bytes; snapshot marked down", kMetricsBytes);
    if (rc != CURLE_OK) {
        m.ornith = OrnithSnapshot{};
        return;
    }
    std::string_view t = m.metrics_text->view();
    m.ornith = OrnithSnapshot{true,
                              metric(t, "vllm:generation_tokens_total"),
                              metric(t, "vllm:prompt_tokens_total"),
                              metric(t, "vllm:num_requests_running"),
                              metric(t, "vllm:spec_decode_num_draft_tokens_total"),
                              metric(t, "vllm:spec_decode_num_accepted_tokens_total"),
                              metric(t, "vllm:kv_cache_usage_perc")};
}

// ---------------------------------------------------------------- completions

void models_poll(Service &s) {
    Models &m = *s.models;
    int running = 0;
    curl_multi_perform(m.multi, &running);
    int left = 0;
    while (CURLMsg *msg = curl_multi_info_read(m.multi, &left)) {
        if (msg->msg != CURLMSG_DONE) continue;
        CURL *h = msg->easy_handle;
        const CURLcode rc = msg->data.result;
        long http = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &http);
        curl_multi_remove_handle(m.multi, h);

        if (h == m.metrics_easy) {
            metrics_done(m, rc);
            continue;
        }
        ModelCall *call = nullptr;
        curl_easy_getinfo(h, CURLINFO_PRIVATE, reinterpret_cast<char **>(&call));
        if (!call) continue;
        if (call->response_overflow) RLOG_WARN("models: response exceeds {} bytes; call failed", kModelResponseBytes);

        ArenaScope scope(s.request);
        bool reposted = false; // a routing call may ask its second question on the same slot
        switch (call->kind) {
        case CallKind::decide:
        case CallKind::think:
        case CallKind::conclude:
        case CallKind::stop_note: on_case_call_done(s, *call, http, rc); break;
        case CallKind::route_intent:
        case CallKind::route_case: reposted = on_route_call_done(s, *call, http, rc); break;
        }
        if (!reposted) release_call(s, *call);
    }
}

} // namespace romeo
