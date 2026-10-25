// Fixed-capacity building blocks shared by the renderer and the case service.
//
// Nothing here allocates after construction. Every container has a compile-time capacity and reports exhaustion
// explicitly (nullptr / false / overflowed()); there is no hidden heap fallback.
#pragma once

#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string_view>
#include <type_traits>

namespace romeo {

// ---------------------------------------------------------------- strings

// Owned, fixed-capacity text. Assigning text longer than N - 1 bytes truncates and returns false.
template <uint32_t N>
struct FixedString {
    static_assert(N > 1);
    char data[N];
    uint32_t len = 0;

    FixedString() { data[0] = 0; }
    explicit FixedString(std::string_view s) { assign(s); }

    bool assign(std::string_view s) {
        uint32_t n = s.size() < N - 1 ? uint32_t(s.size()) : N - 1;
        std::memcpy(data, s.data(), n);
        data[n] = 0;
        len = n;
        return n == s.size();
    }
    std::string_view view() const { return {data, len}; }
    const char *c_str() const { return data; }
    bool empty() const { return len == 0; }
    bool operator==(std::string_view s) const { return view() == s; }
};

// Appends text and numbers into caller-visible fixed storage. The first append that does not fit sets
// overflowed(); later appends are ignored, so a truncated result is never mistaken for a complete one.
template <uint32_t N>
struct FixedStringBuilder {
    char data[N];
    uint32_t len = 0;
    bool overflow = false;

    FixedStringBuilder() { data[0] = 0; }

    bool overflowed() const { return overflow; }
    std::string_view view() const { return {data, len}; }
    const char *c_str() { data[len < N ? len : N - 1] = 0; return data; }
    void clear() { len = 0; overflow = false; data[0] = 0; }

    FixedStringBuilder &add(std::string_view s) {
        if (overflow) return *this;
        if (len + s.size() >= N) { overflow = true; return *this; }
        std::memcpy(data + len, s.data(), s.size());
        len += uint32_t(s.size());
        data[len] = 0;
        return *this;
    }
    FixedStringBuilder &add(char c) { return add(std::string_view(&c, 1)); }

    template <typename I, typename = std::enable_if_t<std::is_integral_v<I> && !std::is_same_v<I, char> && !std::is_same_v<I, bool>>>
    FixedStringBuilder &num(I v) {
        char tmp[24];
        auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
        return add(std::string_view(tmp, size_t(r.ptr - tmp)));
    }
    FixedStringBuilder &num(double v, int precision) {
        char tmp[48];
        auto r = std::to_chars(tmp, tmp + sizeof tmp, v, std::chars_format::fixed, precision);
        if (r.ec != std::errc()) return add("0");
        return add(std::string_view(tmp, size_t(r.ptr - tmp)));
    }
    FixedStringBuilder &boolean(bool b) { return add(b ? "true" : "false"); }

    // JSON string body without the surrounding quotes.
    FixedStringBuilder &escaped(std::string_view s) {
        static constexpr char kHex[] = "0123456789abcdef";
        for (unsigned char c : s) {
            switch (c) {
            case '"': add("\\\""); break;
            case '\\': add("\\\\"); break;
            case '\n': add("\\n"); break;
            case '\t': add("\\t"); break;
            case '\r': break;
            default:
                if (c < 0x20) { char u[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 15]}; add(std::string_view(u, 6)); }
                else add(char(c));
            }
        }
        return *this;
    }
    // JSON string value with quotes.
    FixedStringBuilder &str(std::string_view s) { return add('"').escaped(s).add('"'); }
};

// ---------------------------------------------------------------- memory

// Bump allocator over one block reserved at start-up. Allocation is a pointer increment; reset() or
// rewind(mark) releases everything allocated after that point. Exhaustion returns nullptr.
struct Arena {
    uint8_t *base = nullptr;
    size_t capacity = 0, used = 0, high_water = 0;

    Arena() = default;
    Arena(void *memory, size_t bytes) : base(static_cast<uint8_t *>(memory)), capacity(bytes) {}

    void *alloc_bytes(size_t bytes, size_t align = alignof(std::max_align_t)) {
        size_t start = (used + align - 1) & ~(align - 1);
        if (start + bytes > capacity) return nullptr;
        used = start + bytes;
        if (used > high_water) high_water = used;
        return base + start;
    }
    template <typename T>
    T *alloc(size_t count = 1) {
        static_assert(std::is_trivially_destructible_v<T>, "arena memory is released without destructors");
        void *p = alloc_bytes(sizeof(T) * count, alignof(T));
        if (!p) return nullptr;
        T *t = static_cast<T *>(p);
        for (size_t i = 0; i < count; ++i) new (t + i) T();
        return t;
    }
    size_t mark() const { return used; }
    void rewind(size_t m) { used = m; }
    void reset() { used = 0; }

    // Bytes to budget so that alloc_bytes(bytes, align) fits from any cursor position: the request plus the
    // worst-case alignment padding. Summing these over every planned allocation sizes an arena exactly enough.
    static constexpr size_t budget_bytes(size_t bytes, size_t align = alignof(std::max_align_t)) { return bytes + align - 1; }
    template <typename T>
    static constexpr size_t budget(size_t count = 1) { return budget_bytes(sizeof(T) * count, alignof(T)); }
};

// Scoped bump scratch: everything allocated inside the scope is released when the scope ends.
struct ArenaScope {
    Arena &arena;
    size_t saved;
    explicit ArenaScope(Arena &a) : arena(a), saved(a.mark()) {}
    ~ArenaScope() { arena.rewind(saved); }
    ArenaScope(const ArenaScope &) = delete;
    ArenaScope &operator=(const ArenaScope &) = delete;
};

// A span of T allocated from an arena; capacity fixed at creation.
template <typename T>
struct ArenaArray {
    T *items = nullptr;
    uint32_t count = 0, capacity = 0;

    bool init(Arena &a, uint32_t cap) {
        items = a.alloc<T>(cap);
        capacity = items ? cap : 0;
        count = 0;
        return items != nullptr;
    }
    T *push() { return count < capacity ? &items[count++] : nullptr; }
    bool push(const T &v) { T *p = push(); if (p) *p = v; return p != nullptr; }
    void clear() { count = 0; }
    T &operator[](uint32_t i) { return items[i]; }
    const T &operator[](uint32_t i) const { return items[i]; }
    T *begin() { return items; }
    T *end() { return items + count; }
    const T *begin() const { return items; }
    const T *end() const { return items + count; }
};

// Inline fixed-capacity array.
template <typename T, uint32_t N>
struct FixedArray {
    T items[N];
    uint32_t count = 0;

    T *push() { return count < N ? &items[count++] : nullptr; }
    bool push(const T &v) { T *p = push(); if (p) *p = v; return p != nullptr; }
    void remove_swap(uint32_t i) { items[i] = items[--count]; }
    void clear() { count = 0; }
    bool full() const { return count == N; }
    T &operator[](uint32_t i) { return items[i]; }
    const T &operator[](uint32_t i) const { return items[i]; }
    T *begin() { return items; }
    T *end() { return items + count; }
    const T *begin() const { return items; }
    const T *end() const { return items + count; }
};

// ---------------------------------------------------------------- checksums

inline uint32_t crc32(const void *data, size_t n, uint32_t crc = 0) {
    static const auto table = [] {
        struct T { uint32_t v[256]; } t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t.v[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; ++i) crc = table.v[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}


// ---------------------------------------------------------------- text parts and number parsing

// printf-compatible fixed-point number: decimal(v, 1) prints like "%.1f", signed_decimal(v, 0) like "%+.0f".
struct Decimal {
    double value;
    int precision;
    bool plus;
};
constexpr Decimal decimal(double v, int precision) { return {v, precision, false}; }
constexpr Decimal signed_decimal(double v, int precision) { return {v, precision, true}; }

// A JSON string value: printed with quotes and escaping.
struct JsonString {
    std::string_view text;
};
constexpr JsonString json_string(std::string_view s) { return {s}; }

template <typename T>
concept HasTextView = requires(const T &t) {
    { t.view() } -> std::convertible_to<std::string_view>;
};

// Everything cat() can append to a FixedStringBuilder.
template <typename T>
concept TextPart = std::convertible_to<const T &, std::string_view> || HasTextView<T> || std::same_as<T, char> ||
                   std::same_as<T, Decimal> || std::same_as<T, JsonString> ||
                   (std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char>);

template <uint32_t N, TextPart T>
void append_part(FixedStringBuilder<N> &b, const T &part) {
    if constexpr (std::same_as<T, Decimal>) {
        if (part.plus && !std::signbit(part.value)) b.add('+');
        b.num(part.value, part.precision);
    } else if constexpr (std::same_as<T, JsonString>) {
        b.str(part.text);
    } else if constexpr (std::same_as<T, char>) {
        b.add(part);
    } else if constexpr (std::convertible_to<const T &, std::string_view>) {
        b.add(std::string_view(part));
    } else if constexpr (HasTextView<T>) {
        b.add(part.view());
    } else {
        b.num(part);
    }
}

// Appends every part in order. Overflow follows FixedStringBuilder: the first part that does not fit sets
// overflowed() and the rest are ignored.
template <uint32_t N, TextPart... Parts>
FixedStringBuilder<N> &cat(FixedStringBuilder<N> &b, const Parts &...parts) {
    (append_part(b, parts), ...);
    return b;
}

// C-locale atoi semantics: leading whitespace, optional sign, longest digit prefix; 0 when there is no number.
inline long long parse_leading_int(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))) ++i;
    if (i < s.size() && s[i] == '+') ++i;
    long long v = 0;
    std::from_chars(s.data() + i, s.data() + s.size(), v);
    return v;
}

// C-locale atof semantics for decimal input: leading whitespace, optional sign, longest number prefix
// (including inf and nan); 0 when there is no number.
inline double parse_leading_double(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))) ++i;
    if (i < s.size() && s[i] == '+') ++i;
    double v = 0;
    std::from_chars(s.data() + i, s.data() + s.size(), v);
    return v;
}

} // namespace romeo
