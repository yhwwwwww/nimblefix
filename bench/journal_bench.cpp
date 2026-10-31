#include <nimblefix/journal.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct FaultCount {
    long minor{};
    long major{};
};

struct Row {
    std::string_view operation{};
    std::size_t iteration{};
    std::string_view kind{};
    std::uint64_t elapsed_ns{};
    nimblefix::RecordId record_id{};
    std::uint32_t segment_index{};
    std::size_t payload_bytes{};
    std::uint64_t reader_lag_records{};
    std::size_t committed_records{};
    long minor_faults{-1};
    long major_faults{-1};
};

[[noreturn]] void fail(std::string_view reason) {
    std::cerr << "journal benchmark failed: " << reason << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view reason) {
    if (!condition) fail(reason);
}

bool parse_size(std::string_view text, std::size_t& value) noexcept {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

FaultCount faults() {
    rusage usage{};
    require(::getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
    return {usage.ru_minflt, usage.ru_majflt};
}

FaultCount difference(FaultCount after, FaultCount before) noexcept {
    return {after.minor - before.minor, after.major - before.major};
}

constexpr std::uint32_t segment_index(nimblefix::RecordId id) noexcept {
    return static_cast<std::uint32_t>(id >> 48U);
}

std::string_view kind_name(nimblefix::RecordKind kind) noexcept {
    switch (kind) {
    case nimblefix::RecordKind::inbound: return "inbound";
    case nimblefix::RecordKind::outbound: return "outbound";
    case nimblefix::RecordKind::delivery: return "delivery";
    case nimblefix::RecordKind::epoch_reset: return "epoch_reset";
    }
    return "unknown";
}

void emit_row(const Row& row) {
    std::cout << row.operation << ',' << row.iteration << ',' << row.kind << ','
        << row.elapsed_ns << ',' << row.record_id << ',' << row.segment_index << ','
        << row.payload_bytes << ',' << row.reader_lag_records << ','
        << row.committed_records << ',';
    if (row.minor_faults >= 0) std::cout << row.minor_faults;
    std::cout << ',';
    if (row.major_faults >= 0) std::cout << row.major_faults;
    std::cout << '\n';
}

int main_impl(int argc, char** argv) {
    if (argc != 7) {
        std::cerr << "usage: journal_bench STATE_DIR SAMPLES REOPENS SEGMENT_BYTES MAX_SEGMENTS PAYLOAD_BYTES\n";
        return 2;
    }

    const std::filesystem::path state_dir = std::filesystem::absolute(argv[1]);
    std::size_t samples{}, reopen_samples{}, segment_bytes{}, max_segments{}, payload_bytes{};
    require(parse_size(argv[2], samples) && samples >= 10 && samples <= 100'000,
        "samples must be in 10..100000");
    require(parse_size(argv[3], reopen_samples) && reopen_samples >= 1 && reopen_samples <= 50,
        "reopens must be in 1..50");
    require(parse_size(argv[4], segment_bytes) && segment_bytes >= 8192 &&
        segment_bytes <= (1U << 24U) && (segment_bytes & 7U) == 0,
        "segment bytes must be an aligned value in 8192..16777216");
    require(parse_size(argv[5], max_segments) && max_segments >= 2 && max_segments <= 4096,
        "max segments must be in 2..4096");
    require(parse_size(argv[6], payload_bytes) && payload_bytes >= 16 && payload_bytes <= 2048,
        "payload bytes must be in 16..2048");
    require(samples <= (std::numeric_limits<std::size_t>::max() - 16) / 3,
        "sample count overflows record count");
    require(max_segments <= (std::numeric_limits<std::size_t>::max() - (2U * 1024U * 1024U)) /
        segment_bytes, "storage capacity overflows");

    std::error_code filesystem_error;
    std::filesystem::create_directories(state_dir, filesystem_error);
    require(!filesystem_error, "could not create state directory");
    const auto path = (state_dir / "journal.nfxj").string();
    constexpr std::string_view session_key = "nimblefix-journal-benchmark-v2";
    const std::size_t capacity_bytes = segment_bytes * max_segments + 2U * 1024U * 1024U;
    const std::size_t record_count = samples * 3;
    const nimblefix::JournalConfig config{path, session_key, capacity_bytes,
        record_count + 16, {}, segment_bytes, max_segments, 1,
        static_cast<std::uint64_t>(record_count + 16)};

    std::string inbound_payload(payload_bytes, 'I');
    std::string outbound_payload(payload_bytes, 'O');
    std::vector<Row> rows;
    rows.reserve(record_count * 2 + reopen_samples + 2);
    std::vector<nimblefix::RecordId> inbound_ids;
    inbound_ids.reserve(samples);
    std::vector<std::uint32_t> segment_indices;
    segment_indices.reserve(record_count);

    const FaultCount initial_open_faults_before = faults();
    const auto initial_open_start = Clock::now();
    auto opened = nimblefix::Journal::open(config);
    const auto initial_open_end = Clock::now();
    const FaultCount initial_open_faults = difference(faults(), initial_open_faults_before);
    require(opened.ok(), opened.status().message());
    std::optional<nimblefix::Journal> journal;
    journal.emplace(std::move(opened).value());
    const auto initial_open_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(initial_open_end - initial_open_start).count());
    rows.push_back(Row{"fresh_open", 0, "new_store", initial_open_ns, 0, 0, 0, 0,
        journal->records().size(), initial_open_faults.minor, initial_open_faults.major});

    auto reader_result = journal->create_reader();
    require(reader_result.ok(), reader_result.status().message());
    std::optional<nimblefix::JournalReader> reader;
    reader.emplace(std::move(reader_result).value());

    FaultCount append_before = faults();
    std::uint64_t peak_reader_lag = 0;
    std::size_t inbound_count = 0, outbound_count = 0, delivery_count = 0;
    std::uint64_t inbound_sequence = 1, outbound_sequence = 1;
    std::uint32_t last_segment = std::numeric_limits<std::uint32_t>::max();
    std::size_t segment_transitions = 0;

    for (std::size_t index = 0; index < samples; ++index) {
        const auto inbound_start = Clock::now();
        const auto inbound = journal->append(nimblefix::RecordKind::inbound,
            inbound_sequence, 0, 0, inbound_payload);
        const auto inbound_end = Clock::now();
        require(inbound.ok(), inbound.status().message());
        inbound_ids.push_back(inbound.value().id);
        ++inbound_count;
        auto current_segment = segment_index(inbound.value().id);
        if (last_segment != std::numeric_limits<std::uint32_t>::max() && current_segment != last_segment)
            ++segment_transitions;
        last_segment = current_segment;
        segment_indices.push_back(current_segment);
        auto lag = reader->lag_records();
        peak_reader_lag = std::max(peak_reader_lag, lag);
        rows.push_back(Row{"append", index, kind_name(nimblefix::RecordKind::inbound),
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(inbound_end - inbound_start).count()),
            inbound.value().id, current_segment, inbound_payload.size(), lag,
            journal->records().size(), -1, -1});

        const auto outbound_start = Clock::now();
        const auto outbound = journal->append(nimblefix::RecordKind::outbound,
            outbound_sequence, outbound_sequence, inbound.value().id, outbound_payload);
        const auto outbound_end = Clock::now();
        require(outbound.ok(), outbound.status().message());
        ++outbound_count;
        current_segment = segment_index(outbound.value().id);
        if (current_segment != last_segment) ++segment_transitions;
        last_segment = current_segment;
        segment_indices.push_back(current_segment);
        lag = reader->lag_records();
        peak_reader_lag = std::max(peak_reader_lag, lag);
        rows.push_back(Row{"append", index, kind_name(nimblefix::RecordKind::outbound),
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(outbound_end - outbound_start).count()),
            outbound.value().id, current_segment, outbound_payload.size(), lag,
            journal->records().size(), -1, -1});

        const auto delivery_start = Clock::now();
        const auto delivery = journal->append(nimblefix::RecordKind::delivery,
            inbound_sequence, 0, inbound.value().id, {});
        const auto delivery_end = Clock::now();
        require(delivery.ok(), delivery.status().message());
        ++delivery_count;
        current_segment = segment_index(delivery.value().id);
        if (current_segment != last_segment) ++segment_transitions;
        last_segment = current_segment;
        segment_indices.push_back(current_segment);
        lag = reader->lag_records();
        peak_reader_lag = std::max(peak_reader_lag, lag);
        rows.push_back(Row{"append", index, kind_name(nimblefix::RecordKind::delivery),
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(delivery_end - delivery_start).count()),
            delivery.value().id, current_segment, 0, lag, journal->records().size(), -1, -1});
        ++inbound_sequence;
        ++outbound_sequence;
    }
    const FaultCount append_faults = difference(faults(), append_before);
    require(inbound_count == samples && outbound_count == samples && delivery_count == samples,
        "append category counts did not match samples");
    require(journal->records().size() == record_count, "committed record count mismatch after append");
    require(peak_reader_lag == record_count, "reader did not retain the full append backlog");

    std::array<char, 2048> borrowed_copy{};
    std::uint64_t payload_checksum = 0;
    FaultCount reader_before = faults();
    for (std::size_t index = 0; index < record_count; ++index) {
        nimblefix::JournalRecord record{};
        const auto start = Clock::now();
        const auto status = reader->next(record);
        const auto end = Clock::now();
        require(status.ok(), status.message());
        require(record.payload.size() <= borrowed_copy.size(), "reader payload exceeds copy buffer");
        if (!record.payload.empty()) {
            std::memcpy(borrowed_copy.data(), record.payload.data(), record.payload.size());
            payload_checksum += static_cast<unsigned char>(borrowed_copy.front());
            payload_checksum += static_cast<unsigned char>(borrowed_copy[record.payload.size() - 1]);
        }
        rows.push_back(Row{"reader_next", index, kind_name(record.kind),
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()),
            record.id, segment_index(record.id), record.payload.size(), reader->lag_records(),
            journal->records().size(), -1, -1});
    }
    const FaultCount reader_faults = difference(faults(), reader_before);
    const auto final_reader_lag = reader->lag_records();
    require(final_reader_lag == 0, "reader did not reach the committed tail");
    reader.reset();

    const auto delivery_cursor = inbound_ids.back();
    const FaultCount checkpoint_faults_before = faults();
    const auto checkpoint_start = Clock::now();
    const auto checkpoint_status = journal->publish_checkpoint(delivery_cursor, 1);
    const auto checkpoint_end = Clock::now();
    const FaultCount checkpoint_faults = difference(faults(), checkpoint_faults_before);
    require(checkpoint_status.ok(), checkpoint_status.message());
    require(!journal->checkpoint_stale(), "checkpoint is stale after publish");
    const auto checkpoint_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(checkpoint_end - checkpoint_start).count());
    rows.push_back(Row{"checkpoint", 0, "checkpoint_and_outbound_index", checkpoint_ns,
        delivery_cursor, segment_index(delivery_cursor), 0, final_reader_lag,
        journal->records().size(), checkpoint_faults.minor, checkpoint_faults.major});

    const auto expected_checkpoint_generation = journal->checkpoint().generation;
    std::vector<FaultCount> reopen_faults;
    std::vector<std::uint64_t> reopen_ns;
    reopen_faults.reserve(reopen_samples);
    reopen_ns.reserve(reopen_samples);
    for (std::size_t index = 0; index < reopen_samples; ++index) {
        journal.reset();
        const auto before = faults();
        const auto start = Clock::now();
        auto recovered = nimblefix::Journal::open(config);
        const auto end = Clock::now();
        const auto after = faults();
        require(recovered.ok(), recovered.status().message());
        journal.emplace(std::move(recovered).value());
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        const auto fault_delta = difference(after, before);
        require(journal->records().size() == record_count, "reopen record count mismatch");
        require(!journal->checkpoint_stale(), "normal checkpoint became stale during reopen");
        require(journal->checkpoint().generation == expected_checkpoint_generation,
            "checkpoint generation mismatch on normal reopen");
        require(journal->checkpoint().delivery_cursor == delivery_cursor,
            "delivery cursor mismatch on normal reopen");
        require(journal->find_outbound(outbound_sequence - 1) != nullptr,
            "outbound index was not recovered");
        require(journal->find_record(delivery_cursor) != nullptr,
            "record index was not recovered");
        reopen_ns.push_back(elapsed);
        reopen_faults.push_back(fault_delta);
        const auto recovered_segments = journal->records().empty() ? 0U
            : segment_index(journal->records().back().id);
        rows.push_back(Row{"reopen", index, "normal_checkpoint_index_recovery", elapsed,
            journal->records().back().id, recovered_segments, 0, 0,
            journal->records().size(), fault_delta.minor, fault_delta.major});
    }

    const auto inspection = nimblefix::Journal::inspect_snapshot(config);
    require(inspection.diagnostic.ok(), inspection.diagnostic.message());
    require(inspection.format_version == 2, "journal is not segmented v2");
    require(inspection.committed_records == record_count, "inspection committed record count mismatch");
    require(segment_transitions != 0 && segment_indices.size() == record_count,
        "workload did not rotate actual segment indices");
    std::sort(segment_indices.begin(), segment_indices.end());
    segment_indices.erase(std::unique(segment_indices.begin(), segment_indices.end()), segment_indices.end());
    require(segment_indices.size() >= 2, "workload used fewer than two segment indices");

    std::cout << "operation,iteration,kind,elapsed_ns,record_id,segment_index,payload_bytes,reader_lag_records,committed_records,minor_faults,major_faults\n";
    for (const auto& row : rows) emit_row(row);
    rusage final_usage{};
    require(::getrusage(RUSAGE_SELF, &final_usage) == 0, "final getrusage failed");

    std::cerr << "{\"samples\":" << samples
        << ",\"reopen_samples\":" << reopen_samples
        << ",\"segment_bytes\":" << segment_bytes
        << ",\"max_segments\":" << max_segments
        << ",\"payload_bytes\":" << payload_bytes
        << ",\"process_peak_rss_kib\":" << final_usage.ru_maxrss
        << ",\"records_committed\":" << journal->records().size()
        << ",\"inbound_records\":" << inbound_count
        << ",\"outbound_records\":" << outbound_count
        << ",\"delivery_records\":" << delivery_count
        << ",\"format_version\":" << inspection.format_version
        << ",\"checkpoint_stale\":" << (journal->checkpoint_stale() ? "true" : "false")
        << ",\"checkpoint_generation\":" << journal->checkpoint().generation
        << ",\"reader_peak_lag_records\":" << peak_reader_lag
        << ",\"reader_final_lag_records\":" << final_reader_lag
        << ",\"segment_index_transitions\":" << segment_transitions
        << ",\"segment_indices\":[";
    for (std::size_t i = 0; i < segment_indices.size(); ++i) {
        if (i != 0) std::cerr << ',';
        std::cerr << segment_indices[i];
    }
    std::cerr << "]"
        << ",\"segment_index_count\":" << segment_indices.size()
        << ",\"initial_open_ns\":" << initial_open_ns
        << ",\"checkpoint_publish_ns\":" << checkpoint_ns
        << ",\"initial_open_page_faults\":{\"minor\":" << initial_open_faults.minor
        << ",\"major\":" << initial_open_faults.major << '}'
        << ",\"checkpoint_page_faults\":{\"minor\":" << checkpoint_faults.minor
        << ",\"major\":" << checkpoint_faults.major << '}'
        << ",\"append_page_faults\":{\"minor\":" << append_faults.minor
        << ",\"major\":" << append_faults.major << '}'
        << ",\"reader_page_faults\":{\"minor\":" << reader_faults.minor
        << ",\"major\":" << reader_faults.major << '}'
        << ",\"reopen_page_faults\":[";
    for (std::size_t i = 0; i < reopen_faults.size(); ++i) {
        if (i != 0) std::cerr << ',';
        std::cerr << "{\"minor\":" << reopen_faults[i].minor
            << ",\"major\":" << reopen_faults[i].major << '}';
    }
    std::cerr << "]"
        << ",\"reopen_elapsed_ns\":[";
    for (std::size_t i = 0; i < reopen_ns.size(); ++i) {
        if (i != 0) std::cerr << ',';
        std::cerr << reopen_ns[i];
    }
    std::cerr << "]"
        << ",\"payload_checksum\":" << payload_checksum
        << ",\"per_message_syncs_requested\":0"
        << ",\"sync_scope\":\"Journal append API publishes with release commit marker; benchmark calls no fsync/fdatasync/msync per record\""
        << ",\"checkpoint_index_recovered\":true"
        << ",\"cold_index_rebuild\":\"not measured\"}\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return main_impl(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "journal benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
