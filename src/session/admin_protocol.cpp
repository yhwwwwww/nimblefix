#include "fastfix/session/admin_protocol.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "fastfix/message/typed_message.h"
#include "fastfix/codec/raw_passthrough.h"

namespace fastfix::session {

namespace {

constexpr std::uint64_t kNanosPerSecond = 1'000'000'000ULL;
constexpr std::uint32_t kSessionRejectRequiredTagMissing = 1U;
constexpr std::uint32_t kSessionRejectTagNotDefinedForMessage = 2U;
constexpr std::uint32_t kSessionRejectUndefinedTag = 3U;
constexpr std::uint32_t kSessionRejectValueIncorrect = 5U;
constexpr std::uint32_t kSessionRejectCompIdProblem = 9U;
constexpr std::uint32_t kSessionRejectInvalidMsgType = 11U;
constexpr std::uint32_t kSessionRejectTagAppearsMoreThanOnce = 13U;
constexpr std::uint32_t kSessionRejectIncorrectNumInGroupCount = 16U;
constexpr std::size_t kInitialEncodeBufferBytes = 1024U;

auto IsAdminMessage(std::string_view msg_type) -> bool {
    return msg_type == "0" || msg_type == "1" || msg_type == "2" || msg_type == "3" ||
           msg_type == "4" || msg_type == "5" || msg_type == "A";
}

auto MessageRecordFlagValue(store::MessageRecordFlags flag) -> std::uint16_t {
    return static_cast<std::uint16_t>(flag);
}

auto HasBoolean(const message::MessageView& view, std::uint32_t tag) -> bool {
    const auto value = view.get_boolean(tag);
    return value.has_value() && value.value();
}

auto GetInt(const message::MessageView& view, std::uint32_t tag, std::int64_t fallback) -> std::int64_t {
    const auto value = view.get_int(tag);
    return value.has_value() ? value.value() : fallback;
}

auto GetStringView(const message::MessageView& view, std::uint32_t tag) -> std::string_view {
    const auto value = view.get_string(tag);
    return value.value_or(std::string_view{});
}

auto ParseSequenceResetNewSeq(
    const message::MessageView& view,
    std::uint32_t* new_seq_num,
    std::uint32_t* reject_reason,
    std::string* text) -> bool {
    const auto value = view.get_int(36U);
    if (!value.has_value() || value.value() <= 0) {
        *reject_reason = kSessionRejectRequiredTagMissing;
        *text = "SequenceReset requires NewSeqNo";
        return false;
    }

    *new_seq_num = static_cast<std::uint32_t>(value.value());
    return true;
}

auto BuildAdminMessage(std::string_view msg_type) -> message::MessageBuilder {
    message::MessageBuilder builder{std::string(msg_type)};
    builder.set_string(35U, std::string(msg_type));
    return builder;
}

auto ShouldRejectValidationIssue(
    const ValidationPolicy& policy,
    const codec::ValidationIssue& issue) -> bool {
    switch (issue.kind) {
        case codec::ValidationIssueKind::kUnknownField:
        case codec::ValidationIssueKind::kFieldNotAllowed:
            return policy.reject_unknown_fields;
        case codec::ValidationIssueKind::kDuplicateField:
            return policy.reject_duplicate_fields;
        case codec::ValidationIssueKind::kIncorrectNumInGroupCount:
            return policy.reject_invalid_group_structure;
        default:
            return false;
    }
}

auto ValidationIssueRejectReason(const codec::ValidationIssue& issue) -> std::uint32_t {
    switch (issue.kind) {
        case codec::ValidationIssueKind::kFieldNotAllowed:
            return kSessionRejectTagNotDefinedForMessage;
        case codec::ValidationIssueKind::kUnknownField:
            return kSessionRejectUndefinedTag;
        case codec::ValidationIssueKind::kDuplicateField:
            return kSessionRejectTagAppearsMoreThanOnce;
        case codec::ValidationIssueKind::kIncorrectNumInGroupCount:
            return kSessionRejectIncorrectNumInGroupCount;
        default:
            return 0U;
    }
}

auto IsPreActivationState(SessionState state) -> bool {
    return state == SessionState::kConnected || state == SessionState::kPendingLogon;
}

auto IsActiveAdminState(SessionState state) -> bool {
    return state == SessionState::kActive || state == SessionState::kAwaitingLogout ||
           state == SessionState::kResendProcessing;
}

auto AdminPhaseViolationText(SessionState state, std::string_view msg_type) -> std::optional<std::string> {
    if (!IsAdminMessage(msg_type)) {
        return std::nullopt;
    }

    if (IsPreActivationState(state) && msg_type != "A" && msg_type != "5") {
        return std::string("received ") + std::string(msg_type) + " before Logon completed";
    }

    if (msg_type == "A" && IsActiveAdminState(state)) {
        return std::string("received unexpected Logon after session activation");
    }

    return std::nullopt;
}

}  // namespace

AdminProtocol::AdminProtocol(
    AdminProtocolConfig config,
    const profile::NormalizedDictionaryView& dictionary,
    store::SessionStore* store)
    : config_(std::move(config)),
      dictionary_(dictionary),
      store_(store),
      session_(config_.session) {
    // If transport_profile was left at default but begin_string was set, derive
    // the profile from begin_string so callers that only set begin_string still
    // get correct transport semantics.
    if (config_.transport_profile.begin_string != config_.begin_string &&
        !config_.begin_string.empty()) {
        config_.transport_profile = TransportSessionProfile::FromBeginString(config_.begin_string);
    }
    session_.set_transport_profile(&config_.transport_profile);
    encode_buffer_.storage.reserve(kInitialEncodeBufferBytes);
    for (auto& replay_frames : replay_frame_buffers_) {
        replay_frames = std::make_shared<ProtocolFrameList>();
    }

    auto table = codec::PrecompiledTemplateTable::Build(dictionary_, codec::EncodeTemplateConfig{
        .begin_string = config_.begin_string,
        .sender_comp_id = config_.sender_comp_id,
        .target_comp_id = config_.target_comp_id,
        .default_appl_ver_id = config_.default_appl_ver_id,
    });
    if (table.ok()) {
        encode_templates_ = std::move(table).value();
    }

    decode_table_ = codec::CompiledDecoderTable::Build(dictionary_);

    if (store_ == nullptr) {
        return;
    }

    auto recovery = store_->LoadRecoveryState(session_.session_id());
    if (!recovery.ok()) {
        if (recovery.status().code() != base::ErrorCode::kNotFound) {
            initialization_error_ = recovery.status();
        }
        return;
    }

    auto status = session_.RestoreSequenceState(
        recovery.value().next_in_seq,
        recovery.value().next_out_seq);
    if (!status.ok()) {
        initialization_error_ = status;
    }
}

auto AdminProtocol::EnsureInitialized() const -> base::Status {
    if (initialization_error_.has_value()) {
        return *initialization_error_;
    }
    return base::Status::Ok();
}

auto AdminProtocol::ResolveEncodeTemplate(std::string_view msg_type) -> const codec::FrameEncodeTemplate* {
    if (msg_type.empty()) {
        return nullptr;
    }
    return encode_templates_.find(msg_type);
}

auto AdminProtocol::PersistRecoveryState() -> base::Status {
    if (store_ == nullptr) {
        return base::Status::Ok();
    }

    const auto snapshot = session_.Snapshot();
    return store_->SaveRecoveryState(store::SessionRecoveryState{
        .session_id = snapshot.session_id,
        .next_in_seq = snapshot.next_in_seq,
        .next_out_seq = snapshot.next_out_seq,
        .last_inbound_ns = snapshot.last_inbound_ns,
        .last_outbound_ns = snapshot.last_outbound_ns,
        .active = snapshot.state != SessionState::kDisconnected,
    });
}

auto AdminProtocol::ValidateCompIds(
    const codec::DecodedMessageView& decoded,
    std::uint32_t* ref_tag_id,
    std::uint32_t* reject_reason,
    std::string* text,
    bool* disconnect) const -> bool {
    const bool is_logon = decoded.header.msg_type == "A";
    if (config_.validation_policy.enforce_comp_ids &&
        !config_.target_comp_id.empty() && decoded.header.sender_comp_id != config_.target_comp_id) {
        *ref_tag_id = 49U;
        *reject_reason = kSessionRejectCompIdProblem;
        *text = "unexpected SenderCompID on inbound frame";
        *disconnect = true;
        return false;
    }
    if (config_.validation_policy.enforce_comp_ids &&
        !config_.sender_comp_id.empty() && decoded.header.target_comp_id != config_.sender_comp_id) {
        *ref_tag_id = 56U;
        *reject_reason = kSessionRejectCompIdProblem;
        *text = "unexpected TargetCompID on inbound frame";
        *disconnect = true;
        return false;
    }
    if (is_logon && config_.validation_policy.require_default_appl_ver_id_on_logon &&
        config_.transport_profile.requires_default_appl_ver_id && decoded.header.default_appl_ver_id.empty()) {
        *ref_tag_id = 1137U;
        *reject_reason = kSessionRejectRequiredTagMissing;
        *text = "FIXT.1.1 logon requires DefaultApplVerID";
        *disconnect = true;
        return false;
    }
    if (is_logon && config_.validation_policy.require_default_appl_ver_id_on_logon &&
        !config_.default_appl_ver_id.empty() &&
        decoded.header.default_appl_ver_id != config_.default_appl_ver_id) {
        *ref_tag_id = 1137U;
        *reject_reason = kSessionRejectValueIncorrect;
        *text = "unexpected DefaultApplVerID on inbound frame";
        *disconnect = true;
        return false;
    }
    return true;
}

auto AdminProtocol::ValidatePossDup(const codec::DecodedMessageView& decoded) const -> base::Status {
    if (!config_.validation_policy.require_orig_sending_time_on_poss_dup) {
        return base::Status::Ok();
    }
    if (!decoded.header.poss_dup) {
        return base::Status::Ok();
    }
    if (!decoded.header.orig_sending_time.empty()) {
        return base::Status::Ok();
    }
    return base::Status::InvalidArgument("PossDupFlag requires OrigSendingTime");
}

auto AdminProtocol::ValidateAdministrativeMessage(
    const codec::DecodedMessageView& decoded,
    std::uint32_t* ref_tag_id,
    std::uint32_t* reject_reason,
    std::string* text,
    bool* disconnect) const -> bool {
    if (!IsAdminMessage(decoded.header.msg_type)) {
        return true;
    }

    *disconnect = false;

    if (decoded.validation_issue.present() &&
        ShouldRejectValidationIssue(config_.validation_policy, decoded.validation_issue)) {
        *ref_tag_id = decoded.validation_issue.tag;
        *reject_reason = ValidationIssueRejectReason(decoded.validation_issue);
        *text = decoded.validation_issue.text;
        *disconnect = decoded.header.msg_type == "A";
        return false;
    }

    const auto view = decoded.message.view();
    if (decoded.header.msg_type == "A") {
        if (!view.has_field(98U)) {
            *ref_tag_id = 98U;
            *reject_reason = kSessionRejectRequiredTagMissing;
            *text = "Logon requires EncryptMethod";
            *disconnect = true;
            return false;
        }

        const auto encrypt_method = view.get_int(98U);
        if (!encrypt_method.has_value() || encrypt_method.value() != 0) {
            *ref_tag_id = 98U;
            *reject_reason = kSessionRejectValueIncorrect;
            *text = "Logon EncryptMethod must be 0";
            *disconnect = true;
            return false;
        }

        if (!view.has_field(108U)) {
            *ref_tag_id = 108U;
            *reject_reason = kSessionRejectRequiredTagMissing;
            *text = "Logon requires HeartBtInt";
            *disconnect = true;
            return false;
        }

        const auto heartbeat_interval = view.get_int(108U);
        if (!heartbeat_interval.has_value() || heartbeat_interval.value() <= 0) {
            *ref_tag_id = 108U;
            *reject_reason = kSessionRejectValueIncorrect;
            *text = "Logon HeartBtInt must be positive";
            *disconnect = true;
            return false;
        }
    }

    if (decoded.header.msg_type == "1" && !view.has_field(112U)) {
        *ref_tag_id = 112U;
        *reject_reason = kSessionRejectRequiredTagMissing;
        *text = "TestRequest requires TestReqID";
        return false;
    }

    if (decoded.header.msg_type == "2") {
        if (!view.has_field(7U)) {
            *ref_tag_id = 7U;
            *reject_reason = kSessionRejectRequiredTagMissing;
            *text = "ResendRequest requires BeginSeqNo";
            return false;
        }
        if (!view.has_field(16U)) {
            *ref_tag_id = 16U;
            *reject_reason = kSessionRejectRequiredTagMissing;
            *text = "ResendRequest requires EndSeqNo";
            return false;
        }

        const auto begin_seq = view.get_int(7U);
        const auto end_seq = view.get_int(16U);
        if (!begin_seq.has_value() || begin_seq.value() <= 0) {
            *ref_tag_id = 7U;
            *reject_reason = kSessionRejectValueIncorrect;
            *text = "ResendRequest BeginSeqNo must be positive";
            return false;
        }
        if (!end_seq.has_value() || end_seq.value() < 0) {
            *ref_tag_id = 16U;
            *reject_reason = kSessionRejectValueIncorrect;
            *text = "ResendRequest EndSeqNo must be zero or positive";
            return false;
        }
        if (end_seq.value() != 0 && begin_seq.value() > end_seq.value()) {
            *ref_tag_id = 16U;
            *reject_reason = kSessionRejectValueIncorrect;
            *text = "ResendRequest BeginSeqNo must be less than or equal to EndSeqNo";
            return false;
        }
    }

    return true;
}

auto AdminProtocol::ValidateApplicationMessage(
    const codec::DecodedMessageView& decoded,
    std::uint32_t* ref_tag_id,
    std::uint32_t* reject_reason,
    std::string* text) const -> bool {
    if (IsAdminMessage(decoded.header.msg_type)) {
        return true;
    }

    const auto* message_def = dictionary_.find_message(decoded.header.msg_type);
    if (message_def == nullptr) {
        if (!config_.validation_policy.require_known_app_message_type) {
            return true;
        }
        *ref_tag_id = 35U;
        *reject_reason = kSessionRejectInvalidMsgType;
        *text = "application message type is not present in the bound dictionary";
        return false;
    }

    if (decoded.validation_issue.present() &&
        ShouldRejectValidationIssue(config_.validation_policy, decoded.validation_issue)) {
        *ref_tag_id = decoded.validation_issue.tag;
        *reject_reason = ValidationIssueRejectReason(decoded.validation_issue);
        *text = decoded.validation_issue.text;
        return false;
    }

    if (!config_.validation_policy.require_required_fields_on_app_messages) {
        return true;
    }

    auto typed = message::TypedMessageView::FromParts(dictionary_, decoded.message.view(), message_def);

    std::uint32_t missing_tag = 0U;
    auto status = typed.validate_required_fields(&missing_tag);
    if (status.ok()) {
        return true;
    }

    *ref_tag_id = missing_tag;
    *reject_reason = kSessionRejectRequiredTagMissing;
    *text = status.message();
    return false;
}

auto AdminProtocol::ReserveReplayStorage(std::size_t frame_count) -> void {
    replay_range_buffer_.records.reserve(frame_count);
    for (auto& replay_frames : replay_frame_buffers_) {
        if (!replay_frames) {
            continue;
        }
        replay_frames->reserve(frame_count);
    }
}

auto AdminProtocol::EncodeFrame(
    const message::Message& message,
    bool admin,
    std::uint64_t timestamp_ns,
    bool persist,
    bool poss_dup,
    bool allocate_seq,
    std::uint16_t extra_record_flags,
    std::uint32_t seq_override,
    std::string_view orig_sending_time) -> base::Result<EncodedFrame> {
    return EncodeFrame(
        message.view(),
        admin,
        timestamp_ns,
        persist,
        poss_dup,
        allocate_seq,
        extra_record_flags,
        seq_override,
        std::move(orig_sending_time));
}

auto AdminProtocol::EncodeFrame(
    message::MessageView message,
    bool admin,
    std::uint64_t timestamp_ns,
    bool persist,
    bool poss_dup,
    bool allocate_seq,
    std::uint16_t extra_record_flags,
    std::uint32_t seq_override,
    std::string_view orig_sending_time) -> base::Result<EncodedFrame> {
    encode_buffer_.clear();

    std::uint32_t seq_num = seq_override;
    if (allocate_seq) {
        auto allocated = session_.AllocateOutboundSeq();
        if (!allocated.ok()) {
            return allocated.status();
        }
        seq_num = allocated.value();
    }

    codec::EncodeOptions options;
    options.begin_string = config_.begin_string;
    options.sender_comp_id = config_.sender_comp_id;
    options.target_comp_id = config_.target_comp_id;
    options.default_appl_ver_id = config_.default_appl_ver_id;
    options.orig_sending_time = orig_sending_time;
    options.msg_seq_num = seq_num;
    options.poss_dup = poss_dup;

    const auto msg_type = message.msg_type();
    auto encoded_status = [&]() -> base::Status {
        if (const auto* template_encoder = ResolveEncodeTemplate(msg_type); template_encoder != nullptr) {
            auto templated = template_encoder->EncodeToBuffer(message, options, &encode_buffer_);
            if (templated.ok()) {
                return templated;
            }
        }
        return codec::EncodeFixMessageToBuffer(message, dictionary_, options, &encode_buffer_);
    }();
    if (!encoded_status.ok()) {
        return encoded_status;
    }

    EncodedFrame encoded_frame;
    encoded_frame.bytes.assign(encode_buffer_.bytes());
    encoded_frame.msg_type = std::string(message.msg_type());
    encoded_frame.admin = admin;

    auto status = session_.RecordOutboundActivity(timestamp_ns);
    if (!status.ok()) {
        return status;
    }

    if (persist && store_ != nullptr) {
        std::uint16_t flags = extra_record_flags;
        if (admin) {
            flags |= MessageRecordFlagValue(store::MessageRecordFlags::kAdmin);
        }
        if (poss_dup) {
            flags |= MessageRecordFlagValue(store::MessageRecordFlags::kPossDup);
        }

        const auto snapshot = session_.Snapshot();
        status = store_->SaveOutboundViewAndRecoveryState(
            store::MessageRecordView{
                .session_id = session_.session_id(),
                .seq_num = seq_num,
                .timestamp_ns = timestamp_ns,
                .flags = flags,
                .payload = encoded_frame.bytes.view(),
            },
            store::SessionRecoveryState{
                .session_id = snapshot.session_id,
                .next_in_seq = snapshot.next_in_seq,
                .next_out_seq = snapshot.next_out_seq,
                .last_inbound_ns = snapshot.last_inbound_ns,
                .last_outbound_ns = snapshot.last_outbound_ns,
                .active = snapshot.state != SessionState::kDisconnected,
            });
        if (!status.ok()) {
            return status;
        }
    } else {
        status = PersistRecoveryState();
        if (!status.ok()) {
            return status;
        }
    }

    return encoded_frame;
}

auto AdminProtocol::BuildLogonFrame(std::uint64_t timestamp_ns, bool reset_seq_num) -> base::Result<EncodedFrame> {
    auto builder = BuildAdminMessage("A");
    builder.set_int(98U, 0).set_int(108U, static_cast<std::int64_t>(config_.heartbeat_interval_seconds));
    if (reset_seq_num) {
        builder.set_boolean(141U, true);
    }
    return EncodeFrame(std::move(builder).build(), true, timestamp_ns, true, false, true, 0U);
}

auto AdminProtocol::BuildHeartbeatFrame(std::uint64_t timestamp_ns, std::string_view test_request_id)
    -> base::Result<EncodedFrame> {
    auto builder = BuildAdminMessage("0");
    if (!test_request_id.empty()) {
        builder.set_string(112U, std::string(test_request_id));
    }
    return EncodeFrame(std::move(builder).build(), true, timestamp_ns, true, false, true, 0U);
}

auto AdminProtocol::BuildTestRequestFrame(std::uint64_t timestamp_ns, std::string_view test_request_id)
    -> base::Result<EncodedFrame> {
    auto builder = BuildAdminMessage("1");
    builder.set_string(112U, std::string(test_request_id));
    return EncodeFrame(std::move(builder).build(), true, timestamp_ns, true, false, true, 0U);
}

auto AdminProtocol::BuildResendRequestFrame(
    std::uint32_t begin_seq,
    std::uint32_t end_seq,
    std::uint64_t timestamp_ns) -> base::Result<EncodedFrame> {
    auto builder = BuildAdminMessage("2");
    builder.set_int(7U, static_cast<std::int64_t>(begin_seq))
        .set_int(16U, static_cast<std::int64_t>(end_seq));
    return EncodeFrame(std::move(builder).build(), true, timestamp_ns, true, false, true, 0U);
}

auto AdminProtocol::BuildGapFillFrame(
    std::uint32_t begin_seq,
    std::uint32_t new_seq_num,
    std::uint64_t timestamp_ns) -> base::Result<EncodedFrame> {
    auto builder = BuildAdminMessage("4");
    builder.set_boolean(123U, true).set_int(36U, static_cast<std::int64_t>(new_seq_num));
    builder.set_boolean(43U, true);
    return EncodeFrame(
        std::move(builder).build(),
        true,
        timestamp_ns,
        false,
        true,
        false,
        MessageRecordFlagValue(store::MessageRecordFlags::kGapFill),
        begin_seq);
}

auto AdminProtocol::BuildRejectFrame(
    std::uint32_t ref_seq_num,
    std::string_view ref_msg_type,
    std::uint32_t ref_tag_id,
    std::uint32_t reject_reason,
    std::string text,
    std::uint64_t timestamp_ns) -> base::Result<EncodedFrame> {
    auto builder = BuildAdminMessage("3");
    builder.set_int(45U, static_cast<std::int64_t>(ref_seq_num));
    builder.set_string(372U, std::string(ref_msg_type));
    if (ref_tag_id != 0U) {
        builder.set_int(371U, static_cast<std::int64_t>(ref_tag_id));
    }
    if (reject_reason != 0U) {
        builder.set_int(373U, static_cast<std::int64_t>(reject_reason));
    }
    if (!text.empty()) {
        builder.set_string(58U, std::move(text));
    }
    return EncodeFrame(std::move(builder).build(), true, timestamp_ns, true, false, true, 0U);
}

auto AdminProtocol::RejectInbound(
    const codec::DecodedMessageView& decoded,
    std::uint32_t ref_tag_id,
    std::uint32_t reject_reason,
    std::string text,
    std::uint64_t timestamp_ns,
    bool disconnect) -> base::Result<ProtocolEvent> {
    ProtocolEvent event;
    if (disconnect && decoded.header.msg_type == "A") {
        auto logout = BeginLogout(std::move(text), timestamp_ns);
        if (!logout.ok()) {
            return logout.status();
        }
        event.outbound_frames.push_back(std::move(logout).value());
        event.disconnect = true;
        return event;
    }

    auto reject = BuildRejectFrame(
        decoded.header.msg_seq_num,
        decoded.message.view().msg_type(),
        ref_tag_id,
        reject_reason,
        std::move(text),
        timestamp_ns);
    if (!reject.ok()) {
        return reject.status();
    }
    event.outbound_frames.push_back(std::move(reject).value());
    event.disconnect = disconnect;
    return event;
}

auto AdminProtocol::ReplayOutbound(
    std::uint32_t begin_seq,
    std::uint32_t end_seq,
    std::uint64_t timestamp_ns,
    ProtocolFrameList* frames) -> base::Status {
    if (frames == nullptr) {
        return base::Status::InvalidArgument("replay frame list is null");
    }

    frames->clear();
    if (store_ == nullptr) {
        return base::Status::Ok();
    }

    const auto snapshot = session_.Snapshot();
    const auto bounded_end = (end_seq == 0U || end_seq >= snapshot.next_out_seq) ? snapshot.next_out_seq - 1U : end_seq;
    if (bounded_end < begin_seq || bounded_end == 0U) {
        return base::Status::Ok();
    }

    frames->reserve(static_cast<std::size_t>(bounded_end - begin_seq + 1U));

    auto status = store_->LoadOutboundRangeViews(session_.session_id(), begin_seq, bounded_end, &replay_range_buffer_);
    if (!status.ok()) {
        return status;
    }

    // Pre-build replay options — only seq_num and orig_sending_time change per message.
    codec::UtcTimestampBuffer ts_buf;
    const auto sending_time = codec::CurrentUtcTimestamp(&ts_buf);

    codec::ReplayOptions replay_opts;
    replay_opts.sender_comp_id = config_.sender_comp_id;
    replay_opts.target_comp_id = config_.target_comp_id;
    replay_opts.begin_string = config_.begin_string;
    replay_opts.default_appl_ver_id = config_.default_appl_ver_id;
    replay_opts.sending_time = sending_time;

    std::uint32_t seq = begin_seq;
    std::size_t record_index = 0U;
    const auto& records = replay_range_buffer_.records;
    while (seq <= bounded_end) {
        const store::MessageRecordView* record = nullptr;
        if (record_index < records.size() && records[record_index].seq_num == seq) {
            record = &records[record_index];
        }
        const bool replayable = record != nullptr && !record->is_admin();
        if (!replayable) {
            const auto gap_begin = seq;
            while (seq <= bounded_end) {
                if (record_index < records.size() && records[record_index].seq_num == seq) {
                    if (!records[record_index].is_admin()) {
                        break;
                    }
                    ++record_index;
                }
                ++seq;
            }
            auto gap_fill = BuildGapFillFrame(gap_begin, seq, timestamp_ns);
            if (!gap_fill.ok()) {
                return gap_fill.status();
            }
            frames->push_back(std::move(gap_fill).value());
            continue;
        }

        // Lightweight header-only scan — skip checksum verification since we encoded these bytes.
        auto parsed = codec::DecodeRawPassThrough(record->payload, codec::kFixSoh, false);
        if (!parsed.ok()) {
            return parsed.status();
        }

        replay_opts.msg_seq_num = record->seq_num;
        replay_opts.orig_sending_time = parsed.value().sending_time;

        status = codec::EncodeReplay(parsed.value(), replay_opts, &encode_buffer_);
        if (!status.ok()) {
            return status;
        }

        EncodedFrame frame;
        frame.bytes.assign(encode_buffer_.bytes());
        frame.msg_type = std::string(parsed.value().msg_type);
        frame.admin = false;
        frames->push_back(std::move(frame));

        ++record_index;
        ++seq;
    }

    return base::Status::Ok();
}

auto AdminProtocol::AcquireReplayFrameBuffer() -> std::shared_ptr<ProtocolFrameList> {
    for (std::size_t attempt = 0U; attempt < replay_frame_buffers_.size(); ++attempt) {
        const auto index = (replay_frame_buffer_cursor_ + attempt) % replay_frame_buffers_.size();
        auto& replay_frames = replay_frame_buffers_[index];
        if (!replay_frames || replay_frames.use_count() != 1U) {
            continue;
        }

        replay_frame_buffer_cursor_ = index;
        replay_frames->clear();
        return replay_frames;
    }

    return {};
}

auto AdminProtocol::OnTransportConnected(std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent> {
    ProtocolEvent event;

    auto status = EnsureInitialized();
    if (!status.ok()) {
        return status;
    }

    status = session_.OnTransportConnected();
    if (!status.ok()) {
        return status;
    }
    status = PersistRecoveryState();
    if (!status.ok()) {
        return status;
    }

    if (!config_.session.is_initiator) {
        return event;
    }

    if (config_.reset_seq_num_on_logon) {
        status = session_.RestoreSequenceState(1U, 1U);
        if (!status.ok()) {
            return status;
        }
    }

    status = session_.BeginLogon();
    if (!status.ok()) {
        return status;
    }

    auto logon = BuildLogonFrame(timestamp_ns, config_.reset_seq_num_on_logon);
    if (!logon.ok()) {
        return logon.status();
    }
    event.outbound_frames.push_back(std::move(logon).value());
    return event;
}

auto AdminProtocol::OnTransportClosed() -> base::Status {
    auto status = EnsureInitialized();
    if (!status.ok()) {
        return status;
    }

    outstanding_test_request_id_.clear();
    logout_sent_ = false;
    status = session_.OnTransportClosed();
    if (!status.ok()) {
        return status;
    }
    return PersistRecoveryState();
}

auto AdminProtocol::OnInbound(std::span<const std::byte> frame, std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent> {
    auto decoded = codec::DecodeFixMessageView(frame, dictionary_, decode_table_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    auto event = OnInbound(decoded.value(), timestamp_ns);
    if (!event.ok()) {
        return event.status();
    }
    if (event.value().application_messages.size() == 1U) {
        auto& message = event.value().application_messages.front();
        if (message.valid() && !message.owns_message()) {
            auto& decoded_value = decoded.value();
            event.value().AdoptParsedApplicationMessage(std::move(decoded_value.message), decoded_value.raw);
        }
    } else {
        event.value().MaterializeApplicationMessages();
    }
    return std::move(event).value();
}

auto AdminProtocol::OnInbound(std::vector<std::byte>&& frame, std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent> {
    auto decoded = codec::DecodeFixMessageView(std::span<const std::byte>(frame.data(), frame.size()), dictionary_, decode_table_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    auto event = OnInbound(decoded.value(), timestamp_ns);
    if (!event.ok()) {
        return event.status();
    }
    if (event.value().application_messages.size() == 1U) {
        auto& message = event.value().application_messages.front();
        if (message.valid() && !message.owns_message()) {
            auto& decoded_value = decoded.value();
            event.value().AdoptParsedApplicationMessage(std::move(decoded_value.message), std::move(frame));
        }
    } else {
        event.value().MaterializeApplicationMessages();
    }
    return std::move(event).value();
}

auto AdminProtocol::OnInbound(const codec::DecodedMessageView& decoded, std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent> {
    ProtocolEvent event;

    auto status = EnsureInitialized();
    if (!status.ok()) {
        return status;
    }

    const auto view = decoded.message.view();
    const auto msg_type = view.msg_type();
    const bool inbound_gap_fill = msg_type == "4" && HasBoolean(view, 123U);
    const bool inbound_logon_reset = msg_type == "A" && HasBoolean(view, 141U);

    std::uint32_t ref_tag_id = 0U;
    std::uint32_t reject_reason = 0U;
    std::string reject_text;
    bool disconnect = false;
    if (!ValidateCompIds(decoded, &ref_tag_id, &reject_reason, &reject_text, &disconnect)) {
        return RejectInbound(
            decoded,
            ref_tag_id,
            reject_reason,
            std::move(reject_text),
            timestamp_ns,
            disconnect);
    }

    status = ValidatePossDup(decoded);
    if (!status.ok()) {
        return RejectInbound(
            decoded,
            122U,
            kSessionRejectRequiredTagMissing,
            status.message(),
            timestamp_ns,
            decoded.header.msg_type == "A");
    }

    auto snapshot_before = session_.Snapshot();
    if (inbound_logon_reset) {
        const auto next_out_seq = config_.session.is_initiator ? snapshot_before.next_out_seq : 1U;
        status = session_.RestoreSequenceState(1U, next_out_seq);
        if (!status.ok()) {
            return status;
        }
        snapshot_before = session_.Snapshot();
    }

    const auto phase_violation_text = AdminPhaseViolationText(snapshot_before.state, msg_type);
    const auto send_phase_violation_logout = [&](std::string text) -> base::Result<ProtocolEvent> {
        ProtocolEvent phase_event;
        auto logout = BeginLogout(std::move(text), timestamp_ns);
        if (!logout.ok()) {
            return logout.status();
        }
        phase_event.outbound_frames.push_back(std::move(logout).value());
        phase_event.disconnect = true;
        return phase_event;
    };

    const auto record_inbound_liveness = [&]() -> base::Status {
        auto activity_status = session_.RecordInboundActivity(timestamp_ns);
        if (!activity_status.ok()) {
            return activity_status;
        }
        return PersistRecoveryState();
    };

    if (decoded.header.msg_seq_num < snapshot_before.next_in_seq) {
        if (inbound_gap_fill) {
            std::uint32_t new_seq_num = 0U;
            if (!ParseSequenceResetNewSeq(view, &new_seq_num, &reject_reason, &reject_text)) {
                return RejectInbound(
                    decoded,
                    36U,
                    reject_reason,
                    std::move(reject_text),
                    timestamp_ns,
                    false);
            }

            if (new_seq_num > snapshot_before.next_in_seq) {
                status = session_.AdvanceInboundExpectedSeq(new_seq_num);
                if (!status.ok()) {
                    return status;
                }
            }

            status = record_inbound_liveness();
            if (!status.ok()) {
                return status;
            }
            return event;
        }

        if (decoded.header.poss_dup) {
            status = record_inbound_liveness();
            if (!status.ok()) {
                return status;
            }
            return event;
        }
        if (!config_.validation_policy.reject_on_stale_msg_seq_num) {
            status = record_inbound_liveness();
            if (!status.ok()) {
                return status;
            }
            return event;
        }
        auto logout = BeginLogout("received stale inbound FIX sequence number", timestamp_ns);
        if (!logout.ok()) {
            return logout.status();
        }
        event.outbound_frames.push_back(std::move(logout).value());
        event.disconnect = true;
        return event;
    }

    status = session_.ObserveInboundSeq(decoded.header.msg_seq_num);
    if (!status.ok()) {
        if (decoded.header.msg_seq_num > snapshot_before.next_in_seq && phase_violation_text.has_value()) {
            status = session_.RecordInboundActivity(timestamp_ns);
            if (!status.ok()) {
                return status;
            }
            return send_phase_violation_logout(*phase_violation_text);
        }
        if (decoded.header.msg_seq_num > snapshot_before.next_in_seq && session_.pending_resend().has_value()) {
            status = session_.RecordInboundActivity(timestamp_ns);
            if (!status.ok()) {
                return status;
            }
            const auto& pending = *session_.pending_resend();
            auto resend = BuildResendRequestFrame(pending.begin_seq, pending.end_seq, timestamp_ns);
            if (!resend.ok()) {
                return resend.status();
            }
            event.outbound_frames.push_back(std::move(resend).value());
            return event;
        }
        return status;
    }

    status = session_.RecordInboundActivity(timestamp_ns);
    if (!status.ok()) {
        return status;
    }

    if (store_ != nullptr) {
        std::uint16_t inbound_flags = 0U;
        if (IsAdminMessage(decoded.header.msg_type)) {
            inbound_flags |= MessageRecordFlagValue(store::MessageRecordFlags::kAdmin);
        }
        if (decoded.header.poss_dup) {
            inbound_flags |= MessageRecordFlagValue(store::MessageRecordFlags::kPossDup);
        }
        const auto snapshot = session_.Snapshot();
        status = store_->SaveInboundViewAndRecoveryState(
            store::MessageRecordView{
                .session_id = session_.session_id(),
                .seq_num = decoded.header.msg_seq_num,
                .timestamp_ns = timestamp_ns,
                .flags = inbound_flags,
                .payload = decoded.raw,
            },
            store::SessionRecoveryState{
                .session_id = snapshot.session_id,
                .next_in_seq = snapshot.next_in_seq,
                .next_out_seq = snapshot.next_out_seq,
                .last_inbound_ns = snapshot.last_inbound_ns,
                .last_outbound_ns = snapshot.last_outbound_ns,
                .active = snapshot.state != SessionState::kDisconnected,
            });
        if (!status.ok()) {
            return status;
        }
    } else {
        status = PersistRecoveryState();
        if (!status.ok()) {
            return status;
        }
    }

    if (phase_violation_text.has_value()) {
        return send_phase_violation_logout(*phase_violation_text);
    }

    ref_tag_id = 0U;
    reject_reason = 0U;
    reject_text.clear();
    disconnect = false;
    if (!ValidateAdministrativeMessage(decoded, &ref_tag_id, &reject_reason, &reject_text, &disconnect)) {
        return RejectInbound(
            decoded,
            ref_tag_id,
            reject_reason,
            std::move(reject_text),
            timestamp_ns,
            disconnect);
    }

    if (msg_type == "A") {
        const bool inbound_reset = HasBoolean(view, 141U);
        if (inbound_reset) {
            const auto snapshot = session_.Snapshot();
            const auto next_out = config_.session.is_initiator ? snapshot.next_out_seq : 1U;
            status = session_.RestoreSequenceState(decoded.header.msg_seq_num + 1U, next_out);
            if (!status.ok()) {
                return status;
            }
        }

        if (!config_.session.is_initiator) {
            auto response = BuildLogonFrame(timestamp_ns, config_.reset_seq_num_on_logon || inbound_reset);
            if (!response.ok()) {
                return response.status();
            }
            event.outbound_frames.push_back(std::move(response).value());
        }

        status = session_.OnLogonAccepted();
        if (!status.ok()) {
            return status;
        }
        status = PersistRecoveryState();
        if (!status.ok()) {
            return status;
        }
        event.session_active = true;
        return event;
    }

    if (msg_type == "0") {
        const auto test_request_id = GetStringView(view, 112U);
        if (!test_request_id.empty() && test_request_id == outstanding_test_request_id_) {
            outstanding_test_request_id_.clear();
        }
        return event;
    }

    if (msg_type == "1") {
        auto response = BuildHeartbeatFrame(timestamp_ns, GetStringView(view, 112U));
        if (!response.ok()) {
            return response.status();
        }
        event.outbound_frames.push_back(std::move(response).value());
        return event;
    }

    if (msg_type == "2") {
        const auto begin_seq = static_cast<std::uint32_t>(std::max<std::int64_t>(1, GetInt(view, 7U, 1)));
        const auto end_seq = static_cast<std::uint32_t>(std::max<std::int64_t>(0, GetInt(view, 16U, 0)));
        if (auto replay_frames = AcquireReplayFrameBuffer()) {
            status = ReplayOutbound(begin_seq, end_seq, timestamp_ns, replay_frames.get());
            if (!status.ok()) {
                return status;
            }
            event.outbound_frames.borrow(std::move(replay_frames));
            return event;
        }

        ProtocolFrameList replay_frames;
        status = ReplayOutbound(begin_seq, end_seq, timestamp_ns, &replay_frames);
        if (!status.ok()) {
            return status;
        }
        event.outbound_frames = std::move(replay_frames);
        return event;
    }

    if (msg_type == "4") {
        std::uint32_t new_seq_num = 0U;
        if (!ParseSequenceResetNewSeq(view, &new_seq_num, &reject_reason, &reject_text)) {
            return RejectInbound(
                decoded,
                36U,
                reject_reason,
                std::move(reject_text),
                timestamp_ns,
                false);
        }

        const auto snapshot = session_.Snapshot();
        if (new_seq_num < snapshot.next_in_seq) {
            return RejectInbound(
                decoded,
                36U,
                kSessionRejectValueIncorrect,
                "SequenceReset NewSeqNo must not move inbound sequence backwards",
                timestamp_ns,
                false);
        }

        status = session_.AdvanceInboundExpectedSeq(new_seq_num);
        if (!status.ok()) {
            return status;
        }
        status = PersistRecoveryState();
        if (!status.ok()) {
            return status;
        }
        return event;
    }

    if (msg_type == "5") {
        if (session_.state() != SessionState::kAwaitingLogout) {
            auto response = BeginLogout({}, timestamp_ns);
            if (!response.ok()) {
                return response.status();
            }
            event.outbound_frames.push_back(std::move(response).value());
        }
        event.disconnect = true;
        return event;
    }

    if (msg_type == "3") {
        event.session_reject = true;
        event.application_messages.push_back(message::MessageRef(decoded.message.view()));
        return event;
    }

    if (session_.state() != SessionState::kActive && session_.state() != SessionState::kAwaitingLogout &&
        session_.state() != SessionState::kResendProcessing) {
        return base::Status::InvalidArgument("application message received before session activation");
    }

    ref_tag_id = 0U;
    reject_reason = 0U;
    reject_text.clear();
    if (!ValidateApplicationMessage(decoded, &ref_tag_id, &reject_reason, &reject_text)) {
        return RejectInbound(
            decoded,
            ref_tag_id,
            reject_reason,
            std::move(reject_text),
            timestamp_ns,
            false);
    }

    event.application_messages.push_back(message::MessageRef(decoded.message.view()));
    if (HasBoolean(view, 97U)) {
        event.poss_resend = true;
    }
    return event;
}

auto AdminProtocol::OnTimer(std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent> {
    ProtocolEvent event;

    auto status = EnsureInitialized();
    if (!status.ok()) {
        return status;
    }

    const auto snapshot = session_.Snapshot();
    if (snapshot.state != SessionState::kActive && snapshot.state != SessionState::kAwaitingLogout &&
        snapshot.state != SessionState::kResendProcessing) {
        return event;
    }

    const auto interval_ns = std::max<std::uint64_t>(1U, config_.heartbeat_interval_seconds) * kNanosPerSecond;
    if (!outstanding_test_request_id_.empty() &&
        snapshot.last_inbound_ns != 0U && timestamp_ns > snapshot.last_inbound_ns + interval_ns) {
        event.disconnect = true;
        return event;
    }

    if (snapshot.last_inbound_ns != 0U && timestamp_ns > snapshot.last_inbound_ns + (interval_ns * 2U) &&
        outstanding_test_request_id_.empty()) {
        outstanding_test_request_id_ = std::to_string(timestamp_ns);
        auto test_request = BuildTestRequestFrame(timestamp_ns, outstanding_test_request_id_);
        if (!test_request.ok()) {
            return test_request.status();
        }
        event.outbound_frames.push_back(std::move(test_request).value());
        return event;
    }

    if (snapshot.last_outbound_ns == 0U || timestamp_ns > snapshot.last_outbound_ns + interval_ns) {
        auto heartbeat = BuildHeartbeatFrame(timestamp_ns, {});
        if (!heartbeat.ok()) {
            return heartbeat.status();
        }
        event.outbound_frames.push_back(std::move(heartbeat).value());
    }

    return event;
}

auto AdminProtocol::NextTimerDeadline(std::uint64_t timestamp_ns) const -> std::optional<std::uint64_t> {
    auto status = EnsureInitialized();
    if (!status.ok()) {
        return std::nullopt;
    }

    const auto snapshot = session_.Snapshot();
    if (snapshot.state != SessionState::kActive && snapshot.state != SessionState::kAwaitingLogout &&
        snapshot.state != SessionState::kResendProcessing) {
        return std::nullopt;
    }

    const auto interval_ns = std::max<std::uint64_t>(1U, config_.heartbeat_interval_seconds) * kNanosPerSecond;
    std::optional<std::uint64_t> deadline;
    const auto update_deadline = [&](std::uint64_t candidate_ns) {
        if (!deadline.has_value() || candidate_ns < *deadline) {
            deadline = candidate_ns;
        }
    };

    if (!outstanding_test_request_id_.empty() && snapshot.last_inbound_ns != 0U) {
        update_deadline(snapshot.last_inbound_ns + interval_ns);
    } else if (snapshot.last_inbound_ns != 0U) {
        update_deadline(snapshot.last_inbound_ns + (interval_ns * 2U));
    }

    if (snapshot.last_outbound_ns == 0U) {
        update_deadline(timestamp_ns);
    } else {
        update_deadline(snapshot.last_outbound_ns + interval_ns);
    }

    if (deadline.has_value() && *deadline < timestamp_ns) {
        deadline = timestamp_ns;
    }
    return deadline;
}

auto AdminProtocol::SendApplication(const message::Message& message, std::uint64_t timestamp_ns)
    -> base::Result<EncodedFrame> {
    auto status = EnsureInitialized();
    if (!status.ok()) {
        return status;
    }

    if (session_.state() != SessionState::kActive && session_.state() != SessionState::kAwaitingLogout) {
        return base::Status::InvalidArgument("cannot send application payload on an inactive FIX session");
    }
    return EncodeFrame(message, false, timestamp_ns, true, false, true, 0U);
}

auto AdminProtocol::SendApplication(message::MessageView message, std::uint64_t timestamp_ns)
    -> base::Result<EncodedFrame> {
    auto status = EnsureInitialized();
    if (!status.ok()) {
        return status;
    }

    if (session_.state() != SessionState::kActive && session_.state() != SessionState::kAwaitingLogout) {
        return base::Status::InvalidArgument("cannot send application payload on an inactive FIX session");
    }
    return EncodeFrame(message, false, timestamp_ns, true, false, true, 0U);
}

auto AdminProtocol::SendApplication(const message::MessageRef& message, std::uint64_t timestamp_ns)
    -> base::Result<EncodedFrame> {
    return SendApplication(message.view(), timestamp_ns);
}

auto AdminProtocol::BeginLogout(std::string text, std::uint64_t timestamp_ns) -> base::Result<EncodedFrame> {
    auto status = EnsureInitialized();
    if (!status.ok()) {
        return status;
    }

    if (logout_sent_) {
        return base::Status::InvalidArgument("logout already sent");
    }

    if (session_.state() == SessionState::kActive || session_.state() == SessionState::kResendProcessing) {
        status = session_.BeginLogout();
        if (!status.ok()) {
            return status;
        }
    }

    auto builder = BuildAdminMessage("5");
    if (!text.empty()) {
        builder.set_string(58U, std::move(text));
    }
    logout_sent_ = true;
    return EncodeFrame(std::move(builder).build(), true, timestamp_ns, true, false, true, 0U);
}

}  // namespace fastfix::session