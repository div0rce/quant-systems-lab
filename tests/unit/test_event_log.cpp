#include "qsl/protocol/codec.hpp"
#include "qsl/protocol/endian.hpp"
#include "qsl/replay/event_log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
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
