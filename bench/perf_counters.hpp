#pragma once

// Optional Linux PMU diagnostics for the separately instrumented pass only.
// Existing threads get independent events; inherit includes producers created
// during the phase. No global kernel/CPU configuration is changed.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace nf_bench {
class PerfCounters {
    struct Event {
        const char* name{};
        std::uint64_t configuration{};
        std::vector<int> descriptors;
        std::uint64_t raw{}, enabled_ns{}, running_ns{}, scaled{};
        int error{};
    };
    std::array<Event, 6> events_{{
        {"cycles", PERF_COUNT_HW_CPU_CYCLES, {}},
        {"instructions", PERF_COUNT_HW_INSTRUCTIONS, {}},
        {"cache_references", PERF_COUNT_HW_CACHE_REFERENCES, {}},
        {"cache_misses", PERF_COUNT_HW_CACHE_MISSES, {}},
        {"branches", PERF_COUNT_HW_BRANCH_INSTRUCTIONS, {}},
        {"branch_misses", PERF_COUNT_HW_BRANCH_MISSES, {}}}};
    std::size_t threads_{};
    void close() noexcept {
        for (auto& event : events_) {
            for (const auto fd : event.descriptors) ::close(fd);
            event.descriptors.clear();
        }
    }
public:
    ~PerfCounters() { close(); }
    void start() noexcept {
        close();
        try {
            std::vector<pid_t> tids;
            for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task"))
                tids.push_back(static_cast<pid_t>(std::stol(entry.path().filename().string())));
            threads_=tids.size();
            for (auto& event : events_) {
                event.raw=event.enabled_ns=event.running_ns=event.scaled=0;
                event.error=0;
                event.descriptors.reserve(tids.size());
                for (const auto tid : tids) {
                    perf_event_attr attributes{};
                    attributes.type=PERF_TYPE_HARDWARE;
                    attributes.size=sizeof(attributes);
                    attributes.config=event.configuration;
                    attributes.disabled=1;
                    attributes.inherit=1;
                    attributes.exclude_kernel=1;
                    attributes.exclude_hv=1;
                    attributes.read_format=PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
                    const auto fd=static_cast<int>(::syscall(SYS_perf_event_open, &attributes, tid, -1, -1, PERF_FLAG_FD_CLOEXEC));
                    if (fd<0) { event.error=errno; continue; }
                    event.descriptors.push_back(fd);
                }
            }
            for (auto& event : events_) for (const auto fd : event.descriptors) {
                if (::ioctl(fd,PERF_EVENT_IOC_RESET,0)<0 || ::ioctl(fd,PERF_EVENT_IOC_ENABLE,0)<0)
                    event.error=errno;
            }
        } catch (...) {
            close();
            for (auto& event : events_) event.error=ENOMEM;
        }
    }
    void stop() noexcept {
        for (auto& event : events_) for (const auto fd : event.descriptors) {
            if (::ioctl(fd,PERF_EVENT_IOC_DISABLE,0)<0) event.error=errno;
            struct { std::uint64_t value, enabled, running; } reading{};
            if (::read(fd,&reading,sizeof(reading))!=static_cast<ssize_t>(sizeof(reading))) { event.error=errno?errno:EIO; continue; }
            event.raw+=reading.value;
            event.enabled_ns+=reading.enabled;
            event.running_ns+=reading.running;
            if (reading.running!=0) {
                const auto scaled=static_cast<long double>(reading.value)*reading.enabled/reading.running;
                event.scaled+=static_cast<std::uint64_t>(std::min(scaled,static_cast<long double>(std::numeric_limits<std::uint64_t>::max()-event.scaled)));
            }
        }
    }
    [[nodiscard]] std::string json() const {
        std::ostringstream stream;
        stream << "{\"scope\":\"user-space phase-wide PMU diagnostics; existing threads plus inherited producers; includes harness; multiplex-scaled per-thread sums\",\"threads_at_start\":" << threads_ << ",\"events\":{";
        bool comma=false;
        for (const auto& event : events_) {
            if (comma) stream << ',';
            comma=true;
            stream << '"' << event.name << "\":{\"available\":" << (event.descriptors.empty()?"false":"true")
                << ",\"opened_threads\":" << event.descriptors.size() << ",\"errno\":" << event.error
                << ",\"raw\":" << event.raw << ",\"time_enabled_ns\":" << event.enabled_ns
                << ",\"time_running_ns\":" << event.running_ns << ",\"scaled\":" << event.scaled << '}';
        }
        stream << "}}";
        return stream.str();
    }
};
} // namespace nf_bench
