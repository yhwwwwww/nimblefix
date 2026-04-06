#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <time.h>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "fastfix/codec/fix_codec.h"
#include "fastfix/message/message.h"
#include "fastfix/message/fixed_layout_writer.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/profile/profile_loader.h"
#include "fastfix/session/admin_protocol.h"
#include "fastfix/store/memory_store.h"
#include "fastfix/transport/tcp_transport.h"

#include "bench_support.h"

namespace {

using bench_support::BenchmarkMeasurement;
using bench_support::BenchmarkResult;
using bench_support::BuildFix44BusinessOrder;
using bench_support::DurationNs;
using bench_support::Fix44BusinessOrder;
using bench_support::NowNs;

auto PrintUsage() -> void {
    std::cout << "usage: fastfix-bench [--artifact <profile.art> | --dictionary <profile.ffd> [--dictionary <overlay.ffd> ...]] [--iterations <count>] [--loopback <count>] [--replay <count>] [--replay-span <count>] [--begin-string <value>] [--default-appl-ver-id <value>]\n";
}

auto LoadDictionary(
    const std::filesystem::path& artifact_path,
    std::span<const std::filesystem::path> dictionary_paths)
    -> fastfix::base::Result<fastfix::profile::NormalizedDictionaryView> {
    fastfix::base::Result<fastfix::profile::LoadedProfile> profile = [&]() {
        if (!dictionary_paths.empty()) {
            return fastfix::profile::LoadProfileFromDictionaryFiles(dictionary_paths);
        }
        return fastfix::profile::LoadProfileArtifact(artifact_path);
    }();
    if (!profile.ok()) {
        return profile.status();
    }
    return fastfix::profile::NormalizedDictionaryView::FromProfile(std::move(profile).value());
}

auto PopulateFix44MessageBuilder(fastfix::message::MessageBuilder& builder, const Fix44BusinessOrder& order) -> void {
    builder.set(35U, "D")
        .set(11U, order.cl_ord_id)
        .set(55U, order.symbol)
        .set(54U, order.side)
        .set(60U, order.transact_time.text)
        .set(38U, order.order_qty)
        .set(40U, order.ord_type);
    if (order.price.has_value()) {
        builder.set(44U, order.price.value());
    }
    auto party = builder.add_group_entry(453U);
    party.set(448U, order.party_id)
        .set(447U, order.party_id_source)
        .set(452U, order.party_role);
}

auto BuildFix44MessageFromBusinessOrder(const Fix44BusinessOrder& order) -> fastfix::message::Message {
    fastfix::message::MessageBuilder builder{"D"};
    builder.reserve_fields(order.price.has_value() ? 7U : 6U).reserve_groups(1U).reserve_group_entries(453U, 1U);
    PopulateFix44MessageBuilder(builder, order);
    return std::move(builder).build();
}

auto BuildSampleMessage(bool include_price = true) -> fastfix::message::Message {
    return BuildFix44MessageFromBusinessOrder(BuildFix44BusinessOrder(include_price));
}

auto ExtractOrderQty(fastfix::message::MessageView order) -> std::optional<double> {
    if (auto qty = order.get_float(38U); qty.has_value()) {
        return qty.value();
    }
    if (auto qty = order.get_int(38U); qty.has_value()) {
        return static_cast<double>(qty.value());
    }
    return std::nullopt;
}

auto BuildFix44OrderAckFromNewOrder(
    fastfix::message::MessageView order,
    std::uint32_t execution_id) -> fastfix::base::Result<fastfix::message::Message> {
    if (order.msg_type() != "D") {
        return fastfix::base::Status::InvalidArgument("loopback benchmark expected NewOrderSingle (35=D)");
    }

    const auto cl_ord_id = order.get_string(11U);
    const auto side = order.get_char(54U);
    const auto order_qty = ExtractOrderQty(order);
    if (!cl_ord_id.has_value() || !side.has_value() || !order_qty.has_value()) {
        return fastfix::base::Status::InvalidArgument(
            "loopback benchmark NewOrderSingle missing required ack source fields");
    }

    const auto symbol = order.get_string(55U);
    const auto order_id = std::string("ORDER-") + std::to_string(execution_id);
    const auto exec_id = std::string("EXEC-") + std::to_string(execution_id);

    fastfix::message::MessageBuilder ack{"8"};
    ack.reserve_fields(10U)
        .set(35U, "8")
        .set(37U, order_id)
        .set(11U, cl_ord_id.value())
        .set(17U, exec_id)
        .set(150U, '0')
        .set(39U, '0')
        .set(54U, side.value())
        .set(151U, order_qty.value())
        .set(14U, 0.0)
        .set(6U, 0.0);
    if (symbol.has_value()) {
        ack.set(55U, symbol.value());
    }
    ack.set(38U, order_qty.value());
    return std::move(ack).build();
}

auto MakeEncodeTemplateConfig(const fastfix::codec::EncodeOptions& options) -> fastfix::codec::EncodeTemplateConfig {
    fastfix::codec::EncodeTemplateConfig config;
    config.begin_string = options.begin_string.empty() ? "FIX.4.4" : options.begin_string;
    config.sender_comp_id = options.sender_comp_id;
    config.target_comp_id = options.target_comp_id;
    config.default_appl_ver_id = options.default_appl_ver_id;
    config.delimiter = options.delimiter;
    return config;
}

auto RunEncodeBufferBenchmark(
    const Fix44BusinessOrder& business_order,
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    const fastfix::codec::EncodeOptions& base_options,
    std::uint32_t iterations,
    const fastfix::codec::FrameEncodeTemplate* precompiled_template = nullptr)
    -> fastfix::base::Result<BenchmarkResult> {
    BenchmarkResult result;
    result.samples_ns.reserve(iterations);
    BenchmarkMeasurement measurement;
    fastfix::codec::EncodeBuffer encode_buffer;
    auto options = base_options;
    fastfix::message::MessageBuilder builder{"D"};
    builder.reserve_fields(business_order.price.has_value() ? 7U : 6U).reserve_groups(1U).reserve_group_entries(453U, 1U);
    for (std::uint32_t index = 0; index < iterations; ++index) {
        const auto sample_started = std::chrono::steady_clock::now();
        options.msg_seq_num = index + 1U;
        builder.reset();
        PopulateFix44MessageBuilder(builder, business_order);
        auto status = precompiled_template == nullptr
            ? fastfix::codec::EncodeFixMessageToBuffer(builder.view(), dictionary, options, &encode_buffer)
            : precompiled_template->EncodeToBuffer(builder.view(), options, &encode_buffer);
        if (!status.ok()) {
            return status;
        }
        result.samples_ns.push_back(DurationNs(sample_started, std::chrono::steady_clock::now()));
    }
    measurement.Finish(result);
    return result;
}

auto BuildFix44GenericOrderBuilder(const Fix44BusinessOrder& order) -> fastfix::message::MessageBuilder {
    fastfix::message::MessageBuilder builder{"D"};
    builder.reserve_fields(order.price.has_value() ? 7U : 6U).reserve_groups(1U).reserve_group_entries(453U, 1U);
    PopulateFix44MessageBuilder(builder, order);
    return builder;
}

auto RunGenericBuilderEncodeBufferBenchmark(
    const Fix44BusinessOrder& business_order,
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    const fastfix::codec::EncodeOptions& base_options,
    std::uint32_t iterations,
    const fastfix::codec::PrecompiledTemplateTable* precompiled = nullptr) -> fastfix::base::Result<BenchmarkResult> {
    BenchmarkResult result;
    result.samples_ns.reserve(iterations);
    BenchmarkMeasurement measurement;
    fastfix::codec::EncodeBuffer encode_buffer;
    auto options = base_options;
    for (std::uint32_t index = 0; index < iterations; ++index) {
        const auto sample_started = std::chrono::steady_clock::now();
        options.msg_seq_num = index + 1U;
        auto builder = BuildFix44GenericOrderBuilder(business_order);
        auto status = builder.encode_to_buffer(dictionary, options, &encode_buffer, precompiled);
        if (!status.ok()) {
            return status;
        }
        result.samples_ns.push_back(DurationNs(sample_started, std::chrono::steady_clock::now()));
    }
    measurement.Finish(result);
    return result;
}

auto RunFixedLayoutBuilderEncodeBufferBenchmark(
    const Fix44BusinessOrder& business_order,
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    const fastfix::codec::EncodeOptions& base_options,
    std::uint32_t iterations,
    const fastfix::codec::PrecompiledTemplateTable* precompiled = nullptr) -> fastfix::base::Result<BenchmarkResult> {
    auto layout = fastfix::message::FixedLayout::Build(dictionary, "D");
    if (!layout.ok()) {
        return layout.status();
    }
    BenchmarkResult result;
    result.samples_ns.reserve(iterations);
    BenchmarkMeasurement measurement;
    fastfix::codec::EncodeBuffer encode_buffer;
    auto options = base_options;
    for (std::uint32_t index = 0; index < iterations; ++index) {
        const auto sample_started = std::chrono::steady_clock::now();
        options.msg_seq_num = index + 1U;
        fastfix::message::FixedLayoutWriter writer(layout.value());
        writer.set(11U, business_order.cl_ord_id);
        writer.set(55U, business_order.symbol);
        writer.set(54U, business_order.side);
        writer.set(60U, business_order.transact_time.text);
        writer.set(38U, business_order.order_qty);
        writer.set(40U, business_order.ord_type);
        if (business_order.price.has_value()) {
            writer.set(44U, business_order.price.value());
        }
        writer.reserve_group_entries(453U, 1U);
        auto party = writer.add_group_entry(453U);
        party.set(448U, business_order.party_id)
            .set(447U, business_order.party_id_source)
            .set(452U, business_order.party_role);
        auto status = writer.encode_to_buffer(dictionary, options, &encode_buffer, precompiled);
        if (!status.ok()) {
            return status;
        }
        result.samples_ns.push_back(DurationNs(sample_started, std::chrono::steady_clock::now()));
    }
    measurement.Finish(result);
    return result;
}

auto RunFixedLayoutHybridEncodeBufferBenchmark(
    const Fix44BusinessOrder& business_order,
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    const fastfix::codec::EncodeOptions& base_options,
    std::uint32_t iterations,
    const fastfix::codec::PrecompiledTemplateTable* precompiled = nullptr) -> fastfix::base::Result<BenchmarkResult> {
    auto layout = fastfix::message::FixedLayout::Build(dictionary, "D");
    if (!layout.ok()) {
        return layout.status();
    }
    BenchmarkResult result;
    result.samples_ns.reserve(iterations);
    BenchmarkMeasurement measurement;
    fastfix::codec::EncodeBuffer encode_buffer;
    auto options = base_options;
    for (std::uint32_t index = 0; index < iterations; ++index) {
        const auto sample_started = std::chrono::steady_clock::now();
        options.msg_seq_num = index + 1U;
        fastfix::message::FixedLayoutWriter writer(layout.value());
        writer.set(11U, business_order.cl_ord_id);
        writer.set(55U, business_order.symbol);
        writer.set(54U, business_order.side);
        writer.set(60U, business_order.transact_time.text);
        writer.set(38U, business_order.order_qty);
        writer.set(40U, business_order.ord_type);
        if (business_order.price.has_value()) {
            writer.set(44U, business_order.price.value());
        }
        writer.reserve_group_entries(453U, 1U);
        auto party = writer.add_group_entry(453U);
        party.set(448U, business_order.party_id)
            .set(447U, business_order.party_id_source)
            .set(452U, business_order.party_role);
        // Hybrid path: add extra fields not in the fixed layout.
        writer.set_extra_string(58U, "benchmark-extra-text");
        writer.set_extra_int(9999U, 42);
        auto status = writer.encode_to_buffer(dictionary, options, &encode_buffer, precompiled);
        if (!status.ok()) {
            return status;
        }
        result.samples_ns.push_back(DurationNs(sample_started, std::chrono::steady_clock::now()));
    }
    measurement.Finish(result);
    return result;
}

auto BuildFrame(
    const fastfix::message::Message& message,
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    std::string begin_string,
    std::string sender_comp_id,
    std::string target_comp_id,
    std::string default_appl_ver_id,
    std::uint32_t msg_seq_num) -> fastfix::base::Result<std::vector<std::byte>> {
    fastfix::codec::EncodeOptions options;
    options.begin_string = std::move(begin_string);
    options.sender_comp_id = std::move(sender_comp_id);
    options.target_comp_id = std::move(target_comp_id);
    options.default_appl_ver_id = std::move(default_appl_ver_id);
    options.msg_seq_num = msg_seq_num;
    return fastfix::codec::EncodeFixMessage(message, dictionary, options);
}

auto TotalFrameBytes(const std::vector<std::vector<std::byte>>& frames) -> std::size_t {
    std::size_t total = 0U;
    for (const auto& frame : frames) {
        total += frame.size();
    }
    return total;
}

auto ActivateProtocolPair(
    fastfix::session::AdminProtocol& initiator,
    fastfix::session::AdminProtocol& acceptor) -> fastfix::base::Status {
    auto acceptor_connected = acceptor.OnTransportConnected(NowNs());
    if (!acceptor_connected.ok()) {
        return acceptor_connected.status();
    }

    auto logon = initiator.OnTransportConnected(NowNs());
    if (!logon.ok()) {
        return logon.status();
    }

    for (const auto& outbound : logon.value().outbound_frames) {
        auto acceptor_event = acceptor.OnInbound(outbound.bytes, NowNs());
        if (!acceptor_event.ok()) {
            return acceptor_event.status();
        }
        for (const auto& response : acceptor_event.value().outbound_frames) {
            auto initiator_event = initiator.OnInbound(response.bytes, NowNs());
            if (!initiator_event.ok()) {
                return initiator_event.status();
            }
        }
    }

    if (initiator.session().state() != fastfix::session::SessionState::kActive ||
        acceptor.session().state() != fastfix::session::SessionState::kActive) {
        return fastfix::base::Status::InvalidArgument("failed to activate benchmark FIX session pair");
    }

    return fastfix::base::Status::Ok();
}

auto ReportFastFixMetric(const std::string& label, const BenchmarkResult& result) -> void {
    bench_support::ReportMetric(label, result, 44);
}

auto RunLoopbackBenchmark(
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    std::uint32_t iterations,
    std::string begin_string,
    std::string default_appl_ver_id) -> fastfix::base::Result<BenchmarkResult> {
    auto acceptor = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    if (!acceptor.ok()) {
        return acceptor.status();
    }
    const auto port = acceptor.value().port();
    const auto sample = BuildSampleMessage();
    auto sample_frame = BuildFrame(
        sample,
        dictionary,
        begin_string,
        "BUY",
        "SELL",
        default_appl_ver_id,
        2U);
    if (!sample_frame.ok()) {
        return sample_frame.status();
    }

    auto sample_order_ack = BuildFix44OrderAckFromNewOrder(sample.view(), 1U);
    if (!sample_order_ack.ok()) {
        return sample_order_ack.status();
    }
    auto sample_ack_frame = BuildFrame(
        sample_order_ack.value(),
        dictionary,
        begin_string,
        "SELL",
        "BUY",
        default_appl_ver_id,
        2U);
    if (!sample_ack_frame.ok()) {
        return sample_ack_frame.status();
    }

    const auto sample_frame_bytes = sample_frame.value().size();
    const auto sample_ack_frame_bytes = sample_ack_frame.value().size();
    const auto message_count = static_cast<std::size_t>(iterations);
    constexpr std::size_t kLoopbackAdminReserveRecords = 8U;
    constexpr std::size_t kLoopbackAdminReserveBytes = 4096U;
    const auto per_session_payload_bytes =
        (message_count * (sample_frame_bytes + sample_ack_frame_bytes)) + kLoopbackAdminReserveBytes;

    std::promise<fastfix::base::Status> acceptor_result;
    auto future = acceptor_result.get_future();

    std::jthread acceptor_thread([
        socket = std::move(acceptor).value(),
        &acceptor_result,
        &dictionary,
        begin_string,
        default_appl_ver_id,
        message_count,
        per_session_payload_bytes]() mutable {
        fastfix::store::MemorySessionStore store;
        store.ReserveAdditionalSessionStorage(
            42U,
            message_count + kLoopbackAdminReserveRecords,
            message_count + kLoopbackAdminReserveRecords,
            per_session_payload_bytes);
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 42U,
                    .key = fastfix::session::SessionKey{begin_string, "SELL", "BUY"},
                    .profile_id = dictionary.profile().header().profile_id,
                    .default_appl_ver_id = default_appl_ver_id,
                    .heartbeat_interval_seconds = 30U,
                    .is_initiator = false,
                },
                .begin_string = begin_string,
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .default_appl_ver_id = default_appl_ver_id,
                .heartbeat_interval_seconds = 30U,
            },
            dictionary,
            &store);

        auto connection = socket.Accept(std::chrono::seconds(10));
        if (!connection.ok()) {
            acceptor_result.set_value(connection.status());
            return;
        }
        auto start = protocol.OnTransportConnected(NowNs());
        if (!start.ok()) {
            acceptor_result.set_value(start.status());
            return;
        }

        std::uint32_t execution_id = 0U;

        while (true) {
            auto frame = connection.value().ReceiveFrameView(std::chrono::seconds(10));
            if (!frame.ok()) {
                acceptor_result.set_value(frame.status());
                return;
            }
            auto decoded = fastfix::codec::DecodeFixMessageView(frame.value(), dictionary);
            if (!decoded.ok()) {
                acceptor_result.set_value(decoded.status());
                return;
            }
            auto event = protocol.OnInbound(decoded.value(), NowNs());
            if (!event.ok()) {
                acceptor_result.set_value(event.status());
                return;
            }
            for (const auto& outbound : event.value().outbound_frames) {
                auto status = connection.value().Send(outbound.bytes, std::chrono::seconds(10));
                if (!status.ok()) {
                    acceptor_result.set_value(status);
                    return;
                }
            }
            for (const auto& app : event.value().application_messages) {
                auto order_ack = BuildFix44OrderAckFromNewOrder(app.view(), ++execution_id);
                if (!order_ack.ok()) {
                    acceptor_result.set_value(order_ack.status());
                    return;
                }
                auto ack = protocol.SendApplication(order_ack.value(), NowNs());
                if (!ack.ok()) {
                    acceptor_result.set_value(ack.status());
                    return;
                }
                auto status = connection.value().Send(ack.value().bytes, std::chrono::seconds(10));
                if (!status.ok()) {
                    acceptor_result.set_value(status);
                    return;
                }
            }
            if (event.value().disconnect) {
                protocol.OnTransportClosed();
                connection.value().Close();
                acceptor_result.set_value(fastfix::base::Status::Ok());
                return;
            }
        }
    });

    auto connection = fastfix::transport::TcpConnection::Connect("127.0.0.1", port, std::chrono::seconds(10));
    if (!connection.ok()) {
        return connection.status();
    }

    fastfix::store::MemorySessionStore initiator_store;
    fastfix::session::AdminProtocol initiator(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 24U,
                .key = fastfix::session::SessionKey{begin_string, "BUY", "SELL"},
                .profile_id = dictionary.profile().header().profile_id,
                .default_appl_ver_id = default_appl_ver_id,
                .heartbeat_interval_seconds = 30U,
                .is_initiator = true,
            },
            .begin_string = begin_string,
            .sender_comp_id = "BUY",
            .target_comp_id = "SELL",
            .default_appl_ver_id = default_appl_ver_id,
            .heartbeat_interval_seconds = 30U,
        },
        dictionary,
        &initiator_store);

    auto start = initiator.OnTransportConnected(NowNs());
    if (!start.ok()) {
        return start.status();
    }
    for (const auto& outbound : start.value().outbound_frames) {
        auto status = connection.value().Send(outbound.bytes, std::chrono::seconds(10));
        if (!status.ok()) {
            return status;
        }
    }

    bool active = false;
    while (!active) {
        auto frame = connection.value().ReceiveFrameView(std::chrono::seconds(10));
        if (!frame.ok()) {
            return frame.status();
        }
        auto decoded = fastfix::codec::DecodeFixMessageView(frame.value(), dictionary);
        if (!decoded.ok()) {
            return decoded.status();
        }
        auto event = initiator.OnInbound(decoded.value(), NowNs());
        if (!event.ok()) {
            return event.status();
        }
        for (const auto& outbound : event.value().outbound_frames) {
            auto status = connection.value().Send(outbound.bytes, std::chrono::seconds(10));
            if (!status.ok()) {
                return status;
            }
        }
        active = event.value().session_active;
    }

    initiator_store.ReserveAdditionalSessionStorage(
        24U,
        message_count + kLoopbackAdminReserveRecords,
        message_count + kLoopbackAdminReserveRecords,
        per_session_payload_bytes);

    std::uint32_t acknowledged = 0;
    BenchmarkResult result;
    result.samples_ns.reserve(iterations);
    BenchmarkMeasurement measurement;
    for (std::uint32_t index = 0; index < iterations; ++index) {
        const auto started = std::chrono::steady_clock::now();
        auto outbound = initiator.SendApplication(sample, NowNs());
        if (!outbound.ok()) {
            return outbound.status();
        }
        auto status = connection.value().Send(outbound.value().bytes, std::chrono::seconds(10));
        if (!status.ok()) {
            return status;
        }

        while (acknowledged <= index) {
            auto frame = connection.value().ReceiveFrameView(std::chrono::seconds(10));
            if (!frame.ok()) {
                return frame.status();
            }
            auto decoded = fastfix::codec::DecodeFixMessageView(frame.value(), dictionary);
            if (!decoded.ok()) {
                return decoded.status();
            }
            auto event = initiator.OnInbound(decoded.value(), NowNs());
            if (!event.ok()) {
                return event.status();
            }
            for (const auto& outbound_frame : event.value().outbound_frames) {
                status = connection.value().Send(outbound_frame.bytes, std::chrono::seconds(10));
                if (!status.ok()) {
                    return status;
                }
            }
            for (const auto& app : event.value().application_messages) {
                if (app.view().msg_type() != "8") {
                    return fastfix::base::Status::InvalidArgument(
                        "loopback benchmark expected ExecutionReport order ack");
                }
                ++acknowledged;
            }
        }
        result.samples_ns.push_back(DurationNs(started, std::chrono::steady_clock::now()));
    }
    measurement.Finish(result);

    auto logout = initiator.BeginLogout({}, NowNs());
    if (!logout.ok()) {
        return logout.status();
    }
    auto status = connection.value().Send(logout.value().bytes, std::chrono::seconds(10));
    if (!status.ok()) {
        return status;
    }
    auto logout_ack = connection.value().ReceiveFrameView(std::chrono::seconds(10));
    if (!logout_ack.ok()) {
        return logout_ack.status();
    }
    auto decoded = fastfix::codec::DecodeFixMessageView(logout_ack.value(), dictionary);
    if (!decoded.ok()) {
        return decoded.status();
    }
    auto event = initiator.OnInbound(decoded.value(), NowNs());
    if (!event.ok()) {
        return event.status();
    }

    initiator.OnTransportClosed();
    connection.value().Close();
    const auto peer_status = future.get();
    if (!peer_status.ok()) {
        return peer_status;
    }

    return result;
}

auto RunReplayBenchmark(
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    std::uint32_t iterations,
    std::uint32_t replay_span,
    std::string begin_string,
    std::string default_appl_ver_id) -> fastfix::base::Result<BenchmarkResult> {
    if (replay_span == 0U) {
        return fastfix::base::Status::InvalidArgument("replay benchmark requires replay_span > 0");
    }

    fastfix::store::MemorySessionStore acceptor_store;
    fastfix::store::MemorySessionStore initiator_store;
    fastfix::session::AdminProtocol acceptor(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 420U,
                .key = fastfix::session::SessionKey{begin_string, "SELL", "BUY"},
                .profile_id = dictionary.profile().header().profile_id,
                .default_appl_ver_id = default_appl_ver_id,
                .heartbeat_interval_seconds = 30U,
                .is_initiator = false,
            },
            .begin_string = begin_string,
            .sender_comp_id = "SELL",
            .target_comp_id = "BUY",
            .default_appl_ver_id = default_appl_ver_id,
            .heartbeat_interval_seconds = 30U,
        },
        dictionary,
        &acceptor_store);
    fastfix::session::AdminProtocol initiator(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 240U,
                .key = fastfix::session::SessionKey{begin_string, "BUY", "SELL"},
                .profile_id = dictionary.profile().header().profile_id,
                .default_appl_ver_id = default_appl_ver_id,
                .heartbeat_interval_seconds = 30U,
                .is_initiator = true,
            },
            .begin_string = begin_string,
            .sender_comp_id = "BUY",
            .target_comp_id = "SELL",
            .default_appl_ver_id = default_appl_ver_id,
            .heartbeat_interval_seconds = 30U,
        },
        dictionary,
        &initiator_store);

    auto status = ActivateProtocolPair(initiator, acceptor);
    if (!status.ok()) {
        return status;
    }

    const auto sample = BuildSampleMessage();
    for (std::uint32_t index = 0; index < replay_span; ++index) {
        auto outbound = acceptor.SendApplication(sample, NowNs());
        if (!outbound.ok()) {
            return outbound.status();
        }
    }

    fastfix::message::MessageBuilder resend_request_builder{"2"};
    resend_request_builder.set(35U, "2")
        .set(7U, static_cast<std::int64_t>(2))
        .set(16U, static_cast<std::int64_t>(replay_span + 1U));
    const auto resend_request = std::move(resend_request_builder).build();

    std::vector<std::vector<std::byte>> requests;
    requests.reserve(iterations);
    for (std::uint32_t index = 0; index < iterations; ++index) {
        auto encoded = BuildFrame(
            resend_request,
            dictionary,
            begin_string,
            "BUY",
            "SELL",
            default_appl_ver_id,
            index + 2U);
        if (!encoded.ok()) {
            return encoded.status();
        }
        requests.push_back(std::move(encoded).value());
    }
    acceptor_store.ReserveAdditionalSessionStorage(
        420U,
        requests.size(),
        0U,
        TotalFrameBytes(requests));
    acceptor.ReserveReplayStorage(replay_span);

    BenchmarkResult result;
    result.samples_ns.reserve(iterations);
    result.work_label = "frames";

    BenchmarkMeasurement measurement;
    for (auto& request : requests) {
        const auto started = std::chrono::steady_clock::now();
        auto event = acceptor.OnInbound(std::move(request), NowNs());
        const auto finished = std::chrono::steady_clock::now();
        if (!event.ok()) {
            return event.status();
        }
        result.samples_ns.push_back(DurationNs(started, finished));
        result.work_count += static_cast<std::uint64_t>(event.value().outbound_frames.size());
    }
    measurement.Finish(result);
    return result;
}

auto RunSessionBenchmark(
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    std::uint32_t iterations,
    std::string begin_string,
    std::string default_appl_ver_id) -> fastfix::base::Result<BenchmarkResult> {
    fastfix::store::MemorySessionStore acceptor_store;
    fastfix::store::MemorySessionStore initiator_store;
    fastfix::session::AdminProtocol acceptor(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 421U,
                .key = fastfix::session::SessionKey{begin_string, "SELL", "BUY"},
                .profile_id = dictionary.profile().header().profile_id,
                .default_appl_ver_id = default_appl_ver_id,
                .heartbeat_interval_seconds = 30U,
                .is_initiator = false,
            },
            .begin_string = begin_string,
            .sender_comp_id = "SELL",
            .target_comp_id = "BUY",
            .default_appl_ver_id = default_appl_ver_id,
            .heartbeat_interval_seconds = 30U,
        },
        dictionary,
        &acceptor_store);
    fastfix::session::AdminProtocol initiator(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 241U,
                .key = fastfix::session::SessionKey{begin_string, "BUY", "SELL"},
                .profile_id = dictionary.profile().header().profile_id,
                .default_appl_ver_id = default_appl_ver_id,
                .heartbeat_interval_seconds = 30U,
                .is_initiator = true,
            },
            .begin_string = begin_string,
            .sender_comp_id = "BUY",
            .target_comp_id = "SELL",
            .default_appl_ver_id = default_appl_ver_id,
            .heartbeat_interval_seconds = 30U,
        },
        dictionary,
        &initiator_store);

    auto status = ActivateProtocolPair(initiator, acceptor);
    if (!status.ok()) {
        return status;
    }

    const auto sample = BuildSampleMessage();
    std::vector<std::vector<std::byte>> inbound_frames;
    inbound_frames.reserve(iterations);
    for (std::uint32_t index = 0; index < iterations; ++index) {
        auto frame = BuildFrame(
            sample,
            dictionary,
            begin_string,
            "BUY",
            "SELL",
            default_appl_ver_id,
            index + 2U);
        if (!frame.ok()) {
            return frame.status();
        }
        inbound_frames.push_back(std::move(frame).value());
    }
    acceptor_store.ReserveAdditionalSessionStorage(
        421U,
        inbound_frames.size(),
        0U,
        TotalFrameBytes(inbound_frames));

    BenchmarkResult result;
    result.samples_ns.reserve(iterations);
    result.work_label = "messages";

    BenchmarkMeasurement measurement;
    for (auto& frame : inbound_frames) {
        const auto started = std::chrono::steady_clock::now();
        auto event = acceptor.OnInbound(std::move(frame), NowNs());
        const auto finished = std::chrono::steady_clock::now();
        if (!event.ok()) {
            return event.status();
        }
        result.samples_ns.push_back(DurationNs(started, finished));
        result.work_count += static_cast<std::uint64_t>(event.value().application_messages.size());
    }
    measurement.Finish(result);
    return result;
}

}  // namespace

void* operator new(std::size_t size) {
    auto* memory = bench_support::AllocateRaw(size, alignof(std::max_align_t));
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    bench_support::RecordAllocation(size);
    return memory;
}

void* operator new[](std::size_t size) {
    auto* memory = bench_support::AllocateRaw(size, alignof(std::max_align_t));
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    bench_support::RecordAllocation(size);
    return memory;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    auto* memory = bench_support::AllocateRaw(size, alignof(std::max_align_t));
    if (memory != nullptr) {
        bench_support::RecordAllocation(size);
    }
    return memory;
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    auto* memory = bench_support::AllocateRaw(size, alignof(std::max_align_t));
    if (memory != nullptr) {
        bench_support::RecordAllocation(size);
    }
    return memory;
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    auto* memory = bench_support::AllocateRaw(size, static_cast<std::size_t>(alignment));
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    bench_support::RecordAllocation(size);
    return memory;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    auto* memory = bench_support::AllocateRaw(size, static_cast<std::size_t>(alignment));
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    bench_support::RecordAllocation(size);
    return memory;
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    auto* memory = bench_support::AllocateRaw(size, static_cast<std::size_t>(alignment));
    if (memory != nullptr) {
        bench_support::RecordAllocation(size);
    }
    return memory;
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    auto* memory = bench_support::AllocateRaw(size, static_cast<std::size_t>(alignment));
    if (memory != nullptr) {
        bench_support::RecordAllocation(size);
    }
    return memory;
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}

int main(int argc, char** argv) {
    std::filesystem::path artifact_path;
    std::vector<std::filesystem::path> dictionary_paths;
    std::uint32_t iterations = 100000U;
    std::uint32_t loopback_iterations = 1000U;
    std::uint32_t replay_iterations = 1000U;
    std::uint32_t replay_span = 128U;
    std::string begin_string{"FIX.4.4"};
    std::string default_appl_ver_id;

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        if (arg == "--artifact" && index + 1 < argc) {
            artifact_path = argv[++index];
            continue;
        }
        if (arg == "--dictionary" && index + 1 < argc) {
            dictionary_paths.emplace_back(argv[++index]);
            continue;
        }
        if (arg == "--iterations" && index + 1 < argc) {
            iterations = static_cast<std::uint32_t>(std::stoul(argv[++index]));
            continue;
        }
        if (arg == "--loopback" && index + 1 < argc) {
            loopback_iterations = static_cast<std::uint32_t>(std::stoul(argv[++index]));
            continue;
        }
        if (arg == "--replay" && index + 1 < argc) {
            replay_iterations = static_cast<std::uint32_t>(std::stoul(argv[++index]));
            continue;
        }
        if (arg == "--replay-span" && index + 1 < argc) {
            replay_span = static_cast<std::uint32_t>(std::stoul(argv[++index]));
            continue;
        }
        if (arg == "--begin-string" && index + 1 < argc) {
            begin_string = argv[++index];
            continue;
        }
        if (arg == "--default-appl-ver-id" && index + 1 < argc) {
            default_appl_ver_id = argv[++index];
            continue;
        }
        PrintUsage();
        return 1;
    }

    if (!artifact_path.empty() && !dictionary_paths.empty()) {
        PrintUsage();
        return 1;
    }

    if (artifact_path.empty() && dictionary_paths.empty()) {
        artifact_path = std::filesystem::path(FASTFIX_PROJECT_DIR) / "build/sample-basic.art";
    } else if (!artifact_path.is_absolute()) {
        artifact_path = std::filesystem::path(FASTFIX_PROJECT_DIR) / artifact_path;
    }

    for (auto& dictionary_path : dictionary_paths) {
        if (!dictionary_path.is_absolute()) {
            dictionary_path = std::filesystem::path(FASTFIX_PROJECT_DIR) / dictionary_path;
        }
    }

    auto dictionary = LoadDictionary(artifact_path, dictionary_paths);
    if (!dictionary.ok()) {
        std::cerr << dictionary.status().message() << '\n';
        return 1;
    }

    const auto fix44_business_order = BuildFix44BusinessOrder();
    const auto fix44_business_order_no_price = BuildFix44BusinessOrder(false);
    const auto sample = BuildFix44MessageFromBusinessOrder(fix44_business_order);
    const auto sample_no_price = BuildFix44MessageFromBusinessOrder(fix44_business_order_no_price);
    fastfix::codec::EncodeOptions options;
    options.begin_string = begin_string;
    options.sender_comp_id = "BUY";
    options.target_comp_id = "SELL";
    options.default_appl_ver_id = default_appl_ver_id;

    auto fixed_time_options = options;
    fixed_time_options.sending_time = "20260406-12:34:56.789";

    auto warmup = fastfix::codec::EncodeFixMessage(sample, dictionary.value(), options);
    if (!warmup.ok()) {
        std::cerr << warmup.status().message() << '\n';
        return 1;
    }

    auto precompiled_template = fastfix::codec::CompileFrameEncodeTemplate(
        dictionary.value(), sample.view().msg_type(), MakeEncodeTemplateConfig(options));
    if (!precompiled_template.ok()) {
        std::cerr << precompiled_template.status().message() << '\n';
        return 1;
    }

    fastfix::codec::EncodeBuffer split_warmup_buffer;
    auto split_warmup = fastfix::codec::EncodeFixMessageToBuffer(sample, dictionary.value(), fixed_time_options, &split_warmup_buffer);
    if (!split_warmup.ok()) {
        std::cerr << split_warmup.message() << '\n';
        return 1;
    }
    auto template_warmup = precompiled_template.value().EncodeToBuffer(sample, fixed_time_options, &split_warmup_buffer);
    if (!template_warmup.ok()) {
        std::cerr << template_warmup.message() << '\n';
        return 1;
    }
    auto template_no_float_warmup = precompiled_template.value().EncodeToBuffer(sample_no_price, fixed_time_options, &split_warmup_buffer);
    if (!template_no_float_warmup.ok()) {
        std::cerr << template_no_float_warmup.message() << '\n';
        return 1;
    }

    BenchmarkResult encode_result;
    encode_result.samples_ns.reserve(iterations);
    BenchmarkMeasurement encode_measurement;
    for (std::uint32_t index = 0; index < iterations; ++index) {
        const auto sample_started = std::chrono::steady_clock::now();
        options.msg_seq_num = index + 1U;
        auto business_message = BuildFix44MessageFromBusinessOrder(fix44_business_order);
        auto encoded = fastfix::codec::EncodeFixMessage(business_message, dictionary.value(), options);
        if (!encoded.ok()) {
            std::cerr << encoded.status().message() << '\n';
            return 1;
        }
        encode_result.samples_ns.push_back(DurationNs(sample_started, std::chrono::steady_clock::now()));
    }
    encode_measurement.Finish(encode_result);
    ReportFastFixMetric("encode", encode_result);

    auto encode_buffer_result = RunEncodeBufferBenchmark(fix44_business_order, dictionary.value(), options, iterations);
    if (!encode_buffer_result.ok()) {
        std::cerr << encode_buffer_result.status().message() << '\n';
        return 1;
    }
    ReportFastFixMetric("encode-buffer", encode_buffer_result.value());

    auto encode_buffer_fixed_time = RunEncodeBufferBenchmark(
        fix44_business_order,
        dictionary.value(),
        fixed_time_options,
        iterations);
    if (!encode_buffer_fixed_time.ok()) {
        std::cerr << encode_buffer_fixed_time.status().message() << '\n';
        return 1;
    }
    ReportFastFixMetric("encode-buffer-time-fixed", encode_buffer_fixed_time.value());

    auto encode_buffer_precompiled = RunEncodeBufferBenchmark(
        fix44_business_order, dictionary.value(), options, iterations, &precompiled_template.value());
    if (!encode_buffer_precompiled.ok()) {
        std::cerr << encode_buffer_precompiled.status().message() << '\n';
        return 1;
    }
    ReportFastFixMetric("encode-buffer-template", encode_buffer_precompiled.value());

    auto encode_buffer_fixed_time_template = RunEncodeBufferBenchmark(
        fix44_business_order,
        dictionary.value(),
        fixed_time_options,
        iterations,
        &precompiled_template.value());
    if (!encode_buffer_fixed_time_template.ok()) {
        std::cerr << encode_buffer_fixed_time_template.status().message() << '\n';
        return 1;
    }
    ReportFastFixMetric("encode-buffer-time-fixed-template", encode_buffer_fixed_time_template.value());

    auto encode_buffer_fixed_time_template_no_float = RunEncodeBufferBenchmark(
        fix44_business_order_no_price,
        dictionary.value(),
        fixed_time_options,
        iterations,
        &precompiled_template.value());
    if (!encode_buffer_fixed_time_template_no_float.ok()) {
        std::cerr << encode_buffer_fixed_time_template_no_float.status().message() << '\n';
        return 1;
    }
    ReportFastFixMetric(
        "encode-buffer-time-fixed-template-no-float",
        encode_buffer_fixed_time_template_no_float.value());

    {
        auto precompiled_table = fastfix::codec::PrecompiledTemplateTable::Build(
            dictionary.value(), MakeEncodeTemplateConfig(options));
        if (precompiled_table.ok()) {
            BenchmarkResult table_result;
            table_result.samples_ns.reserve(iterations);
            BenchmarkMeasurement table_measurement;
            fastfix::codec::EncodeBuffer table_buffer;
            auto table_options = fixed_time_options;
            for (std::uint32_t index = 0; index < iterations; ++index) {
                const auto sample_started = std::chrono::steady_clock::now();
                table_options.msg_seq_num = index + 1U;
                auto business_message = BuildFix44MessageFromBusinessOrder(fix44_business_order);
                auto status = fastfix::codec::EncodeFixMessageToBuffer(
                    business_message,
                    dictionary.value(),
                    table_options,
                    &table_buffer,
                    &precompiled_table.value());
                if (!status.ok()) {
                    std::cerr << status.message() << '\n';
                    return 1;
                }
                table_result.samples_ns.push_back(DurationNs(sample_started, std::chrono::steady_clock::now()));
            }
            table_measurement.Finish(table_result);
            ReportFastFixMetric("encode-buffer-precompiled-table", table_result);
        }
    }

    // Object-to-wire encode compare: three FastFix paths for FIX4.4 NewOrderSingle.
    // Compare these against quickfix-encode / quickfix-encode-buffer from bench/quickfix_main.cpp.
    std::cout << "encode-compare uses FIX4.4 NewOrderSingle (D) with fixed SendingTime and business-object to wire timing\n";
    std::cout << "compare builder-generic-e2e-buffer / builder-fixed-layout-buffer / builder-fixed-layout-hybrid-buffer against QuickFIX encode metrics\n";

    auto builder_precompiled = fastfix::codec::PrecompiledTemplateTable::Build(
        dictionary.value(), MakeEncodeTemplateConfig(fixed_time_options));
    const fastfix::codec::PrecompiledTemplateTable* builder_precompiled_ptr =
        builder_precompiled.ok() ? &builder_precompiled.value() : nullptr;

    auto generic_builder_result = RunGenericBuilderEncodeBufferBenchmark(
        fix44_business_order, dictionary.value(), fixed_time_options, iterations, builder_precompiled_ptr);
    if (!generic_builder_result.ok()) {
        std::cerr << generic_builder_result.status().message() << '\n';
        return 1;
    }
    ReportFastFixMetric("builder-generic-e2e-buffer", generic_builder_result.value());

    auto fixed_layout_result = RunFixedLayoutBuilderEncodeBufferBenchmark(
        fix44_business_order, dictionary.value(), fixed_time_options, iterations, builder_precompiled_ptr);
    if (!fixed_layout_result.ok()) {
        std::cerr << fixed_layout_result.status().message() << '\n';
        return 1;
    }
    ReportFastFixMetric("builder-fixed-layout-buffer", fixed_layout_result.value());

    auto hybrid_layout_result = RunFixedLayoutHybridEncodeBufferBenchmark(
        fix44_business_order, dictionary.value(), fixed_time_options, iterations, builder_precompiled_ptr);
    if (!hybrid_layout_result.ok()) {
        std::cerr << hybrid_layout_result.status().message() << '\n';
        return 1;
    }
    ReportFastFixMetric("builder-fixed-layout-hybrid-buffer", hybrid_layout_result.value());

    BenchmarkResult peek_result;
    peek_result.samples_ns.reserve(iterations);
    BenchmarkMeasurement peek_measurement;
    for (std::uint32_t index = 0; index < iterations; ++index) {
        const auto sample_started = std::chrono::steady_clock::now();
        auto header = fastfix::codec::PeekSessionHeaderView(warmup.value());
        if (!header.ok()) {
            std::cerr << header.status().message() << '\n';
            return 1;
        }
        peek_result.samples_ns.push_back(DurationNs(sample_started, std::chrono::steady_clock::now()));
    }
    peek_measurement.Finish(peek_result);
    ReportFastFixMetric("peek", peek_result);

    BenchmarkResult parse_result;
    parse_result.samples_ns.reserve(iterations);
    BenchmarkMeasurement parse_measurement;
    for (std::uint32_t index = 0; index < iterations; ++index) {
        const auto sample_started = std::chrono::steady_clock::now();
        auto decoded = fastfix::codec::DecodeFixMessageView(warmup.value(), dictionary.value());
        if (!decoded.ok()) {
            std::cerr << decoded.status().message() << '\n';
            return 1;
        }
        parse_result.samples_ns.push_back(DurationNs(sample_started, std::chrono::steady_clock::now()));
    }
    parse_measurement.Finish(parse_result);
    ReportFastFixMetric("parse", parse_result);

    auto session_benchmark =
        RunSessionBenchmark(dictionary.value(), iterations, begin_string, default_appl_ver_id);
    if (!session_benchmark.ok()) {
        std::cerr << session_benchmark.status().message() << '\n';
        return 1;
    }
    ReportFastFixMetric("session-inbound", session_benchmark.value());

    if (replay_iterations > 0U) {
        auto replay = RunReplayBenchmark(
            dictionary.value(),
            replay_iterations,
            replay_span,
            begin_string,
            default_appl_ver_id);
        if (!replay.ok()) {
            std::cerr << replay.status().message() << '\n';
            return 1;
        }
        ReportFastFixMetric("replay", replay.value());
    } else {
        std::cout << "replay skipped: --replay 0\n";
    }

    if (loopback_iterations > 0U) {
        auto loopback = RunLoopbackBenchmark(dictionary.value(), loopback_iterations, begin_string, default_appl_ver_id);
        if (!loopback.ok()) {
            std::cerr << loopback.status().message() << '\n';
            return 1;
        }
        ReportFastFixMetric("loopback-roundtrip", loopback.value());
    } else {
        std::cout << "loopback skipped: --loopback 0\n";
    }
    return 0;
}