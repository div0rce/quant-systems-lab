#pragma once

#include "qsl/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace qsl::replay {

using core::SeqNo;
using core::Timestamp;

// What a log record carries. Both accepted commands and emitted engine events are
// recordable; the payload is the message's serialized bytes (opaque to the log).
enum class RecordType : std::uint16_t {
    Command = 1,
    Event = 2,
};

struct LogRecord {
    SeqNo seq_no;
    RecordType type;
    Timestamp logical_timestamp;
    std::vector<std::byte> payload;
    bool operator==(const LogRecord &) const = default;
};

// Deterministic outcomes when reading a (possibly corrupt) log.
enum class LogError : std::uint8_t {
    None = 0,
    Truncated,       // buffer ends before a full record (header, payload, or checksum)
    BadChecksum,     // record checksum does not match its bytes
    PayloadTooLarge, // declared payload size exceeds kMaxPayload
};

// Framing: header (big-endian) + payload + checksum.
//   seq_no u64 | record_type u16 | logical_timestamp u64 | payload_size u32 | payload | checksum
//   u32
inline constexpr std::size_t kRecordHeaderSize = 22;   // 8 + 2 + 8 + 4
inline constexpr std::size_t kChecksumSize = 4;        // u32
inline constexpr std::uint32_t kMaxPayload = 1U << 20; // 1 MiB guard

/// Append one record's framed bytes to `out`.
void encode_record(const LogRecord &record, std::vector<std::byte> &out);

struct RecordRead {
    LogError error;
    LogRecord record;
    std::size_t next_offset;
};

/// Decode the record starting at `offset`. Never reads out of bounds; returns a
/// deterministic error for truncated/corrupt input.
[[nodiscard]] RecordRead decode_record(std::span<const std::byte> bytes, std::size_t offset);

struct LogReadResult {
    std::vector<LogRecord> records;
    LogError error; // None when the buffer ends cleanly on a record boundary
    bool operator==(const LogReadResult &) const = default;
};

/// Decode all records in `bytes`. Stops at the first corrupt/truncated record and reports
/// the error alongside the records read so far.
[[nodiscard]] LogReadResult read_log(std::span<const std::byte> bytes);

// RAII closer so the writer/reader can hold a C FILE* without leaking.
struct FileCloser {
    void operator()(std::FILE *file) const noexcept;
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

/// Append-only log file writer. Opens in binary append mode and only ever appends;
/// it never seeks or rewrites existing records.
class EventLogWriter {
  public:
    explicit EventLogWriter(const std::filesystem::path &path);
    [[nodiscard]] bool good() const noexcept { return file_ != nullptr; }
    bool append(const LogRecord &record);

  private:
    FilePtr file_;
};

/// Reads an entire log file and decodes it.
class EventLogReader {
  public:
    explicit EventLogReader(std::filesystem::path path);
    [[nodiscard]] LogReadResult read_all() const;

  private:
    std::filesystem::path path_;
};

} // namespace qsl::replay
