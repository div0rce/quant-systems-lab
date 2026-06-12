#include "qsl/protocol/codec.hpp"
#include "qsl/protocol/endian.hpp"
#include "qsl/replay/event_log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <span>
#include <vector>

using namespace qsl::replay;

static_assert(static_cast<std::uint16_t>(RecordType::CommandRecord) == 1);
static_assert(static_cast<std::uint16_t>(RecordType::EventRecord) == 2);

namespace {
std::vector<std::byte> bytes_of(std::initializer_list<std::uint8_t> values) {
    std::vector<std::byte> out;
    for (const std::uint8_t v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

const LogRecord kR1{1, RecordType::CommandRecord, 10, bytes_of({1, 2, 3})};
const LogRecord kR2{2, RecordType::EventRecord, 11, bytes_of({4, 5})};

std::vector<LogRecord> records_of(std::initializer_list<LogRecord> records) {
    return {records};
}

void expect_record_read(const RecordRead &read, const LogRecord &record, std::size_t next_offset) {
    REQUIRE(read.error == LogError::None);
    REQUIRE(read.record == record);
    REQUIRE(read.next_offset == next_offset);
}

std::size_t encoded_size(const LogRecord &record) {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(record, buf));
    return buf.size();
}

void expect_log_read(const LogReadResult &result, LogError error,
                     std::initializer_list<LogRecord> records) {
    REQUIRE(result.error == error);
    REQUIRE(result.records == records_of(records));
}

void expect_file_records(const std::filesystem::path &path,
                         std::initializer_list<LogRecord> records) {
    expect_log_read(EventLogReader(path).read_all(), LogError::None, records);
}

struct RecoverySummary {
    LogError error;
    TailState tail;
    LogError tail_error;
    std::size_t record_count;
    std::size_t valid_bytes;

    bool operator==(const RecoverySummary &) const = default;
};

RecoverySummary summarize(const LogRecovery &recovery) {
    return {recovery.error, recovery.tail, recovery.tail_error, recovery.records.size(),
            recovery.valid_bytes};
}

void expect_recovery(const LogRecovery &recovery, const RecoverySummary &summary,
                     std::initializer_list<LogRecord> records) {
    REQUIRE(summarize(recovery) == summary);
    REQUIRE(recovery.records == records_of(records));
}

void write_raw_file(const std::filesystem::path &path, std::span<const std::byte> bytes) {
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}
} // namespace

TEST_CASE("a record round-trips through the byte codec", "[log]") {
    const LogRecord rec{42, RecordType::CommandRecord, 7, bytes_of({0xDE, 0xAD, 0xBE, 0xEF})};
    std::vector<std::byte> buf;
    REQUIRE(encode_record(rec, buf));

    const auto rr = decode_record(buf, 0);
    expect_record_read(rr, rec, buf.size());
}

TEST_CASE("read_log reconstructs multiple appended records", "[log]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    REQUIRE(encode_record(kR2, buf));

    expect_log_read(read_log(buf), LogError::None, {kR1, kR2});
}

TEST_CASE("records round-trip through an append-only file", "[log]") {
    const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_rt.bin";
    std::filesystem::remove(path);
    {
        EventLogWriter writer(path);
        REQUIRE(writer.good());
        REQUIRE(writer.append(kR1));
        REQUIRE(writer.append(kR2));
    }
    expect_file_records(path, {kR1, kR2});
    std::filesystem::remove(path);
}

TEST_CASE("missing log paths report open failure", "[log]") {
    const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_missing.bin";
    std::filesystem::remove(path);

    expect_log_read(EventLogReader(path).read_all(), LogError::OpenFailed, {});
}

TEST_CASE("an existing empty log reads cleanly as zero records", "[log]") {
    const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_empty.bin";
    std::filesystem::remove(path);
    {
        EventLogWriter writer(path);
        REQUIRE(writer.good());
    }

    expect_file_records(path, {});
    std::filesystem::remove(path);
}

TEST_CASE("a truncated log fails safely, keeping intact records", "[log]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    REQUIRE(encode_record(kR2, buf));
    buf.resize(buf.size() - 1); // chop the last byte of the second record

    expect_log_read(read_log(buf), LogError::Truncated, {kR1});
}

TEST_CASE("a corrupted payload fails the checksum", "[log]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    const auto orig = std::to_integer<std::uint8_t>(buf[kRecordHeaderSize]);
    buf[kRecordHeaderSize] = static_cast<std::byte>(static_cast<std::uint8_t>(orig ^ 0xFF));

    expect_log_read(read_log(buf), LogError::BadChecksum, {});
}

TEST_CASE("appending is append-only: prior records are unchanged", "[log]") {
    const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_append.bin";
    std::filesystem::remove(path);
    {
        EventLogWriter writer(path);
        REQUIRE(writer.append(kR1));
    }
    {
        EventLogWriter writer(path); // reopen -> append, must not rewrite kR1
        REQUIRE(writer.append(kR2));
    }
    expect_file_records(path, {kR1, kR2});
    std::filesystem::remove(path);
}

TEST_CASE("writer rejects oversized payload without appending", "[log]") {
    const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_oversized.bin";
    std::filesystem::remove(path);
    {
        EventLogWriter writer(path);
        REQUIRE(writer.good());
        REQUIRE(writer.append(kR1));

        LogRecord oversized{2, RecordType::EventRecord, 11,
                            std::vector<std::byte>(kMaxPayload + 1)};
        REQUIRE_FALSE(writer.append(oversized));
    }
    expect_file_records(path, {kR1});
    std::filesystem::remove(path);
}

TEST_CASE("writer reports append failure after opening an unwritable sink", "[log]") {
    const std::filesystem::path path{"/dev/full"};
    if (!std::filesystem::exists(path)) {
        SUCCEED("/dev/full is not available on this platform");
        return;
    }

    EventLogWriter writer(path);
    REQUIRE(writer.good());
    REQUIRE_FALSE(writer.append(kR1));
}

TEST_CASE("encode_record rejects oversized payload without modifying output", "[log]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    const std::vector<std::byte> before = buf;

    LogRecord oversized{2, RecordType::EventRecord, 11, std::vector<std::byte>(kMaxPayload + 1)};
    REQUIRE_FALSE(encode_record(oversized, buf));
    REQUIRE(buf == before);
}

TEST_CASE("declared oversized payload decodes as PayloadTooLarge", "[log]") {
    std::vector<std::byte> buf(kRecordHeaderSize);
    qsl::protocol::store_be<std::uint64_t>(buf.data() + 0, 1);
    qsl::protocol::store_be<std::uint16_t>(buf.data() + 8,
                                           static_cast<std::uint16_t>(RecordType::CommandRecord));
    qsl::protocol::store_be<std::uint64_t>(buf.data() + 10, 10);
    qsl::protocol::store_be<std::uint32_t>(buf.data() + 18, kMaxPayload + 1);

    expect_log_read(read_log(buf), LogError::PayloadTooLarge, {});
}

TEST_CASE("header corruption fails the checksum", "[log]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    const auto orig = std::to_integer<std::uint8_t>(buf[0]);
    buf[0] = static_cast<std::byte>(static_cast<std::uint8_t>(orig ^ 0x01));

    expect_log_read(read_log(buf), LogError::BadChecksum, {});
}

TEST_CASE("decode_record rejects offsets beyond the buffer", "[log]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));

    const auto result = decode_record(buf, buf.size() + 1);
    REQUIRE(result.error == LogError::Truncated);
    REQUIRE(result.next_offset == buf.size() + 1);
}

TEST_CASE("every durability mode produces an identical readable log", "[log]") {
    for (const auto mode : {DurabilityMode::BufferedOnly, DurabilityMode::FlushOnAppend,
                            DurabilityMode::FsyncOnAppend}) {
        const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_durability.bin";
        std::filesystem::remove(path);
        {
            EventLogWriter writer(path, mode);
            REQUIRE(writer.good());
            REQUIRE(writer.append(kR1));
            REQUIRE(writer.append(kR2));
            REQUIRE(writer.sync()); // explicit group-commit point works in every mode
        }
        expect_file_records(path, {kR1, kR2});
        std::filesystem::remove(path);
    }
}

TEST_CASE("recover_log classifies a boundary-aligned log as clean", "[log][recovery]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    REQUIRE(encode_record(kR2, buf));

    expect_recovery(recover_log(buf),
                    {LogError::None, TailState::CleanTail, LogError::None, 2, buf.size()},
                    {kR1, kR2});
}

TEST_CASE("recovery of a log cut at every byte yields the exact valid prefix", "[log][recovery]") {
    std::vector<std::byte> buf;
    std::vector<std::size_t> boundaries{0};
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        const LogRecord rec{seq, RecordType::CommandRecord, seq * 10,
                            bytes_of({static_cast<std::uint8_t>(seq), 0xAB})};
        REQUIRE(encode_record(rec, buf));
        boundaries.push_back(buf.size());
    }

    for (std::size_t cut = 0; cut <= buf.size(); ++cut) {
        const std::span<const std::byte> torn(buf.data(), cut);
        const auto recovery = recover_log(torn);

        std::size_t last_boundary = 0;
        std::size_t whole_records = 0;
        for (std::size_t i = 0; i < boundaries.size(); ++i) {
            if (boundaries[i] <= cut) {
                last_boundary = boundaries[i];
                whole_records = i; // boundaries[0] == 0 -> zero whole records
            }
        }
        if (cut == last_boundary) {
            REQUIRE(summarize(recovery) == RecoverySummary{LogError::None, TailState::CleanTail,
                                                           LogError::None, whole_records,
                                                           last_boundary});
        } else if (cut - last_boundary < kRecordHeaderSize) {
            REQUIRE(summarize(recovery) == RecoverySummary{LogError::None, TailState::TornTail,
                                                           LogError::Truncated, whole_records,
                                                           last_boundary});
        } else {
            REQUIRE(summarize(recovery) == RecoverySummary{LogError::None, TailState::Corrupt,
                                                           LogError::Truncated, whole_records,
                                                           last_boundary});
        }
    }
}

TEST_CASE("a checksum failure confined to the final record is a torn tail", "[log][recovery]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    const std::size_t second_start = buf.size();
    REQUIRE(encode_record(kR2, buf));
    const auto orig = std::to_integer<std::uint8_t>(buf[second_start + kRecordHeaderSize]);
    buf[second_start + kRecordHeaderSize] =
        static_cast<std::byte>(static_cast<std::uint8_t>(orig ^ 0xFF));

    expect_recovery(recover_log(buf),
                    {LogError::None, TailState::TornTail, LogError::BadChecksum, 1, second_start},
                    {kR1});
}

TEST_CASE("a checksum failure followed by more records is corruption", "[log][recovery]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    const std::size_t second_start = buf.size();
    REQUIRE(encode_record(kR2, buf));
    REQUIRE(encode_record(kR1, buf)); // a valid record after the damaged one
    const auto orig = std::to_integer<std::uint8_t>(buf[second_start + kRecordHeaderSize]);
    buf[second_start + kRecordHeaderSize] =
        static_cast<std::byte>(static_cast<std::uint8_t>(orig ^ 0xFF));

    expect_recovery(recover_log(buf),
                    {LogError::None, TailState::Corrupt, LogError::BadChecksum, 1, second_start},
                    {kR1});
}

TEST_CASE("an untrustworthy header is corruption, not a torn tail", "[log][recovery]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    const std::size_t bad_start = buf.size();
    buf.resize(bad_start + kRecordHeaderSize);
    qsl::protocol::store_be<std::uint64_t>(buf.data() + bad_start + 0, 2);
    qsl::protocol::store_be<std::uint16_t>(buf.data() + bad_start + 8,
                                           static_cast<std::uint16_t>(RecordType::EventRecord));
    qsl::protocol::store_be<std::uint64_t>(buf.data() + bad_start + 10, 11);
    qsl::protocol::store_be<std::uint32_t>(buf.data() + bad_start + 18, kMaxPayload + 1);

    expect_recovery(recover_log(buf),
                    {LogError::None, TailState::Corrupt, LogError::PayloadTooLarge, 1, bad_start},
                    {kR1});
}

TEST_CASE("an in-range truncated header span is corruption, not a repairable tail",
          "[log][recovery]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    const std::size_t bad_start = buf.size();
    REQUIRE(encode_record(kR2, buf));
    REQUIRE(encode_record(kR1, buf)); // bytes after the damaged header must not be discarded
    qsl::protocol::store_be<std::uint32_t>(buf.data() + bad_start + 18, kMaxPayload);

    expect_recovery(recover_log(buf),
                    {LogError::None, TailState::Corrupt, LogError::Truncated, 1, bad_start}, {kR1});
}

TEST_CASE("repair truncates a torn tail and the log accepts appends again", "[log][recovery]") {
    const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_repair.bin";
    const std::size_t first_record_size = encoded_size(kR1);
    std::filesystem::remove(path);
    {
        EventLogWriter writer(path, DurabilityMode::FsyncOnAppend);
        REQUIRE(writer.append(kR1));
        REQUIRE(writer.append(kR2));
    }
    // Tear the final append before a complete header exists; this is the repairable case.
    const auto torn_size = first_record_size + kRecordHeaderSize - 1;
    std::filesystem::resize_file(path, torn_size);

    const auto recovery = recover_log_file(path);
    expect_recovery(
        recovery, {LogError::None, TailState::TornTail, LogError::Truncated, 1, first_record_size},
        {kR1});
    REQUIRE(repair_log_file(path, recovery));

    const auto repaired = recover_log_file(path);
    expect_recovery(repaired,
                    {LogError::None, TailState::CleanTail, LogError::None, 1, recovery.valid_bytes},
                    {kR1});

    {
        EventLogWriter writer(path, DurabilityMode::FsyncOnAppend);
        REQUIRE(writer.append(kR2));
    }
    expect_file_records(path, {kR1, kR2});
    std::filesystem::remove(path);
}

TEST_CASE("repair refuses a corrupt log and leaves it untouched", "[log][recovery]") {
    const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_corrupt.bin";
    std::filesystem::remove(path);
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    const std::size_t second_start = buf.size();
    REQUIRE(encode_record(kR2, buf));
    REQUIRE(encode_record(kR1, buf));
    const auto orig = std::to_integer<std::uint8_t>(buf[second_start + kRecordHeaderSize]);
    buf[second_start + kRecordHeaderSize] =
        static_cast<std::byte>(static_cast<std::uint8_t>(orig ^ 0xFF));
    write_raw_file(path, buf);

    const auto recovery = recover_log_file(path);
    expect_recovery(recovery,
                    {LogError::None, TailState::Corrupt, LogError::BadChecksum, 1, second_start},
                    {kR1});
    REQUIRE_FALSE(repair_log_file(path, recovery));
    REQUIRE(std::filesystem::file_size(path) == buf.size());
    std::filesystem::remove(path);
}

TEST_CASE("repair of a clean log is a no-op", "[log][recovery]") {
    const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_cleanrepair.bin";
    std::filesystem::remove(path);
    {
        EventLogWriter writer(path);
        REQUIRE(writer.append(kR1));
    }
    const auto before = std::filesystem::file_size(path);
    const auto recovery = recover_log_file(path);
    expect_recovery(recovery, {LogError::None, TailState::CleanTail, LogError::None, 1, before},
                    {kR1});
    REQUIRE(repair_log_file(path, recovery));
    REQUIRE(std::filesystem::file_size(path) == before);
    std::filesystem::remove(path);
}

TEST_CASE("recovery of a missing log reports open failure", "[log][recovery]") {
    const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_norecover.bin";
    std::filesystem::remove(path);

    const auto recovery = recover_log_file(path);
    expect_recovery(recovery, {LogError::OpenFailed, TailState::Corrupt, LogError::None, 0, 0}, {});
    REQUIRE_FALSE(repair_log_file(path, recovery));
}

TEST_CASE("a serialized command payload survives the log", "[log]") {
    const qsl::protocol::NewOrder order{/*order_id=*/1,
                                        /*symbol=*/2,
                                        /*price=*/12345,
                                        /*quantity=*/10,
                                        qsl::protocol::Side::Buy,
                                        qsl::protocol::OrderType::Limit,
                                        qsl::protocol::TimeInForce::GTC};
    const std::vector<std::byte> frame = qsl::protocol::encode(order, /*seq=*/5);
    const LogRecord rec{5, RecordType::CommandRecord, 99, frame};

    std::vector<std::byte> buf;
    REQUIRE(encode_record(rec, buf));
    const auto result = read_log(buf);
    REQUIRE(result.records.size() == 1);
    REQUIRE(result.records[0].payload == frame);

    const auto decoded = qsl::protocol::decode_new_order(result.records[0].payload);
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value.order_id == 1);
    REQUIRE(decoded.value.price == 12345);
}
