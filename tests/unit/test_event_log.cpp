#include "qsl/protocol/codec.hpp"
#include "qsl/protocol/endian.hpp"
#include "qsl/replay/event_log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
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
} // namespace

TEST_CASE("a record round-trips through the byte codec", "[log]") {
    const LogRecord rec{42, RecordType::CommandRecord, 7, bytes_of({0xDE, 0xAD, 0xBE, 0xEF})};
    std::vector<std::byte> buf;
    REQUIRE(encode_record(rec, buf));

    const auto rr = decode_record(buf, 0);
    REQUIRE(rr.error == LogError::None);
    REQUIRE(rr.record == rec);
    REQUIRE(rr.next_offset == buf.size());
}

TEST_CASE("read_log reconstructs multiple appended records", "[log]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    REQUIRE(encode_record(kR2, buf));

    const auto result = read_log(buf);
    REQUIRE(result.error == LogError::None);
    REQUIRE(result.records.size() == 2);
    REQUIRE(result.records[0] == kR1);
    REQUIRE(result.records[1] == kR2);
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
    const EventLogReader reader(path);
    const auto result = reader.read_all();
    REQUIRE(result.error == LogError::None);
    REQUIRE(result.records.size() == 2);
    REQUIRE(result.records[0] == kR1);
    REQUIRE(result.records[1] == kR2);
    std::filesystem::remove(path);
}

TEST_CASE("missing log paths report open failure", "[log]") {
    const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_missing.bin";
    std::filesystem::remove(path);

    const auto result = EventLogReader(path).read_all();
    REQUIRE(result.error == LogError::OpenFailed);
    REQUIRE(result.records.empty());
}

TEST_CASE("an existing empty log reads cleanly as zero records", "[log]") {
    const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_empty.bin";
    std::filesystem::remove(path);
    {
        EventLogWriter writer(path);
        REQUIRE(writer.good());
    }

    const auto result = EventLogReader(path).read_all();
    REQUIRE(result.error == LogError::None);
    REQUIRE(result.records.empty());
    std::filesystem::remove(path);
}

TEST_CASE("a truncated log fails safely, keeping intact records", "[log]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    REQUIRE(encode_record(kR2, buf));
    buf.resize(buf.size() - 1); // chop the last byte of the second record

    const auto result = read_log(buf);
    REQUIRE(result.error == LogError::Truncated);
    REQUIRE(result.records.size() == 1);
    REQUIRE(result.records[0] == kR1);
}

TEST_CASE("a corrupted payload fails the checksum", "[log]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    const auto orig = std::to_integer<std::uint8_t>(buf[kRecordHeaderSize]);
    buf[kRecordHeaderSize] = static_cast<std::byte>(static_cast<std::uint8_t>(orig ^ 0xFF));

    const auto result = read_log(buf);
    REQUIRE(result.error == LogError::BadChecksum);
    REQUIRE(result.records.empty());
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
    const auto result = EventLogReader(path).read_all();
    REQUIRE(result.records.size() == 2);
    REQUIRE(result.records[0] == kR1);
    REQUIRE(result.records[1] == kR2);
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
    const auto result = EventLogReader(path).read_all();
    REQUIRE(result.error == LogError::None);
    REQUIRE(result.records.size() == 1);
    REQUIRE(result.records[0] == kR1);
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

    const auto result = read_log(buf);
    REQUIRE(result.error == LogError::PayloadTooLarge);
    REQUIRE(result.records.empty());
}

TEST_CASE("header corruption fails the checksum", "[log]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    const auto orig = std::to_integer<std::uint8_t>(buf[0]);
    buf[0] = static_cast<std::byte>(static_cast<std::uint8_t>(orig ^ 0x01));

    const auto result = read_log(buf);
    REQUIRE(result.error == LogError::BadChecksum);
    REQUIRE(result.records.empty());
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
        const auto result = EventLogReader(path).read_all();
        REQUIRE(result.error == LogError::None);
        REQUIRE(result.records.size() == 2);
        REQUIRE(result.records[0] == kR1);
        REQUIRE(result.records[1] == kR2);
        std::filesystem::remove(path);
    }
}

TEST_CASE("recover_log classifies a boundary-aligned log as clean", "[log][recovery]") {
    std::vector<std::byte> buf;
    REQUIRE(encode_record(kR1, buf));
    REQUIRE(encode_record(kR2, buf));

    const auto recovery = recover_log(buf);
    REQUIRE(recovery.error == LogError::None);
    REQUIRE(recovery.tail == TailState::CleanTail);
    REQUIRE(recovery.tail_error == LogError::None);
    REQUIRE(recovery.records.size() == 2);
    REQUIRE(recovery.valid_bytes == buf.size());
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
        REQUIRE(recovery.records.size() == whole_records);
        REQUIRE(recovery.valid_bytes == last_boundary);
        if (cut == last_boundary) {
            REQUIRE(recovery.tail == TailState::CleanTail);
        } else {
            REQUIRE(recovery.tail == TailState::TornTail);
            REQUIRE(recovery.tail_error == LogError::Truncated);
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

    const auto recovery = recover_log(buf);
    REQUIRE(recovery.tail == TailState::TornTail);
    REQUIRE(recovery.tail_error == LogError::BadChecksum);
    REQUIRE(recovery.records.size() == 1);
    REQUIRE(recovery.records[0] == kR1);
    REQUIRE(recovery.valid_bytes == second_start);
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

    const auto recovery = recover_log(buf);
    REQUIRE(recovery.tail == TailState::Corrupt);
    REQUIRE(recovery.tail_error == LogError::BadChecksum);
    REQUIRE(recovery.records.size() == 1);
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

    const auto recovery = recover_log(buf);
    REQUIRE(recovery.tail == TailState::Corrupt);
    REQUIRE(recovery.tail_error == LogError::PayloadTooLarge);
    REQUIRE(recovery.records.size() == 1);
    REQUIRE(recovery.valid_bytes == bad_start);
}

TEST_CASE("repair truncates a torn tail and the log accepts appends again", "[log][recovery]") {
    const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_repair.bin";
    std::filesystem::remove(path);
    {
        EventLogWriter writer(path, DurabilityMode::FsyncOnAppend);
        REQUIRE(writer.append(kR1));
        REQUIRE(writer.append(kR2));
    }
    // Tear the final append: chop bytes so kR2's record is incomplete.
    const auto torn_size = std::filesystem::file_size(path) - 3;
    std::filesystem::resize_file(path, torn_size);

    const auto recovery = recover_log_file(path);
    REQUIRE(recovery.tail == TailState::TornTail);
    REQUIRE(recovery.records.size() == 1);
    REQUIRE(repair_log_file(path, recovery));

    const auto repaired = recover_log_file(path);
    REQUIRE(repaired.tail == TailState::CleanTail);
    REQUIRE(repaired.records.size() == 1);
    REQUIRE(repaired.records[0] == kR1);

    {
        EventLogWriter writer(path, DurabilityMode::FsyncOnAppend);
        REQUIRE(writer.append(kR2));
    }
    const auto result = EventLogReader(path).read_all();
    REQUIRE(result.error == LogError::None);
    REQUIRE(result.records.size() == 2);
    REQUIRE(result.records[1] == kR2);
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
    {
        std::FILE *file = std::fopen(path.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        REQUIRE(std::fwrite(buf.data(), 1, buf.size(), file) == buf.size());
        REQUIRE(std::fclose(file) == 0);
    }

    const auto recovery = recover_log_file(path);
    REQUIRE(recovery.tail == TailState::Corrupt);
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
    REQUIRE(recovery.tail == TailState::CleanTail);
    REQUIRE(repair_log_file(path, recovery));
    REQUIRE(std::filesystem::file_size(path) == before);
    std::filesystem::remove(path);
}

TEST_CASE("recovery of a missing log reports open failure", "[log][recovery]") {
    const auto path = std::filesystem::temp_directory_path() / "qsl_eventlog_norecover.bin";
    std::filesystem::remove(path);

    const auto recovery = recover_log_file(path);
    REQUIRE(recovery.error == LogError::OpenFailed);
    REQUIRE(recovery.records.empty());
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
