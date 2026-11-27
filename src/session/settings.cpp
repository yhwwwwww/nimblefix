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
bool managed(std::uint32_t field_tag) noexcept {
    if (detail::is_managed_tag(field_tag)) return true;
    for (const auto value : {
             tag::encrypt_method,
             tag::heart_bt_int,
             tag::reset_seq_num_flag,
             tag::username,
             tag::password,
             tag::default_appl_ver_id,
         })
        if (field_tag == value)
            return true;
    return false;
}
} // namespace

Status validate_session_settings(const SessionSettings &settings, bool acceptor, bool tls_enabled) noexcept {
    if (begin_string(settings.version).empty() ||
        (settings.version == FixVersion::fixt11 && !valid_appl_ver_id(settings.default_appl_ver_id)))
        return {ErrorCode::unsupported_protocol};
    if (!settings.schedule.validate().ok() || !plain(settings.qualifier) ||
        settings.qualifier.size() > max_qualifier_bytes ||
        !plain(settings.logon.username) || !plain(settings.logon.password) ||
        settings.logon.username.size() > max_credential_user_bytes ||
        settings.logon.password.size() > max_credential_secret_bytes ||
        settings.logon.extra_fields.size() > max_logon_extra_fields ||
        (settings.logon.request_sequence_reset && !settings.allow_sequence_reset) ||
        (settings.schedule.reset_on_schedule_boundary &&
         (!settings.schedule.enabled || !settings.allow_sequence_reset)))
        return {ErrorCode::invalid_config};
    if (settings.unexpected_field_policy != UnexpectedFieldPolicy::reject &&
        settings.unexpected_field_policy != UnexpectedFieldPolicy::ignore_with_metric &&
        settings.unexpected_field_policy != UnexpectedFieldPolicy::passthrough)
        return {ErrorCode::invalid_config};
    if (settings.schema != nullptr) {
        if (settings.schema->messages.empty())
            return {ErrorCode::schema_mismatch};
        if (!settings.schema->matches_session(settings.version, settings.default_appl_ver_id))
            return {ErrorCode::unsupported_protocol, tag::begin_string};
    }
    const auto session_hints = settings.schema_wire_hints.length_data_pairs;
    for (std::size_t i = 0; i < session_hints.size(); ++i) {
        const auto left = session_hints[i];
        if (left.length_tag == 0 || left.data_tag == 0 || left.length_tag == left.data_tag)
            return {ErrorCode::invalid_config};
        for (std::size_t j = i + 1; j < session_hints.size(); ++j) {
            const auto right = session_hints[j];
            const bool conflict =
                (left.length_tag == right.length_tag && left.data_tag != right.data_tag) ||
                (left.data_tag == right.data_tag && left.length_tag != right.length_tag) ||
                (left.length_tag == right.data_tag && left.data_tag != right.length_tag) ||
                (left.data_tag == right.length_tag && left.length_tag != right.data_tag);
            if (conflict) return {ErrorCode::invalid_config};
        }
    }
    for (std::size_t i = 0; i < settings.logon.extra_fields.size(); ++i) {
        const auto &field = settings.logon.extra_fields[i];
        if (field.tag == 0 || field.tag > max_fix_tag || managed(field.tag) || field.value.empty() ||
            field.value.size() > max_credential_secret_bytes || !plain(field.value))
            return {ErrorCode::invalid_config, static_cast<int>(field.tag)};
        for (std::size_t j = 0; j < i; ++j)
            if (settings.logon.extra_fields[j].tag == field.tag)
                return {ErrorCode::duplicate_field, static_cast<int>(field.tag)};
    }
    const auto &auth = settings.authentication;
    if (auth.method != AuthenticationMethod::none && auth.method != AuthenticationMethod::credentials &&
        auth.method != AuthenticationMethod::callback)
        return {ErrorCode::invalid_config};
    if (!plain(auth.username) || !plain(auth.password) ||
        auth.username.size() > max_credential_user_bytes ||
        auth.password.size() > max_credential_secret_bytes)
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
    const auto username = message.find_unique(tag::username);
    const auto password = message.find_unique(tag::password);
    if (!username.ok() || !password.ok() ||
        username.value().value.size() > max_credential_user_bytes ||
        password.value().value.size() > max_credential_secret_bytes)
        return {ErrorCode::authentication_failed};
    const bool valid_user = secret_equal(username.value().value, auth.username);
    const bool valid_password = secret_equal(password.value().value, auth.password);
    return valid_user && valid_password ? Status::success() : Status{ErrorCode::authentication_failed};
}

} // namespace nimblefix
