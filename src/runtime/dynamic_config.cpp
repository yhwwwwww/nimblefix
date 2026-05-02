#include "nimblefix/runtime/dynamic_config.h"

#include <sstream>
#include <string_view>
#include <unordered_map>

namespace nimble::runtime {

namespace {

auto
BoolText(bool value) -> std::string_view
{
  return value ? "true" : "false";
}

auto
AppendReason(std::string& target, std::string_view reason) -> void
{
  if (!target.empty()) {
    target.append("; ");
  }
  target.append(reason);
}

auto
AddEngineChange(ConfigDelta& delta, std::string name, std::string description, bool requires_restart = false) -> void
{
  delta.changes.push_back(ConfigChange{
    .kind = ConfigChangeKind::kEngineFieldChanged,
    .session_id = 0U,
    .name = std::move(name),
    .description = std::move(description),
  });
  if (requires_restart) {
    delta.requires_restart = true;
    AppendReason(delta.restart_reason, delta.changes.back().description);
  }
}

template<typename Value>
auto
AppendFieldIfChanged(const Value& current,
                     const Value& proposed,
                     std::string_view field_name,
                     std::vector<std::string>& fields) -> void
{
  if (!(current == proposed)) {
    fields.emplace_back(field_name);
  }
}

auto
SessionKeyEquals(const session::SessionKey& left, const session::SessionKey& right) -> bool
{
  return left.begin_string == right.begin_string && left.sender_comp_id == right.sender_comp_id &&
         left.target_comp_id == right.target_comp_id && left.sender_location_id == right.sender_location_id &&
         left.target_location_id == right.target_location_id && left.session_qualifier == right.session_qualifier;
}

auto
SessionConfigEquals(const session::SessionConfig& left, const session::SessionConfig& right) -> bool
{
  return left.session_id == right.session_id && SessionKeyEquals(left.key, right.key) &&
         left.profile_id == right.profile_id && left.default_appl_ver_id == right.default_appl_ver_id &&
         left.heartbeat_interval_seconds == right.heartbeat_interval_seconds && left.is_initiator == right.is_initiator;
}

auto
TransportProfileEquals(const session::TransportSessionProfile& left, const session::TransportSessionProfile& right)
  -> bool
{
  return left.version == right.version && left.begin_string == right.begin_string &&
         left.requires_default_appl_ver_id == right.requires_default_appl_ver_id &&
         left.supports_reset_on_logon == right.supports_reset_on_logon &&
         left.default_heartbeat_interval_seconds == right.default_heartbeat_interval_seconds &&
         left.supports_next_expected_msg_seq_num == right.supports_next_expected_msg_seq_num &&
         left.supports_msg_seq_num_reset == right.supports_msg_seq_num_reset &&
         left.transport_and_app_version_decoupled == right.transport_and_app_version_decoupled;
}

auto
ValidationPolicyEquals(const session::ValidationPolicy& left, const session::ValidationPolicy& right) -> bool
{
  return left.mode == right.mode && left.enforce_comp_ids == right.enforce_comp_ids &&
         left.require_default_appl_ver_id_on_logon == right.require_default_appl_ver_id_on_logon &&
         left.require_orig_sending_time_on_poss_dup == right.require_orig_sending_time_on_poss_dup &&
         left.reject_on_stale_msg_seq_num == right.reject_on_stale_msg_seq_num &&
         left.require_known_app_message_type == right.require_known_app_message_type &&
         left.require_required_fields_on_app_messages == right.require_required_fields_on_app_messages &&
         left.reject_unknown_fields == right.reject_unknown_fields &&
         left.reject_duplicate_fields == right.reject_duplicate_fields &&
         left.reject_tag_without_value == right.reject_tag_without_value &&
         left.reject_incorrect_data_format == right.reject_incorrect_data_format &&
         left.reject_fields_out_of_order == right.reject_fields_out_of_order &&
         left.reject_invalid_group_structure == right.reject_invalid_group_structure &&
         left.verify_checksum == right.verify_checksum;
}

auto
SessionTimeEquals(const std::optional<SessionTimeOfDay>& left, const std::optional<SessionTimeOfDay>& right) -> bool
{
  if (left.has_value() != right.has_value()) {
    return false;
  }
  if (!left.has_value()) {
    return true;
  }
  return left->hour == right->hour && left->minute == right->minute && left->second == right->second;
}

auto
SessionScheduleEquals(const SessionScheduleConfig& left, const SessionScheduleConfig& right) -> bool
{
  return left.use_local_time == right.use_local_time && left.non_stop_session == right.non_stop_session &&
         SessionTimeEquals(left.start_time, right.start_time) && SessionTimeEquals(left.end_time, right.end_time) &&
         left.start_day == right.start_day && left.end_day == right.end_day &&
         SessionTimeEquals(left.logon_time, right.logon_time) &&
         SessionTimeEquals(left.logout_time, right.logout_time) && left.logon_day == right.logon_day &&
         left.logout_day == right.logout_day;
}

auto
TlsClientEquals(const TlsClientConfig& left, const TlsClientConfig& right) -> bool
{
  return left.enabled == right.enabled && left.server_name == right.server_name &&
         left.expected_peer_name == right.expected_peer_name && left.ca_file == right.ca_file &&
         left.ca_path == right.ca_path && left.certificate_chain_file == right.certificate_chain_file &&
         left.private_key_file == right.private_key_file && left.verify_peer == right.verify_peer &&
         left.min_version == right.min_version && left.max_version == right.max_version &&
         left.cipher_list == right.cipher_list && left.cipher_suites == right.cipher_suites &&
         left.session_resumption == right.session_resumption;
}

auto
TlsServerEquals(const TlsServerConfig& left, const TlsServerConfig& right) -> bool
{
  return left.enabled == right.enabled && left.certificate_chain_file == right.certificate_chain_file &&
         left.private_key_file == right.private_key_file && left.ca_file == right.ca_file &&
         left.ca_path == right.ca_path && left.verify_peer == right.verify_peer &&
         left.require_client_certificate == right.require_client_certificate && left.min_version == right.min_version &&
         left.max_version == right.max_version && left.cipher_list == right.cipher_list &&
         left.cipher_suites == right.cipher_suites && left.session_cache == right.session_cache;
}

auto
DayCutEquals(const session::DayCutConfig& left, const session::DayCutConfig& right) -> bool
{
  return left.mode == right.mode && left.reset_hour == right.reset_hour && left.reset_minute == right.reset_minute &&
         left.utc_offset_seconds == right.utc_offset_seconds;
}

auto
CounterpartyEquals(const CounterpartyConfig& left, const CounterpartyConfig& right) -> bool
{
  return left.name == right.name && SessionConfigEquals(left.session, right.session) &&
         TransportProfileEquals(left.transport_profile, right.transport_profile) &&
         left.store_path == right.store_path && left.default_appl_ver_id == right.default_appl_ver_id &&
         left.supported_app_msg_types == right.supported_app_msg_types &&
         left.contract_service_subsets == right.contract_service_subsets &&
         left.sending_time_threshold_seconds == right.sending_time_threshold_seconds &&
         left.application_messages_available == right.application_messages_available &&
         left.store_mode == right.store_mode && left.durable_flush_threshold == right.durable_flush_threshold &&
         left.durable_rollover_mode == right.durable_rollover_mode &&
         left.durable_archive_limit == right.durable_archive_limit &&
         left.durable_local_utc_offset_seconds == right.durable_local_utc_offset_seconds &&
         left.durable_use_system_timezone == right.durable_use_system_timezone &&
         left.recovery_mode == right.recovery_mode && left.dispatch_mode == right.dispatch_mode &&
         ValidationPolicyEquals(left.validation_policy, right.validation_policy) &&
         left.reset_seq_num_on_logon == right.reset_seq_num_on_logon &&
         left.reset_seq_num_on_logout == right.reset_seq_num_on_logout &&
         left.reset_seq_num_on_disconnect == right.reset_seq_num_on_disconnect &&
         left.refresh_on_logon == right.refresh_on_logon &&
         left.send_next_expected_msg_seq_num == right.send_next_expected_msg_seq_num &&
         SessionScheduleEquals(left.session_schedule, right.session_schedule) &&
         TlsClientEquals(left.tls_client, right.tls_client) &&
         left.acceptor_transport_security == right.acceptor_transport_security &&
         left.reconnect_enabled == right.reconnect_enabled && left.reconnect_initial_ms == right.reconnect_initial_ms &&
         left.reconnect_max_ms == right.reconnect_max_ms && left.reconnect_max_retries == right.reconnect_max_retries &&
         DayCutEquals(left.day_cut, right.day_cut);
}

auto
ListenerEquals(const ListenerConfig& left, const ListenerConfig& right) -> bool
{
  return left.name == right.name && left.host == right.host && left.port == right.port &&
         left.worker_hint == right.worker_hint && TlsServerEquals(left.tls_server, right.tls_server);
}

auto
JoinFields(const std::vector<std::string>& fields) -> std::string
{
  std::string result;
  for (const auto& field : fields) {
    if (!result.empty()) {
      result.append(", ");
    }
    result.append(field);
  }
  return result;
}

auto
CounterpartyChangedFields(const CounterpartyConfig& current, const CounterpartyConfig& proposed)
  -> std::vector<std::string>
{
  std::vector<std::string> fields;

  AppendFieldIfChanged(current.name, proposed.name, "name", fields);
  AppendFieldIfChanged(current.session.session_id, proposed.session.session_id, "session.session_id", fields);
  if (!SessionKeyEquals(current.session.key, proposed.session.key)) {
    fields.emplace_back("session.key");
  }
  AppendFieldIfChanged(current.session.profile_id, proposed.session.profile_id, "session.profile_id", fields);
  AppendFieldIfChanged(
    current.session.default_appl_ver_id, proposed.session.default_appl_ver_id, "session.default_appl_ver_id", fields);
  AppendFieldIfChanged(current.session.heartbeat_interval_seconds,
                       proposed.session.heartbeat_interval_seconds,
                       "session.heartbeat_interval_seconds",
                       fields);
  AppendFieldIfChanged(current.session.is_initiator, proposed.session.is_initiator, "session.is_initiator", fields);
  if (!TransportProfileEquals(current.transport_profile, proposed.transport_profile)) {
    fields.emplace_back("transport_profile");
  }
  AppendFieldIfChanged(current.store_path, proposed.store_path, "store_path", fields);
  AppendFieldIfChanged(current.default_appl_ver_id, proposed.default_appl_ver_id, "default_appl_ver_id", fields);
  AppendFieldIfChanged(
    current.supported_app_msg_types, proposed.supported_app_msg_types, "supported_app_msg_types", fields);
  AppendFieldIfChanged(
    current.contract_service_subsets, proposed.contract_service_subsets, "contract_service_subsets", fields);
  AppendFieldIfChanged(current.sending_time_threshold_seconds,
                       proposed.sending_time_threshold_seconds,
                       "sending_time_threshold_seconds",
                       fields);
  AppendFieldIfChanged(current.application_messages_available,
                       proposed.application_messages_available,
                       "application_messages_available",
                       fields);
  AppendFieldIfChanged(current.store_mode, proposed.store_mode, "store_mode", fields);
  AppendFieldIfChanged(
    current.durable_flush_threshold, proposed.durable_flush_threshold, "durable_flush_threshold", fields);
  AppendFieldIfChanged(current.durable_rollover_mode, proposed.durable_rollover_mode, "durable_rollover_mode", fields);
  AppendFieldIfChanged(current.durable_archive_limit, proposed.durable_archive_limit, "durable_archive_limit", fields);
  AppendFieldIfChanged(current.durable_local_utc_offset_seconds,
                       proposed.durable_local_utc_offset_seconds,
                       "durable_local_utc_offset_seconds",
                       fields);
  AppendFieldIfChanged(
    current.durable_use_system_timezone, proposed.durable_use_system_timezone, "durable_use_system_timezone", fields);
  AppendFieldIfChanged(current.recovery_mode, proposed.recovery_mode, "recovery_mode", fields);
  AppendFieldIfChanged(current.dispatch_mode, proposed.dispatch_mode, "dispatch_mode", fields);
  if (!ValidationPolicyEquals(current.validation_policy, proposed.validation_policy)) {
    fields.emplace_back("validation_policy");
  }
  AppendFieldIfChanged(
    current.reset_seq_num_on_logon, proposed.reset_seq_num_on_logon, "reset_seq_num_on_logon", fields);
  AppendFieldIfChanged(
    current.reset_seq_num_on_logout, proposed.reset_seq_num_on_logout, "reset_seq_num_on_logout", fields);
  AppendFieldIfChanged(
    current.reset_seq_num_on_disconnect, proposed.reset_seq_num_on_disconnect, "reset_seq_num_on_disconnect", fields);
  AppendFieldIfChanged(current.refresh_on_logon, proposed.refresh_on_logon, "refresh_on_logon", fields);
  AppendFieldIfChanged(current.send_next_expected_msg_seq_num,
                       proposed.send_next_expected_msg_seq_num,
                       "send_next_expected_msg_seq_num",
                       fields);
  if (!SessionScheduleEquals(current.session_schedule, proposed.session_schedule)) {
    fields.emplace_back("session_schedule");
  }
  if (!TlsClientEquals(current.tls_client, proposed.tls_client)) {
    fields.emplace_back("tls_client");
  }
  AppendFieldIfChanged(
    current.acceptor_transport_security, proposed.acceptor_transport_security, "acceptor_transport_security", fields);
  AppendFieldIfChanged(current.reconnect_enabled, proposed.reconnect_enabled, "reconnect_enabled", fields);
  AppendFieldIfChanged(current.reconnect_initial_ms, proposed.reconnect_initial_ms, "reconnect_initial_ms", fields);
  AppendFieldIfChanged(current.reconnect_max_ms, proposed.reconnect_max_ms, "reconnect_max_ms", fields);
  AppendFieldIfChanged(current.reconnect_max_retries, proposed.reconnect_max_retries, "reconnect_max_retries", fields);
  if (!DayCutEquals(current.day_cut, proposed.day_cut)) {
    fields.emplace_back("day_cut");
  }

  return fields;
}

auto
CounterpartyRequiresRemoveAddFields(const CounterpartyConfig& current, const CounterpartyConfig& proposed)
  -> std::vector<std::string>
{
  std::vector<std::string> fields;
  AppendFieldIfChanged(current.store_mode, proposed.store_mode, "store_mode", fields);
  AppendFieldIfChanged(current.store_path, proposed.store_path, "store_path", fields);
  AppendFieldIfChanged(current.recovery_mode, proposed.recovery_mode, "recovery_mode", fields);
  AppendFieldIfChanged(current.dispatch_mode, proposed.dispatch_mode, "dispatch_mode", fields);
  AppendFieldIfChanged(current.session.profile_id, proposed.session.profile_id, "session.profile_id", fields);
  if (!SessionKeyEquals(current.session.key, proposed.session.key)) {
    fields.emplace_back("session.key");
  }
  if (!TransportProfileEquals(current.transport_profile, proposed.transport_profile)) {
    fields.emplace_back("transport_profile");
  }
  if (!ValidationPolicyEquals(current.validation_policy, proposed.validation_policy)) {
    fields.emplace_back("validation_policy");
  }
  return fields;
}

template<typename Config>
auto
MapBySessionId(const std::vector<Config>& counterparties) -> std::unordered_map<std::uint64_t, const Config*>
{
  std::unordered_map<std::uint64_t, const Config*> result;
  result.reserve(counterparties.size());
  for (const auto& counterparty : counterparties) {
    result.emplace(counterparty.session.session_id, &counterparty);
  }
  return result;
}

auto
MapListenersByName(const std::vector<ListenerConfig>& listeners)
  -> std::unordered_map<std::string_view, const ListenerConfig*>
{
  std::unordered_map<std::string_view, const ListenerConfig*> result;
  result.reserve(listeners.size());
  for (const auto& listener : listeners) {
    result.emplace(listener.name, &listener);
  }
  return result;
}

} // namespace

auto
ComputeConfigDelta(const EngineConfig& current, const EngineConfig& proposed) -> ConfigDelta
{
  ConfigDelta delta;

  if (current.worker_count != proposed.worker_count) {
    AddEngineChange(delta, "worker_count", "worker_count changed and requires restart", true);
  }
  if (current.io_backend != proposed.io_backend) {
    AddEngineChange(delta, "io_backend", "io_backend changed and requires restart", true);
  }
  if (current.poll_mode != proposed.poll_mode) {
    AddEngineChange(delta, "poll_mode", "poll_mode changed and requires restart", true);
  }
  if (current.queue_app_mode != proposed.queue_app_mode) {
    AddEngineChange(delta, "queue_app_mode", "queue_app_mode changed and requires restart", true);
  }
  if (current.trace_mode != proposed.trace_mode || current.trace_capacity != proposed.trace_capacity) {
    AddEngineChange(delta, "trace", "trace_mode or trace_capacity changed");
  }
  if (current.enable_metrics != proposed.enable_metrics) {
    AddEngineChange(
      delta, "enable_metrics", "enable_metrics changed to " + std::string(BoolText(proposed.enable_metrics)));
  }
  if (current.front_door_cpu != proposed.front_door_cpu) {
    AddEngineChange(
      delta, "front_door_cpu", "front_door_cpu changed; live config updated but affinity takes effect on restart");
  }
  if (current.worker_cpu_affinity != proposed.worker_cpu_affinity) {
    AddEngineChange(delta,
                    "worker_cpu_affinity",
                    "worker_cpu_affinity changed; live config updated but affinity takes effect on restart");
  }
  if (current.app_cpu_affinity != proposed.app_cpu_affinity) {
    AddEngineChange(
      delta, "app_cpu_affinity", "app_cpu_affinity changed; live config updated but affinity takes effect on restart");
  }
  if (current.accept_unknown_sessions != proposed.accept_unknown_sessions) {
    AddEngineChange(delta,
                    "accept_unknown_sessions",
                    "accept_unknown_sessions changed to " + std::string(BoolText(proposed.accept_unknown_sessions)));
  }
  if (current.profile_artifacts != proposed.profile_artifacts) {
    AddEngineChange(delta, "profile_artifacts", "profile_artifacts changed; profiles will be reloaded");
  }
  if (current.profile_dictionaries != proposed.profile_dictionaries) {
    AddEngineChange(delta, "profile_dictionaries", "profile_dictionaries changed; profiles will be reloaded");
  }
  if (current.profile_contracts != proposed.profile_contracts) {
    AddEngineChange(delta, "profile_contracts", "profile_contracts changed; contracts will be reloaded");
  }
  if (current.profile_madvise != proposed.profile_madvise) {
    AddEngineChange(delta, "profile_madvise", "profile_madvise changed; applied on next profile load");
  }
  if (current.profile_mlock != proposed.profile_mlock) {
    AddEngineChange(delta, "profile_mlock", "profile_mlock changed; applied on next profile load");
  }

  const auto current_counterparties = MapBySessionId(current.counterparties);
  const auto proposed_counterparties = MapBySessionId(proposed.counterparties);

  for (const auto& [session_id, proposed_counterparty] : proposed_counterparties) {
    const auto current_it = current_counterparties.find(session_id);
    if (current_it == current_counterparties.end()) {
      delta.changes.push_back(ConfigChange{
        .kind = ConfigChangeKind::kAddCounterparty,
        .session_id = session_id,
        .name = proposed_counterparty->name,
        .description = "counterparty added",
      });
      continue;
    }

    const auto& current_counterparty = *current_it->second;
    if (CounterpartyEquals(current_counterparty, *proposed_counterparty)) {
      continue;
    }

    const auto fields = CounterpartyChangedFields(current_counterparty, *proposed_counterparty);
    const auto remove_add_fields = CounterpartyRequiresRemoveAddFields(current_counterparty, *proposed_counterparty);
    std::string description = "counterparty modified";
    if (!fields.empty()) {
      description.append(": ");
      description.append(JoinFields(fields));
    }
    if (!remove_add_fields.empty()) {
      description.append("; requires remove+add for: ");
      description.append(JoinFields(remove_add_fields));
    }
    delta.changes.push_back(ConfigChange{
      .kind = ConfigChangeKind::kModifyCounterparty,
      .session_id = session_id,
      .name = proposed_counterparty->name,
      .description = std::move(description),
    });
  }

  for (const auto& [session_id, current_counterparty] : current_counterparties) {
    if (!proposed_counterparties.contains(session_id)) {
      delta.changes.push_back(ConfigChange{
        .kind = ConfigChangeKind::kRemoveCounterparty,
        .session_id = session_id,
        .name = current_counterparty->name,
        .description = "counterparty removed",
      });
    }
  }

  const auto current_listeners = MapListenersByName(current.listeners);
  const auto proposed_listeners = MapListenersByName(proposed.listeners);
  for (const auto& [name, proposed_listener] : proposed_listeners) {
    const auto current_it = current_listeners.find(name);
    if (current_it == current_listeners.end()) {
      delta.changes.push_back(ConfigChange{
        .kind = ConfigChangeKind::kAddListener,
        .session_id = 0U,
        .name = std::string(name),
        .description = "listener added",
      });
      continue;
    }
    if (!ListenerEquals(*current_it->second, *proposed_listener)) {
      delta.changes.push_back(ConfigChange{
        .kind = ConfigChangeKind::kModifyListener,
        .session_id = 0U,
        .name = std::string(name),
        .description = "listener modified",
      });
    }
  }
  for (const auto& [name, current_listener] : current_listeners) {
    (void)current_listener;
    if (!proposed_listeners.contains(name)) {
      delta.changes.push_back(ConfigChange{
        .kind = ConfigChangeKind::kRemoveListener,
        .session_id = 0U,
        .name = std::string(name),
        .description = "listener removed",
      });
    }
  }

  return delta;
}

auto
ApplyConfigResult::summary() const -> std::string
{
  std::ostringstream out;
  out << applied.size() << " applied, " << skipped.size() << " skipped";
  if (!skipped.empty()) {
    out << " (requires restart: ";
    for (std::size_t index = 0; index < skipped.size(); ++index) {
      if (index != 0U) {
        out << "; ";
      }
      out << skipped[index].name;
      if (!skipped[index].description.empty()) {
        out << " - " << skipped[index].description;
      }
    }
    out << ')';
  }
  return out.str();
}

} // namespace nimble::runtime
