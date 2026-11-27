#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

#include "status.hpp"

namespace nimblefix {

enum class FixVersion : std::uint8_t { fix42, fix43, fix44, fixt11 };

namespace tag {

// Standard FIX session, framing, and administrative tags
inline constexpr std::uint32_t begin_seq_no = 7;
inline constexpr std::uint32_t begin_string = 8;
inline constexpr std::uint32_t body_length = 9;
inline constexpr std::uint32_t check_sum = 10;
inline constexpr std::uint32_t end_seq_no = 16;
inline constexpr std::uint32_t msg_seq_num = 34;
inline constexpr std::uint32_t msg_type = 35;
inline constexpr std::uint32_t new_seq_no = 36;
inline constexpr std::uint32_t poss_dup_flag = 43;
inline constexpr std::uint32_t ref_seq_num = 45;
inline constexpr std::uint32_t sender_comp_id = 49;
inline constexpr std::uint32_t sender_sub_id = 50;
inline constexpr std::uint32_t sending_time = 52;
inline constexpr std::uint32_t target_comp_id = 56;
inline constexpr std::uint32_t target_sub_id = 57;
inline constexpr std::uint32_t text = 58;
inline constexpr std::uint32_t signature = 89;
inline constexpr std::uint32_t secure_data_len = 90;
inline constexpr std::uint32_t secure_data = 91;
inline constexpr std::uint32_t signature_length = 93;
inline constexpr std::uint32_t raw_data_length = 95;
inline constexpr std::uint32_t raw_data = 96;
inline constexpr std::uint32_t poss_resend = 97;
inline constexpr std::uint32_t encrypt_method = 98;
inline constexpr std::uint32_t heart_bt_int = 108;
inline constexpr std::uint32_t test_req_id = 112;
inline constexpr std::uint32_t on_behalf_of_comp_id = 115;
inline constexpr std::uint32_t on_behalf_of_sub_id = 116;
inline constexpr std::uint32_t orig_sending_time = 122;
inline constexpr std::uint32_t gap_fill_flag = 123;
inline constexpr std::uint32_t deliver_to_comp_id = 128;
inline constexpr std::uint32_t deliver_to_sub_id = 129;
inline constexpr std::uint32_t reset_seq_num_flag = 141;
inline constexpr std::uint32_t sender_location_id = 142;
inline constexpr std::uint32_t target_location_id = 143;
inline constexpr std::uint32_t on_behalf_of_location_id = 144;
inline constexpr std::uint32_t deliver_to_location_id = 145;

// Standard FIX binary LENGTH/DATA field tags
inline constexpr std::uint32_t xml_data_len = 212;
inline constexpr std::uint32_t xml_data = 213;
inline constexpr std::uint32_t message_encoding = 347;
inline constexpr std::uint32_t encoded_issuer_len = 348;
inline constexpr std::uint32_t encoded_issuer = 349;
inline constexpr std::uint32_t encoded_security_desc_len = 350;
inline constexpr std::uint32_t encoded_security_desc = 351;
inline constexpr std::uint32_t encoded_list_exec_inst_len = 352;
inline constexpr std::uint32_t encoded_list_exec_inst = 353;
inline constexpr std::uint32_t encoded_text_len = 354;
inline constexpr std::uint32_t encoded_text = 355;
inline constexpr std::uint32_t encoded_subject_len = 356;
inline constexpr std::uint32_t encoded_subject = 357;
inline constexpr std::uint32_t encoded_headline_len = 358;
inline constexpr std::uint32_t encoded_headline = 359;
inline constexpr std::uint32_t encoded_alloc_text_len = 360;
inline constexpr std::uint32_t encoded_alloc_text = 361;
inline constexpr std::uint32_t encoded_underlying_issuer_len = 362;
inline constexpr std::uint32_t encoded_underlying_issuer = 363;
inline constexpr std::uint32_t encoded_underlying_security_desc_len = 364;
inline constexpr std::uint32_t encoded_underlying_security_desc = 365;
inline constexpr std::uint32_t last_msg_seq_num_processed = 369;
inline constexpr std::uint32_t ref_tag_id = 371;
inline constexpr std::uint32_t ref_msg_type = 372;
inline constexpr std::uint32_t session_reject_reason = 373;
inline constexpr std::uint32_t encoded_list_status_text_len = 445;
inline constexpr std::uint32_t encoded_list_status_text = 446;
inline constexpr std::uint32_t username = 553;
inline constexpr std::uint32_t password = 554;
inline constexpr std::uint32_t encoded_leg_issuer_len = 618;
inline constexpr std::uint32_t encoded_leg_issuer = 619;
inline constexpr std::uint32_t encoded_leg_security_desc_len = 621;
inline constexpr std::uint32_t encoded_leg_security_desc = 622;

// FIXT 1.1 / FIX 5.0+ application versioning tags
inline constexpr std::uint32_t appl_ver_id = 1128;
inline constexpr std::uint32_t cstm_appl_ver_id = 1129;
inline constexpr std::uint32_t default_appl_ver_id = 1137;
inline constexpr std::uint32_t appl_ext_id = 1156;

} // namespace tag

namespace msg_type {

inline constexpr std::string_view heartbeat = "0";
inline constexpr std::string_view test_request = "1";
inline constexpr std::string_view resend_request = "2";
inline constexpr std::string_view reject = "3";
inline constexpr std::string_view sequence_reset = "4";
inline constexpr std::string_view logout = "5";
inline constexpr std::string_view execution_report = "8";
inline constexpr std::string_view logon = "A";
inline constexpr std::string_view new_order_single = "D";
inline constexpr std::string_view business_message_reject = "j";

[[nodiscard]] constexpr bool is_session_admin(std::string_view value) noexcept {
    return value == logon || value == heartbeat || value == test_request ||
           value == resend_request || value == reject || value == sequence_reset ||
           value == logout;
}

} // namespace msg_type

namespace appl_ver_id {

inline constexpr std::string_view fix42 = "4";
inline constexpr std::string_view fix43 = "5";
inline constexpr std::string_view fix44 = "6";
inline constexpr std::string_view fix50 = "7";
inline constexpr std::string_view fix50sp1 = "8";
inline constexpr std::string_view fix50sp2 = "9";

} // namespace appl_ver_id

namespace reject_reason {

inline constexpr std::uint64_t invalid_tag_number = 0;
inline constexpr std::uint64_t required_tag_missing = 1;
inline constexpr std::uint64_t tag_not_defined_for_message = 2;
inline constexpr std::uint64_t undefined_tag = 3;
inline constexpr std::uint64_t tag_specified_without_value = 4;
inline constexpr std::uint64_t value_is_incorrect = 5;
inline constexpr std::uint64_t incorrect_data_format = 6;
inline constexpr std::uint64_t decryption_problem = 7;
inline constexpr std::uint64_t signature_problem = 8;
inline constexpr std::uint64_t comp_id_problem = 9;
inline constexpr std::uint64_t sending_time_accuracy_problem = 10;
inline constexpr std::uint64_t invalid_msg_type = 11;
inline constexpr std::uint64_t other = 99;

} // namespace reject_reason

// Protocol framing & bounded input constants
inline constexpr std::uint32_t max_fix_tag = 999'999'999;
inline constexpr std::size_t min_fix_frame_bytes = 26;
inline constexpr std::size_t checksum_field_bytes = 7; // "10=ddd\x01"
inline constexpr std::size_t min_body_length_bytes = 5; // "35=X\x01"
inline constexpr std::uint64_t encrypt_method_none = 0;
inline constexpr std::string_view encrypt_method_none_str = "0";
inline constexpr std::uint32_t seconds_per_day = 86'400;
inline constexpr std::uint8_t all_weekdays_mask = 0x7f;
inline constexpr std::size_t max_admin_reason_bytes = 200;
inline constexpr std::size_t max_qualifier_bytes = 64;
inline constexpr std::size_t max_credential_user_bytes = 256;
inline constexpr std::size_t max_credential_secret_bytes = 4096;
inline constexpr std::size_t max_logon_extra_fields = 16;
inline constexpr std::size_t max_tls_hostname_bytes = 253;
inline constexpr std::size_t timestamp_seconds_len = 17;
inline constexpr std::size_t timestamp_millis_len = 21;
inline constexpr std::size_t timestamp_micros_len = 24;
inline constexpr std::size_t timestamp_nanos_len = 27;

[[nodiscard]] constexpr std::string_view begin_string(FixVersion version) noexcept {
    switch (version) {
    case FixVersion::fix42:
        return "FIX.4.2";
    case FixVersion::fix43:
        return "FIX.4.3";
    case FixVersion::fix44:
        return "FIX.4.4";
    case FixVersion::fixt11:
        return "FIXT.1.1";
    }
    return {};
}

[[nodiscard]] inline Result<FixVersion> parse_fix_version(std::string_view text) noexcept {
    for (const auto version : {FixVersion::fix42, FixVersion::fix43, FixVersion::fix44, FixVersion::fixt11})
        if (begin_string(version) == text)
            return version;
    return Status{ErrorCode::unsupported_protocol, tag::begin_string};
}

[[nodiscard]] constexpr bool valid_appl_ver_id(std::string_view value) noexcept {
    return value == appl_ver_id::fix42 || value == appl_ver_id::fix43 ||
           value == appl_ver_id::fix44 || value == appl_ver_id::fix50 ||
           value == appl_ver_id::fix50sp1 || value == appl_ver_id::fix50sp2;
}

enum class TimestampPrecision : std::uint8_t {
    milliseconds,
    microseconds,
    nanoseconds,
};

[[nodiscard]] constexpr std::size_t timestamp_length(TimestampPrecision precision) noexcept {
    switch (precision) {
    case TimestampPrecision::milliseconds:
        return timestamp_millis_len;
    case TimestampPrecision::microseconds:
        return timestamp_micros_len;
    case TimestampPrecision::nanoseconds:
        return timestamp_nanos_len;
    }
    return timestamp_millis_len;
}

[[nodiscard]] inline Result<TimestampPrecision> parse_timestamp_precision(std::string_view text) noexcept {
    if (text == "ms" || text == "milliseconds") return TimestampPrecision::milliseconds;
    if (text == "us" || text == "microseconds") return TimestampPrecision::microseconds;
    if (text == "ns" || text == "nanoseconds") return TimestampPrecision::nanoseconds;
    return Status{ErrorCode::invalid_config};
}

} // namespace nimblefix
