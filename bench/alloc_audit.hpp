#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace nimblefix::bench_audit {

// Unified allocation auditor shared across benchmark binaries.
//
// Scope:
//   - Linker -Wl,--wrap={malloc,calloc,realloc,aligned_alloc,posix_memalign}
//     intercepts all direct C allocator calls in this executable and every
//     statically linked library (including NimbleFIX and bundled QuickFIX).
//   - Global operator new/new[] (including C++17 std::align_val_t and nothrow
//     overloads) intercepts all C++ heap allocations across translation units.
//   - Allocations made internally by dynamically linked system libraries that
//     do not route through overridden operator new are outside --wrap scope.
inline std::atomic<bool> g_track{false};
inline std::atomic<std::uint64_t> g_calls{0};
inline std::atomic<std::uint64_t> g_bytes{0};

inline void reset() noexcept {
    g_calls.store(0, std::memory_order_relaxed);
    g_bytes.store(0, std::memory_order_relaxed);
}

inline void enable() noexcept {
    g_track.store(true, std::memory_order_release);
}

inline void disable() noexcept {
    g_track.store(false, std::memory_order_release);
}

[[nodiscard]] inline std::uint64_t calls() noexcept {
    return g_calls.load(std::memory_order_acquire);
}

[[nodiscard]] inline std::uint64_t bytes() noexcept {
    return g_bytes.load(std::memory_order_acquire);
}

inline void record(std::size_t size) noexcept {
    if (g_track.load(std::memory_order_relaxed)) {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        g_bytes.fetch_add(size, std::memory_order_relaxed);
    }
}

struct ScopedGuard {
    ScopedGuard() noexcept { enable(); }
    ~ScopedGuard() noexcept { disable(); }
    ScopedGuard(const ScopedGuard&) = delete;
    ScopedGuard& operator=(const ScopedGuard&) = delete;
};

} // namespace nimblefix::bench_audit

extern "C" {
void* __real_malloc(std::size_t);
void* __real_calloc(std::size_t, std::size_t);
void* __real_realloc(void*, std::size_t);
void* __real_aligned_alloc(std::size_t, std::size_t);
int __real_posix_memalign(void**, std::size_t, std::size_t);

void* __wrap_malloc(std::size_t size) {
    nimblefix::bench_audit::record(size);
    return __real_malloc(size);
}

void* __wrap_calloc(std::size_t nmemb, std::size_t size) {
    const std::size_t total = (nmemb != 0 && size > (static_cast<std::size_t>(-1) / nmemb))
        ? static_cast<std::size_t>(-1)
        : nmemb * size;
    nimblefix::bench_audit::record(total);
    return __real_calloc(nmemb, size);
}

void* __wrap_realloc(void* ptr, std::size_t size) {
    nimblefix::bench_audit::record(size);
    return __real_realloc(ptr, size);
}

void* __wrap_aligned_alloc(std::size_t alignment, std::size_t size) {
    nimblefix::bench_audit::record(size);
    return __real_aligned_alloc(alignment, size);
}

int __wrap_posix_memalign(void** memptr, std::size_t alignment, std::size_t size) {
    nimblefix::bench_audit::record(size);
    return __real_posix_memalign(memptr, alignment, size);
}
} // extern "C"

void* operator new(std::size_t size) {
    const std::size_t actual = size == 0 ? 1 : size;
    nimblefix::bench_audit::record(actual);
    if (void* p = __real_malloc(actual)) return p;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    const std::size_t actual = size == 0 ? 1 : size;
    nimblefix::bench_audit::record(actual);
    return __real_malloc(actual);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    const std::size_t actual = size == 0 ? 1 : size;
    const std::size_t align = std::max<std::size_t>(static_cast<std::size_t>(alignment), sizeof(void*));
    nimblefix::bench_audit::record(actual);
    void* p = nullptr;
    if (__real_posix_memalign(&p, align, actual) == 0 && p != nullptr) return p;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    const std::size_t actual = size == 0 ? 1 : size;
    const std::size_t align = std::max<std::size_t>(static_cast<std::size_t>(alignment), sizeof(void*));
    nimblefix::bench_audit::record(actual);
    void* p = nullptr;
    return (__real_posix_memalign(&p, align, actual) == 0) ? p : nullptr;
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, alignment, tag);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
