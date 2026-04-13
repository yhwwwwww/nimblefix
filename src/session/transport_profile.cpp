#include "fastfix/session/transport_profile.h"

namespace fastfix::session {

auto TransportSessionProfile::Fix40() -> TransportSessionProfile {
    return TransportSessionProfile{
        .version = TransportVersion::kFix40,
        .begin_string = "FIX.4.0",
        .requires_default_appl_ver_id = false,
        .supports_reset_on_logon = false,
        .default_heartbeat_interval_seconds = 30,
        .supports_next_expected_msg_seq_num = false,
        .supports_msg_seq_num_reset = true,
        .transport_and_app_version_decoupled = false,
    };
}

auto TransportSessionProfile::Fix41() -> TransportSessionProfile {
    return TransportSessionProfile{
        .version = TransportVersion::kFix41,
        .begin_string = "FIX.4.1",
        .requires_default_appl_ver_id = false,
        .supports_reset_on_logon = true,
        .default_heartbeat_interval_seconds = 30,
        .supports_next_expected_msg_seq_num = false,
        .supports_msg_seq_num_reset = true,
        .transport_and_app_version_decoupled = false,
    };
}

auto TransportSessionProfile::Fix42() -> TransportSessionProfile {
    return TransportSessionProfile{
        .version = TransportVersion::kFix42,
        .begin_string = "FIX.4.2",
        .requires_default_appl_ver_id = false,
        .supports_reset_on_logon = true,
        .default_heartbeat_interval_seconds = 30,
        .supports_next_expected_msg_seq_num = false,
        .supports_msg_seq_num_reset = true,
        .transport_and_app_version_decoupled = false,
    };
}

auto TransportSessionProfile::Fix43() -> TransportSessionProfile {
    return TransportSessionProfile{
        .version = TransportVersion::kFix43,
        .begin_string = "FIX.4.3",
        .requires_default_appl_ver_id = false,
        .supports_reset_on_logon = true,
        .default_heartbeat_interval_seconds = 30,
        .supports_next_expected_msg_seq_num = false,
        .supports_msg_seq_num_reset = true,
        .transport_and_app_version_decoupled = false,
    };
}

auto TransportSessionProfile::Fix44() -> TransportSessionProfile {
    return TransportSessionProfile{
        .version = TransportVersion::kFix44,
        .begin_string = "FIX.4.4",
        .requires_default_appl_ver_id = false,
        .supports_reset_on_logon = true,
        .default_heartbeat_interval_seconds = 30,
        .supports_next_expected_msg_seq_num = false,
        .supports_msg_seq_num_reset = true,
        .transport_and_app_version_decoupled = false,
    };
}

auto TransportSessionProfile::FixT11() -> TransportSessionProfile {
    return TransportSessionProfile{
        .version = TransportVersion::kFixT11,
        .begin_string = "FIXT.1.1",
        .requires_default_appl_ver_id = true,
        .supports_reset_on_logon = true,
        .default_heartbeat_interval_seconds = 30,
        .supports_next_expected_msg_seq_num = true,
        .supports_msg_seq_num_reset = true,
        .transport_and_app_version_decoupled = true,
    };
}

auto TransportSessionProfile::FromBeginString(std::string_view begin_string) -> TransportSessionProfile {
    if (begin_string == "FIX.4.0") return Fix40();
    if (begin_string == "FIX.4.1") return Fix41();
    if (begin_string == "FIX.4.2") return Fix42();
    if (begin_string == "FIX.4.3") return Fix43();
    if (begin_string == "FIX.4.4") return Fix44();
    if (begin_string == "FIXT.1.1") return FixT11();

    // Unknown begin_string: return a default FIX.4.4 profile with the
    // caller-supplied begin_string so the wire value is preserved.
    auto profile = Fix44();
    profile.begin_string = std::string(begin_string);
    return profile;
}

}  // namespace fastfix::session
