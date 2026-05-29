#include "qsl/replay/event_log.hpp"

#include "qsl/protocol/endian.hpp"

#include <algorithm>
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

void FileCloser::operator()(std::FILE *file) const noexcept {
    if (file != nullptr) {
        std::fclose(file);
    }
}

EventLogWriter::EventLogWriter(const std::filesystem::path &path)
    : file_(std::fopen(path.string().c_str(), "ab")) {}

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
    return std::fflush(file_.get()) == 0;
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
