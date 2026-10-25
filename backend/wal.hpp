// Write-ahead log of case events: runs/data/wal.log.
//
// Record layout (little-endian, packed, 20-byte header):
//   u32 magic 'RWAL' | u32 payload length | u32 crc32(payload) | u64 lsn | payload
// The payload is one event JSON line without a newline: {"seq","case","t_ms","type","data"}. LSNs start at 1 and
// increase by one per record.
//
// Appends go to a fixed write buffer. commit() writes the buffer and makes it durable (fdatasync on Linux, fsync on
// macOS) once per reactor iteration. Nothing that depends on an appended record may leave the process (stream
// broadcast, renderer job start, HTTP response) before commit() has returned.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "service.hpp"

namespace romeo {

static_assert(std::endian::native == std::endian::little, "the WAL header is written in native little-endian order");

inline constexpr uint32_t kWalMagic = 0x4C415752u; // "RWAL"
inline constexpr uint32_t kWalHeaderBytes = 20;

struct WalRecord {
    uint64_t lsn;
    uint64_t payload_offset; // file offset of the payload
    std::string_view payload;
};

struct Wal {
    int fd = -1;
    FixedString<kPathBytes> path;
    uint64_t durable_end = 0; // bytes on disk and synced
    uint64_t next_lsn = 1;
    uint64_t durable_lsn = 0; // highest committed LSN
    uint8_t *buffer = nullptr;
    uint32_t capacity = 0, used = 0;

    bool dirty() const { return used > 0; }
};

// Binds the write buffer. The buffer is owned by the start-up reservation.
void wal_init(Wal &w, uint8_t *buffer, uint32_t capacity);

// Opens (creating if needed) the log, scans every record, truncates a torn or corrupt tail, and calls on_record for
// each good record in order. scratch holds one payload at a time. Returns false only if the file cannot be opened.
template <typename OnRecord>
bool wal_open_and_recover(Wal &w, std::string_view path, Arena &scratch, OnRecord &&on_record);

// Appends one record to the write buffer. Returns false (and changes nothing) if the payload exceeds
// kMaxEventBytes. A full buffer is first made durable with wal_flush(), so on_flushed runs for those records.
template <typename OnCommitted>
bool wal_append(Wal &w, std::string_view payload, WalRecord &out, OnCommitted &&on_flushed);

// Writes and syncs the buffer, then calls on_committed for each record just made durable, then empties the buffer.
// A write or sync failure aborts the process: an event that cannot be made durable must not be acted on.
template <typename OnCommitted>
void wal_commit(Wal &w, OnCommitted &&on_committed);

// Copies one payload (durable or still buffered) into dst.
bool wal_read(const Wal &w, uint64_t payload_offset, uint32_t length, char *dst);

void wal_close(Wal &w);

// ---------------------------------------------------------------- implementation details

namespace wal_detail {
bool open_file(Wal &w, std::string_view path, uint64_t &size);
bool read_header(const Wal &w, uint64_t offset, uint32_t &magic, uint32_t &length, uint32_t &crc, uint64_t &lsn);
bool read_payload(const Wal &w, uint64_t offset, uint32_t length, char *dst);
void truncate_tail(Wal &w, uint64_t good_end, uint64_t file_size);
void write_and_sync(Wal &w);
} // namespace wal_detail

template <typename OnRecord>
bool wal_open_and_recover(Wal &w, std::string_view path, Arena &scratch, OnRecord &&on_record) {
    uint64_t size = 0;
    if (!wal_detail::open_file(w, path, size)) return false;

    uint64_t offset = 0;
    uint64_t expected_lsn = 1;
    while (offset + kWalHeaderBytes <= size) {
        uint32_t magic = 0, length = 0, crc = 0;
        uint64_t lsn = 0;
        if (!wal_detail::read_header(w, offset, magic, length, crc, lsn)) break;
        if (magic != kWalMagic || length == 0 || length > kMaxEventBytes || lsn != expected_lsn) break;
        if (offset + kWalHeaderBytes + length > size) break;

        ArenaScope scope(scratch);
        char *payload = static_cast<char *>(scratch.alloc_bytes(length + 1, 1));
        if (!payload || !wal_detail::read_payload(w, offset + kWalHeaderBytes, length, payload)) break;
        if (crc32(payload, length) != crc) break;
        payload[length] = 0;

        on_record(WalRecord{lsn, offset + kWalHeaderBytes, std::string_view(payload, length)});
        offset += kWalHeaderBytes + length;
        ++expected_lsn;
    }

    if (offset != size) wal_detail::truncate_tail(w, offset, size);
    w.durable_end = offset;
    w.next_lsn = expected_lsn;
    w.durable_lsn = expected_lsn - 1;
    return true;
}

template <typename OnCommitted>
void wal_commit(Wal &w, OnCommitted &&on_committed) {
    if (w.used == 0) return;
    const uint64_t base = w.durable_end;
    wal_detail::write_and_sync(w);
    w.durable_lsn = w.next_lsn - 1;

    uint32_t at = 0;
    while (at < w.used) {
        uint32_t length = 0;
        uint64_t lsn = 0;
        std::memcpy(&length, w.buffer + at + 4, 4);
        std::memcpy(&lsn, w.buffer + at + 12, 8);
        on_committed(WalRecord{lsn, base + at + kWalHeaderBytes,
                               std::string_view(reinterpret_cast<const char *>(w.buffer + at + kWalHeaderBytes), length)});
        at += kWalHeaderBytes + length;
    }
    w.durable_end = base + w.used;
    w.used = 0;
}

template <typename OnCommitted>
bool wal_append(Wal &w, std::string_view payload, WalRecord &out, OnCommitted &&on_flushed) {
    if (payload.empty() || payload.size() > kMaxEventBytes) return false;
    const uint32_t length = uint32_t(payload.size());
    if (w.used + kWalHeaderBytes + length > w.capacity) wal_commit(w, on_flushed);

    const uint32_t crc = crc32(payload.data(), length);
    const uint64_t lsn = w.next_lsn++;
    uint8_t *h = w.buffer + w.used;
    std::memcpy(h, &kWalMagic, 4);
    std::memcpy(h + 4, &length, 4);
    std::memcpy(h + 8, &crc, 4);
    std::memcpy(h + 12, &lsn, 8);
    std::memcpy(h + kWalHeaderBytes, payload.data(), length);

    out = WalRecord{lsn, w.durable_end + w.used + kWalHeaderBytes,
                    std::string_view(reinterpret_cast<const char *>(h + kWalHeaderBytes), length)};
    w.used += kWalHeaderBytes + length;
    return true;
}

} // namespace romeo
