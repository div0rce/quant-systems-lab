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
    CommandRecord = 1,
    EventRecord = 2,
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
    OpenFailed,      // file path could not be opened or read
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

/// Append one record's framed bytes to `out`. Returns false without modifying `out` when
/// the payload exceeds the readable cap.
[[nodiscard]] bool encode_record(const LogRecord &record, std::vector<std::byte> &out);

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

// How the writer orders an append against the host's volatile buffers. None of these modes
// makes the simulator production durable; they exist to make the durability tradeoff explicit
// and testable (see docs/persistence.md).
enum class DurabilityMode : std::uint8_t {
    BufferedOnly,  // fwrite into the stdio buffer; data may sit in user space after append
    FlushOnAppend, // fwrite + fflush; data reaches the kernel page cache (pre-M45 behavior)
    FsyncOnAppend, // fwrite + fflush + fsync; the kernel is asked to reach stable storage
};

// Crash-recovery classification of a log's tail after scanning the longest valid prefix.
enum class TailState : std::uint8_t {
    CleanTail, // the log ends exactly on a record boundary
    TornTail,  // the damage is confined to the final record: consistent with a crash mid-append
    Corrupt,   // the damage is not confined to the final record: not safely repairable by
               // tail truncation, because valid acknowledged records may follow it
};

struct LogRecovery {
    LogError error;                 // OpenFailed when the file could not be read; otherwise None
    TailState tail;                 // meaningful only when error == None
    LogError tail_error;            // the decode error that ended the scan; None for CleanTail
    std::vector<LogRecord> records; // longest valid record prefix
    std::size_t valid_bytes;        // byte offset of the last valid record boundary
};

/// Scan `bytes` like `read_log`, then classify the tail. `Truncated` can only occur at the
/// end of the buffer, so it is always a torn tail. `BadChecksum` is a torn tail only when
/// the failing record's frame ends exactly at the end of the buffer (an interrupted final
/// append); a checksum failure followed by more bytes, or an invalid header
/// (`PayloadTooLarge`), is classified `Corrupt`.
[[nodiscard]] LogRecovery recover_log(std::span<const std::byte> bytes);

/// Read the whole file and run `recover_log`. A missing/unreadable file reports
/// `LogError::OpenFailed`; an existing empty file is a clean empty log.
[[nodiscard]] LogRecovery recover_log_file(const std::filesystem::path &path);

/// Repair a torn tail by truncating the file to `recovery.valid_bytes`. Returns true when
/// the file is clean afterwards (CleanTail is a no-op). Refuses `Corrupt` logs and leaves
/// them untouched: truncating mid-file damage would silently discard records after it, and
/// that decision belongs to a human, not automation.
[[nodiscard]] bool repair_log_file(const std::filesystem::path &path, const LogRecovery &recovery);

// RAII closer so the writer/reader can hold a C FILE* without leaking.
struct FileCloser {
    void operator()(std::FILE *file) const noexcept;
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

/// Append-only log file writer. Opens in binary append mode and only ever appends;
/// it never seeks or rewrites existing records. The durability mode controls how far an
/// acknowledged append has traveled toward stable storage; `FlushOnAppend` preserves the
/// pre-M45 behavior.
class EventLogWriter {
  public:
    explicit EventLogWriter(const std::filesystem::path &path,
                            DurabilityMode mode = DurabilityMode::FlushOnAppend);
    [[nodiscard]] bool good() const noexcept { return file_ != nullptr; }
    bool append(const LogRecord &record);
    /// Explicit group-commit point: flush stdio and fsync regardless of mode. Lets a caller
    /// batch appends in a weaker mode and pay the fsync cost once per batch.
    bool sync();

  private:
    FilePtr file_;
    DurabilityMode mode_;
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
