#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "fastfix/transport/tcp_transport.h"

#include "test_support.h"

namespace {

auto MakeSocketPair() -> std::array<int, 2> {
    std::array<int, 2> fds{-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0) {
        return {-1, -1};
    }
    return fds;
}

auto SetNonBlocking(int fd) -> bool {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

auto SendRaw(int fd, const std::vector<std::byte>& bytes, std::size_t offset, std::size_t length) -> bool {
    const auto rc = send(fd, bytes.data() + offset, length, MSG_NOSIGNAL);
    return rc == static_cast<ssize_t>(length);
}

}  // namespace

TEST_CASE("transport-fault", "[transport-fault]") {
    SECTION("partial frame") {
        auto fds = MakeSocketPair();
        REQUIRE((fds[0] >= 0 && fds[1] >= 0));
        REQUIRE(SetNonBlocking(fds[0]));

        fastfix::transport::TcpConnection connection(fds[0]);
        const auto frame = ::fastfix::tests::EncodeFixFrame(
            "35=0|34=1|49=BUY|56=SELL|52=20260403-00:00:00.000|");
        REQUIRE(SendRaw(fds[1], frame, 0U, 12U));

        auto partial = connection.TryReceiveFrame();
        REQUIRE(partial.ok());
        REQUIRE(!partial.value().has_value());

        REQUIRE(SendRaw(fds[1], frame, 12U, frame.size() - 12U));
        auto completed = connection.ReceiveFrame(std::chrono::seconds(1));
        REQUIRE(completed.ok());
        REQUIRE(completed.value() == frame);
        close(fds[1]);
    }

    SECTION("coalesced frames") {
        auto fds = MakeSocketPair();
        REQUIRE((fds[0] >= 0 && fds[1] >= 0));
        REQUIRE(SetNonBlocking(fds[0]));

        fastfix::transport::TcpConnection connection(fds[0]);
        const auto first = ::fastfix::tests::EncodeFixFrame(
            "35=0|34=1|49=BUY|56=SELL|52=20260403-00:00:00.000|");
        const auto second = ::fastfix::tests::EncodeFixFrame(
            "35=5|34=2|49=BUY|56=SELL|52=20260403-00:00:01.000|");

        std::vector<std::byte> combined;
        combined.reserve(first.size() + second.size());
        combined.insert(combined.end(), first.begin(), first.end());
        combined.insert(combined.end(), second.begin(), second.end());
        REQUIRE(SendRaw(fds[1], combined, 0U, combined.size()));

        auto first_frame = connection.ReceiveFrame(std::chrono::seconds(1));
        REQUIRE(first_frame.ok());
        REQUIRE(first_frame.value() == first);

        auto second_frame = connection.TryReceiveFrame();
        REQUIRE(second_frame.ok());
        REQUIRE(second_frame.value().has_value());
        REQUIRE(second_frame.value().value() == second);
        close(fds[1]);
    }

    SECTION("split and coalesce") {
        auto fds = MakeSocketPair();
        REQUIRE((fds[0] >= 0 && fds[1] >= 0));
        REQUIRE(SetNonBlocking(fds[0]));

        fastfix::transport::TcpConnection connection(fds[0]);
        const auto first = ::fastfix::tests::EncodeFixFrame(
            "35=0|34=1|49=BUY|56=SELL|52=20260403-00:00:00.000|");
        const auto second = ::fastfix::tests::EncodeFixFrame(
            "35=1|34=2|49=BUY|56=SELL|52=20260403-00:00:01.000|112=PING|");
        const auto third = ::fastfix::tests::EncodeFixFrame(
            "35=5|34=3|49=BUY|56=SELL|52=20260403-00:00:02.000|");

        std::vector<std::byte> first_batch;
        first_batch.reserve(first.size() + (second.size() / 2U));
        first_batch.insert(first_batch.end(), first.begin(), first.end());
        first_batch.insert(
            first_batch.end(),
            second.begin(),
            second.begin() + static_cast<std::ptrdiff_t>(second.size() / 2U));
        REQUIRE(SendRaw(fds[1], first_batch, 0U, first_batch.size()));

        auto first_frame = connection.ReceiveFrame(std::chrono::seconds(1));
        REQUIRE(first_frame.ok());
        REQUIRE(first_frame.value() == first);

        auto partial_second = connection.TryReceiveFrame();
        REQUIRE(partial_second.ok());
        REQUIRE(!partial_second.value().has_value());

        std::vector<std::byte> second_batch;
        second_batch.reserve((second.size() - (second.size() / 2U)) + third.size());
        second_batch.insert(
            second_batch.end(),
            second.begin() + static_cast<std::ptrdiff_t>(second.size() / 2U),
            second.end());
        second_batch.insert(second_batch.end(), third.begin(), third.end());
        REQUIRE(SendRaw(fds[1], second_batch, 0U, second_batch.size()));

        auto completed_second = connection.ReceiveFrame(std::chrono::seconds(1));
        REQUIRE(completed_second.ok());
        REQUIRE(completed_second.value() == second);

        auto completed_third = connection.TryReceiveFrame();
        REQUIRE(completed_third.ok());
        REQUIRE(completed_third.value().has_value());
        REQUIRE(completed_third.value().value() == third);
        close(fds[1]);
    }

    SECTION("truncated connection") {
        auto fds = MakeSocketPair();
        REQUIRE((fds[0] >= 0 && fds[1] >= 0));
        REQUIRE(SetNonBlocking(fds[0]));

        fastfix::transport::TcpConnection connection(fds[0]);
        const auto frame = ::fastfix::tests::EncodeFixFrame(
            "35=0|34=1|49=BUY|56=SELL|52=20260403-00:00:00.000|");
        REQUIRE(SendRaw(fds[1], frame, 0U, frame.size() / 2U));
        close(fds[1]);

        auto truncated = connection.ReceiveFrame(std::chrono::seconds(1));
        REQUIRE(!truncated.ok());
    }

    SECTION("malformed body length") {
        auto fds = MakeSocketPair();
        REQUIRE((fds[0] >= 0 && fds[1] >= 0));
        REQUIRE(SetNonBlocking(fds[0]));

        fastfix::transport::TcpConnection connection(fds[0]);
        auto invalid = ::fastfix::tests::EncodeFixFrame(
            "35=0|34=1|49=BUY|56=SELL|52=20260403-00:00:00.000|");

        std::size_t body_length_digit = 0U;
        while (body_length_digit + 2U < invalid.size()) {
            if (std::to_integer<char>(invalid[body_length_digit]) == '9' &&
                std::to_integer<char>(invalid[body_length_digit + 1U]) == '=') {
                body_length_digit += 2U;
                break;
            }
            ++body_length_digit;
        }
        REQUIRE(body_length_digit + 1U < invalid.size());
        invalid[body_length_digit] = static_cast<std::byte>('X');
        REQUIRE(SendRaw(fds[1], invalid, 0U, invalid.size()));

        auto malformed = connection.ReceiveFrame(std::chrono::seconds(1));
        REQUIRE(!malformed.ok());
        close(fds[1]);
    }

    SECTION("missing body length") {
        auto fds = MakeSocketPair();
        REQUIRE((fds[0] >= 0 && fds[1] >= 0));
        REQUIRE(SetNonBlocking(fds[0]));

        fastfix::transport::TcpConnection connection(fds[0]);
        const auto invalid = ::fastfix::tests::Bytes("8=FIX.4.4\00135=0\001");
        REQUIRE(SendRaw(fds[1], invalid, 0U, invalid.size()));

        auto malformed = connection.ReceiveFrame(std::chrono::seconds(1));
        REQUIRE(!malformed.ok());
        close(fds[1]);
    }

    SECTION("partial second frame truncated") {
        auto fds = MakeSocketPair();
        REQUIRE((fds[0] >= 0 && fds[1] >= 0));
        REQUIRE(SetNonBlocking(fds[0]));

        fastfix::transport::TcpConnection connection(fds[0]);
        const auto first = ::fastfix::tests::EncodeFixFrame(
            "35=0|34=1|49=BUY|56=SELL|52=20260403-00:00:00.000|");
        const auto second = ::fastfix::tests::EncodeFixFrame(
            "35=1|34=2|49=BUY|56=SELL|52=20260403-00:00:01.000|112=PING|");

        std::vector<std::byte> partial;
        partial.reserve(first.size() + (second.size() / 2U));
        partial.insert(partial.end(), first.begin(), first.end());
        partial.insert(partial.end(), second.begin(), second.begin() + static_cast<std::ptrdiff_t>(second.size() / 2U));
        REQUIRE(SendRaw(fds[1], partial, 0U, partial.size()));

        auto first_frame = connection.ReceiveFrame(std::chrono::seconds(1));
        REQUIRE(first_frame.ok());
        REQUIRE(first_frame.value() == first);

        close(fds[1]);
        auto truncated_second = connection.ReceiveFrame(std::chrono::seconds(1));
        REQUIRE(!truncated_second.ok());
    }

    SECTION("send to closed fd") {
        auto fds = MakeSocketPair();
        REQUIRE((fds[0] >= 0 && fds[1] >= 0));
        REQUIRE(SetNonBlocking(fds[0]));

        fastfix::transport::TcpConnection connection(fds[0]);
        close(fds[1]);

        const auto payload = ::fastfix::tests::EncodeFixFrame(
            "35=0|34=1|49=BUY|56=SELL|52=20260403-00:00:00.000|");
        auto status = connection.Send(payload, std::chrono::seconds(1));
        REQUIRE(!status.ok());
        REQUIRE(status.code() == fastfix::base::ErrorCode::kIoError);
    }

    SECTION("write buffer backpressure") {
        auto fds = MakeSocketPair();
        REQUIRE((fds[0] >= 0 && fds[1] >= 0));
        REQUIRE(SetNonBlocking(fds[0]));

        // Shrink the send and receive buffers so the kernel saturates quickly.
        int buf_size = 4096;
        REQUIRE(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size)) == 0);
        REQUIRE(setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size)) == 0);

        fastfix::transport::TcpConnection connection(fds[0]);

        // Payload deliberately larger than the combined kernel buffers.
        std::vector<std::byte> big_payload(256U * 1024U, static_cast<std::byte>(0xAB));

        // Nobody reads from fds[1], so Send stalls once the buffer fills
        // and the poll timeout fires.
        auto status = connection.Send(big_payload, std::chrono::milliseconds(200));
        REQUIRE(!status.ok());
        REQUIRE(status.code() == fastfix::base::ErrorCode::kIoError);
        close(fds[1]);
    }

    SECTION("partial write handling") {
        auto fds = MakeSocketPair();
        REQUIRE((fds[0] >= 0 && fds[1] >= 0));
        REQUIRE(SetNonBlocking(fds[0]));
        REQUIRE(SetNonBlocking(fds[1]));

        // Small send buffer forces the kernel to accept partial writes.
        int buf_size = 2048;
        REQUIRE(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size)) == 0);

        fastfix::transport::TcpConnection connection(fds[0]);

        const std::size_t payload_size = 64U * 1024U;
        std::vector<std::byte> payload(payload_size);
        for (std::size_t i = 0; i < payload_size; ++i) {
            payload[i] = static_cast<std::byte>(i & 0xFFU);
        }

        // Drain the receiver on a separate thread so Send can make progress
        // through repeated partial writes.
        std::vector<std::byte> received;
        received.reserve(payload_size);
        std::jthread reader([&received, fd = fds[1], payload_size]() {
            std::byte buf[4096];
            while (received.size() < payload_size) {
                pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLIN;
                const int rc = poll(&pfd, 1, 5000);
                if (rc <= 0) break;
                const auto n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                received.insert(received.end(), buf, buf + n);
            }
        });

        auto status = connection.Send(payload, std::chrono::seconds(5));
        REQUIRE(status.ok());

        reader.join();
        REQUIRE(received.size() == payload_size);
        REQUIRE(received == payload);
        close(fds[1]);
    }

    SECTION("connection timeout") {
        // 10.255.255.1 is a non-routable address; connect will stall until
        // the poll timeout expires.
        auto result = fastfix::transport::TcpConnection::Connect(
            "10.255.255.1", 9999U, std::chrono::milliseconds(200));
        REQUIRE(!result.ok());
        REQUIRE(result.status().code() == fastfix::base::ErrorCode::kIoError);
    }

    SECTION("read timeout on idle connection") {
        auto fds = MakeSocketPair();
        REQUIRE((fds[0] >= 0 && fds[1] >= 0));
        REQUIRE(SetNonBlocking(fds[0]));

        fastfix::transport::TcpConnection connection(fds[0]);

        // No data is sent on fds[1], so ReceiveFrame should time out.
        auto result = connection.ReceiveFrame(std::chrono::milliseconds(100));
        REQUIRE(!result.ok());
        REQUIRE(result.status().code() == fastfix::base::ErrorCode::kIoError);
        close(fds[1]);
    }

}