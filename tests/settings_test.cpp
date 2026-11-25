#include <nimblefix/settings.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace nf = nimblefix;
#define CHECK(value)                                                                                         \
    do {                                                                                                     \
        if (!(value)) {                                                                                      \
            std::cerr << "settings: " << __LINE__ << ": " #value "\n";                                       \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (false)

int main() {
    using namespace std::chrono;
    nf::SessionSchedule schedule;
    CHECK(schedule.contains(sys_days{2026y / October / 30}));
    schedule.enabled = true;
    schedule.weekdays = 1U << 5; // Friday.
    schedule.start_second_utc = 22U * 3600U;
    schedule.end_second_utc = 2U * 3600U;
    CHECK(schedule.validate().ok());
    CHECK(!schedule.contains(sys_days{2026y / October / 30} + hours{21}));
    CHECK(schedule.contains(sys_days{2026y / October / 30} + hours{22}));
    CHECK(schedule.contains(sys_days{2026y / October / 31} + hours{1}));
    CHECK(!schedule.contains(sys_days{2026y / October / 31} + hours{2}));
    CHECK(!schedule.contains(sys_days{2026y / October / 31} + hours{23}));
    schedule.start_second_utc = 8U * 3600U;
    schedule.end_second_utc = 16U * 3600U;
    CHECK(schedule.contains(sys_days{2026y / October / 30} + hours{10}));
    CHECK(!schedule.contains(sys_days{2026y / October / 31} + hours{10}));
    schedule.weekdays = 0;
    CHECK(!schedule.validate().ok());

    nf::SessionSettings settings;
    CHECK(nf::validate_session_settings(settings, false, false).ok());
    settings.version = nf::FixVersion::fixt11;
    settings.default_appl_ver_id = "99";
    CHECK(nf::validate_session_settings(settings, false, false).code == nf::ErrorCode::unsupported_protocol);
    settings.default_appl_ver_id = "9";
    settings.logon.extra_fields.push_back({34, "1", false});
    CHECK(nf::validate_session_settings(settings, false, false).code == nf::ErrorCode::invalid_config);
    settings.logon.extra_fields.clear();
    settings.logon.extra_fields.push_back({369, "1", false});
    CHECK(nf::validate_session_settings(settings, false, false).code == nf::ErrorCode::invalid_config);
    settings.logon.extra_fields.clear();
    settings.authentication.method = nf::AuthenticationMethod::credentials;
    settings.authentication.username = "demo";
    settings.authentication.password = "secret";
    CHECK(!nf::validate_session_settings(settings, true, false).ok());
    CHECK(nf::validate_session_settings(settings, true, true).ok());
    CHECK(!nf::validate_session_settings(settings, false, true).ok());

    std::array<char, 512> wire;
    auto logon = nf::message<"A">().set<98>(0).set<108>(30).set<553>("demo").set<554>("secret");
    auto encoded = nf::encode_fix44(logon, {"BUY", "SELL", 1, "20261030-00:00:00.000"}, wire);
    CHECK(encoded.ok());
    nf::FrameView frame;
    CHECK(nf::inspect_frame({wire.data(), encoded.value()}, frame).ok());
    std::array<nf::FieldView, 32> scratch;
    nf::FlatMessageView message;
    CHECK(nf::parse_flat(frame, scratch, message).ok());
    CHECK(nf::authenticate_logon(settings.authentication, message, true).ok());
    CHECK(nf::authenticate_logon(settings.authentication, message, false).code ==
          nf::ErrorCode::authentication_failed);
    settings.authentication.password = "wrong";
    CHECK(nf::authenticate_logon(settings.authentication, message, true).code ==
          nf::ErrorCode::authentication_failed);
    settings.authentication.method = nf::AuthenticationMethod::callback;
    settings.authentication.verify = [](const auto &) -> nf::Status { throw 1; };
    CHECK(nf::authenticate_logon(settings.authentication, message, true).code ==
          nf::ErrorCode::authentication_failed);
    CHECK(nf::parse_fix_version("FIX.4.2").value() == nf::FixVersion::fix42);
    CHECK(nf::parse_fix_version("FIX.9.9").status().code == nf::ErrorCode::unsupported_protocol);
    CHECK(nf::parse_timestamp_precision("ms").value() == nf::TimestampPrecision::milliseconds);
    CHECK(nf::parse_timestamp_precision("milliseconds").value() == nf::TimestampPrecision::milliseconds);
    CHECK(nf::parse_timestamp_precision("us").value() == nf::TimestampPrecision::microseconds);
    CHECK(nf::parse_timestamp_precision("microseconds").value() == nf::TimestampPrecision::microseconds);
    CHECK(nf::parse_timestamp_precision("ns").value() == nf::TimestampPrecision::nanoseconds);
    CHECK(nf::parse_timestamp_precision("nanoseconds").value() == nf::TimestampPrecision::nanoseconds);
    CHECK(nf::parse_timestamp_precision("sec").status().code == nf::ErrorCode::invalid_config);
    CHECK(nf::timestamp_length(nf::TimestampPrecision::milliseconds) == 21);
    CHECK(nf::timestamp_length(nf::TimestampPrecision::microseconds) == 24);
    CHECK(nf::timestamp_length(nf::TimestampPrecision::nanoseconds) == 27);
    CHECK(settings.verify_checksum == true);
    CHECK(settings.timestamp_precision == nf::TimestampPrecision::milliseconds);
    std::cout << "UTC schedule, settings validation and credential authentication passed\n";
}
