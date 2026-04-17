// bench/tcp_rtt.cpp
//
// Measures raw TCP round-trip latency between two loopback ports.
// No FIX protocol — just a fixed-size payload echo.
//
// Model:
//   Child process: echo server — recv N bytes, send N bytes back, repeat.
//   Parent process: initiator — records T0, sends payload, recvs echo, records
//   T1. Sender fires at fixed rate (--send-interval-us); receive runs
//   independently.
//
// Usage:
//   tcp-rtt [--iterations N] [--warmup W] [--payload-bytes B]
//           [--send-interval-us U]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// CPU affinity
// ---------------------------------------------------------------------------

static void
pin_to_core(int core)
{
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
    std::perror("sched_setaffinity");
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int
make_listener(std::uint16_t& out_port)
{
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::perror("socket");
    return -1;
  }

  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::perror("bind");
    ::close(fd);
    return -1;
  }
  if (::listen(fd, 4) < 0) {
    std::perror("listen");
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  out_port = ntohs(addr.sin_port);
  return fd;
}

static int
connect_to(std::uint16_t port)
{
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::perror("socket");
    return -1;
  }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::perror("connect");
    ::close(fd);
    return -1;
  }
  return fd;
}

// Full blocking send
static bool
send_all(int fd, const std::uint8_t* buf, std::size_t n)
{
  while (n > 0) {
    const ssize_t r = ::send(fd, buf, n, MSG_NOSIGNAL);
    if (r <= 0)
      return false;
    buf += r;
    n -= static_cast<std::size_t>(r);
  }
  return true;
}

// Full blocking recv
static bool
recv_all(int fd, std::uint8_t* buf, std::size_t n)
{
  while (n > 0) {
    const ssize_t r = ::recv(fd, buf, n, 0);
    if (r <= 0)
      return false;
    buf += r;
    n -= static_cast<std::size_t>(r);
  }
  return true;
}

// Busy-spin recv: never sleeps, spins on EAGAIN
static bool
busy_recv_all(int fd, std::uint8_t* buf, std::size_t n)
{
  while (n > 0) {
    const ssize_t r = ::recv(fd, buf, n, MSG_DONTWAIT);
    if (r == 0)
      return false;
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      return false;
    }
    buf += r;
    n -= static_cast<std::size_t>(r);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct Stats
{
  std::size_t count{ 0 };
  double avg_ns{ 0.0 };
  std::uint64_t min_ns{ 0 }, p50_ns{ 0 }, p95_ns{ 0 }, p99_ns{ 0 }, p999_ns{ 0 }, max_ns{ 0 };
};

static Stats
compute(std::vector<std::uint64_t> v)
{
  if (v.empty())
    return {};
  std::sort(v.begin(), v.end());
  const auto n = v.size();
  Stats s;
  s.count = n;
  s.min_ns = v.front();
  s.max_ns = v.back();
  s.p50_ns = v[n * 50 / 100];
  s.p95_ns = v[n * 95 / 100];
  s.p99_ns = v[n * 99 / 100];
  s.p999_ns = v[n * 999 / 1000];
  double sum = 0.0;
  for (auto x : v)
    sum += static_cast<double>(x);
  s.avg_ns = sum / static_cast<double>(n);
  return s;
}

static void
print_stats(const char* label, const Stats& s)
{
  if (s.count == 0) {
    std::printf("  %-24s  no data\n", label);
    return;
  }
  std::printf("  %-24s  count=%5zu  avg=%7.0f ns  p50=%7.0f ns"
              "  p95=%7.0f ns  p99=%7.0f ns  p999=%7.0f ns\n",
              label,
              s.count,
              s.avg_ns,
              static_cast<double>(s.p50_ns),
              static_cast<double>(s.p95_ns),
              static_cast<double>(s.p99_ns),
              static_cast<double>(s.p999_ns));
}

// ---------------------------------------------------------------------------
// Echo server (child process)
// ---------------------------------------------------------------------------

[[noreturn]] static void
run_server(int listen_fd, std::size_t payload_bytes)
{
  // Signal parent we're ready (via the listen socket existing); just accept.
  int conn = ::accept(listen_fd, nullptr, nullptr);
  ::close(listen_fd);
  if (conn < 0) {
    std::perror("[server] accept");
    _exit(1);
  }
  int one = 1;
  ::setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  std::vector<std::uint8_t> buf(payload_bytes);
  while (true) {
    if (!busy_recv_all(conn, buf.data(), payload_bytes))
      break;
    if (!send_all(conn, buf.data(), payload_bytes))
      break;
  }
  ::close(conn);
  _exit(0);
}

// ---------------------------------------------------------------------------
// Initiator (parent process)
// ---------------------------------------------------------------------------

static std::vector<std::uint64_t>
run_initiator(int fd,
              std::uint32_t iterations,
              std::uint32_t warmup,
              std::uint32_t send_interval_us,
              std::size_t payload_bytes)
{

  const std::uint32_t total = warmup + iterations;

  // Pre-allocated T0 table: t0_table[seq-1] written by sender before send;
  // readable by receiver once seq <= sent_count.
  std::vector<std::chrono::steady_clock::time_point> t0_table(total);
  std::atomic<std::uint32_t> sent_count{ 0 };

  std::vector<std::uint64_t> rtts;
  rtts.reserve(total);

  // --- Receiver thread --- pinned to core 1
  std::thread receiver([&] {
    pin_to_core(1);
    std::vector<std::uint8_t> buf(payload_bytes);
    std::uint32_t done = 0;

    while (done < total) {
      // Non-blocking peek: if nothing ready, spin briefly then retry.
      std::uint8_t first;
      ssize_t r = ::recv(fd, &first, 1, MSG_DONTWAIT);
      if (r == 0)
        break;
      if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          continue;
        break;
      }

      buf[0] = first;
      if (payload_bytes > 1) {
        if (!recv_all(fd, buf.data() + 1, payload_bytes - 1))
          break;
      }
      const auto t1 = std::chrono::steady_clock::now(); // T1: full echo received

      // Sequence number embedded in first 4 bytes.
      std::uint32_t seq = 0;
      std::memcpy(&seq, buf.data(), sizeof(seq));

      if (seq == 0 || seq > total)
        continue;

      // T0 is guaranteed written before send; send is guaranteed before
      // echo arrives → safe to read without additional synchronisation.
      // Check sent_count as a guard against any reordering edge cases.
      while (sent_count.load(std::memory_order_acquire) < seq) {
        // Spin; should be extremely rare / zero iterations.
      }
      const auto t0 = t0_table[seq - 1];
      const auto rtt =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      rtts.push_back(rtt);
      ++done;
    }
  });

  // --- Sender (main thread) ---
  std::vector<std::uint8_t> payload(payload_bytes, 0xAB);
  for (std::uint32_t i = 1; i <= total; ++i) {
    std::memcpy(payload.data(), &i, sizeof(i));

    t0_table[i - 1] = std::chrono::steady_clock::now(); // T0: before send
    send_all(fd, payload.data(), payload_bytes);
    sent_count.store(i, std::memory_order_release);

    if (send_interval_us > 0) {
      const auto deadline = t0_table[i - 1] + std::chrono::microseconds(send_interval_us);
      while (std::chrono::steady_clock::now() < deadline) {
      }
    }
  }

  receiver.join();
  return rtts;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int
main(int argc, char** argv)
{
  std::uint32_t iterations = 1000U;
  std::uint32_t warmup = 50U;
  std::uint32_t send_interval_us = 0U;
  std::size_t payload_bytes = 64U;

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (std::strcmp(arg, "--iterations") == 0 && i + 1 < argc)
      iterations = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    else if (std::strcmp(arg, "--warmup") == 0 && i + 1 < argc)
      warmup = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    else if (std::strcmp(arg, "--send-interval-us") == 0 && i + 1 < argc)
      send_interval_us = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    else if (std::strcmp(arg, "--payload-bytes") == 0 && i + 1 < argc)
      payload_bytes = std::stoul(argv[++i]);
    else {
      std::fprintf(stderr,
                   "usage: tcp-rtt [--iterations N] [--warmup W]"
                   " [--payload-bytes B] [--send-interval-us U]\n");
      return 1;
    }
  }
  if (payload_bytes < sizeof(std::uint32_t))
    payload_bytes = sizeof(std::uint32_t);

  // ---- Set up listener before fork so child inherits it ----
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  if (listen_fd < 0)
    return 1;

  // Pipe: child writes 1 byte when accept()ing so parent knows server is ready.
  int pipefd[2];
  if (::pipe(pipefd) != 0) {
    std::perror("pipe");
    return 1;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    std::perror("fork");
    return 1;
  }

  if (pid == 0) {
    // Child: echo server — pin to core 0
    pin_to_core(0);
    ::close(pipefd[0]);
    // Signal parent that we're about to accept (listener already bound).
    const std::uint8_t rdy = 1;
    ::write(pipefd[1], &rdy, 1);
    ::close(pipefd[1]);
    run_server(listen_fd, payload_bytes);
  }

  // Parent: initiator
  ::close(pipefd[1]);
  {
    std::uint8_t rdy = 0;
    if (::read(pipefd[0], &rdy, 1) != 1) {
      std::fprintf(stderr, "parent: failed to read ready signal\n");
      ::kill(pid, SIGTERM);
      return 1;
    }
  }
  ::close(pipefd[0]);
  ::close(listen_fd);

  const int fd = connect_to(port);
  if (fd < 0) {
    ::kill(pid, SIGTERM);
    return 1;
  }

  // Parent sender (main thread) pinned to core 2
  pin_to_core(2);

  std::printf("TCP loopback RTT: %u iterations, %u warmup, %zu-byte payload,"
              " send-interval=%u us  [pinned: server=cpu0 receiver=cpu1 sender=cpu2]\n",
              iterations,
              warmup,
              payload_bytes,
              send_interval_us);

  const auto wall_start = std::chrono::steady_clock::now();
  auto all_rtts = run_initiator(fd, iterations, warmup, send_interval_us, payload_bytes);
  const auto wall_end = std::chrono::steady_clock::now();

  ::close(fd);
  ::kill(pid, SIGTERM);
  int status = 0;
  ::waitpid(pid, &status, 0);

  // Strip warmup
  if (all_rtts.size() > warmup) {
    all_rtts.erase(all_rtts.begin(), all_rtts.begin() + static_cast<std::ptrdiff_t>(warmup));
  } else {
    all_rtts.clear();
  }

  const double wall_ms =
    static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count()) / 1e6;

  std::printf("\n=== TCP loopback RTT ===\n");
  print_stats("round-trip", compute(all_rtts));
  std::printf("  Total wall time: %.3f ms  (%zu messages)\n\n", wall_ms, all_rtts.size());

  return 0;
}
