#include "wal.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.hpp"

namespace romeo {

void wal_init(Wal &w, uint8_t *buffer, uint32_t capacity) {
    w.buffer = buffer;
    w.capacity = capacity;
    w.used = 0;
}

void wal_close(Wal &w) {
    if (w.fd >= 0) close(w.fd);
    w.fd = -1;
    w.used = 0;
    w.durable_end = 0;
    w.next_lsn = 1;
    w.durable_lsn = 0;
}

bool wal_read(const Wal &w, uint64_t payload_offset, uint32_t length, char *dst) {
    if (payload_offset >= w.durable_end) {
        uint64_t at = payload_offset - w.durable_end;
        if (at + length > w.used) return false;
        std::memcpy(dst, w.buffer + at, length);
        return true;
    }
    return wal_detail::read_payload(w, payload_offset, length, dst);
}

namespace wal_detail {

bool open_file(Wal &w, std::string_view path, uint64_t &size) {
    w.path.assign(path);
    w.fd = open(w.path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (w.fd < 0) {
        RLOG_ERROR("wal: cannot open {}: {}", w.path.view(), std::string_view(std::strerror(errno)));
        return false;
    }
    struct stat st {};
    if (fstat(w.fd, &st) != 0) return false;
    size = uint64_t(st.st_size);
    return true;
}

bool read_payload(const Wal &w, uint64_t offset, uint32_t length, char *dst) {
    uint32_t got = 0;
    while (got < length) {
        ssize_t n = pread(w.fd, dst + got, length - got, off_t(offset + got));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        got += uint32_t(n);
    }
    return true;
}

bool read_header(const Wal &w, uint64_t offset, uint32_t &magic, uint32_t &length, uint32_t &crc, uint64_t &lsn) {
    char h[kWalHeaderBytes];
    if (!read_payload(w, offset, kWalHeaderBytes, h)) return false;
    std::memcpy(&magic, h, 4);
    std::memcpy(&length, h + 4, 4);
    std::memcpy(&crc, h + 8, 4);
    std::memcpy(&lsn, h + 12, 8);
    return true;
}

static void sync_fd(int fd) {
#if defined(__APPLE__)
    int rc = fsync(fd);
#else
    int rc = fdatasync(fd);
#endif
    if (rc != 0) {
        RLOG_ERROR("wal: sync failed: {}", std::string_view(std::strerror(errno)));
        log_flush();
        std::abort();
    }
}

void truncate_tail(Wal &w, uint64_t good_end, uint64_t file_size) {
    RLOG_WARN("wal: {} has a torn or corrupt tail; truncating from {} to {} bytes ({} bytes dropped)", w.path.view(), file_size,
              good_end, file_size - good_end);
    if (ftruncate(w.fd, off_t(good_end)) != 0) {
        RLOG_ERROR("wal: truncate failed: {}", std::string_view(std::strerror(errno)));
        log_flush();
        std::abort();
    }
    sync_fd(w.fd);
}

void write_and_sync(Wal &w) {
    uint32_t done = 0;
    while (done < w.used) {
        ssize_t n = pwrite(w.fd, w.buffer + done, w.used - done, off_t(w.durable_end + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            RLOG_ERROR("wal: write failed: {}", std::string_view(std::strerror(errno)));
            log_flush();
            std::abort();
        }
        done += uint32_t(n);
    }
    sync_fd(w.fd);
}

} // namespace wal_detail
} // namespace romeo
