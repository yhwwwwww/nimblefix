#include "nimblefix/runtime/warmup.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <sstream>
#include <string_view>

#include "nimblefix/advanced/message_data_writer.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/runtime/engine.h"

namespace nimble::runtime {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<std::string_view, 4> kDefaultWarmupMsgTypes{ "D", "8", "0", "A" };

auto
BuildSyntheticMessage(std::string_view msg_type) -> message::Message
{
  using namespace codec::tags;

  message::MessageDataWriter builder{ std::string(msg_type) };
  builder.set_string(kMsgType, msg_type);
  if (msg_type == codec::msg_types::kLogon) {
    builder.set_int(kEncryptMethod, 0).set_int(kHeartBtInt, 30);
  } else if (msg_type == codec::msg_types::kNewOrderSingle) {
    builder.set_string(kClOrdID, "WARMUP-ORDER")
      .set_string(kSymbol, "NIMBLE")
      .set_char(kSide, '1')
      .set_int(kOrderQty, 1)
      .set_char(kOrdType, '1')
      .set_string(kTransactTime, "20260430-00:00:00.000");
  } else if (msg_type == codec::msg_types::kExecutionReport) {
    builder.set_string(17U, "WARMUP-EXEC")
      .set_string(37U, "WARMUP-ORDER")
      .set_char(39U, '0')
      .set_char(150U, '0')
      .set_char(kSide, '1')
      .set_string(kSymbol, "NIMBLE")
      .set_int(14U, 0)
      .set_int(151U, 1)
      .set_float(6U, 0.0);
  }
  return std::move(builder).build();
}

auto
MakeEncodeOptions(std::uint32_t seq_num = 1U) -> codec::EncodeOptions
{
  codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "WARMUP-SENDER";
  options.target_comp_id = "WARMUP-TARGET";
  options.msg_seq_num = seq_num;
  options.sending_time = "20260430-00:00:00.000";
  return options;
}

auto
EncodeSynthetic(const profile::NormalizedDictionaryView& dictionary,
                std::string_view msg_type,
                std::uint32_t seq_num,
                codec::EncodeBuffer* buffer) -> base::Status
{
  auto message = BuildSyntheticMessage(msg_type.empty() ? std::string_view(codec::msg_types::kHeartbeat) : msg_type);
  return codec::EncodeFixMessageToBuffer(message, dictionary, MakeEncodeOptions(seq_num), buffer);
}

auto
EnsureRawMessage(const profile::NormalizedDictionaryView& dictionary,
                 const WarmupStep& step,
                 codec::EncodeBuffer* scratch) -> base::Result<std::span<const std::byte>>
{
  if (!step.raw_message.empty()) {
    return std::span<const std::byte>(step.raw_message.data(), step.raw_message.size());
  }

  const auto status = EncodeSynthetic(dictionary, step.msg_type, 1U, scratch);
  if (!status.ok()) {
    return status;
  }
  return scratch->bytes();
}

auto
TouchDictionary(const profile::NormalizedDictionaryView& dictionary) -> void
{
  volatile std::uint64_t accumulator = dictionary.profile().profile_id() ^ dictionary.profile().schema_hash();
  for (const auto& field : dictionary.fields()) {
    accumulator += field.tag;
  }
  for (const auto& message : dictionary.messages()) {
    accumulator += message.field_rule_count;
  }
  for (const auto& group : dictionary.groups()) {
    accumulator += group.count_tag;
  }
  for (const auto& rule : dictionary.header_field_rules()) {
    accumulator += rule.tag;
  }
  (void)accumulator;
}

auto
BudgetExpired(Clock::time_point started_at, std::chrono::milliseconds budget) -> bool
{
  if (budget.count() < 0) {
    return true;
  }
  return Clock::now() - started_at >= budget;
}

} // namespace

auto
WarmupResult::summary() const -> std::string
{
  std::ostringstream out;
  out << "warmup completed " << steps_completed << " step(s), skipped " << steps_skipped << " step(s), elapsed "
      << std::chrono::duration_cast<std::chrono::microseconds>(total_elapsed).count() << "us";
  if (!within_budget) {
    out << " (budget exceeded)";
  }
  return out.str();
}

auto
RunWarmup(Engine& engine, const WarmupConfig& config) -> base::Result<WarmupResult>
{
  if (engine.config() == nullptr) {
    return base::Status::InvalidArgument("engine must be booted before warmup");
  }

  WarmupResult result;
  result.steps.reserve(config.steps.size());
  const auto warmup_started = Clock::now();

  for (const auto& step : config.steps) {
    WarmupStepResult step_result;
    step_result.action = step.action;
    step_result.msg_type = step.msg_type;

    if (BudgetExpired(warmup_started, config.time_budget)) {
      step_result.skipped = true;
      step_result.skip_reason = "time budget exceeded before step";
      ++result.steps_skipped;
      result.within_budget = false;
      result.steps.push_back(std::move(step_result));
      continue;
    }

    const auto step_started = Clock::now();
    auto dictionary = engine.LoadDictionaryView(step.profile_id);
    if (!dictionary.ok()) {
      step_result.skipped = true;
      step_result.skip_reason = std::string(dictionary.status().message());
      step_result.elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - step_started);
      ++result.steps_skipped;
      result.steps.push_back(std::move(step_result));
      continue;
    }

    auto status = base::Status::Ok();
    codec::EncodeBuffer encode_buffer;
    codec::EncodeBuffer parse_source_buffer;
    const auto iterations = std::max(step.iterations, 1U);

    for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
      if (BudgetExpired(warmup_started, config.time_budget)) {
        step_result.skipped = true;
        step_result.skip_reason = "time budget exceeded during step";
        result.within_budget = false;
        break;
      }

      switch (step.action) {
        case WarmupAction::kTouchProfile:
          TouchDictionary(dictionary.value());
          break;
        case WarmupAction::kEncode:
        case WarmupAction::kDrySend:
          status = EncodeSynthetic(dictionary.value(), step.msg_type, iteration + 1U, &encode_buffer);
          break;
        case WarmupAction::kParse: {
          auto raw = EnsureRawMessage(dictionary.value(), step, &parse_source_buffer);
          if (!raw.ok()) {
            status = raw.status();
            break;
          }
          auto decoded = codec::DecodeFixMessageView(raw.value(), dictionary.value());
          status = decoded.ok() ? base::Status::Ok() : decoded.status();
          break;
        }
        case WarmupAction::kRoundTrip: {
          status = EncodeSynthetic(dictionary.value(), step.msg_type, iteration + 1U, &encode_buffer);
          if (status.ok()) {
            auto decoded = codec::DecodeFixMessageView(encode_buffer.bytes(), dictionary.value());
            status = decoded.ok() ? base::Status::Ok() : decoded.status();
          }
          break;
        }
      }

      if (!status.ok()) {
        step_result.skipped = true;
        step_result.skip_reason = std::string(status.message());
        break;
      }
      ++step_result.iterations_completed;
    }

    step_result.elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - step_started);
    if (step_result.skipped) {
      ++result.steps_skipped;
    } else {
      ++result.steps_completed;
    }
    result.steps.push_back(std::move(step_result));
  }

  result.total_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - warmup_started);
  if (result.total_elapsed > config.time_budget) {
    result.within_budget = false;
  }
  if (!config.report_timing) {
    for (auto& step : result.steps) {
      step.elapsed = std::chrono::nanoseconds{};
    }
  }
  return result;
}

auto
DefaultWarmupConfig(const Engine& engine, std::uint32_t iterations_per_step) -> WarmupConfig
{
  WarmupConfig config;
  const auto* engine_config = engine.config();
  if (engine_config == nullptr) {
    return config;
  }

  for (const auto& counterparty : engine_config->counterparties) {
    for (const auto msg_type : kDefaultWarmupMsgTypes) {
      config.steps.push_back(WarmupStep{
        .action = WarmupAction::kEncode,
        .profile_id = counterparty.session.profile_id,
        .msg_type = std::string(msg_type),
        .raw_message = {},
        .iterations = iterations_per_step,
      });
      config.steps.push_back(WarmupStep{
        .action = WarmupAction::kParse,
        .profile_id = counterparty.session.profile_id,
        .msg_type = std::string(msg_type),
        .raw_message = {},
        .iterations = iterations_per_step,
      });
    }
  }
  return config;
}

} // namespace nimble::runtime
