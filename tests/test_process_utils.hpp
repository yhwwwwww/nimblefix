#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace nimblefix::test_util {

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const char* prefix = "nimblefix-test-") {
        std::string tmpl = std::string{"/tmp/"} + prefix + "XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        char* created = ::mkdtemp(buf.data());
        if (!created) throw std::runtime_error("mkdtemp failed");
        path = created;
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    ~TempDir() {
        if (path.empty()) return;
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ec);
        for (auto it = std::filesystem::recursive_directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (it->is_directory(ec)) {
                std::filesystem::permissions(it->path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ec);
            }
        }
        std::filesystem::remove_all(path, ec);
    }
};

inline std::string read_file_bytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read file: " + p.string());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline void write_file_bytes(const std::filesystem::path& p, std::string_view data) {
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write file: " + p.string());
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) throw std::runtime_error("write failed: " + p.string());
}

struct CommandResult {
    int exit_code{-1};
    int term_signal{0};
    std::string stdout_text;
    std::string stderr_text;

    [[nodiscard]] bool ok() const noexcept { return exit_code == 0 && term_signal == 0; }
};

inline CommandResult run_command(
    const std::vector<std::string>& argv,
    const std::filesystem::path& cwd = {},
    const std::map<std::string, std::string>& env_overrides = {},
    std::string_view stdin_data = {},
    bool merge_stderr = false,
    int timeout_ms = 60'000) {
    int in_pipe[2]{-1, -1}, out_pipe[2]{-1, -1}, err_pipe[2]{-1, -1};
    if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0 || (!merge_stderr && ::pipe(err_pipe) != 0)) {
        throw std::runtime_error("pipe failed");
    }
    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0) {
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        if (merge_stderr) ::dup2(out_pipe[1], STDERR_FILENO);
        else ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        if (!merge_stderr) { ::close(err_pipe[0]); ::close(err_pipe[1]); }
        if (!cwd.empty()) {
            if (::chdir(cwd.c_str()) != 0) ::_exit(126);
        }
        for (const auto& [k, v] : env_overrides) {
            ::setenv(k.c_str(), v.c_str(), 1);
        }
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        ::_exit(127);
    }
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    if (!merge_stderr) ::close(err_pipe[1]);

    if (!stdin_data.empty()) {
        std::size_t off = 0;
        while (off < stdin_data.size()) {
            const ssize_t n = ::write(in_pipe[1], stdin_data.data() + off, stdin_data.size() - off);
            if (n > 0) off += static_cast<std::size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
    }
    ::close(in_pipe[1]);

    ::fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    if (!merge_stderr) ::fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

    CommandResult res{};
    int out_fd = out_pipe[0];
    int err_fd = merge_stderr ? -1 : err_pipe[0];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    while (out_fd >= 0 || err_fd >= 0) {
        pollfd pfds[2]{};
        int nfds = 0;
        if (out_fd >= 0) pfds[nfds++] = {out_fd, POLLIN, 0};
        if (err_fd >= 0) pfds[nfds++] = {err_fd, POLLIN, 0};
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            ::kill(pid, SIGKILL);
            break;
        }
        const int rem = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        const int rc = ::poll(pfds, static_cast<nfds_t>(nfds), std::min(rem, 100));
        if (rc < 0 && errno == EINTR) continue;
        for (int i = 0; i < nfds; ++i) {
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            for (;;) {
                const ssize_t n = ::read(pfds[i].fd, buf, sizeof(buf));
                if (n > 0) {
                    if (pfds[i].fd == out_fd) res.stdout_text.append(buf, static_cast<std::size_t>(n));
                    else res.stderr_text.append(buf, static_cast<std::size_t>(n));
                } else if (n == 0) {
                    ::close(pfds[i].fd);
                    if (pfds[i].fd == out_fd) out_fd = -1;
                    else err_fd = -1;
                    break;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    ::close(pfds[i].fd);
                    if (pfds[i].fd == out_fd) out_fd = -1;
                    else err_fd = -1;
                    break;
                }
            }
        }
    }
    if (out_fd >= 0) ::close(out_fd);
    if (err_fd >= 0) ::close(err_fd);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) res.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) res.term_signal = WTERMSIG(status);
    return res;
}

class ChildProcess {
public:
    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& o) noexcept
        : pid_(std::exchange(o.pid_, -1)),
          stdin_fd_(std::exchange(o.stdin_fd_, -1)),
          stdout_fd_(std::exchange(o.stdout_fd_, -1)),
          stderr_fd_(std::exchange(o.stderr_fd_, -1)),
          stdout_buf_(std::move(o.stdout_buf_)),
          stderr_buf_(std::move(o.stderr_buf_)),
          reaped_(o.reaped_),
          exit_code_(o.exit_code_),
          term_signal_(o.term_signal_) {}
    ChildProcess& operator=(ChildProcess&& o) noexcept {
        if (this == &o) return *this;
        terminate();
        pid_ = std::exchange(o.pid_, -1);
        stdin_fd_ = std::exchange(o.stdin_fd_, -1);
        stdout_fd_ = std::exchange(o.stdout_fd_, -1);
        stderr_fd_ = std::exchange(o.stderr_fd_, -1);
        stdout_buf_ = std::move(o.stdout_buf_);
        stderr_buf_ = std::move(o.stderr_buf_);
        reaped_ = o.reaped_;
        exit_code_ = o.exit_code_;
        term_signal_ = o.term_signal_;
        return *this;
    }
    ~ChildProcess() { terminate(); }

    static ChildProcess spawn(const std::vector<std::string>& argv,
                              const std::filesystem::path& cwd = {},
                              const std::map<std::string, std::string>& env_overrides = {}) {
        int in_p[2], out_p[2], err_p[2];
        if (::pipe(in_p) != 0 || ::pipe(out_p) != 0 || ::pipe(err_p) != 0)
            throw std::runtime_error("pipe failed");
        const pid_t pid = ::fork();
        if (pid < 0) throw std::runtime_error("fork failed");
        if (pid == 0) {
            ::dup2(in_p[0], STDIN_FILENO);
            ::dup2(out_p[1], STDOUT_FILENO);
            ::dup2(err_p[1], STDERR_FILENO);
            ::close(in_p[0]); ::close(in_p[1]);
            ::close(out_p[0]); ::close(out_p[1]);
            ::close(err_p[0]); ::close(err_p[1]);
            if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(126);
            for (const auto& [k, v] : env_overrides) ::setenv(k.c_str(), v.c_str(), 1);
            std::vector<char*> cargv;
            for (const auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
            cargv.push_back(nullptr);
            ::execvp(cargv[0], cargv.data());
            ::_exit(127);
        }
        ::close(in_p[0]);
        ::close(out_p[1]);
        ::close(err_p[1]);
        ::fcntl(out_p[0], F_SETFL, O_NONBLOCK);
        ::fcntl(err_p[0], F_SETFL, O_NONBLOCK);
        ChildProcess p;
        p.pid_ = pid;
        p.stdin_fd_ = in_p[1];
        p.stdout_fd_ = out_p[0];
        p.stderr_fd_ = err_p[0];
        return p;
    }

    void write_line(std::string_view line) {
        if (stdin_fd_ < 0) throw std::runtime_error("stdin closed");
        std::string full{line};
        full.push_back('\n');
        std::size_t off = 0;
        while (off < full.size()) {
            const ssize_t n = ::write(stdin_fd_, full.data() + off, full.size() - off);
            if (n > 0) off += static_cast<std::size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else throw std::runtime_error("write stdin failed");
        }
    }

    void close_stdin() {
        if (stdin_fd_ >= 0) { ::close(stdin_fd_); stdin_fd_ = -1; }
    }

    void send_signal(int sig) {
        if (pid_ > 0 && !reaped_) ::kill(pid_, sig);
    }

    [[nodiscard]] bool poll_alive() {
        if (pid_ <= 0 || reaped_) return false;
        int status = 0;
        const pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == pid_) {
            reaped_ = true;
            if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) term_signal_ = WTERMSIG(status);
            return false;
        }
        return true;
    }

    void drain_stderr_nonblock() {
        if (stderr_fd_ < 0) return;
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(stderr_fd_, buf, sizeof(buf));
            if (n > 0) stderr_buf_.append(buf, static_cast<std::size_t>(n));
            else if (n == 0) { ::close(stderr_fd_); stderr_fd_ = -1; break; }
            else break;
        }
    }

    std::optional<std::string> read_stdout_line(int timeout_ms = 10'000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            drain_stderr_nonblock();
            if (const auto nl = stdout_buf_.find('\n'); nl != std::string::npos) {
                std::string line = stdout_buf_.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                stdout_buf_.erase(0, nl + 1);
                all_stdout_lines_.push_back(line);
                return line;
            }
            if (stdout_fd_ < 0) return std::nullopt;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::nullopt;
            const int rem = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            pollfd pfd{stdout_fd_, POLLIN, 0};
            const int rc = ::poll(&pfd, 1, std::min(rem, 100));
            if (rc < 0 && errno == EINTR) continue;
            if (rc > 0) {
                char buf[4096];
                const ssize_t n = ::read(stdout_fd_, buf, sizeof(buf));
                if (n > 0) stdout_buf_.append(buf, static_cast<std::size_t>(n));
                else if (n == 0) { ::close(stdout_fd_); stdout_fd_ = -1; }
            }
        }
    }

    CommandResult wait_exit(int timeout_ms = 10'000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        char buf[4096];
        while (stdout_fd_ >= 0 || stderr_fd_ >= 0 || !reaped_) {
            (void)poll_alive();
            if (stdout_fd_ >= 0) {
                for (;;) {
                    const ssize_t n = ::read(stdout_fd_, buf, sizeof(buf));
                    if (n > 0) stdout_buf_.append(buf, static_cast<std::size_t>(n));
                    else if (n == 0) { ::close(stdout_fd_); stdout_fd_ = -1; break; }
                    else break;
                }
            }
            drain_stderr_nonblock();
            if (reaped_ && stdout_fd_ < 0 && stderr_fd_ < 0) break;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                if (!reaped_) ::kill(pid_, SIGKILL);
                break;
            }
            pollfd pfds[2]{};
            int nfds = 0;
            if (stdout_fd_ >= 0) pfds[nfds++] = {stdout_fd_, POLLIN, 0};
            if (stderr_fd_ >= 0) pfds[nfds++] = {stderr_fd_, POLLIN, 0};
            if (nfds > 0) (void)::poll(pfds, static_cast<nfds_t>(nfds), 20);
            else ::usleep(10'000);
        }
        if (pid_ > 0 && !reaped_) {
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
            reaped_ = true;
            if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) term_signal_ = WTERMSIG(status);
        }
        std::string full_out;
        for (const auto& l : all_stdout_lines_) { full_out += l; full_out.push_back('\n'); }
        full_out += stdout_buf_;
        return CommandResult{exit_code_, term_signal_, std::move(full_out), stderr_buf_};
    }

    void terminate() {
        close_stdin();
        if (pid_ > 0 && !reaped_) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
            reaped_ = true;
        }
        if (stdout_fd_ >= 0) { ::close(stdout_fd_); stdout_fd_ = -1; }
        if (stderr_fd_ >= 0) { ::close(stderr_fd_); stderr_fd_ = -1; }
    }

    [[nodiscard]] const std::string& stderr_text() const noexcept { return stderr_buf_; }

private:
    pid_t pid_{-1};
    int stdin_fd_{-1};
    int stdout_fd_{-1};
    int stderr_fd_{-1};
    std::string stdout_buf_;
    std::string stderr_buf_;
    std::vector<std::string> all_stdout_lines_;
    bool reaped_{false};
    int exit_code_{-1};
    int term_signal_{0};
};

inline bool generate_self_signed_cert(const std::filesystem::path& dir,
                                      std::filesystem::path& cert_out,
                                      std::filesystem::path& key_out) {
    cert_out = dir / "cert.pem";
    key_out = dir / "key.pem";
    const auto res = run_command({
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
        "-keyout", key_out.string(), "-out", cert_out.string(),
        "-subj", "/CN=localhost",
        "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
    }, {}, {}, {}, true, 20'000);
    return res.ok() && std::filesystem::is_regular_file(cert_out) && std::filesystem::is_regular_file(key_out);
}

inline std::string utc_now_fix_ms() {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    std::tm utc{};
    ::gmtime_r(&ts.tv_sec, &utc);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d.%03u",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<unsigned>(ts.tv_nsec / 1'000'000L));
    return std::string{buf};
}

inline std::string make_fix_wire(
    std::string_view version,
    std::uint64_t seq,
    std::string_view msg_type,
    std::string_view sender,
    std::string_view target,
    const std::vector<std::pair<int, std::string>>& extra = {},
    std::string_view orig_sending_time = {}) {
    const std::string now = utc_now_fix_ms();
    std::string body = "35=" + std::string{msg_type} + "\x01"
                       "49=" + std::string{sender} + "\x01"
                       "56=" + std::string{target} + "\x01"
                       "34=" + std::to_string(seq) + "\x01";
    if (!orig_sending_time.empty()) body += "43=Y\x01";
    body += "52=" + now + "\x01";
    if (!orig_sending_time.empty()) body += "122=" + std::string{orig_sending_time} + "\x01";
    for (const auto& [t, v] : extra) {
        body += std::to_string(t) + "=" + v + "\x01";
    }
    std::string raw = "8=" + std::string{version} + "\x01" "9=" + std::to_string(body.size()) + "\x01" + body;
    unsigned sum = 0;
    for (unsigned char c : raw) sum += c;
    char csum[8];
    std::snprintf(csum, sizeof(csum), "10=%03u\x01", sum % 256U);
    raw.append(csum, 7);
    return raw;
}

class RawTcpSocket {
public:
    RawTcpSocket() = default;
    explicit RawTcpSocket(int fd) noexcept : fd_(fd) {}
    RawTcpSocket(const RawTcpSocket&) = delete;
    RawTcpSocket& operator=(const RawTcpSocket&) = delete;
    RawTcpSocket(RawTcpSocket&& o) noexcept : fd_(std::exchange(o.fd_, -1)), buf_(std::move(o.buf_)) {}
    RawTcpSocket& operator=(RawTcpSocket&& o) noexcept {
        if (this == &o) return *this;
        close();
        fd_ = std::exchange(o.fd_, -1);
        buf_ = std::move(o.buf_);
        return *this;
    }
    ~RawTcpSocket() { close(); }

    void close() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }
    [[nodiscard]] int fd() const noexcept { return fd_; }

    static std::pair<RawTcpSocket, std::uint16_t> listen_loopback() {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) throw std::runtime_error("socket failed");
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 8) != 0) {
            ::close(fd);
            throw std::runtime_error("bind/listen failed");
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ::close(fd);
            throw std::runtime_error("getsockname failed");
        }
        return {RawTcpSocket{fd}, ::ntohs(addr.sin_port)};
    }

    RawTcpSocket accept(int timeout_ms = 10'000, std::uint16_t* peer_port = nullptr) {
        pollfd pfd{fd_, POLLIN, 0};
        const int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc <= 0) throw std::runtime_error("accept timed out");
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int cli = ::accept(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (cli < 0) throw std::runtime_error("accept failed");
        if (peer_port) *peer_port = ::ntohs(addr.sin_port);
        return RawTcpSocket{cli};
    }

    static RawTcpSocket connect_loopback(std::uint16_t port, int timeout_ms = 5'000) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) throw std::runtime_error("socket failed");
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = ::htons(port);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            throw std::runtime_error("connect failed");
        }
        return RawTcpSocket{fd};
    }

    void send_all(std::string_view data) {
        std::size_t off = 0;
        while (off < data.size()) {
            const ssize_t n = ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n > 0) off += static_cast<std::size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else throw std::runtime_error("send failed");
        }
    }

    std::string recv_some(std::size_t max_bytes, int timeout_ms = 5'000) {
        pollfd pfd{fd_, POLLIN, 0};
        const int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc <= 0) return {};
        std::string out(max_bytes, '\0');
        const ssize_t n = ::recv(fd_, out.data(), max_bytes, 0);
        if (n <= 0) return {};
        out.resize(static_cast<std::size_t>(n));
        return out;
    }

    std::optional<std::map<std::string, std::string>> read_fix_frame(int timeout_ms = 10'000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const auto first = buf_.find('\x01');
            const auto second = (first != std::string::npos) ? buf_.find('\x01', first + 1) : std::string::npos;
            if (first != std::string::npos && second != std::string::npos) {
                if (first + 3 > second || buf_.substr(first + 1, 2) != "9=") {
                    throw std::runtime_error("bad FIX framing");
                }
                const std::size_t body_len = std::stoull(buf_.substr(first + 3, second - first - 3));
                const std::size_t total = second + 1 + body_len + 7;
                if (buf_.size() >= total) {
                    const std::string raw = buf_.substr(0, total);
                    buf_.erase(0, total);
                    unsigned sum = 0;
                    for (std::size_t i = 0; i + 7 < raw.size(); ++i) sum += static_cast<unsigned char>(raw[i]);
                    if (raw.substr(raw.size() - 7, 3) != "10=" ||
                        static_cast<unsigned>(std::stoi(raw.substr(raw.size() - 4, 3))) != (sum % 256U)) {
                        throw std::runtime_error("bad FIX checksum");
                    }
                    std::map<std::string, std::string> fields;
                    std::size_t p = 0;
                    while (p < raw.size()) {
                        const auto soh = raw.find('\x01', p);
                        if (soh == std::string::npos) break;
                        const auto eq = raw.find('=', p);
                        if (eq != std::string::npos && eq < soh) {
                            fields[raw.substr(p, eq - p)] = raw.substr(eq + 1, soh - eq - 1);
                        }
                        p = soh + 1;
                    }
                    return fields;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::nullopt;
            const int rem = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            pollfd pfd{fd_, POLLIN, 0};
            const int rc = ::poll(&pfd, 1, std::min(rem, 100));
            if (rc < 0 && errno == EINTR) continue;
            if (rc == 0) continue;
            char tmp[8192];
            const ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
            if (n == 0) throw std::runtime_error("unexpected peer EOF");
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("recv failed");
            }
            buf_.append(tmp, static_cast<std::size_t>(n));
        }
    }

private:
    int fd_{-1};
    std::string buf_;
};

// Lightweight JSON value helpers for flat/simple CLI JSON lines.
inline std::optional<std::string> json_string_field(std::string_view json, std::string_view key) {
    const std::string pat = "\"" + std::string{key} + "\":\"";
    const auto pos = json.find(pat);
    if (pos == std::string_view::npos) return std::nullopt;
    std::size_t i = pos + pat.size();
    std::string out;
    while (i < json.size() && json[i] != '"') {
        if (json[i] == '\\' && i + 1 < json.size()) {
            ++i;
            if (json[i] == 'n') out.push_back('\n');
            else if (json[i] == 't') out.push_back('\t');
            else out.push_back(json[i]);
        } else {
            out.push_back(json[i]);
        }
        ++i;
    }
    return out;
}

inline std::optional<std::int64_t> json_int_field(std::string_view json, std::string_view key) {
    const std::string pat = "\"" + std::string{key} + "\":";
    const auto pos = json.find(pat);
    if (pos == std::string_view::npos) return std::nullopt;
    std::size_t i = pos + pat.size();
    while (i < json.size() && json[i] == ' ') ++i;
    std::int64_t val = 0;
    const auto r = std::from_chars(json.data() + i, json.data() + json.size(), val);
    if (r.ec != std::errc{}) return std::nullopt;
    return val;
}

inline std::optional<bool> json_bool_field(std::string_view json, std::string_view key) {
    const std::string pat = "\"" + std::string{key} + "\":";
    const auto pos = json.find(pat);
    if (pos == std::string_view::npos) return std::nullopt;
    std::size_t i = pos + pat.size();
    while (i < json.size() && json[i] == ' ') ++i;
    if (json.substr(i).starts_with("true")) return true;
    if (json.substr(i).starts_with("false")) return false;
    return std::nullopt;
}

} // namespace nimblefix::test_util
