#include "fastfix/runtime/thread_affinity.h"

#include <algorithm>
#include <string>

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#endif

namespace fastfix::runtime {

auto ApplyCurrentThreadAffinity(std::uint32_t cpu_id, std::string_view role) -> base::Status {
#if defined(__linux__)
    if (cpu_id >= CPU_SETSIZE) {
        return base::Status::InvalidArgument(
            std::string(role) + " cpu id exceeds CPU_SETSIZE");
    }

    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(static_cast<int>(cpu_id), &cpu_set);

    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
    if (rc != 0) {
        return base::Status::IoError(
            std::string("failed to pin ") + std::string(role) +
            " thread to cpu " + std::to_string(cpu_id) + ": " + std::strerror(rc));
    }

#else
    (void)cpu_id;
    (void)role;
#endif
    return base::Status::Ok();
}

auto SetCurrentThreadName(std::string_view name) -> void {
#if defined(__linux__)
    if (name.empty()) {
        return;
    }

    constexpr std::size_t kLinuxThreadNameLimit = 15U;
    std::string thread_name(name.substr(0U, std::min(name.size(), kLinuxThreadNameLimit)));
    static_cast<void>(pthread_setname_np(pthread_self(), thread_name.c_str()));
#else
    (void)name;
#endif
}

}  // namespace fastfix::runtime