#include <nimblefix/settings.hpp>

#include <algorithm>

namespace nimblefix {
namespace {
bool plain(std::string_view value) noexcept {
    for (const unsigned char ch : value)
        if (ch < 0x20 || ch == 0x7f)
            return false;
    return true;
}
bool secret_equal(std::string_view first, std::string_view second) noexcept {
    // Fixed-length constant-time comparison preventing compiler short-circuiting
    // and avoiding length-oracle timing differences across credential inputs.
    volatile std::size_t difference = first.size() ^ second.size();
    const std::size_t size = std::max(first.size(), second.size());
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char a = i < first.size() ? static_cast<unsigned char>(first[i]) : 0U;
        const unsigned char b = i < second.size() ? static_cast<unsigned char>(second[i]) : 0U;
        difference = difference | static_cast<std::size_t>(a ^ b);
    }
    return difference == 0;
}
bool managed(std::uint32_t tag) noexcept {
    if (detail::is_managed_tag(tag)) return true;
    for (const auto value :
         {8U, 9U, 10U, 34U, 35U, 43U, 49U, 52U, 56U, 98U, 108U, 122U, 141U, 553U, 554U, 1137U})
        if (tag == value)
            return true;
    return false;
}
} // namespace

Status validate_session_settings(const SessionSettings &settings, bool acceptor, bool tls_enabled) noexcept {
    if (begin_string(settings.version).empty() ||
        (settings.version == FixVersion::fixt11 && !valid_appl_ver_id(settings.default_appl_ver_id)))
        return {ErrorCode::unsupported_protocol};
    if (!settings.schedule.validate().ok() || !plain(settings.qualifier) || settings.qualifier.size() > 64 ||
        !plain(settings.logon.username) || !plain(settings.logon.password) ||
        settings.logon.username.size() > 256 || settings.logon.password.size() > 4096 ||
        settings.logon.extra_fields.size() > 16 ||
        (settings.logon.request_sequence_reset && !settings.allow_sequence_reset))
        return {ErrorCode::invalid_config};
    for (std::size_t i = 0; i < settings.logon.extra_fields.size(); ++i) {
        const auto &field = settings.logon.extra_fields[i];
        if (field.tag == 0 || field.tag > 999'999'999 || managed(field.tag) || field.value.empty() ||
            field.value.size() > 4096 || !plain(field.value))
            return {ErrorCode::invalid_config, static_cast<int>(field.tag)};
        for (std::size_t j = 0; j < i; ++j)
            if (settings.logon.extra_fields[j].tag == field.tag)
                return {ErrorCode::duplicate_field, static_cast<int>(field.tag)};
    }
    const auto &auth = settings.authentication;
    if (auth.method != AuthenticationMethod::none && auth.method != AuthenticationMethod::credentials &&
        auth.method != AuthenticationMethod::callback)
        return {ErrorCode::invalid_config};
    if (!plain(auth.username) || !plain(auth.password) || auth.username.size() > 256 ||
        auth.password.size() > 4096)
        return {ErrorCode::invalid_config};
    if (auth.method != AuthenticationMethod::none && !acceptor)
        return {ErrorCode::invalid_config};
    if (auth.method == AuthenticationMethod::credentials && (auth.username.empty() || auth.password.empty()))
        return {ErrorCode::invalid_config};
    if (auth.method == AuthenticationMethod::callback && !auth.verify)
        return {ErrorCode::invalid_config};
    if (auth.method != AuthenticationMethod::none && auth.require_tls && !tls_enabled)
        return {ErrorCode::invalid_config};
    if (settings.flow_control) {
        const auto &flow = *settings.flow_control;
        if (flow.max_pending_messages <= 8 || flow.max_pending_bytes == 0 ||
            flow.max_pending_inbound_messages == 0 || flow.max_pending_inbound_bytes == 0 ||
            flow.max_delivery_age_ms == 0 || flow.receive_pause_timeout_ms == 0 || flow.write_stall_ms == 0)
            return {ErrorCode::invalid_config};
    }
    return Status::success();
}

Status authenticate_logon(const AuthenticationConfig &auth, const FlatMessageView &message,
                          bool tls_enabled) noexcept {
    if (auth.method == AuthenticationMethod::none)
        return Status::success();
    if (auth.require_tls && !tls_enabled)
        return {ErrorCode::authentication_failed};
    if (auth.method == AuthenticationMethod::callback) {
        try {
            return auth.verify ? auth.verify(message) : Status{ErrorCode::authentication_failed};
        } catch (...) {
            return {ErrorCode::authentication_failed};
        }
    }
    const auto username = message.find_unique(553);
    const auto password = message.find_unique(554);
    if (!username.ok() || !password.ok() || username.value().value.size() > 256 ||
        password.value().value.size() > 4096)
        return {ErrorCode::authentication_failed};
    const bool valid_user = secret_equal(username.value().value, auth.username);
    const bool valid_password = secret_equal(password.value().value, auth.password);
    return valid_user && valid_password ? Status::success() : Status{ErrorCode::authentication_failed};
}

} // namespace nimblefix
