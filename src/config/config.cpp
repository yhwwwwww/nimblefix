#include <nimblefix/config.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>

#define TOML_MAX_NESTED_VALUES 64
#include <toml.hpp>

namespace nimblefix {
namespace {
constexpr std::size_t max_config_bytes = 1024U * 1024U;
constexpr std::size_t max_config_diagnostics = 128;

std::string json_string(std::string_view text) {
    std::string result{"\""};
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char ch : text) {
        if (ch == '"' || ch == '\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(ch));
        } else if (ch < 0x20) {
            result.append("\\u00");
            result.push_back(digits[ch >> 4]);
            result.push_back(digits[ch & 15]);
        } else
            result.push_back(static_cast<char>(ch));
    }
    result.push_back('"');
    return result;
}

bool is_secret_path(std::string_view path) {
    return path.find("password") != std::string_view::npos || path.find("secret") != std::string_view::npos ||
           path.find("token") != std::string_view::npos;
}

std::string field_path(std::string_view prefix, std::string_view name) {
    return prefix.empty() ? std::string{name} : std::string{prefix} + "." + std::string{name};
}

std::string node_summary(const toml::node *node, std::string_view field) {
    if (is_secret_path(field))
        return "<redacted>";
    if (node == nullptr)
        return "<missing>";
    if (const auto value = node->value<std::string>())
        return value->substr(0, 256);
    if (const auto value = node->value<std::int64_t>())
        return std::to_string(*value);
    if (const auto value = node->value<bool>())
        return *value ? "true" : "false";
    if (node->is_table())
        return "<table>";
    if (node->is_array())
        return "<array>";
    return "<incorrect type>";
}

class Loader {
  public:
    std::filesystem::path source;
    std::filesystem::path base;
    std::vector<ConfigDiagnostic> diagnostics;
    ConfigDocument document;

    void error(const toml::node *node, std::string field, std::string expected,
               std::string fix = "change the named configuration value") {
        if (diagnostics.size() == max_config_diagnostics)
            return;
        ConfigDiagnostic item;
        item.source = source;
        if (node != nullptr) {
            item.line = node->source().begin.line;
            item.column = node->source().begin.column;
        }
        item.received = node_summary(node, field);
        item.field = std::move(field);
        item.expected = std::move(expected);
        item.fix = std::move(fix);
        diagnostics.push_back(std::move(item));
    }

    void keys(const toml::table &table, std::string_view prefix,
              std::initializer_list<std::string_view> allowed) {
        for (const auto &[key, value] : table) {
            const auto name = key.str();
            if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
                error(&value,
                      prefix.empty() ? std::string{name} : std::string{prefix} + "." + std::string{name},
                      "a documented configuration key", "remove or correct the unknown key");
        }
    }

    std::string string(const toml::table &table, std::string_view name, std::string_view prefix,
                       std::string fallback = {}, bool required = false) {
        const auto *node = table.get(name);
        if (node == nullptr && !required)
            return fallback;
        if (node != nullptr)
            if (const auto value = node->value<std::string>())
                return *value;
        error(node, field_path(prefix, name), "a string");
        return fallback;
    }

    bool boolean(const toml::table &table, std::string_view name, std::string_view prefix, bool fallback) {
        const auto *node = table.get(name);
        if (node == nullptr)
            return fallback;
        if (const auto value = node->value<bool>())
            return *value;
        error(node, field_path(prefix, name), "true or false");
        return fallback;
    }

    std::uint64_t integer(const toml::table &table, std::string_view name, std::string_view prefix,
                          std::uint64_t fallback, std::uint64_t minimum, std::uint64_t maximum) {
        const auto *node = table.get(name);
        if (node == nullptr)
            return fallback;
        if (const auto value = node->value<std::int64_t>(); value && *value >= 0 &&
                                                            static_cast<std::uint64_t>(*value) >= minimum &&
                                                            static_cast<std::uint64_t>(*value) <= maximum)
            return static_cast<std::uint64_t>(*value);
        error(node, field_path(prefix, name),
              "integer in " + std::to_string(minimum) + ".." + std::to_string(maximum));
        return fallback;
    }

    const toml::table *table(const toml::table &parent, std::string_view name, std::string_view prefix) {
        const auto *node = parent.get(name);
        if (node == nullptr)
            return nullptr;
        if (const auto *result = node->as_table())
            return result;
        error(node, field_path(prefix, name), "a table");
        return nullptr;
    }

    std::filesystem::path path(std::string value) const {
        if (value.empty())
            return {};
        const std::filesystem::path input{std::move(value)};
        return (input.is_absolute() ? input : base / input).lexically_normal();
    }

    std::string password(const toml::table &table, std::string_view prefix) {
        const auto variable = string(table, "password_env", prefix);
        if (variable.empty())
            return {};
        if (variable.find('\0') != std::string::npos) {
            error(table.get("password_env"), std::string{prefix} + ".password_env",
                  "an environment variable name");
            return {};
        }
        const char *value = std::getenv(variable.c_str());
        if (value == nullptr || std::char_traits<char>::length(value) > 4096) {
            error(table.get("password_env"), std::string{prefix} + ".password_env",
                  "an explicitly set environment variable of at most 4096 bytes",
                  "set the referenced environment variable before validation or startup");
            return {};
        }
        return value;
    }

    TcpEndpoint endpoint(const toml::table &table, std::string_view name, std::string_view prefix,
                         bool listener) {
        const auto value = string(table, name, prefix, {}, true);
        std::string host;
        std::string_view port;
        if (value.starts_with('[')) {
            const auto end = value.find(']');
            if (end != std::string::npos && end + 1 < value.size() && value[end + 1] == ':') {
                host = value.substr(1, end - 1);
                port = std::string_view{value}.substr(end + 2);
            }
        } else {
            const auto end = value.rfind(':');
            if (end != std::string::npos && value.find(':') == end) {
                host = value.substr(0, end);
                port = std::string_view{value}.substr(end + 1);
            }
        }
        std::uint32_t number{};
        if (port.empty()) {
            error(table.get(name), std::string{prefix} + "." + std::string{name},
                  "a numeric endpoint with a port");
            return {};
        }
        const auto converted = std::from_chars(port.data(), port.data() + port.size(), number);
        if (host.empty() || port.empty() || converted.ec != std::errc{} ||
            converted.ptr != port.data() + port.size() || number > 65'535 || (!listener && number == 0)) {
            error(table.get(name), std::string{prefix} + "." + std::string{name},
                  listener ? "numeric-IP:port (0..65535), bracket IPv6"
                           : "numeric-IP:port (1..65535), bracket IPv6");
            return {};
        }
        return {std::move(host), static_cast<std::uint16_t>(number)};
    }

    TlsConfig tls(const toml::table &parent, std::string_view prefix) {
        TlsConfig config;
        const auto *value = table(parent, "tls", prefix);
        if (value == nullptr)
            return config;
        const auto path_prefix = std::string{prefix} + ".tls";
        keys(*value, path_prefix,
             {"enabled", "ca_file", "certificate_file", "private_key_file", "server_name",
              "require_client_certificate", "minimum_version", "handshake_timeout_ms",
              "max_certificate_bytes"});
        config.enabled = boolean(*value, "enabled", path_prefix, true);
        config.ca_file = path(string(*value, "ca_file", path_prefix));
        config.certificate_file = path(string(*value, "certificate_file", path_prefix));
        config.private_key_file = path(string(*value, "private_key_file", path_prefix));
        config.server_name = string(*value, "server_name", path_prefix);
        config.require_client_certificate = boolean(*value, "require_client_certificate", path_prefix, false);
        config.minimum_version =
            static_cast<std::uint16_t>(integer(*value, "minimum_version", path_prefix, 12, 12, 13));
        config.handshake_timeout_ms = static_cast<std::uint32_t>(
            integer(*value, "handshake_timeout_ms", path_prefix, 10'000, 1, 3'600'000));
        config.max_certificate_bytes =
            integer(*value, "max_certificate_bytes", path_prefix, 65'536, 1024, 4U * 1024U * 1024U);
        return config;
    }

    SessionSettings settings(const toml::table &session, std::string_view prefix, bool acceptor) {
        SessionSettings result;
        const auto version = string(session, "transport", prefix, "FIX.4.4");
        const auto parsed = parse_fix_version(version);
        if (!parsed.ok())
            error(session.get("transport"), std::string{prefix} + ".transport",
                  "FIX.4.2, FIX.4.3, FIX.4.4 or FIXT.1.1");
        else
            result.version = parsed.value();
        result.qualifier = string(session, "qualifier", prefix);
        result.default_appl_ver_id = string(session, "default_appl_ver_id", prefix, "9");
        result.allow_sequence_reset = boolean(session, "allow_sequence_reset", prefix, false);
        result.emit_last_processed_seq = boolean(session, "emit_last_processed_seq", prefix, true);
        result.check_sending_time = boolean(session, "check_sending_time", prefix, true);
        result.max_sending_time_skew_ms = static_cast<std::uint32_t>(
            integer(session, "max_sending_time_skew_ms", prefix, 120'000, 0, 86'400'000));
        const auto precision_text = string(session, "timestamp_precision", prefix, "ms");
        const auto parsed_precision = parse_timestamp_precision(precision_text);
        if (!parsed_precision.ok())
            error(session.get("timestamp_precision"), std::string{prefix} + ".timestamp_precision",
                  "ms, us, ns (or milliseconds, microseconds, nanoseconds)");
        else
            result.timestamp_precision = parsed_precision.value();
        result.verify_checksum = boolean(session, "verify_checksum", prefix, true);
        if (const auto *logon = table(session, "logon", prefix)) {
            const auto p = std::string{prefix} + ".logon";
            keys(*logon, p, {"username", "password_env", "reset_sequence", "fields"});
            result.logon.username = string(*logon, "username", p);
            result.logon.password = password(*logon, p);
            result.logon.request_sequence_reset = boolean(*logon, "reset_sequence", p, false);
            if (const auto *fields = logon->get("fields")) {
                const auto *array = fields->as_array();
                if (array == nullptr || array->size() > 16)
                    error(fields, p + ".fields", "an array of at most 16 tables");
                else
                    for (const auto &field : *array) {
                        const auto *item = field.as_table();
                        if (item == nullptr) {
                            error(&field, p + ".fields", "field tables");
                            continue;
                        }
                        keys(*item, p + ".fields", {"tag", "value", "value_env", "sensitive"});
                        LogonField extra;
                        extra.tag = static_cast<std::uint32_t>(
                            integer(*item, "tag", p + ".fields", 0, 1, 999'999'999));
                        extra.sensitive = boolean(*item, "sensitive", p + ".fields", false);
                        if (item->contains("value_env")) {
                            extra.sensitive = true;
                            const auto variable = string(*item, "value_env", p + ".fields");
                            const auto *value = variable.find('\0') == std::string::npos
                                                    ? std::getenv(variable.c_str())
                                                    : nullptr;
                            if (value == nullptr || std::char_traits<char>::length(value) > 4096)
                                error(item->get("value_env"), p + ".fields.secret",
                                      "an explicitly set environment variable of at most 4096 bytes");
                            else
                                extra.value = value;
                            if (item->contains("value"))
                                error(item->get("value"), p + ".fields.secret",
                                      "exactly one of value or value_env");
                        } else
                            extra.value =
                                string(*item, "value", extra.sensitive ? p + ".fields.secret" : p + ".fields",
                                       {}, true);
                        result.logon.extra_fields.push_back(std::move(extra));
                    }
            }
        }
        if (const auto *auth = table(session, "authentication", prefix)) {
            const auto p = std::string{prefix} + ".authentication";
            keys(*auth, p, {"method", "username", "password_env", "require_tls"});
            const auto method = string(*auth, "method", p, "none");
            if (method == "none")
                result.authentication.method = AuthenticationMethod::none;
            else if (method == "credentials")
                result.authentication.method = AuthenticationMethod::credentials;
            else
                error(auth->get("method"), p + ".method", "none or credentials (callbacks use the C++ API)");
            result.authentication.username = string(*auth, "username", p);
            result.authentication.password = password(*auth, p);
            result.authentication.require_tls = boolean(*auth, "require_tls", p, true);
            if (!acceptor)
                error(auth, p, "authentication policy on an acceptor session");
        }
        if (const auto *schedule = table(session, "schedule", prefix)) {
            const auto p = std::string{prefix} + ".schedule";
            keys(*schedule, p, {"enabled", "weekdays", "start_utc", "end_utc"});
            result.schedule.enabled = boolean(*schedule, "enabled", p, true);
            if (const auto *weekdays = schedule->get("weekdays")) {
                const auto *array = weekdays->as_array();
                if (array == nullptr)
                    error(weekdays, p + ".weekdays", "an array of weekday integers, Sunday=0..Saturday=6");
                else {
                    result.schedule.weekdays = 0;
                    for (const auto &day : *array) {
                        const auto value = day.value<std::int64_t>();
                        if (!value || *value < 0 || *value > 6)
                            error(&day, p + ".weekdays", "integer 0..6");
                        else
                            result.schedule.weekdays |= static_cast<std::uint8_t>(1U << *value);
                    }
                }
            }
            const auto parse_time = [&](std::string text, bool end) -> std::uint32_t {
                if (text == "24:00:00" && end)
                    return 86'400;
                if (text.size() != 8 || text[2] != ':' || text[5] != ':') {
                    error(schedule, p, "UTC HH:MM:SS");
                    return end ? 86'400 : 0;
                }
                unsigned parts[3]{};
                for (unsigned i = 0; i < 3; ++i) {
                    const auto *first = text.data() + i * 3;
                    const auto converted = std::from_chars(first, first + 2, parts[i]);
                    if (converted.ec != std::errc{} || converted.ptr != first + 2) {
                        error(schedule, p, "UTC HH:MM:SS");
                        return end ? 86'400 : 0;
                    }
                }
                if (parts[0] > 23 || parts[1] > 59 || parts[2] > 59)
                    error(schedule, p, "valid UTC clock time");
                return parts[0] * 3600 + parts[1] * 60 + parts[2];
            };
            result.schedule.start_second_utc =
                parse_time(string(*schedule, "start_utc", p, "00:00:00"), false);
            result.schedule.end_second_utc = parse_time(string(*schedule, "end_utc", p, "24:00:00"), true);
        }
        if (const auto *flow = table(session, "flow_control", prefix)) {
            const auto p = std::string{prefix} + ".flow_control";
            keys(*flow, p,
                 {"max_pending_messages", "max_pending_bytes", "max_pending_inbound_messages",
                  "max_pending_inbound_bytes", "max_pending_age_ms", "max_delivery_age_ms",
                  "receive_pause_timeout_ms", "write_stall_ms"});
            FlowControlConfig config;
            config.max_pending_messages =
                integer(*flow, "max_pending_messages", p, document.engine.max_pending_messages, 9, 65'536);
            config.max_pending_bytes = integer(*flow, "max_pending_bytes", p,
                                               document.engine.max_pending_bytes, 128, 1024U * 1024U * 1024U);
            config.max_pending_inbound_messages =
                integer(*flow, "max_pending_inbound_messages", p,
                        document.engine.max_pending_inbound_messages, 1, 65'536);
            config.max_pending_inbound_bytes =
                integer(*flow, "max_pending_inbound_bytes", p, document.engine.max_pending_inbound_bytes, 128,
                        1024U * 1024U * 1024U);
            config.max_pending_age_ms = static_cast<std::uint32_t>(
                integer(*flow, "max_pending_age_ms", p, document.engine.max_pending_age_ms, 0, 86'400'000));
            config.max_delivery_age_ms =
                static_cast<std::uint32_t>(integer(*flow, "max_delivery_age_ms", p, 30'000, 1, 86'400'000));
            config.receive_pause_timeout_ms = static_cast<std::uint32_t>(
                integer(*flow, "receive_pause_timeout_ms", p, 30'000, 1, 86'400'000));
            config.write_stall_ms = static_cast<std::uint32_t>(
                integer(*flow, "write_stall_ms", p, document.engine.write_stall_ms, 1, 86'400'000));
            result.flow_control = config;
        }
        return result;
    }

    void load(const toml::table &root) {
        keys(root, "", {"version", "engine", "sessions", "listeners"});
        document.version = static_cast<std::uint32_t>(integer(root, "version", "", 0, 1, 1));
        if (root.get("version") == nullptr)
            error(nullptr, "version", "configuration version 1");
        if (const auto *engine = table(root, "engine", "")) {
            keys(*engine, "engine",
                 {"dispatch",
                  "schema",
                  "dictionary",
                  "dictionaries",
                  "storage",
                  "heartbeat_ms",
                  "connect_timeout_ms",
                  "reconnect_delay_ms",
                  "logon_timeout_ms",
                  "write_stall_ms",
                  "logout_timeout_ms",
                  "max_pending_age_ms",
                  "max_frame_bytes",
                  "max_sessions",
                  "max_pending_connections",
                  "max_pending_messages",
                  "max_pending_bytes",
                  "max_pending_inbound_messages",
                  "max_pending_inbound_bytes", "max_pending_sender_messages", "max_pending_sender_bytes",
                  "sender_receipt_capacity", "max_producer_lanes_per_session", "diagnostic_capacity", "queued",
                  "io_backend"});
            const auto dispatch = string(*engine, "dispatch", "engine", "inline");
            if (dispatch == "queued")
                document.dispatch = ConfigDispatch::queued_callbacks;
            else if (dispatch != "inline")
                error(engine->get("dispatch"), "engine.dispatch", "inline or queued");
            const auto backend = string(*engine, "io_backend", "engine", "epoll");
            if (backend == "epoll")
                document.engine.io_backend.kind = IoBackendKind::epoll;
            else if (backend == "busy_poll")
                document.engine.io_backend.kind = IoBackendKind::busy_poll;
            else if (backend == "io_uring") {
                if (!io_uring_available())
                    error(engine->get("io_backend"), "engine.io_backend", "io_uring support was not enabled at build time");
                document.engine.io_backend.kind = IoBackendKind::io_uring;
            } else {
                error(engine->get("io_backend"), "engine.io_backend", "epoll, busy_poll or io_uring");
            }
            document.schema = string(*engine, "schema", "engine", "none");
            if (document.schema != "none" && document.schema != "fix44")
                error(engine->get("schema"), "engine.schema", "none or fix44");
            if (engine->contains("dictionary"))
                document.dictionaries.push_back(path(string(*engine, "dictionary", "engine")));
            if (const auto *files = engine->get("dictionaries")) {
                const auto *array = files->as_array();
                if (array == nullptr || array->size() > 32)
                    error(files, "engine.dictionaries", "an array of at most 32 XML paths");
                else
                    for (const auto &file : *array) {
                        if (const auto value = file.value<std::string>())
                            document.dictionaries.push_back(path(*value));
                        else
                            error(&file, "engine.dictionaries", "XML file paths");
                    }
            }
            auto &c = document.engine;
            const auto millis = [&](std::string_view name, std::uint32_t fallback, bool zero = false) {
                return static_cast<std::uint32_t>(
                    integer(*engine, name, "engine", fallback, zero ? 0 : 1, 86'400'000));
            };
            c.heartbeat_ms = millis("heartbeat_ms", c.heartbeat_ms);
            c.connect_timeout_ms = millis("connect_timeout_ms", c.connect_timeout_ms);
            c.reconnect_delay_ms = millis("reconnect_delay_ms", c.reconnect_delay_ms);
            c.logon_timeout_ms = millis("logon_timeout_ms", c.logon_timeout_ms);
            c.write_stall_ms = millis("write_stall_ms", c.write_stall_ms);
            c.logout_timeout_ms = millis("logout_timeout_ms", c.logout_timeout_ms);
            c.max_pending_age_ms = millis("max_pending_age_ms", c.max_pending_age_ms, true);
            c.max_frame_bytes =
                integer(*engine, "max_frame_bytes", "engine", c.max_frame_bytes, 128, default_max_frame_bytes);
            c.max_sessions = integer(*engine, "max_sessions", "engine", c.max_sessions, 1, 65'536);
            c.max_pending_connections =
                integer(*engine, "max_pending_connections", "engine", c.max_pending_connections, 1, 65'536);
            c.max_pending_messages =
                integer(*engine, "max_pending_messages", "engine", c.max_pending_messages, 9, 65'536);
            c.max_pending_bytes = integer(*engine, "max_pending_bytes", "engine", c.max_pending_bytes, 128,
                                          1024U * 1024U * 1024U);
            c.max_pending_inbound_messages = integer(*engine, "max_pending_inbound_messages", "engine",
                                                     c.max_pending_inbound_messages, 1, 65'536);
            c.max_pending_inbound_bytes = integer(*engine, "max_pending_inbound_bytes", "engine",
                                                  c.max_pending_inbound_bytes, 128, 1024U * 1024U * 1024U);
            c.max_pending_sender_messages = integer(*engine, "max_pending_sender_messages", "engine", c.max_pending_sender_messages, 1, 65'536);
            c.max_pending_sender_bytes = integer(*engine, "max_pending_sender_bytes", "engine", c.max_pending_sender_bytes, 128, 1024U*1024U*1024U);
            c.sender_receipt_capacity = integer(*engine, "sender_receipt_capacity", "engine", c.sender_receipt_capacity, 1, 1U*1024U*1024U);
            c.max_producer_lanes_per_session = integer(*engine, "max_producer_lanes_per_session", "engine", c.max_producer_lanes_per_session, 0, 64);
            c.diagnostic_capacity = integer(*engine, "diagnostic_capacity", "engine", c.diagnostic_capacity, 1, 65'536);
            if (const auto* queued = table(*engine, "queued", "engine")) {
                keys(*queued, "engine.queued", {"worker_threads", "max_pending_callbacks", "max_pending_callback_bytes", "delivery_strategy"});
                c.queued.worker_threads = integer(*queued, "worker_threads", "engine.queued", c.queued.worker_threads, 1, 256);
                c.queued.max_pending_callbacks = integer(*queued, "max_pending_callbacks", "engine.queued", c.queued.max_pending_callbacks, 1, 65'536);
                c.queued.max_pending_callback_bytes = integer(*queued, "max_pending_callback_bytes", "engine.queued", c.queued.max_pending_callback_bytes, 128, 1024U*1024U*1024U);
                const auto strategy = string(*queued, "delivery_strategy", "engine.queued", "descriptor_ring");
                if (strategy == "journal_cursor") c.queued.delivery_strategy = QueuedDeliveryStrategy::journal_cursor;
                else if (strategy == "descriptor_ring") c.queued.delivery_strategy = QueuedDeliveryStrategy::descriptor_ring;
                else error(queued->get("delivery_strategy"), "engine.queued.delivery_strategy", "journal_cursor or descriptor_ring");
            }
            if (const auto *storage = table(*engine, "storage", "engine")) {
                keys(*storage, "engine.storage",
                     {"directory", "confirmation", "capacity_bytes", "max_records", "segment_bytes",
                      "max_segments", "retain_outbound_messages"});
                c.storage.directory = path(string(*storage, "directory", "engine.storage", {}, true));
                if (string(*storage, "confirmation", "engine.storage", "appended") != "appended")
                    error(storage->get("confirmation"), "engine.storage.confirmation", "appended");
                c.storage.journal_capacity_bytes =
                    integer(*storage, "capacity_bytes", "engine.storage", c.storage.journal_capacity_bytes,
                            8192, 64ULL * 1024ULL * 1024ULL * 1024ULL);
                c.storage.journal_max_records = integer(*storage, "max_records", "engine.storage",
                                                        c.storage.journal_max_records, 1, 4U * 1024U * 1024U);
                c.storage.segment_bytes = integer(*storage, "segment_bytes", "engine.storage",
                                                  c.storage.segment_bytes, 0, 1024U * 1024U * 1024U);
                c.storage.max_segments =
                    integer(*storage, "max_segments", "engine.storage", c.storage.max_segments, 0, 65'535);
                c.storage.retain_outbound_messages =
                    integer(*storage, "retain_outbound_messages", "engine.storage",
                            c.storage.retain_outbound_messages, 0, 4U * 1024U * 1024U);
            }
        }
        if (const auto *node = root.get("listeners")) {
            const auto *array = node->as_array();
            if (array == nullptr || array->size() > 1024)
                error(node, "listeners", "an array of at most 1024 tables");
            else
                for (std::size_t i = 0; i < array->size(); ++i) {
                    const auto *listener = array->get(i)->as_table();
                    const auto p = "listeners[" + std::to_string(i) + "]";
                    if (!listener) {
                        error(array->get(i), p, "a listener table");
                        continue;
                    }
                    keys(*listener, p, {"name", "endpoint", "backlog", "tls"});
                    ListenerConfig value;
                    value.name = string(*listener, "name", p, {}, true);
                    value.endpoint = endpoint(*listener, "endpoint", p, true);
                    value.backlog = integer(*listener, "backlog", p, 128, 1, 65'535);
                    value.tls = tls(*listener, p);
                    document.listeners.push_back(std::move(value));
                }
        }
        if (const auto *node = root.get("sessions")) {
            const auto *array = node->as_array();
            if (array == nullptr || array->size() > 65'536)
                error(node, "sessions", "an array of at most 65536 tables");
            else
                for (std::size_t i = 0; i < array->size(); ++i) {
                    const auto *session = array->get(i)->as_table();
                    const auto p = "sessions[" + std::to_string(i) + "]";
                    if (!session) {
                        error(array->get(i), p, "a session table");
                        continue;
                    }
                    keys(*session, p,
                         {"name",
                          "role",
                          "sender_comp_id",
                          "target_comp_id",
                          "local_comp_id",
                          "remote_comp_id",
                          "listener",
                          "endpoint",
                          "transport",
                          "qualifier",
                          "default_appl_ver_id",
                          "allow_sequence_reset",
                          "check_sending_time",
                          "max_sending_time_skew_ms",
                          "timestamp_precision",
                          "verify_checksum",
                          "emit_last_processed_seq",
                          "logon",
                          "authentication",
                          "schedule",
                          "flow_control",
                          "tls"});
                    const auto role = string(*session, "role", p, {}, true);
                    if (role == "initiator") {
                        InitiatorConfig value;
                        value.name = string(*session, "name", p, {}, true);
                        value.sender_comp_id = string(*session, "sender_comp_id", p, {}, true);
                        value.target_comp_id = string(*session, "target_comp_id", p, {}, true);
                        value.endpoint = endpoint(*session, "endpoint", p, false);
                        value.settings = settings(*session, p, false);
                        value.tls = tls(*session, p);
                        document.initiators.push_back(std::move(value));
                    } else if (role == "acceptor") {
                        AcceptorConfig value;
                        value.name = string(*session, "name", p, {}, true);
                        value.local_comp_id = string(*session, "local_comp_id", p, {}, true);
                        value.remote_comp_id = string(*session, "remote_comp_id", p, {}, true);
                        value.listener = string(*session, "listener", p, {}, true);
                        value.settings = settings(*session, p, true);
                        if (session->contains("tls"))
                            error(session->get("tls"), p + ".tls",
                                  "TLS configured on the referenced listener");
                        document.acceptors.push_back(std::move(value));
                    } else
                        error(session->get("role"), p + ".role", "initiator or acceptor");
                }
        }
    }
};
} // namespace

ConfigLoadResult load_config(const std::filesystem::path &path) noexcept {
    ConfigLoadResult result;
    try {
        Loader loader;
        loader.source = std::filesystem::absolute(path).lexically_normal();
        loader.base = loader.source.parent_path();
        std::error_code error;
        const auto size = std::filesystem::file_size(loader.source, error);
        if (error || size > max_config_bytes) {
            loader.error(nullptr, "config", "a readable TOML file at most 1 MiB",
                         "correct --config or reduce the file size");
            result.diagnostics = std::move(loader.diagnostics);
            return result;
        }
        std::ifstream input(loader.source, std::ios::binary);
        std::string bytes(max_config_bytes + 1, '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const auto count = input.gcount();
        if (count < 0 || static_cast<std::size_t>(count) > max_config_bytes ||
            (!input.eof() && input.fail())) {
            loader.error(nullptr, "config", "a readable TOML file at most 1 MiB");
            result.diagnostics = std::move(loader.diagnostics);
            return result;
        }
        bytes.resize(static_cast<std::size_t>(count));
        auto parsed = toml::parse(bytes, loader.source.string());
        loader.load(parsed);
        auto semantic = validate_config(loader.document);
        for (auto &item : semantic) {
            if (loader.diagnostics.size() == max_config_diagnostics) break;
            item.source = loader.source;
            loader.diagnostics.push_back(std::move(item));
        }
        if (loader.diagnostics.empty())
            result.document = std::move(loader.document);
        result.diagnostics = std::move(loader.diagnostics);
    } catch (const toml::parse_error &error) {
        ConfigDiagnostic item;
        item.source = path;
        item.line = error.source().begin.line;
        item.column = error.source().begin.column;
        item.field = "config";
        item.received = "<invalid TOML>";
        item.expected = std::string(error.description());
        item.fix = "correct the TOML syntax at the reported location";
        result.diagnostics.push_back(std::move(item));
    } catch (...) {
        ConfigDiagnostic item;
        item.source = path;
        item.field = "config";
        item.expected = "readable bounded configuration";
        item.fix = "check path permissions and available memory";
        result.diagnostics.push_back(std::move(item));
    }
    return result;
}

std::vector<ConfigDiagnostic> validate_config(const ConfigDocument &document) noexcept {
    std::vector<ConfigDiagnostic> errors;
    try {
        const auto add = [&](Status status, std::string field, std::string expected) {
            if (errors.size() == max_config_diagnostics) return;
            ConfigDiagnostic item;
            item.status = status;
            item.field = std::move(field);
            item.received = "<configuration>";
            item.expected = std::move(expected);
            item.fix = "correct the named setting before startup";
            errors.push_back(std::move(item));
        };
        if (document.engine.storage.directory.empty())
            add({ErrorCode::invalid_config}, "engine.storage.directory",
                "an explicit writable state directory");
        const auto validate_with = [&]<class DispatchPolicy>() {
            auto created = Engine<FlatSchema, DispatchPolicy>::create(document.engine);
            if (!created.ok()) {
                add(created.status(), "engine", std::string(created.status().message()));
                return;
            }
            auto engine = std::move(created).value();
            (void)engine.on_message([](auto &, const auto &) { return Status::success(); });
            for (const auto &listener : document.listeners) {
                const auto added = engine.add_listener(listener);
                if (!added.ok())
                    add(added.status(), "listeners." + listener.name, std::string(added.status().message()));
            }
            for (const auto &session : document.initiators) {
                const auto added = engine.add_initiator(session);
                if (!added.ok())
                    add(added.status(), "sessions." + session.name, std::string(added.status().message()));
            }
            for (const auto &session : document.acceptors) {
                const auto added = engine.add_acceptor(session);
                if (!added.ok())
                    add(added.status(), "sessions." + session.name, std::string(added.status().message()));
            }
            const auto valid = engine.validate();
            if (!valid.ok())
                add(valid, "engine", std::string(valid.message()));
        };
        if (document.dispatch == ConfigDispatch::queued_callbacks) {
            validate_with.template operator()<Queued>();
        } else {
            validate_with.template operator()<Inline>();
        }
    } catch (...) {
        ConfigDiagnostic item;
        item.status = {ErrorCode::capacity_exceeded};
        item.field = "config";
        item.expected = "configuration within resource bounds";
        errors.push_back(std::move(item));
    }
    return errors;
}

std::string diagnostics_json(const std::vector<ConfigDiagnostic> &diagnostics) {
    std::string result{"["};
    for (std::size_t i = 0; i < diagnostics.size(); ++i) {
        if (i != 0)
            result.push_back(',');
        const auto &d = diagnostics[i];
        result += "{\"code\":" + std::to_string(static_cast<unsigned>(d.status.code)) +
                  ",\"message\":" + json_string(d.status.message());
        result += ",\"source\":" + json_string(d.source.string()) + ",\"line\":" + std::to_string(d.line) +
                  ",\"column\":" + std::to_string(d.column);
        result += ",\"field\":" + json_string(d.field) + ",\"received\":" + json_string(d.received) +
                  ",\"expected\":" + json_string(d.expected) + ",\"fix\":" + json_string(d.fix) + "}";
    }
    result.push_back(']');
    return result;
}

} // namespace nimblefix
