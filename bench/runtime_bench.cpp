#include <nimblefix/engine.hpp>
#include <nimblefix/message.hpp>

#include "perf_counters.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>

namespace nf = nimblefix;

namespace {

using Clock = std::chrono::steady_clock;

enum class SendSource { callback, mpsc, lane };

struct Options {
    std::string mode;
    SendSource send_source{SendSource::callback};
    std::filesystem::path state_directory;
    std::filesystem::path sample_csv;
    std::size_t samples{};
    std::size_t warmup{};
    std::size_t producers{};
    std::size_t payload_bytes{};
    std::size_t rate{};
    std::size_t journal_segment_bytes{};
    std::size_t max_journal_segments{};
    std::size_t retain_outbound_messages{};
    bool diagnostic_pass{};
};

struct Sample {
    std::atomic<std::uint64_t> scheduled_ns{0};
    std::atomic<std::uint64_t> origin_ns{0};
    std::atomic<std::uint64_t> api_start_ns{0};
    std::atomic<std::uint64_t> accepted_ns{0};
    std::atomic<std::uint64_t> kernel_write_return_ns{0};
    std::atomic<std::uint64_t> ingress_origin_ns{0};
    std::atomic<std::uint64_t> callback_start_ns{0};
    std::atomic<std::uint64_t> callback_end_ns{0};
    std::atomic<std::uint64_t> send_receipt_id{0};
    std::atomic<std::size_t> producer_lane_index{0};
    std::atomic<int> receipt_state{static_cast<int>(nf::ReceiptState::accepted)};
    std::atomic<bool> ready{false};
    std::atomic<int> error_code{0};
};

struct LaneWorker {
    nf::ProducerLane<> lane;
    std::size_t producer_index{};
    std::size_t producer_count{};
    std::size_t count{};
    std::size_t offset{};
    bool diagnostic_pass{};
    std::uint64_t schedule_start_ns{};
    std::uint64_t interval_ns{};
    std::atomic<std::uint64_t> command_epoch{0};
    std::atomic<std::uint64_t> completed_epoch{0};
    std::atomic<bool> stopping{false};
    std::atomic<int> error_code{0};
    std::thread thread;
};

using RequestShape = nf::Fields<nf::Field<11, nf::FieldKind::string>,
    nf::Field<55, nf::FieldKind::string>, nf::Field<58, nf::FieldKind::string>,
    nf::Field<900001, nf::FieldKind::string>, nf::Field<900003, nf::FieldKind::string>>;
using ReplyShape = nf::Fields<nf::Field<11, nf::FieldKind::string>,
    nf::Field<17, nf::FieldKind::string>, nf::Field<58, nf::FieldKind::string>,
    nf::Field<900001, nf::FieldKind::string>>;
using FixedRequest = nf::FixedMessage<"D", RequestShape, 66'000>;
using FixedReply = nf::FixedMessage<"8", ReplyShape, 66'000>;

template <std::size_t N>
std::string_view format_u64(std::uint64_t value, std::array<char, N>& output) noexcept {
    const auto converted = std::to_chars(output.data(), output.data() + output.size(), value);
    if (converted.ec != std::errc{}) return {};
    return {output.data(), static_cast<std::size_t>(converted.ptr - output.data())};
}

struct Counters {
    std::atomic<std::uint64_t> allocations{0};
    std::atomic<std::uint64_t> allocated_bytes{0};
    std::atomic<std::uint64_t> mutex_locks{0};
    std::atomic<std::uint64_t> fsync_calls{0};
    std::atomic<std::uint64_t> fdatasync_calls{0};
    std::atomic<std::uint64_t> msync_calls{0};
};

std::atomic<bool> tracking{false};
Counters counters{};
std::atomic<bool> kernel_send_tracking{false};
std::atomic<std::uint64_t> kernel_send_epoch{0};
std::atomic<Sample*> kernel_send_samples{nullptr};
std::atomic<std::size_t> kernel_send_sample_count{0};
std::atomic<char> kernel_send_expected_type{0};
std::atomic<std::uint64_t> kernel_send_completed_frames{0};

struct SendFrameTracker {
    std::array<char, 512> prefix{};
    std::size_t prefix_size{};
    std::size_t field_start{};
    std::size_t frame_bytes{};
    std::size_t frame_body_start{};
    std::size_t frame_total{};
    std::uint64_t message_id{};
    char message_type{};
    bool have_body_length{};
    bool have_message_id{};
    bool have_message_type{};
    std::uint64_t epoch{};

    void reset_frame() noexcept {
        prefix_size = 0;
        field_start = 0;
        frame_bytes = 0;
        frame_body_start = 0;
        frame_total = 0;
        message_id = 0;
        message_type = 0;
        have_body_length = false;
        have_message_id = false;
        have_message_type = false;
    }

    void completed(std::uint64_t send_return_ns) noexcept {
        if (!have_body_length || !have_message_id || !have_message_type ||
            message_type != kernel_send_expected_type.load(std::memory_order_relaxed)) return;
        auto* samples = kernel_send_samples.load(std::memory_order_acquire);
        const auto count = kernel_send_sample_count.load(std::memory_order_relaxed);
        if (samples == nullptr || message_id >= count) return;
        std::uint64_t empty = 0;
        if (samples[message_id].kernel_write_return_ns.compare_exchange_strong(
                empty, send_return_ns, std::memory_order_release, std::memory_order_relaxed))
            kernel_send_completed_frames.fetch_add(1, std::memory_order_relaxed);
    }

    void field_terminated() noexcept {
        if (prefix_size == 0 || field_start >= prefix_size) return;
        const auto field_size = prefix_size - field_start - 1;
        const char* first = prefix.data() + field_start;
        const char* last = first + field_size;
        const auto equals = std::find(first, last, '=');
        if (equals == last) return;
        std::uint32_t tag{};
        const auto parsed_tag = std::from_chars(first, equals, tag);
        if (parsed_tag.ec != std::errc{} || parsed_tag.ptr != equals) return;
        const char* value = equals + 1;
        const auto value_size = static_cast<std::size_t>(last - value);
        if (tag == 9) {
            std::size_t body_length{};
            const auto parsed_length = std::from_chars(value, last, body_length);
            if (parsed_length.ec == std::errc{} && parsed_length.ptr == last &&
                body_length <= 1U * 1024U * 1024U &&
                frame_bytes <= std::numeric_limits<std::size_t>::max() - body_length - 7) {
                have_body_length = true;
                frame_body_start = frame_bytes;
                frame_total = frame_body_start + body_length + 7;
            }
        } else if (tag == 35 && value_size == 1) {
            message_type = value[0];
            have_message_type = true;
        } else if (tag == 11) {
            std::uint64_t id{};
            const auto parsed_id = std::from_chars(value, last, id);
            if (parsed_id.ec == std::errc{} && parsed_id.ptr == last) {
                message_id = id;
                have_message_id = true;
            }
        }
    }

    void consume(const char* bytes, std::size_t count, std::uint64_t send_return_ns) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            const char byte = bytes[index];
            ++frame_bytes;
            if (prefix_size < prefix.size()) prefix[prefix_size++] = byte;
            if (byte == nf::soh) {
                field_terminated();
                field_start = prefix_size;
            }
            if (frame_total != 0 && frame_bytes == frame_total) {
                completed(send_return_ns);
                reset_frame();
            }
        }
    }
};

thread_local SendFrameTracker send_frame_tracker{};

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count());
}

void begin_kernel_send_probe(Sample* samples, std::size_t count, char expected_type) noexcept {
    kernel_send_samples.store(samples, std::memory_order_release);
    kernel_send_sample_count.store(count, std::memory_order_relaxed);
    kernel_send_expected_type.store(expected_type, std::memory_order_relaxed);
    kernel_send_completed_frames.store(0, std::memory_order_relaxed);
    kernel_send_epoch.fetch_add(1, std::memory_order_release);
    kernel_send_tracking.store(true, std::memory_order_release);
}

void end_kernel_send_probe() noexcept {
    kernel_send_tracking.store(false, std::memory_order_release);
}

void observe_successful_send(const void* buffer, std::size_t size, std::uint64_t send_return_ns) noexcept {
    const auto epoch = kernel_send_epoch.load(std::memory_order_acquire);
    if (send_frame_tracker.epoch != epoch) {
        send_frame_tracker.reset_frame();
        send_frame_tracker.epoch = epoch;
    }
    send_frame_tracker.consume(static_cast<const char*>(buffer),
        size, send_return_ns);
}

bool parse_size(std::string_view value, std::size_t& output) noexcept {
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

bool parse_u64(std::string_view value, std::uint64_t& output) noexcept {
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

bool field_u64(const nf::FlatMessageView& message, std::uint32_t tag,
    std::uint64_t& value) noexcept {
    const auto field = message.find_unique(tag);
    return field.ok() && parse_u64(field.value().value, value);
}

bool field_text(const nf::FlatMessageView& message, std::uint32_t tag,
    std::string_view& value) noexcept {
    const auto field = message.find_unique(tag);
    if (!field.ok()) return false;
    value = field.value().value;
    return true;
}

bool wait_for_callbacks(const std::atomic<std::size_t>& completed,
    std::size_t target, std::chrono::seconds timeout) noexcept {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (completed.load(std::memory_order_acquire) >= target) return true;
        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
    return completed.load(std::memory_order_acquire) >= target;
}

bool wait_for_kernel_frames(std::size_t target, std::chrono::seconds timeout) noexcept {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (kernel_send_completed_frames.load(std::memory_order_acquire) >= target) return true;
        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
    return kernel_send_completed_frames.load(std::memory_order_acquire) >= target;
}

void producer_lane_loop(LaneWorker& worker, Sample* samples,
    std::string_view payload) noexcept {
    std::uint64_t observed_epoch = 0;
    for (;;) {
        const auto epoch = worker.command_epoch.load(std::memory_order_acquire);
        if (epoch == observed_epoch) {
            if (worker.stopping.load(std::memory_order_acquire)) return;
            worker.command_epoch.wait(observed_epoch, std::memory_order_acquire);
            continue;
        }
        observed_epoch = epoch;
        if (worker.stopping.load(std::memory_order_acquire)) return;
        for (std::size_t local_index = worker.producer_index;
             local_index < worker.count; local_index += worker.producer_count) {
            const auto id = worker.offset + local_index;
            auto& sample = samples[id];
            const auto scheduled_ns = worker.schedule_start_ns + local_index * worker.interval_ns;
            while (now_ns() < scheduled_ns)
                std::this_thread::sleep_for(std::chrono::microseconds{50});
            const auto origin = now_ns();
            sample.scheduled_ns.store(scheduled_ns, std::memory_order_relaxed);
            sample.origin_ns.store(origin, std::memory_order_relaxed);
            std::array<char, 32> id_buffer{}, origin_buffer{}, scheduled_buffer{};
            const auto id_text = format_u64(id, id_buffer);
            const auto origin_text = format_u64(origin, origin_buffer);
            const auto scheduled_text = format_u64(scheduled_ns, scheduled_buffer);
            FixedRequest message;
            auto status = message.set<11>(id_text);
            if (status.ok()) status = message.set<55>("AAPL");
            if (status.ok()) status = message.set<58>(payload);
            if (status.ok()) status = message.set<900001>(origin_text);
            if (status.ok()) status = message.set<900003>(scheduled_text);
            if (!status.ok()) {
                worker.error_code.store(static_cast<int>(status.code), std::memory_order_release);
                break;
            }
            sample.producer_lane_index.store(worker.producer_index, std::memory_order_relaxed);
            bool first_attempt = true;
            for (;;) {
                if (first_attempt) {
                    if (worker.diagnostic_pass)
                        sample.api_start_ns.store(now_ns(), std::memory_order_relaxed);
                    first_attempt = false;
                }
                const auto accepted = worker.lane.send(message);
                if (accepted.ok()) {
                    sample.accepted_ns.store(now_ns(), std::memory_order_relaxed);
                    sample.send_receipt_id.store(accepted.value().id, std::memory_order_relaxed);
                    break;
                }
                const auto code = accepted.status().code;
                if (code != nf::ErrorCode::queue_full && code != nf::ErrorCode::not_ready) {
                    worker.error_code.store(static_cast<int>(code), std::memory_order_release);
                    break;
                }
                std::this_thread::yield();
            }
            if (worker.error_code.load(std::memory_order_acquire) != 0) break;
        }
        worker.completed_epoch.store(epoch, std::memory_order_release);
        worker.completed_epoch.notify_one();
    }
}

void reset_counters() noexcept {
    counters.allocations.store(0, std::memory_order_relaxed);
    counters.allocated_bytes.store(0, std::memory_order_relaxed);
    counters.mutex_locks.store(0, std::memory_order_relaxed);
    counters.fsync_calls.store(0, std::memory_order_relaxed);
    counters.fdatasync_calls.store(0, std::memory_order_relaxed);
    counters.msync_calls.store(0, std::memory_order_relaxed);
}

void write_diagnostics(std::ostream& output) {
    output << "{\"allocation_calls\":" << counters.allocations.load()
        << ",\"allocation_bytes\":" << counters.allocated_bytes.load()
        << ",\"pthread_mutex_lock_calls\":" << counters.mutex_locks.load()
        << ",\"fsync_calls\":" << counters.fsync_calls.load()
        << ",\"fdatasync_calls\":" << counters.fdatasync_calls.load()
        << ",\"msync_calls\":" << counters.msync_calls.load() << '}';
}

template <class Policy>
int run(const Options& options) {
    const auto total = options.samples + options.warmup;
    auto samples = std::make_unique<Sample[]>(total == 0 ? 1 : total);
    std::string payload(options.payload_bytes, 'X');
    nf::EngineConfig config;
    config.storage.directory = options.state_directory;
    config.storage.journal_capacity_bytes = 256U * 1024U * 1024U;
    config.storage.journal_max_records = std::max<std::size_t>(65'536, total * 8 + 1024);
    config.storage.segment_bytes = options.journal_segment_bytes;
    config.storage.max_segments = options.max_journal_segments;
    config.storage.retain_outbound_messages = options.retain_outbound_messages;
    config.max_pending_sender_messages = 256;
    config.sender_receipt_capacity = total + 32;
    config.max_producer_lanes_per_session = options.send_source == SendSource::lane
        ? options.producers : 0;
    if constexpr (std::is_same_v<Policy, nf::Queued>) {
        config.queued.worker_threads = options.producers;
        config.queued.max_pending_callbacks = 128;
        config.queued.max_pending_callback_bytes = 4U * 1024U * 1024U;
        config.queued.delivery_strategy = options.mode == "queued-journal"
            ? nf::QueuedDeliveryStrategy::journal_cursor
            : nf::QueuedDeliveryStrategy::descriptor_ring;
    }

    auto created = nf::Engine<nf::FlatSchema, Policy>::create(std::move(config));
    if (!created.ok()) {
        std::cerr << "engine creation failed: " << created.status().message() << '\n';
        return 2;
    }
    auto engine = std::move(created).value();
    std::atomic<std::size_t> callback_count{0};
    std::atomic<unsigned> errors{0};
    auto on_message = engine.on_message([&](nf::SessionContext& context,
        const nf::FlatMessageView& message) -> nf::Status {
        const auto callback_start = now_ns();
        std::string_view type;
        if (!field_text(message, 35, type)) return {nf::ErrorCode::invalid_header, 35};
        const bool request_source = options.send_source == SendSource::callback;
        if ((request_source && type != "D") || (!request_source && type != "8"))
            return nf::Status::success();

        std::uint64_t id{}, origin{}, scheduled{};
        if (!field_u64(message, 11, id) || id >= total ||
            !field_u64(message, 900001, origin))
            return {nf::ErrorCode::invalid_value, 11};
        if (options.send_source == SendSource::callback &&
            !field_u64(message, 900003, scheduled))
            return {nf::ErrorCode::invalid_value, 900003};
        auto& sample = samples[id];
        std::uint64_t queue_origin = origin;
        if (options.send_source != SendSource::callback &&
            !field_u64(message, 900002, queue_origin))
            return {nf::ErrorCode::invalid_value, 900002};

        sample.origin_ns.store(origin, std::memory_order_relaxed);
        if (options.send_source == SendSource::callback)
            sample.scheduled_ns.store(scheduled, std::memory_order_relaxed);
        if (options.send_source == SendSource::callback) {
            std::string_view id_text, request_payload;
            if (!field_text(message, 11, id_text) || !field_text(message, 58, request_payload))
                return {nf::ErrorCode::invalid_value};
            std::array<char, 32> origin_buffer{};
            const auto origin_text = format_u64(origin, origin_buffer);
            FixedReply reply;
            auto status = reply.set<11>(id_text);
            if (status.ok()) status = reply.set<17>("EXEC-1");
            if (status.ok()) status = reply.set<58>(request_payload);
            if (status.ok()) status = reply.set<900001>(origin_text);
            if (!status.ok()) return status;
            if (options.diagnostic_pass)
                sample.api_start_ns.store(now_ns(), std::memory_order_relaxed);
            const auto accepted = context.send(reply);
            if (!accepted.ok()) {
                sample.error_code.store(static_cast<int>(accepted.status().code),
                    std::memory_order_release);
                return accepted.status();
            }
            sample.send_receipt_id.store(accepted.value().id, std::memory_order_relaxed);
            sample.accepted_ns.store(now_ns(), std::memory_order_relaxed);
            sample.receipt_state.store(static_cast<int>(accepted.value().state),
                std::memory_order_relaxed);
        }

        sample.ingress_origin_ns.store(queue_origin, std::memory_order_relaxed);
        sample.callback_start_ns.store(callback_start, std::memory_order_relaxed);
        sample.callback_end_ns.store(now_ns(), std::memory_order_relaxed);
        sample.ready.store(true, std::memory_order_release);
        callback_count.fetch_add(1, std::memory_order_release);
        return nf::Status::success();
    });
    if (!on_message.ok()) return 3;
    if (!engine.on_error([&](const nf::ErrorEvent& event) {
            errors.fetch_add(1, std::memory_order_relaxed);
            std::cerr << event.format() << '\n';
        }).ok()) return 4;
    if (!engine.add_listener({.name = "runtime-bench", .endpoint = {"127.0.0.1", 0}}).ok()) return 5;
    nf::SessionSettings settings;
    settings.emit_last_processed_seq = true;
    auto session = engine.add_acceptor({.name = "peer", .listener = "runtime-bench",
        .local_comp_id = "SELL", .remote_comp_id = "BUY", .settings = std::move(settings)});
    if (!session.ok()) return 6;
    auto sender = session.value().template sender<>();

    std::unique_ptr<LaneWorker[]> lane_workers;
    if (options.send_source == SendSource::lane) {
        if (options.producers > 64) {
            std::cerr << "ProducerLane supports at most 64 producer threads\n";
            return 6;
        }
        lane_workers = std::make_unique<LaneWorker[]>(options.producers);
        const auto per_lane_receipts = (total + options.producers - 1) / options.producers + 32;
        const auto lane_bytes = std::min<std::size_t>(4U * 1024U * 1024U,
            std::max<std::size_t>(128U * 1024U, (options.payload_bytes + 4096) * 8));
        for (std::size_t producer = 0; producer < options.producers; ++producer) {
            auto lane = session.value().template producer_lane<>(nf::ProducerLaneConfig{
                .capacity_messages = 128,
                .capacity_bytes = lane_bytes,
                .receipt_capacity = per_lane_receipts});
            if (!lane.ok()) {
                std::cerr << "ProducerLane creation failed: " << lane.status().message() << '\n';
                return 6;
            }
            lane_workers[producer].lane = std::move(lane).value();
            lane_workers[producer].producer_index = producer;
            lane_workers[producer].producer_count = options.producers;
            lane_workers[producer].diagnostic_pass = options.diagnostic_pass;
        }
        for (std::size_t producer = 0; producer < options.producers; ++producer) {
            lane_workers[producer].thread = std::thread([&, producer] {
                producer_lane_loop(lane_workers[producer], samples.get(), payload);
            });
        }
    }

    const auto stop_lane_workers = [&]() noexcept {
        if (!lane_workers) return;
        for (std::size_t producer = 0; producer < options.producers; ++producer) {
            lane_workers[producer].stopping.store(true, std::memory_order_release);
            lane_workers[producer].command_epoch.fetch_add(1, std::memory_order_release);
            lane_workers[producer].command_epoch.notify_one();
        }
        for (std::size_t producer = 0; producer < options.producers; ++producer)
            if (lane_workers[producer].thread.joinable()) lane_workers[producer].thread.join();
    };

    nf::Status run_status{};
    std::atomic<bool> owner_done{false};
    std::thread owner([&] {
        run_status = engine.run();
        owner_done.store(true, std::memory_order_release);
    });
    std::uint16_t port{};
    const auto ready_deadline = Clock::now() + std::chrono::seconds{10};
    while (!owner_done.load(std::memory_order_acquire) && Clock::now() < ready_deadline) {
        const auto endpoint = engine.listener_endpoint("runtime-bench");
        if (endpoint.ok()) {
            port = endpoint.value().port;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (port == 0) {
        engine.request_stop();
        owner.join();
        stop_lane_workers();
        return 7;
    }
    std::cout << "READY " << port << std::endl;

    bool failed = false;
    std::size_t phases_completed = 0;
    rusage measured_before{}, measured_after{};
    std::uint64_t measured_elapsed_ns{};
    nf_bench::PerfCounters perf{};
    for (std::string command; std::getline(std::cin, command);) {
        if (command == "STOP") {
            engine.request_stop();
            break;
        }
        if (command.rfind("RUN ", 0) != 0) {
            std::cerr << "invalid control command\n";
            failed = true;
            break;
        }
        std::size_t count{}, offset{};
        const auto separator = command.find(' ', 4);
        if (separator == std::string::npos ||
            !parse_size(std::string_view{command}.substr(4, separator - 4), count) ||
            !parse_size(std::string_view{command}.substr(separator + 1), offset) ||
            offset > total || count > total - offset) {
            std::cerr << "invalid RUN range\n";
            failed = true;
            break;
        }
        if (count == 0) {
            std::cout << "RUN_DONE 0 0" << std::endl;
            ++phases_completed;
            continue;
        }

        const bool measured_phase = offset == options.warmup && count == options.samples;
        if (measured_phase) {
            (void)::getrusage(RUSAGE_SELF, &measured_before);
            if (options.diagnostic_pass) {
                reset_counters();
                perf.start();
                begin_kernel_send_probe(samples.get(), total,
                    options.send_source == SendSource::callback ? '8' : 'D');
                tracking.store(true, std::memory_order_release);
            }
        }
        const auto phase_started = now_ns();
        const auto callback_target = callback_count.load(std::memory_order_acquire) + count;
        if (options.send_source == SendSource::callback) std::cout << "RUN_READY" << std::endl;
        std::vector<std::uint64_t> receipt_ids;
        if (options.send_source == SendSource::mpsc) {
            const auto active_deadline = Clock::now() + std::chrono::seconds{5};
            while (!sender.can_send() && Clock::now() < active_deadline)
                std::this_thread::sleep_for(std::chrono::microseconds{100});
            if (!sender.can_send()) {
                std::cerr << "MPSC sender did not become active\n";
                failed = true;
            }
            if (!failed) {
            std::atomic<std::size_t> next{0};
            std::atomic<int> producer_error{0};
            const auto schedule_start_ns = now_ns() + 10'000'000;
            const auto interval_ns = std::max<std::size_t>(1, 1'000'000'000ULL / options.rate);
            std::vector<std::thread> producers;
            producers.reserve(options.producers);
            for (std::size_t producer = 0; producer < options.producers; ++producer) {
                producers.emplace_back([&, producer] {
                    (void)producer;
                    while (producer_error.load(std::memory_order_acquire) == 0) {
                        const auto local_index = next.fetch_add(1, std::memory_order_relaxed);
                        if (local_index >= count) return;
                        const auto id = offset + local_index;
                        auto& sample = samples[id];
                        const auto scheduled_ns = schedule_start_ns + local_index * interval_ns;
                        while (now_ns() < scheduled_ns)
                            std::this_thread::sleep_for(std::chrono::microseconds{50});
                        const auto origin = now_ns();
                        sample.scheduled_ns.store(scheduled_ns, std::memory_order_relaxed);
                        sample.origin_ns.store(origin, std::memory_order_relaxed);
                        std::array<char, 32> id_buffer{}, origin_buffer{}, scheduled_buffer{};
                        const auto id_text = format_u64(id, id_buffer);
                        const auto origin_text = format_u64(origin, origin_buffer);
                        const auto scheduled_text = format_u64(scheduled_ns, scheduled_buffer);
                        FixedRequest message;
                        auto status = message.set<11>(id_text);
                        if (status.ok()) status = message.set<55>("AAPL");
                        if (status.ok()) status = message.set<58>(payload);
                        if (status.ok()) status = message.set<900001>(origin_text);
                        if (status.ok()) status = message.set<900003>(scheduled_text);
                        if (!status.ok()) {
                            producer_error.store(static_cast<int>(status.code), std::memory_order_release);
                            return;
                        }
                        bool first_attempt = true;
                        for (;;) {
                            if (first_attempt) {
                                if (options.diagnostic_pass)
                                    sample.api_start_ns.store(now_ns(), std::memory_order_relaxed);
                                first_attempt = false;
                            }
                            const auto accepted = sender.send(message);
                            if (accepted.ok()) {
                                const auto accepted_at = now_ns();
                                sample.accepted_ns.store(accepted_at, std::memory_order_relaxed);
                                sample.send_receipt_id.store(accepted.value().id, std::memory_order_relaxed);
                                break;
                            }
                            const auto code = accepted.status().code;
                            if (code != nf::ErrorCode::queue_full && code != nf::ErrorCode::not_ready) {
                                producer_error.store(static_cast<int>(code), std::memory_order_release);
                                return;
                            }
                            std::this_thread::yield();
                        }
                    }
                });
            }
            for (auto& producer : producers) producer.join();
            if (producer_error.load(std::memory_order_acquire) != 0) failed = true;
            }
        } else if (options.send_source == SendSource::lane) {
            const auto schedule_start_ns = now_ns() + 10'000'000;
            const auto interval_ns = std::max<std::size_t>(1, 1'000'000'000ULL / options.rate);
            for (std::size_t producer = 0; producer < options.producers; ++producer) {
                auto& worker = lane_workers[producer];
                worker.count = count;
                worker.offset = offset;
                worker.schedule_start_ns = schedule_start_ns;
                worker.interval_ns = interval_ns;
                worker.error_code.store(0, std::memory_order_relaxed);
            }
            for (std::size_t producer = 0; producer < options.producers; ++producer) {
                auto& worker = lane_workers[producer];
                const auto epoch = worker.command_epoch.load(std::memory_order_relaxed) + 1;
                worker.command_epoch.store(epoch, std::memory_order_release);
                worker.command_epoch.notify_one();
            }
            for (std::size_t producer = 0; producer < options.producers; ++producer) {
                auto& worker = lane_workers[producer];
                const auto epoch = worker.command_epoch.load(std::memory_order_acquire);
                auto completed = worker.completed_epoch.load(std::memory_order_acquire);
                while (completed != epoch && worker.error_code.load(std::memory_order_acquire) == 0) {
                    worker.completed_epoch.wait(completed, std::memory_order_acquire);
                    completed = worker.completed_epoch.load(std::memory_order_acquire);
                }
                if (worker.error_code.load(std::memory_order_acquire) != 0) failed = true;
            }
        }

        const auto expected_seconds = count / options.rate;
        const auto callback_timeout = std::chrono::seconds{
            std::max<std::size_t>(120, expected_seconds * 3 + 30)};
        if (!failed && !wait_for_callbacks(callback_count, callback_target,
                callback_timeout)) {
            std::cerr << "callback wait failed: completed="
                << callback_count.load(std::memory_order_acquire)
                << " target=" << callback_target << '\n';
            failed = true;
        }
        if (!failed) {
            for (std::size_t local_index = 0; local_index < count; ++local_index) {
                auto& sample = samples[offset + local_index];
                if (!sample.ready.load(std::memory_order_acquire) ||
                    sample.error_code.load(std::memory_order_acquire) != 0) {
                    std::cerr << "sample callback failed: id=" << offset + local_index
                        << " ready=" << sample.ready.load(std::memory_order_relaxed)
                        << " error=" << sample.error_code.load(std::memory_order_relaxed) << '\n';
                    failed = true;
                    break;
                }
                const bool sender_receipt_available = options.send_source == SendSource::mpsc ||
                    options.send_source == SendSource::lane ||
                    (std::is_same_v<Policy, nf::Queued> &&
                        options.send_source != SendSource::callback);
                if (sender_receipt_available) {
                    const auto receipt_id = sample.send_receipt_id.load(std::memory_order_acquire);
                    const auto deadline = Clock::now() + std::chrono::seconds{30};
                    for (;;) {
                        const auto receipt = options.send_source == SendSource::lane
                            ? lane_workers[sample.producer_lane_index.load(std::memory_order_relaxed)].lane.receipt(receipt_id)
                            : sender.receipt(receipt_id);
                        if (!receipt.ok()) {
                            std::cerr << "receipt lookup failed: id=" << offset + local_index
                                << " receipt=" << receipt_id << " status="
                                << receipt.status().message() << '\n';
                            failed = true;
                            break;
                        }
                        if (receipt.value().state == nf::ReceiptState::written) {
                            sample.receipt_state.store(static_cast<int>(nf::ReceiptState::written),
                                std::memory_order_relaxed);
                            break;
                        }
                        if (receipt.value().state == nf::ReceiptState::failed ||
                            receipt.value().state == nf::ReceiptState::expired ||
                            Clock::now() >= deadline) {
                            std::cerr << "receipt did not become written: id=" << offset + local_index
                                << " receipt=" << receipt_id << " state="
                                << static_cast<int>(receipt.value().state) << " status="
                                << receipt.value().status.message() << '\n';
                            failed = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::microseconds{100});
                    }
                }
                if (failed) break;
            }
        }
        if (!failed && measured_phase && options.diagnostic_pass &&
            options.send_source == SendSource::callback &&
            !wait_for_kernel_frames(count, std::chrono::seconds{30})) {
            std::cerr << "callback kernel-send probe timed out: completed="
                << kernel_send_completed_frames.load(std::memory_order_acquire)
                << " expected=" << count << '\n';
            failed = true;
        }
        auto elapsed = now_ns() - phase_started;
        if (measured_phase) {
            auto first_scheduled = std::numeric_limits<std::uint64_t>::max();
            std::uint64_t last_completion{};
            for (std::size_t id = offset; id < offset + count; ++id) {
                const auto scheduled = samples[id].scheduled_ns.load(std::memory_order_relaxed);
                const auto callback_start = samples[id].callback_start_ns.load(std::memory_order_relaxed);
                const auto callback_end = samples[id].callback_end_ns.load(std::memory_order_relaxed);
                first_scheduled = std::min(first_scheduled, scheduled);
                const auto completion = options.send_source == SendSource::callback
                    ? callback_end : callback_start;
                last_completion = std::max(last_completion, completion);
            }
            if (first_scheduled != 0 &&
                first_scheduled != std::numeric_limits<std::uint64_t>::max() &&
                last_completion >= first_scheduled)
                elapsed = last_completion - first_scheduled;
            measured_elapsed_ns = elapsed;
            if (options.diagnostic_pass) end_kernel_send_probe();
            tracking.store(false, std::memory_order_release);
            if (options.diagnostic_pass) perf.stop();
            (void)::getrusage(RUSAGE_SELF, &measured_after);
        }
        if (failed) {
            std::cerr << "phase failed: source="
                << (options.send_source == SendSource::callback ? "callback" :
                    (options.send_source == SendSource::mpsc ? "mpsc" : "lane")) << '\n';
            break;
        }
        std::cout << "RUN_DONE " << count << ' ' << elapsed << std::endl;
        ++phases_completed;
    }

    if (owner.joinable()) {
        if (!owner_done.load(std::memory_order_acquire)) engine.request_stop();
        owner.join();
    }
    stop_lane_workers();
    if (!run_status.ok() || errors.load(std::memory_order_acquire) != 0 || failed ||
        phases_completed < (options.warmup == 0 ? 1U : 2U)) return 8;

    std::ofstream samples_file(options.sample_csv);
    if (!samples_file) return 9;
    samples_file << "id,scheduled_ns,origin_ns,generation_lateness_ns,api_start_ns,accepted_ns,"
        << "kernel_write_return_ns,callback_start_ns,callback_end_ns,callback_ingress_origin_ns,"
        << "callback_ingress_delay_ns,callback_duration_ns,server_round_trip_ns,"
        << "actual_origin_round_trip_ns,receipt_id,receipt_state,callback_ready\n";
    for (std::size_t id = options.warmup; id < total; ++id) {
        const auto& sample = samples[id];
        const auto ready = sample.ready.load(std::memory_order_acquire);
        const auto scheduled = sample.scheduled_ns.load(std::memory_order_relaxed);
        const auto origin = sample.origin_ns.load(std::memory_order_relaxed);
        const auto api_start = sample.api_start_ns.load(std::memory_order_relaxed);
        const auto accepted = sample.accepted_ns.load(std::memory_order_relaxed);
        const auto kernel_write_return = sample.kernel_write_return_ns.load(std::memory_order_relaxed);
        const auto ingress_origin = sample.ingress_origin_ns.load(std::memory_order_relaxed);
        const auto callback_start = sample.callback_start_ns.load(std::memory_order_relaxed);
        const auto callback_end = sample.callback_end_ns.load(std::memory_order_relaxed);
        const auto receipt_id = sample.send_receipt_id.load(std::memory_order_relaxed);
        const auto receipt_state = static_cast<nf::ReceiptState>(
            sample.receipt_state.load(std::memory_order_relaxed));
        const auto receipt_state_text = receipt_state == nf::ReceiptState::written ? "written" :
            (receipt_state == nf::ReceiptState::appended ? "appended" :
                (receipt_state == nf::ReceiptState::accepted ? "accepted" : "failed"));
        const auto ingress_delay = callback_start >= ingress_origin
            ? callback_start - ingress_origin : 0;
        const auto completion = options.send_source == SendSource::callback
            ? callback_end : callback_start;
        const auto scheduled_rtt = completion >= scheduled ? completion - scheduled : 0;
        const auto actual_origin_rtt = completion >= origin ? completion - origin : 0;
        const auto lateness = origin >= scheduled ? origin - scheduled : 0;
        samples_file << id << ',' << scheduled << ',' << origin << ',' << lateness << ','
            << api_start << ',' << accepted << ',' << kernel_write_return << ','
            << callback_start << ',' << callback_end << ',' << ingress_origin << ','
            << ingress_delay << ','
            << (callback_end >= callback_start ? callback_end - callback_start : 0) << ','
            << scheduled_rtt << ',' << actual_origin_rtt << ','
            << receipt_id << ',' << receipt_state_text << ','
            << (ready ? 1 : 0) << '\n';
    }

    std::uint64_t completed_kernel_samples = 0;
    std::uint64_t written_before_accept_return = 0;
    if (options.diagnostic_pass) {
        for (std::size_t id = options.warmup; id < total; ++id) {
            const auto kernel_write = samples[id].kernel_write_return_ns.load(std::memory_order_relaxed);
            if (kernel_write == 0) continue;
            ++completed_kernel_samples;
            const auto accepted = samples[id].accepted_ns.load(std::memory_order_relaxed);
            if (accepted != 0 && kernel_write < accepted) ++written_before_accept_return;
        }
    }
    std::cerr << "{\"measured_elapsed_ns\":" << measured_elapsed_ns
        << ",\"minor_page_faults\":" << (measured_after.ru_minflt - measured_before.ru_minflt)
        << ",\"major_page_faults\":" << (measured_after.ru_majflt - measured_before.ru_majflt)
        << ",\"peak_rss_kib\":" << measured_after.ru_maxrss
        << ",\"diagnostic_pass\":" << (options.diagnostic_pass ? "true" : "false")
        << ",\"kernel_send_expected_type\":\""
        << (options.send_source == SendSource::callback ? '8' : 'D') << "\""
        << ",\"kernel_send_completed_samples\":" << completed_kernel_samples
        << ",\"kernel_send_expected_samples\":" << (options.diagnostic_pass ? options.samples : 0)
        << ",\"kernel_write_before_accept_return_samples\":" << written_before_accept_return
        << ",\"tracking_scope\":\"measured phase only; phase-wide including benchmark harness, producer wake/poll and receipt polling; static executable wrappers; send wrapper remains linked in timing pass with tracking disabled and atomic flag-read overhead\",\"counters\":";
    write_diagnostics(std::cerr);
    std::cerr << ",\"perf_counters\":" << perf.json();
    std::cerr << "}\n";
    return 0;
}

bool parse_options(int argc, char** argv, Options& result) {
    if (argc != 13) return false;
    result.mode = argv[1];
    if (result.mode != "inline" && result.mode != "queued-journal" &&
        result.mode != "queued-ring") return false;
    const std::string_view source{argv[2]};
    if (source == "callback") result.send_source = SendSource::callback;
    else if (source == "mpsc") result.send_source = SendSource::mpsc;
    else if (source == "lane") result.send_source = SendSource::lane;
    else return false;
    if (!parse_size(argv[3], result.samples) || result.samples == 0 ||
        !parse_size(argv[4], result.warmup) ||
        !parse_size(argv[5], result.producers) || result.producers == 0 ||
        !parse_size(argv[6], result.payload_bytes) || result.payload_bytes > 65'000 ||
        !parse_size(argv[7], result.rate) || result.rate == 0)
        return false;
    result.diagnostic_pass = std::string_view{argv[8]} == "diagnostics";
    if (!result.diagnostic_pass && std::string_view{argv[8]} != "timed") return false;
    result.state_directory = argv[9];
    if (!parse_size(argv[10], result.journal_segment_bytes) ||
        !parse_size(argv[11], result.max_journal_segments) ||
        !parse_size(argv[12], result.retain_outbound_messages)) return false;
    result.sample_csv = result.state_directory / "samples.csv";
    try { std::filesystem::create_directories(result.state_directory); }
    catch (...) { return false; }
    return true;
}

} // namespace

extern "C" ssize_t __real_send(int, const void*, std::size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void* buffer, std::size_t size, int flags) {
    const bool track = kernel_send_tracking.load(std::memory_order_acquire);
    const auto result = __real_send(fd, buffer, size, flags);
    if (result > 0 && track) {
        const auto send_return_ns = now_ns();
        observe_successful_send(buffer, static_cast<std::size_t>(result), send_return_ns);
    }
    return result;
}

extern "C" void* __real_malloc(std::size_t);
extern "C" void* __real_calloc(std::size_t, std::size_t);
extern "C" void* __real_realloc(void*, std::size_t);
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __real_fsync(int);
extern "C" int __real_fdatasync(int);
extern "C" int __real_msync(void*, std::size_t, int);

extern "C" void* __wrap_malloc(std::size_t size) {
    auto* result = __real_malloc(size);
    if (result != nullptr && tracking.load(std::memory_order_relaxed)) {
        counters.allocations.fetch_add(1, std::memory_order_relaxed);
        counters.allocated_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    return result;
}

extern "C" void* __wrap_calloc(std::size_t count, std::size_t size) {
    auto* result = __real_calloc(count, size);
    if (result != nullptr && tracking.load(std::memory_order_relaxed)) {
        counters.allocations.fetch_add(1, std::memory_order_relaxed);
        counters.allocated_bytes.fetch_add(count * size, std::memory_order_relaxed);
    }
    return result;
}

extern "C" void* __wrap_realloc(void* pointer, std::size_t size) {
    auto* result = __real_realloc(pointer, size);
    if (result != nullptr && tracking.load(std::memory_order_relaxed)) {
        counters.allocations.fetch_add(1, std::memory_order_relaxed);
        counters.allocated_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    return result;
}

extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (tracking.load(std::memory_order_relaxed))
        counters.mutex_locks.fetch_add(1, std::memory_order_relaxed);
    return __real_pthread_mutex_lock(mutex);
}

extern "C" int __wrap_fsync(int fd) {
    if (tracking.load(std::memory_order_relaxed))
        counters.fsync_calls.fetch_add(1, std::memory_order_relaxed);
    return __real_fsync(fd);
}

extern "C" int __wrap_fdatasync(int fd) {
    if (tracking.load(std::memory_order_relaxed))
        counters.fdatasync_calls.fetch_add(1, std::memory_order_relaxed);
    return __real_fdatasync(fd);
}

extern "C" int __wrap_msync(void* address, std::size_t length, int flags) {
    if (tracking.load(std::memory_order_relaxed))
        counters.msync_calls.fetch_add(1, std::memory_order_relaxed);
    return __real_msync(address, length, flags);
}

void* operator new(std::size_t size) {
    const auto actual = size == 0 ? 1 : size;
    auto* result = __real_malloc(actual);
    if (result == nullptr) throw std::bad_alloc{};
    if (tracking.load(std::memory_order_relaxed)) {
        counters.allocations.fetch_add(1, std::memory_order_relaxed);
        counters.allocated_bytes.fetch_add(actual, std::memory_order_relaxed);
    }
    return result;
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

void* operator new(std::size_t size, std::align_val_t alignment) {
    void* result = nullptr;
    const auto actual = size == 0 ? 1 : size;
    if (::posix_memalign(&result, static_cast<std::size_t>(alignment), actual) != 0)
        throw std::bad_alloc{};
    if (tracking.load(std::memory_order_relaxed)) {
        counters.allocations.fetch_add(1, std::memory_order_relaxed);
        counters.allocated_bytes.fetch_add(actual, std::memory_order_relaxed);
    }
    return result;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr << "usage: runtime_bench <inline|queued-journal|queued-ring> "
            << "<callback|mpsc|lane> <samples> <warmup> <producers> <payload-bytes> "
            << "<rate> <timed|diagnostics> <state-directory> <segment-bytes> "
            << "<max-segments> <retain-outbound-messages>\n";
        return 2;
    }
    if (options.mode == "inline") return run<nf::Inline>(options);
    return run<nf::Queued>(options);
}
