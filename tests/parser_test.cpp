#include "nimblefix/codec.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {

using nimblefix::CodecLimits;
using nimblefix::default_max_frame_bytes;
using nimblefix::ErrorCode;
using nimblefix::FieldView;
using nimblefix::FlatMessageView;
using nimblefix::FrameView;
using nimblefix::LengthDataPair;
using nimblefix::Status;
using nimblefix::WireHints;

int failures = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n'; \
            ++failures;                                                                     \
        }                                                                                   \
    } while (false)

constexpr std::string_view ordinary_frame =
    "8=FIX.4.4\001"
    "9=24\001"
    "35=0\001"
    "49=S\001"
    "56=T\001"
    "58=hello\001"
    "10=142\001";

constexpr std::string_view embedded_soh_frame =
    "8=FIX.4.4\001"
    "9=24\001"
    "35=Z\001"
    "95=5\001"
    "96=A\001B\001C\001"
    "49=X\001"
    "10=087\001";

constexpr std::string_view custom_pair_frame =
    "8=FIX.4.4\001"
    "9=26\001"
    "35=Z\001"
    "5000=3\001"
    "5001=A\001B\001"
    "58=q\001"
    "10=218\001";

std::string make_frame(std::string_view body) {
    std::string frame = "8=FIX.4.4\0019=" + std::to_string(body.size()) + "\001";
    frame.append(body);
    unsigned sum = 0;
    for (const char c : frame)
        sum = (sum + static_cast<unsigned char>(c)) & 0xffU;
    frame += "10=";
    const auto hundreds = static_cast<char>('0' + (sum / 100U));
    const auto tens = static_cast<char>('0' + ((sum / 10U) % 10U));
    const auto ones = static_cast<char>('0' + (sum % 10U));
    frame.push_back(hundreds);
    frame.push_back(tens);
    frame.push_back(ones);
    frame.push_back('\001');
    return frame;
}

Status parse(std::string_view wire,
             FlatMessageView& message,
             std::span<FieldView> scratch,
             WireHints hints = {},
             CodecLimits limits = {}) {
    FrameView frame{};
    const auto inspected = nimblefix::inspect_frame(wire, frame, hints, limits);
    if (!inspected.ok()) return inspected;
    return nimblefix::parse_flat(frame, scratch, message, hints, limits);
}

void test_hand_written_frames_and_order() {
    CHECK(ordinary_frame.size() == 46);
    CHECK(embedded_soh_frame.size() == 46);

    FrameView first{};
    const auto status = nimblefix::inspect_frame(ordinary_frame, first);
    CHECK(status.ok());
    CHECK(first.bytes == ordinary_frame);
    CHECK(first.consumed == ordinary_frame.size());

    std::array<FieldView, 16> scratch{};
    FlatMessageView message{};
    CHECK(nimblefix::parse_flat(first, scratch, message).ok());
    CHECK(message.frame() == ordinary_frame);
    CHECK(message.fields().size() == 7);
    CHECK(message.fields()[0].tag == 8);
    CHECK(message.fields()[1].tag == 9);
    CHECK(message.fields()[2].tag == 35);
    CHECK(message.fields()[3].tag == 49);
    CHECK(message.fields()[6].tag == 10);
    const auto text = message.find_unique(58);
    CHECK(text.ok());
    CHECK(text.value().value == "hello");
    CHECK(message.find_unique(49).value().value == "S");
    CHECK(message.find_unique(55).status().code == ErrorCode::missing_field);

    const std::string coalesced = std::string(ordinary_frame) + std::string(embedded_soh_frame);
    FrameView coalesced_first{};
    CHECK(nimblefix::inspect_frame(coalesced, coalesced_first).ok());
    CHECK(coalesced_first.consumed == ordinary_frame.size());
    CHECK(coalesced_first.bytes == ordinary_frame);
}

void test_partial_input_at_every_boundary() {
    for (std::size_t size = 0; size < ordinary_frame.size(); ++size) {
        FrameView frame{};
        const auto status = nimblefix::inspect_frame(ordinary_frame.substr(0, size), frame);
        CHECK(status.code == ErrorCode::need_more);
        CHECK(frame.bytes.empty());
        CHECK(frame.consumed == 0);
    }
}

void test_frame_rejections_and_limits() {
    FrameView frame{};

    std::string bad_checksum(ordinary_frame);
    bad_checksum[bad_checksum.size() - 4] = '3';
    const auto bad_checksum_status = nimblefix::inspect_frame(bad_checksum, frame);
    CHECK(bad_checksum_status.code == ErrorCode::invalid_checksum);
    CHECK(bad_checksum_status.offset == ordinary_frame.size() - 4);

    std::string bad_checksum_delimiter(ordinary_frame);
    bad_checksum_delimiter.back() = 'x';
    CHECK(nimblefix::inspect_frame(bad_checksum_delimiter, frame).code == ErrorCode::invalid_checksum);

    std::string bad_body_length(ordinary_frame);
    bad_body_length[13] = '5'; // 9=25 changes the expected checksum boundary.
    CHECK(nimblefix::inspect_frame(bad_body_length, frame).code == ErrorCode::invalid_frame);

    const std::string malformed_length = "8=FIX.4.4\0019=x\001";
    const auto malformed_length_status = nimblefix::inspect_frame(malformed_length, frame);
    CHECK(malformed_length_status.code == ErrorCode::invalid_body_length);
    CHECK(malformed_length_status.offset == 12);
    const std::string overflowing_length = "8=FIX.4.4\0019=184467440737095516160\001";
    CHECK(nimblefix::inspect_frame(overflowing_length, frame).code == ErrorCode::invalid_body_length);
    const std::string too_large_length = "8=FIX.4.4\0019=999999\001";
    const auto too_large_status = nimblefix::inspect_frame(too_large_length, frame);
    CHECK(too_large_status.code == ErrorCode::too_large);
    CHECK(too_large_status.offset == 10);

    const std::string wrong_begin = "8=FIX.4.2\0019=24\001";
    CHECK(nimblefix::inspect_frame(wrong_begin, frame).code == ErrorCode::invalid_header);
    const std::string wrong_header_order = "8=FIX.4.4\00135=0\0019=24\001";
    CHECK(nimblefix::inspect_frame(wrong_header_order, frame).code == ErrorCode::invalid_header);
    const std::string no_msg_type = "8=FIX.4.4\0019=0\001";
    CHECK(nimblefix::inspect_frame(no_msg_type, frame).code == ErrorCode::invalid_body_length);

    CodecLimits tiny{ordinary_frame.size() - 1};
    CHECK(nimblefix::inspect_frame(ordinary_frame, frame, {}, tiny).code == ErrorCode::too_large);
    CodecLimits impossible{25};
    CHECK(nimblefix::inspect_frame({}, frame, {}, impossible).code == ErrorCode::too_large);

    std::string unframed_body = make_frame("35=Z\00149=X");
    const auto missing_body_delimiter = nimblefix::inspect_frame(unframed_body, frame);
    CHECK(missing_body_delimiter.code == ErrorCode::invalid_frame);
    CHECK(missing_body_delimiter.offset == unframed_body.rfind('X'));

    std::string long_unterminated_length = "8=FIX.4.4\0019=";
    long_unterminated_length.append(70'000, '0');
    const auto bounded_header = nimblefix::inspect_frame(long_unterminated_length, frame);
    CHECK(bounded_header.code == ErrorCode::too_large);
    CHECK(bounded_header.offset == default_max_frame_bytes);
}

void test_builtin_length_data_pairs() {
    FrameView frame{};
    CHECK(nimblefix::inspect_frame(embedded_soh_frame, frame).ok());
    std::array<FieldView, 16> scratch{};
    FlatMessageView message{};
    CHECK(nimblefix::parse_flat(frame, scratch, message).ok());
    CHECK(message.fields().size() == 7);
    CHECK(message.fields()[3].tag == 95);
    CHECK(message.fields()[4].tag == 96);
    CHECK(message.fields()[4].value == std::string_view("A\001B\001C", 5));
    CHECK(message.fields()[5].tag == 49);
    CHECK(message.find_unique(96).value().value.size() == 5);
}

void test_custom_pair_and_malformed_data() {
    constexpr std::array custom_pairs{LengthDataPair{5000, 5001}};
    const WireHints hints{custom_pairs};
    FrameView frame{};
    CHECK(nimblefix::inspect_frame(custom_pair_frame, frame, hints).ok());
    std::array<FieldView, 16> scratch{};
    FlatMessageView message{};
    CHECK(nimblefix::parse_flat(frame, scratch, message, hints).ok());
    CHECK(message.find_unique(5001).value().value == std::string_view("A\001B", 3));
    CHECK(parse(custom_pair_frame, message, scratch).code == ErrorCode::invalid_tag);

    std::string binary_body = "35=Z\00195=3\00196=A";
    binary_body.push_back('\0');
    binary_body += "B\001";
    const auto binary_nul = make_frame(binary_body);
    CHECK(parse(binary_nul, message, scratch).ok());
    CHECK(message.find_unique(96).value().value.size() == 3);
    CHECK(message.find_unique(96).value().value[1] == '\0');

    auto invalid = make_frame("35=Z\00195=x\00196=a\001");
    CHECK(parse(invalid, message, scratch).code == ErrorCode::invalid_value);
    invalid = make_frame("35=Z\00195=4\00196=abc\001");
    CHECK(parse(invalid, message, scratch).code == ErrorCode::invalid_value);
    invalid = make_frame("35=Z\00195=1\00158=x\001");
    CHECK(parse(invalid, message, scratch).code == ErrorCode::invalid_value);
    invalid = make_frame("35=Z\00196=a\001");
    CHECK(parse(invalid, message, scratch).code == ErrorCode::invalid_value);

    constexpr std::array conflicting_pairs{LengthDataPair{95, 777}};
    CHECK(nimblefix::inspect_frame(ordinary_frame, frame, WireHints{conflicting_pairs})
              .code == ErrorCode::invalid_value);

    constexpr std::array<std::uint32_t, 8> managed_tags{8, 9, 10, 34, 35, 49, 52, 56};
    for (const auto managed_tag : managed_tags) {
        const std::array managed_pair{LengthDataPair{5000, managed_tag}};
        CHECK(nimblefix::inspect_frame(ordinary_frame, frame, WireHints{managed_pair})
                  .code == ErrorCode::invalid_value);
    }
    constexpr std::array managed_length{LengthDataPair{52, 5000}};
    CHECK(nimblefix::inspect_frame(ordinary_frame, frame, WireHints{managed_length})
              .code == ErrorCode::invalid_value);

    invalid = make_frame("35=Z\00195=0\00196=\001");
    CHECK(parse(invalid, message, scratch).code == ErrorCode::invalid_value);
    std::array<LengthDataPair, 65> too_many{};
    CHECK(nimblefix::inspect_frame(ordinary_frame, frame, WireHints{too_many})
              .code == ErrorCode::capacity_exceeded);
}

void test_duplicate_order_and_capacity() {
    const auto duplicate_frame = make_frame("35=0\00158=first\00149=S\00158=second\001");
    FrameView frame{};
    CHECK(nimblefix::inspect_frame(duplicate_frame, frame).ok());
    std::array<FieldView, 16> scratch{};
    FlatMessageView message{};
    CHECK(nimblefix::parse_flat(frame, scratch, message).ok());
    CHECK(message.fields().size() == 7);
    CHECK(message.fields()[3].tag == 58);
    CHECK(message.fields()[4].tag == 49);
    CHECK(message.fields()[5].tag == 58);
    CHECK(message.find_unique(58).status().code == ErrorCode::duplicate_field);

    std::array<FieldView, 3> too_small{};
    CHECK(nimblefix::parse_flat(frame, too_small, message).code == ErrorCode::capacity_exceeded);
    CHECK(message.fields().empty());

    const auto bad_tag_frame = make_frame("35=Z\00101=x\001");
    const auto bad_tag_status = parse(bad_tag_frame, message, scratch);
    CHECK(bad_tag_status.code == ErrorCode::invalid_tag);
    CHECK(bad_tag_status.offset == bad_tag_frame.find("01=x"));

    const auto max_fix_tag_frame = make_frame("35=Z\001999999999=ok\001");
    CHECK(parse(max_fix_tag_frame, message, scratch).ok());
    const auto too_large_tag_frame = make_frame("35=Z\0011000000000=ok\001");
    CHECK(parse(too_large_tag_frame, message, scratch).code == ErrorCode::invalid_tag);
}

void test_plain_value_rules() {
    std::array<FieldView, 16> scratch{};
    FlatMessageView message{};

    const auto empty = make_frame("35=Z\00158=\001");
    CHECK(parse(empty, message, scratch).code == ErrorCode::invalid_value);

    const auto equals_is_value_data = make_frame("35=Z\00158=left=right\001");
    CHECK(parse(equals_is_value_data, message, scratch).ok());
    CHECK(message.find_unique(58).value().value == "left=right");

    const auto spaces = make_frame("35=Z\00158= \001");
    CHECK(parse(spaces, message, scratch).ok());
    CHECK(message.find_unique(58).value().value == " ");

    constexpr std::array<char, 4> controls{'\0', '\t', '\n', static_cast<char>(0x7f)};
    for (const char control : controls) {
        std::string body = "35=Z\00158=";
        body.push_back(control);
        body.push_back('\001');
        const auto invalid_control = make_frame(body);
        CHECK(parse(invalid_control, message, scratch).code == ErrorCode::invalid_value);
    }
}

void test_deterministic_malformed_buffer_sweep() {
    std::uint32_t state = 0x5a17c9e3U;
    const auto next = [&state]() noexcept {
        state = state * 1'664'525U + 1'013'904'223U;
        return state;
    };

    std::array<FieldView, 128> scratch{};
    FlatMessageView message{};
    for (std::size_t iteration = 0; iteration < 2'000; ++iteration) {
        const std::size_t random_size = next() % 128U;
        std::string arbitrary;
        arbitrary.reserve(random_size);
        for (std::size_t i = 0; i < random_size; ++i)
            arbitrary.push_back(static_cast<char>(next() & 0xffU));
        FrameView ignored{};
        const auto inspected = nimblefix::inspect_frame(arbitrary, ignored);
        if (inspected.ok()) (void)nimblefix::parse_flat(ignored, scratch, message);

        std::string body = "35=Z\001";
        const std::size_t body_noise_size = next() % 80U;
        body.reserve(body.size() + body_noise_size + 1);
        for (std::size_t i = 0; i < body_noise_size; ++i)
            body.push_back(static_cast<char>(next() & 0xffU));
        body.push_back('\001');
        const std::string framed_noise = make_frame(body);
        FrameView validated{};
        const auto valid_frame = nimblefix::inspect_frame(framed_noise, validated);
        CHECK(valid_frame.ok());
        if (valid_frame.ok()) (void)nimblefix::parse_flat(validated, scratch, message);
    }
}

void test_replay_binary_and_header_contract() {
    constexpr std::string_view original_time = "20261027-00:00:00.000";
    constexpr std::string_view later_time = "20261027-00:01:00.000";
    std::string body = "35=Z\00149=BUY\00156=SELL\00134=7\00152=";
    body.append(original_time).append("\00195=5\00196=");
    const std::string binary{"A\001\0=B", 5};
    body.append(binary).append("\00158=first\00158=second\001");
    const auto wire = make_frame(body);
    FlatMessageView original;
    std::array<FieldView, 24> scratch{};
    CHECK(parse(wire, original, scratch).ok());
    std::array<char, 1024> output{};
    const auto encoded = nimblefix::encode_fix44_replay(original,
        {"BUY", "SELL", 7, later_time}, output);
    CHECK(encoded.ok());
    if (!encoded.ok()) return;
    FlatMessageView replay;
    std::array<FieldView, 24> replay_scratch{};
    CHECK(parse({output.data(), encoded.value()}, replay, replay_scratch).ok());
    CHECK(replay.find_unique(34).value().value == "7");
    CHECK(replay.find_unique(43).value().value == "Y");
    CHECK(replay.find_unique(122).value().value == original_time);
    CHECK(replay.find_unique(96).value().value == binary);
    CHECK(replay.find_unique(58).status().code == ErrorCode::duplicate_field);
    std::array<char, 1024> second_output{};
    const auto second = nimblefix::encode_fix44_replay(replay,
        {"BUY", "SELL", 7, "20261027-00:02:00.000"}, second_output);
    CHECK(second.ok());
    CHECK(std::string_view(second_output.data(), second.ok() ? second.value() : 0)
        .find("122=20261027-00:00:00.000\001") != std::string_view::npos);

    std::array<char, 8> small{};
    small.fill('#');
    CHECK(nimblefix::encode_fix44_replay(original, {"BUY", "SELL", 7, later_time}, small)
        .status().code == ErrorCode::capacity_exceeded);
    CHECK(std::string_view(small.data(), small.size()) == "########");
    const auto duplicate_wire = make_frame(
        "35=Z\00149=BUY\00156=SELL\00134=7\00143=Y\00152=20261027-00:01:00.000\001"
        "122=20261027-00:00:00.000\001122=20261027-00:00:00.000\001");
    CHECK(parse(duplicate_wire, original, scratch).ok());
    const auto duplicate = nimblefix::encode_fix44_replay(original,
        {"BUY", "SELL", 7, later_time}, output);
    CHECK(duplicate.status().code == ErrorCode::duplicate_field && duplicate.status().tag == 122);
}

} // namespace

int main() {
    test_hand_written_frames_and_order();
    test_partial_input_at_every_boundary();
    test_frame_rejections_and_limits();
    test_builtin_length_data_pairs();
    test_custom_pair_and_malformed_data();
    test_duplicate_order_and_capacity();
    test_plain_value_rules();
    test_deterministic_malformed_buffer_sweep();
    test_replay_binary_and_header_contract();
    if (failures != 0) {
        std::cerr << failures << " parser test(s) failed\n";
        return 1;
    }
    std::cout << "parser tests passed\n";
    return 0;
}
