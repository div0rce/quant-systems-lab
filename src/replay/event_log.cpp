#include "qsl/replay/event_log.hpp"

#include "qsl/protocol/endian.hpp"

#include <algorithm>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace qsl::replay {
namespace {

// FNV-1a 32-bit over the record's header+payload bytes. Deterministic, host-independent.
std::uint32_t checksum(std::span<const std::byte> data) noexcept {
    std::uint32_t hash = 2166136261U;
    for (const std::byte b : data) {
        hash ^= std::to_integer<std::uint32_t>(b);
        hash *= 16777619U;
    }
    return hash;
}

// Ask the kernel to push a stdio stream's file to stable storage. On macOS, fsync only
// flushes to the drive, not through its volatile write cache; F_FULLFSYNC is the documented
// stronger barrier, with fsync as the fallback on filesystems that reject it.
bool sync_to_storage(std::FILE *file) noexcept {
    const int fd = fileno(file);
    if (fd < 0) {
        return false;
    }
#ifdef __APPLE__
    if (fcntl(fd, F_FULLFSYNC) == 0) {
        return true;
    }
#endif
    return ::fsync(fd) == 0;
}

// A new file's directory entry is only durable once the parent directory is synced; an
// fsync'd log whose name never reached the disk can vanish entirely after a power loss.
bool sync_parent_directory(const std::filesystem::path &path) noexcept {
    std::error_code ec;
    std::filesystem::path parent = std::filesystem::absolute(path, ec).parent_path();
    if (ec || parent.empty()) {
        return false;
    }
    const int fd = ::open(parent.string().c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

} // namespace

bool encode_record(const LogRecord &record, std::vector<std::byte> &out) {
    if (record.payload.size() > kMaxPayload) {
        return false;
    }
    const auto payload_size = static_cast<std::uint32_t>(record.payload.size());
    const std::size_t start = out.size();
    out.resize(start + kRecordHeaderSize + record.payload.size() + kChecksumSize);
    std::byte *p = out.data() + start;
    protocol::store_be<std::uint64_t>(p + 0, record.seq_no);
    protocol::store_be<std::uint16_t>(p + 8, static_cast<std::uint16_t>(record.type));
    protocol::store_be<std::uint64_t>(p + 10, record.logical_timestamp);
    protocol::store_be<std::uint32_t>(p + 18, payload_size);
    std::copy(record.payload.begin(), record.payload.end(), p + kRecordHeaderSize);
    const std::uint32_t cs =
        checksum(std::span<const std::byte>(p, kRecordHeaderSize + record.payload.size()));
    protocol::store_be<std::uint32_t>(p + kRecordHeaderSize + record.payload.size(), cs);
    return true;
}

RecordRead decode_record(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size()) {
        return {LogError::Truncated, {}, offset};
    }
    if (bytes.size() - offset < kRecordHeaderSize) {
        return {LogError::Truncated, {}, offset};
    }
    const std::byte *p = bytes.data() + offset;
    LogRecord record;
    record.seq_no = protocol::load_be<std::uint64_t>(p + 0);
    record.type = static_cast<RecordType>(protocol::load_be<std::uint16_t>(p + 8));
    record.logical_timestamp = protocol::load_be<std::uint64_t>(p + 10);
    const std::uint32_t payload_size = protocol::load_be<std::uint32_t>(p + 18);
    if (payload_size > kMaxPayload) {
        return {LogError::PayloadTooLarge, {}, offset};
    }
    const std::size_t total = kRecordHeaderSize + payload_size + kChecksumSize;
    if (bytes.size() - offset < total) {
        return {LogError::Truncated, {}, offset};
    }
    const std::byte *payload_begin = p + kRecordHeaderSize;
    record.payload.assign(payload_begin, payload_begin + payload_size);
    const std::uint32_t stored = protocol::load_be<std::uint32_t>(payload_begin + payload_size);
    const std::uint32_t computed =
        checksum(std::span<const std::byte>(p, kRecordHeaderSize + payload_size));
    if (stored != computed) {
        return {LogError::BadChecksum, {}, offset};
    }
    return {LogError::None, std::move(record), offset + total};
}

LogReadResult read_log(std::span<const std::byte> bytes) {
    LogReadResult result;
    result.error = LogError::None;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        RecordRead read = decode_record(bytes, offset);
        if (read.error != LogError::None) {
            result.error = read.error;
            break;
        }
        result.records.push_back(std::move(read.record));
        offset = read.next_offset;
    }
    return result;
}

namespace {

// A BadChecksum record still has complete framing, so its total size is known; the failure
// is "torn" only when that frame is the final thing in the buffer.
bool checksum_failure_confined_to_tail(std::span<const std::byte> bytes, std::size_t offset) {
    const std::byte *p = bytes.data() + offset;
    const std::uint32_t payload_size = protocol::load_be<std::uint32_t>(p + 18);
    const std::size_t total = kRecordHeaderSize + payload_size + kChecksumSize;
    return offset + total == bytes.size();
}

TailState classify_tail(std::span<const std::byte> bytes, LogError tail_error,
                        std::size_t valid_bytes) {
    switch (tail_error) {
    case LogError::None:
        return TailState::CleanTail;
    case LogError::Truncated:
        // decode_record reports Truncated only when the buffer ends mid-record, so the
        // damage is necessarily confined to the final (partial) append.
        return TailState::TornTail;
    case LogError::BadChecksum:
        return checksum_failure_confined_to_tail(bytes, valid_bytes) ? TailState::TornTail
                                                                     : TailState::Corrupt;
    case LogError::PayloadTooLarge:
    case LogError::OpenFailed:
        break;
    }
    // An invalid header is indistinguishable from arbitrary damage: the declared size cannot
    // be trusted, so the damage cannot be proven confined to one final append.
    return TailState::Corrupt;
}

} // namespace

LogRecovery recover_log(std::span<const std::byte> bytes) {
    LogRecovery recovery;
    recovery.error = LogError::None;
    recovery.tail_error = LogError::None;
    recovery.valid_bytes = 0;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        RecordRead read = decode_record(bytes, offset);
        if (read.error != LogError::None) {
            recovery.tail_error = read.error;
            break;
        }
        recovery.records.push_back(std::move(read.record));
        offset = read.next_offset;
        recovery.valid_bytes = offset;
    }
    recovery.tail = classify_tail(bytes, recovery.tail_error, recovery.valid_bytes);
    return recovery;
}

LogRecovery recover_log_file(const std::filesystem::path &path) {
    std::vector<std::byte> buf;
    FilePtr file(std::fopen(path.string().c_str(), "rb"));
    if (!file) {
        return {LogError::OpenFailed, TailState::Corrupt, LogError::None, {}, 0};
    }
    while (true) {
        std::byte chunk[4096];
        const std::size_t got = std::fread(chunk, 1, sizeof(chunk), file.get());
        buf.insert(buf.end(), chunk, chunk + got);
        if (got < sizeof(chunk)) {
            if (std::ferror(file.get()) != 0) {
                return {LogError::OpenFailed, TailState::Corrupt, LogError::None, {}, 0};
            }
            break;
        }
    }
    return recover_log(buf);
}

bool repair_log_file(const std::filesystem::path &path, const LogRecovery &recovery) {
    if (recovery.error != LogError::None || recovery.tail == TailState::Corrupt) {
        return false;
    }
    if (recovery.tail == TailState::CleanTail) {
        return true;
    }
    std::error_code ec;
    std::filesystem::resize_file(path, recovery.valid_bytes, ec);
    if (ec) {
        return false;
    }
    // The truncation itself must be durable, or a power loss after repair could resurrect
    // the torn tail the caller believes is gone.
    FilePtr file(std::fopen(path.string().c_str(), "ab"));
    return file && sync_to_storage(file.get());
}

void FileCloser::operator()(std::FILE *file) const noexcept {
    if (file != nullptr) {
        std::fclose(file);
    }
}

EventLogWriter::EventLogWriter(const std::filesystem::path &path, DurabilityMode mode)
    : file_(std::fopen(path.string().c_str(), "ab")), mode_(mode) {
    // Durability of a newly created log includes its directory entry, not just its bytes.
    if (file_ && mode_ == DurabilityMode::FsyncOnAppend && !sync_parent_directory(path)) {
        file_.reset();
    }
}

bool EventLogWriter::append(const LogRecord &record) {
    if (!file_) {
        return false;
    }
    std::vector<std::byte> buf;
    if (!encode_record(record, buf)) {
        return false;
    }
    const std::size_t written = std::fwrite(buf.data(), 1, buf.size(), file_.get());
    if (written != buf.size()) {
        return false;
    }
    if (mode_ == DurabilityMode::BufferedOnly) {
        return true;
    }
    if (std::fflush(file_.get()) != 0) {
        return false;
    }
    if (mode_ == DurabilityMode::FlushOnAppend) {
        return true;
    }
    return sync_to_storage(file_.get());
}

bool EventLogWriter::sync() {
    if (!file_) {
        return false;
    }
    return std::fflush(file_.get()) == 0 && sync_to_storage(file_.get());
}

EventLogReader::EventLogReader(std::filesystem::path path) : path_(std::move(path)) {}

LogReadResult EventLogReader::read_all() const {
    std::vector<std::byte> buf;
    FilePtr file(std::fopen(path_.string().c_str(), "rb"));
    if (!file) {
        return {{}, LogError::OpenFailed};
    }

    while (true) {
        std::byte chunk[4096];
        const std::size_t got = std::fread(chunk, 1, sizeof(chunk), file.get());
        buf.insert(buf.end(), chunk, chunk + got);
        if (got < sizeof(chunk)) {
            if (std::ferror(file.get()) != 0) {
                return {{}, LogError::OpenFailed};
            }
            break;
        }
    }
    return read_log(buf);
}

} // namespace qsl::replay
