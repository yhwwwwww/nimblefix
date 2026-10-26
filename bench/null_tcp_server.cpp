// Minimal nonblocking byte echo. No FIX codec, session, or store.
#include <array>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace { volatile std::sig_atomic_t stopping = 0; void stop(int) { stopping = 1; } }
int main() {
    std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
    const int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener, 8)) return 1;
    socklen_t length = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length)) return 1;
    std::cout << ntohs(address.sin_port) << std::endl;
    int client = -1;
    std::array<char, 65536> buffer;
    std::size_t used = 0, offset = 0;
    while (!stopping) {
        pollfd descriptors[2]{{listener, POLLIN, 0}, {client, static_cast<short>(used == offset ? POLLIN : POLLOUT), 0}};
        if (poll(descriptors, 2, 100) < 0) { if (errno == EINTR) continue; break; }
        if (descriptors[0].revents & POLLIN) {
            int incoming = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (incoming >= 0) {
                const int enabled = 1;
                if (client < 0 && setsockopt(incoming, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0)
                    client = incoming;
                else close(incoming);
            }
        }
        if (client < 0) continue;
        if (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) { close(client); client = -1; used = offset = 0; continue; }
        if (descriptors[1].revents & POLLIN) {
            const auto bytes = recv(client, buffer.data(), buffer.size(), 0);
            if (bytes > 0) { used = static_cast<std::size_t>(bytes); offset = 0; }
            else if (bytes == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { close(client); client = -1; }
        }
        if (client >= 0 && used > offset) {
            const auto bytes = send(client, buffer.data() + offset, used - offset, MSG_NOSIGNAL);
            if (bytes > 0) offset += static_cast<std::size_t>(bytes);
            else if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { close(client); client = -1; used = offset = 0; }
        }
    }
    if (client >= 0) close(client);
    close(listener);
}
