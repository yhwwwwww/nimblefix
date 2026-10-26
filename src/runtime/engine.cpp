#include <nimblefix/engine.hpp>

#include "../transport/tcp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/eventfd.h>
#include <unistd.h>

namespace nimblefix {
namespace detail {
namespace {

constexpr std::size_t no_index = std::numeric_limits<std::size_t>::max();
constexpr std::size_t io_byte_budget = 64U * 1024U;
constexpr std::size_t frame_budget = 16;
constexpr std::size_t read_chunk_size = 8192;

std::uint64_t monotonic_ms() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count());
}

std::array<char, 32> wall_timestamp() noexcept {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto seconds = time_point_cast<std::chrono::seconds>(now);
    const auto millis = duration_cast<milliseconds>(now - seconds).count();
    const std::time_t value = system_clock::to_time_t(now);
    std::tm utc{};
    std::array<char, 32> result{};
    if (::gmtime_r(&value, &utc) == nullptr) {
        std::memcpy(result.data(), "19700101-00:00:00.000", 22);
        return result;
    }
    const auto date_size = ::strftime(result.data(), result.size(), "%Y%m%d-%H:%M:%S", &utc);
    if (date_size != 17) {
        std::memcpy(result.data(), "19700101-00:00:00.000", 22);
        return result;
    }
    (void)std::snprintf(result.data() + date_size, result.size() - date_size,
        ".%03u", static_cast<unsigned>(millis));
    return result;
}

constexpr std::string_view timestamp_view(const std::array<char, 32>& value) noexcept {
    return {value.data(), 21};
}

bool plain_identifier(std::string_view value, std::size_t max_size) noexcept {
    if (value.empty() || value.size() > max_size) return false;
    for (const unsigned char c : value) {
        if (c < 0x21 || c > 0x7e || c == '=' || c == soh) return false;
    }
    return true;
}

std::string stable_session_key(std::string_view first, std::string_view second) {
    std::string key{"FIX.4.4|"};
    key.append(std::to_string(first.size()));
    key.push_back(':');
    key.append(first);
    key.push_back('|');
    key.append(std::to_string(second.size()));
    key.push_back(':');
    key.append(second);
    return key;
}

Status field_value(const FlatMessageView& message, std::uint32_t tag,
                   std::string_view& value) noexcept {
    auto found = message.find_unique(tag);
    if (!found.ok()) return found.status();
    value = found.value().value;
    return Status::success();
}

} // namespace

struct EngineRuntime::Impl {
    struct Listener {
        ListenerConfig config;
        std::optional<TcpListener> socket;
        TcpEndpoint published;
    };

    struct Peer;

    struct Acceptor {
        AcceptorConfig config;
        std::size_t listener_index{no_index};
        Peer* active{};
    };

    enum class Connection : std::uint8_t { disconnected, connecting, connected };

    struct Peer {
        bool initiator{};
        bool closed{};
        std::string name;
        std::string local_comp_id;
        std::string remote_comp_id;
        TcpEndpoint endpoint;
        std::size_t acceptor_index{no_index};
        Connection connection{Connection::disconnected};
        TcpStream stream;
        std::optional<SessionDriver> driver;
        std::string receive_buffer;
        std::uint64_t reconnect_at{};
        std::uint64_t connect_deadline{};
        bool driver_connected{};
    };

    struct Pending {
        bool closed{};
        std::size_t listener_index{no_index};
        TcpStream stream;
        std::string receive_buffer;
        std::uint64_t deadline{};
    };

    enum class TargetKind : std::uint8_t { wake, listener, pending, peer };
    struct PollTarget { TargetKind kind{}; void* pointer{}; };

    explicit Impl(EngineConfig value)
        : config(std::move(value)), field_scratch((config.max_frame_bytes / 4) + 16) {
        wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    }

    ~Impl() {
        close_all();
        if (wake_fd >= 0) ::close(wake_fd);
    }

    void close_all() noexcept {
        for (auto& peer : peers) {
            peer->stream.close();
            peer->driver.reset();
        }
        for (auto& pending_peer : pending) pending_peer->stream.close();
        for (auto& listener : listeners) {
            if (listener.socket) listener.socket->close();
        }
    }

    [[nodiscard]] Status validate_config() const noexcept {
        if (wake_fd < 0 || config.storage.directory.empty() ||
            config.heartbeat_ms == 0 || config.connect_timeout_ms == 0 ||
            config.reconnect_delay_ms == 0 || config.logon_timeout_ms == 0 ||
            config.write_stall_ms == 0 || config.logout_timeout_ms == 0 ||
            config.max_frame_bytes < 128 || config.max_frame_bytes > default_max_frame_bytes ||
            config.max_sessions == 0 || config.max_pending_connections == 0 ||
            config.max_pending_messages == 0 || config.max_pending_bytes == 0 ||
            config.max_pending_inbound_messages == 0)
            return {ErrorCode::invalid_config};
        if (config.max_pending_messages <= 8 ||
            config.max_frame_bytes > std::numeric_limits<std::size_t>::max() / 3 ||
            config.max_pending_bytes < config.max_frame_bytes * 3 ||
            config.max_pending_inbound_bytes < config.max_frame_bytes)
            return {ErrorCode::invalid_config};
        constexpr std::size_t journal_superblock_bytes = 4096;
        constexpr std::size_t journal_record_header_bytes = 64;
        if (config.storage.journal_capacity_bytes <
                journal_superblock_bytes + journal_record_header_bytes ||
            config.storage.journal_max_records == 0 ||
            config.storage.journal_max_records >
                (config.storage.journal_capacity_bytes - journal_superblock_bytes) /
                    journal_record_header_bytes)
            return {ErrorCode::invalid_config};
        if (initiators.size() + acceptors.size() > config.max_sessions)
            return {ErrorCode::invalid_config};
        return Status::success();
    }

    void wake() noexcept {
        if (wake_fd < 0) return;
        const std::uint64_t one = 1;
        const auto ignored = ::write(wake_fd, &one, sizeof(one));
        (void)ignored;
    }

    void report_error(std::string_view name, Status status) noexcept {
        if (status.ok() || observer_failed) return;
        try {
            if (error_callback) error_callback(ErrorEvent{std::string{name}, status});
        } catch (...) {
            observer_failed = true;
            fatal_status = {ErrorCode::callback_failed};
            stopping.store(true, std::memory_order_release);
            wake();
        }
    }

    SessionConfig make_session_config(const Peer& peer, SessionRole role) const {
        const auto key = stable_session_key(peer.local_comp_id, peer.remote_comp_id);
        SessionConfig session;
        session.role = role;
        session.name = peer.name;
        session.storage_directory = config.storage.directory.string();
        session.session_key = key;
        session.local_comp_id = peer.local_comp_id;
        session.remote_comp_id = peer.remote_comp_id;
        session.heartbeat_ms = config.heartbeat_ms;
        session.logon_timeout_ms = config.logon_timeout_ms;
        session.write_stall_ms = config.write_stall_ms;
        session.logout_timeout_ms = config.logout_timeout_ms;
        session.max_pending_age_ms = config.max_pending_age_ms;
        session.max_frame_bytes = config.max_frame_bytes;
        session.max_pending_messages = config.max_pending_messages;
        session.max_pending_bytes = config.max_pending_bytes;
        session.admin_reserve_bytes = config.max_frame_bytes * 2;
        session.max_pending_inbound_messages = config.max_pending_inbound_messages;
        session.max_pending_inbound_bytes = config.max_pending_inbound_bytes;
        session.journal_capacity_bytes = config.storage.journal_capacity_bytes;
        session.journal_max_records = config.storage.journal_max_records;
        return session;
    }

    [[nodiscard]] Status open_driver(Peer& peer, SessionRole role) noexcept {
        try {
            SessionCallbacks callbacks;
            callbacks.on_message = [this](SessionContext& context,
                const FlatMessageView& message, const DeliveryInfo& info) -> Status {
                if (!message_callback) return Status{ErrorCode::unsupported};
                return message_callback(context, message, info);
            };
            callbacks.on_active = [this](SessionContext& context) -> Status {
                if (!active_callback) return Status::success();
                return active_callback(context);
            };
            callbacks.on_error = [this, peer_ptr = &peer](Status status) {
                report_error(peer_ptr->name, status);
            };
            auto opened = SessionDriver::create(make_session_config(peer, role), std::move(callbacks));
            if (!opened.ok()) return opened.status();
            peer.driver.emplace(std::move(opened).value());
            return Status::success();
        } catch (...) {
            return {ErrorCode::capacity_exceeded};
        }
    }

    [[nodiscard]] Status start_initiators() noexcept {
        try {
            peers.reserve(peers.size() + initiators.size());
            for (const auto& declaration : initiators) {
                auto peer = std::make_unique<Peer>();
                peer->initiator = true;
                peer->name = declaration.name;
                peer->local_comp_id = declaration.sender_comp_id;
                peer->remote_comp_id = declaration.target_comp_id;
                peer->endpoint = declaration.endpoint;
                peer->receive_buffer.reserve(config.max_frame_bytes + read_chunk_size);
                auto opened = open_driver(*peer, SessionRole::initiator);
                if (!opened.ok()) {
                    report_error(peer->name, opened);
                    peer->reconnect_at = monotonic_ms() + config.reconnect_delay_ms;
                }
                peers.push_back(std::move(peer));
            }
            return Status::success();
        } catch (...) {
            return {ErrorCode::capacity_exceeded};
        }
    }

    [[nodiscard]] Status bind_listeners() noexcept {
        try {
        for (auto& listener : listeners) {
            auto bound = TcpListener::bind(listener.config.endpoint.host,
                listener.config.endpoint.port, static_cast<int>(listener.config.backlog));
            if (!bound.ok()) return bound.status();
            listener.socket.emplace(std::move(bound).value());
            auto endpoint = listener.socket->local_endpoint();
            if (!endpoint.ok()) return endpoint.status();
            {
                std::lock_guard lock(endpoint_mutex);
                listener.published = TcpEndpoint{endpoint.value().host, endpoint.value().port};
            }
        }
        return Status::success();
        } catch (...) {
            return {ErrorCode::capacity_exceeded};
        }
    }

    [[nodiscard]] Status create_storage_directory() noexcept {
        try {
            std::error_code error;
            std::filesystem::create_directories(config.storage.directory, error);
            if (error) return {ErrorCode::io_error, 0, static_cast<std::size_t>(error.value())};
            return Status::success();
        } catch (...) {
            return {ErrorCode::io_error};
        }
    }

    [[nodiscard]] Status begin_connect(Peer& peer, std::uint64_t now) noexcept {
        if (!peer.initiator || peer.connection != Connection::disconnected ||
            peer.closed || now < peer.reconnect_at) return Status::success();
        if (!peer.driver) {
            const auto opened = open_driver(peer, SessionRole::initiator);
            if (!opened.ok()) {
                report_error(peer.name, opened);
                peer.reconnect_at = now + config.reconnect_delay_ms;
                return opened;
            }
        }
        bool in_progress = false;
        auto connected = TcpStream::connect(peer.endpoint.host, peer.endpoint.port, in_progress);
        if (!connected.ok()) {
            report_error(peer.name, connected.status());
            peer.reconnect_at = now + config.reconnect_delay_ms;
            return connected.status();
        }
        peer.stream = std::move(connected).value();
        peer.connection = in_progress ? Connection::connecting : Connection::connected;
        peer.connect_deadline = now + config.connect_timeout_ms;
        if (!in_progress) return connected_peer(peer, now);
        return Status::success();
    }

    [[nodiscard]] Status connected_peer(Peer& peer, std::uint64_t now) noexcept {
        peer.connection = Connection::connected;
        peer.receive_buffer.clear();
        const auto wall = wall_timestamp();
        const auto status = peer.driver
            ? peer.driver->connected(now, timestamp_view(wall))
            : Status{ErrorCode::invalid_config};
        if (!status.ok()) {
            drop_peer(peer, status, now, false);
            return status;
        }
        peer.driver_connected = true;
        return Status::success();
    }

    void drop_peer(Peer& peer, Status reason, std::uint64_t now,
                   bool report) noexcept {
        const bool graceful_eof = reason.code == ErrorCode::connection_closed &&
            peer.driver && peer.driver->state() == SessionState::closed;
        if (shutdown_started && !reason.ok() && reason.code != ErrorCode::engine_stopped &&
            !graceful_eof) {
            if (shutdown_status.ok()) shutdown_status = reason;
            // A raw EOF before the Logout acknowledgement has no driver
            // error notification. Transport write errors already notified.
            if (reason.code == ErrorCode::connection_closed && peer.driver &&
                peer.driver->status().ok()) report = true;
        }
        peer.stream.close();
        const bool was_connected = peer.connection == Connection::connected;
        peer.connection = Connection::disconnected;
        peer.receive_buffer.clear();
        if (was_connected && peer.driver && peer.driver_connected) {
            const auto disconnected = peer.driver->disconnected(
                Status{ErrorCode::connection_closed}, now);
            if (!disconnected.ok() && report) report_error(peer.name, disconnected);
        }
        peer.driver_connected = false;
        // Reopen the journal from its committed sequence state on reconnect.
        // This also discards a failed driver after a protocol/callback error.
        peer.driver.reset();
        if (report) report_error(peer.name, reason);
        if (peer.initiator) {
            peer.reconnect_at = now + config.reconnect_delay_ms;
        } else {
            peer.closed = true;
            if (peer.acceptor_index < acceptors.size() &&
                acceptors[peer.acceptor_index].active == &peer)
                acceptors[peer.acceptor_index].active = nullptr;
        }
    }

    [[nodiscard]] Status start_accepts(Listener& listener, std::size_t listener_index,
                                      std::uint64_t now) noexcept {
        for (std::size_t count = 0; count < 16; ++count) {
            auto accepted = listener.socket->accept();
            if (!accepted.ok()) {
                if (accepted.status().code == ErrorCode::need_more) return Status::success();
                report_error(listener.config.name, accepted.status());
                return accepted.status();
            }
            if (pending.size() >= config.max_pending_connections)
                continue; // The RAII accepted stream closes here.
            try {
                auto connection = std::make_unique<Pending>();
                connection->listener_index = listener_index;
                connection->stream = std::move(accepted).value();
                connection->deadline = now + config.logon_timeout_ms;
                connection->receive_buffer.reserve(config.max_frame_bytes + read_chunk_size);
                pending.push_back(std::move(connection));
            } catch (...) {
                return {ErrorCode::capacity_exceeded};
            }
        }
        return Status::success();
    }

    [[nodiscard]] std::size_t match_acceptor(std::size_t listener_index,
        std::string_view sender, std::string_view target) const noexcept {
        for (std::size_t i = 0; i < acceptors.size(); ++i) {
            const auto& acceptor = acceptors[i];
            if (acceptor.listener_index == listener_index &&
                acceptor.config.remote_comp_id == sender &&
                acceptor.config.local_comp_id == target) return i;
        }
        return no_index;
    }

    [[nodiscard]] Status adopt_pending(Pending& source, FrameView first_frame,
                                       std::uint64_t now) noexcept {
        Status frame_status{};
        std::string error_name;
        bool identity_rejected = false;
        bool driver_registered = false;
        try {
        const std::string first_wire{first_frame.bytes};
        frame_status = [&]() -> Status {
            const auto scratch_size = field_scratch.size();
            FlatMessageView message;
            auto status = parse_flat(first_frame,
                std::span<FieldView>{field_scratch.data(), scratch_size}, message,
                {}, CodecLimits{config.max_frame_bytes});
            if (!status.ok()) return status;
            std::string_view type, sender, target;
            if (!(status = field_value(message, 35, type)).ok() ||
                !(status = field_value(message, 49, sender)).ok() ||
                !(status = field_value(message, 56, target)).ok()) return status;
            if (type != "A") return {ErrorCode::protocol_error, 35};
            const auto acceptor_index = match_acceptor(source.listener_index, sender, target);
            if (acceptor_index == no_index) {
                identity_rejected = true;
                return {ErrorCode::protocol_error, 49};
            }
            auto& acceptor = acceptors[acceptor_index];
            error_name = acceptor.config.name;
            if (acceptor.active != nullptr && !acceptor.active->closed) {
                identity_rejected = true;
                return {ErrorCode::protocol_error, 49};
            }

            auto peer = std::make_unique<Peer>();
            peer->name = acceptor.config.name;
            peer->local_comp_id = acceptor.config.local_comp_id;
            peer->remote_comp_id = acceptor.config.remote_comp_id;
            peer->acceptor_index = acceptor_index;
            peer->stream = std::move(source.stream);
            peer->receive_buffer = std::move(source.receive_buffer);
            peer->receive_buffer.erase(0, first_frame.consumed);
            peer->receive_buffer.reserve(config.max_frame_bytes + read_chunk_size);
            auto opened = open_driver(*peer, SessionRole::acceptor);
            if (!opened.ok()) return opened;
            peers.push_back(std::move(peer));
            auto& stored = *peers.back();
            driver_registered = true;
            acceptor.active = &stored;
            const auto connected_status = connected_peer(stored, now);
            if (!connected_status.ok()) return connected_status;
            const auto wall = wall_timestamp();
            const auto received = stored.driver->receive(first_wire, now, timestamp_view(wall));
            if (!received.ok()) {
                drop_peer(stored, received, now, false);
                return received;
            }
            return process_peer_buffer(stored, now);
        }();
        } catch (...) {
            frame_status = {ErrorCode::capacity_exceeded};
        }
        source.closed = true;
        source.stream.close();
        if (!frame_status.ok()) {
            // A rejected connection is isolated. Unknown identities do not
            // acquire a driver, journal, or sequence state.
            if (!identity_rejected && !driver_registered)
                report_error(error_name, frame_status);
        }
        return frame_status;
    }

    [[nodiscard]] Status process_peer_buffer(Peer& peer, std::uint64_t now) noexcept {
        std::size_t processed = 0;
        while (!peer.closed && peer.connection == Connection::connected &&
               !peer.receive_buffer.empty() && processed < frame_budget) {
            FrameView frame{};
            const auto status = inspect_frame(peer.receive_buffer, frame, {},
                CodecLimits{config.max_frame_bytes});
            if (status.code == ErrorCode::need_more) return Status::success();
            if (!status.ok()) {
                drop_peer(peer, status, now, true);
                return status;
            }
            const auto wall = wall_timestamp();
            const auto received = peer.driver->receive(frame.bytes, now, timestamp_view(wall));
            peer.receive_buffer.erase(0, frame.consumed);
            ++processed;
            if (!received.ok()) {
                drop_peer(peer, received, now, false);
                return received;
            }
        }
        return Status::success();
    }

    [[nodiscard]] bool has_complete_buffered_frame(Peer& peer) noexcept {
        if (peer.closed || peer.connection != Connection::connected ||
            peer.receive_buffer.empty()) return false;
        FrameView frame{};
        return inspect_frame(peer.receive_buffer, frame, {},
            CodecLimits{config.max_frame_bytes}).ok();
    }

    [[nodiscard]] Status process_pending_buffer(Pending& connection,
                                               std::uint64_t now) noexcept {
        FrameView frame{};
        const auto status = inspect_frame(connection.receive_buffer, frame, {},
            CodecLimits{config.max_frame_bytes});
        if (status.code == ErrorCode::need_more) return Status::success();
        if (!status.ok()) {
            connection.closed = true;
            connection.stream.close();
            report_error({}, status);
            return status;
        }
        return adopt_pending(connection, frame, now);
    }

    [[nodiscard]] Status read_peer(Peer& peer, std::uint64_t now) noexcept {
        std::array<char, read_chunk_size> scratch{};
        std::size_t total = 0;
        while (!peer.closed && total < io_byte_budget) {
            const auto already_buffered = process_peer_buffer(peer, now);
            if (!already_buffered.ok() || peer.closed) return already_buffered;
            if (has_complete_buffered_frame(peer)) break;
            if (peer.receive_buffer.size() >= config.max_frame_bytes) {
                drop_peer(peer, Status{ErrorCode::too_large}, now, true);
                return {ErrorCode::too_large};
            }
            const auto high_water = config.max_frame_bytes + read_chunk_size;
            const auto available = high_water - peer.receive_buffer.size();
            const auto budget = io_byte_budget - total;
            const auto read_size = std::min({scratch.size(), available, budget});
            if (read_size == 0) break;
            const auto io = peer.stream.read(std::span<char>{scratch.data(), read_size});
            if (!io.status.ok()) {
                drop_peer(peer, io.status, now, true);
                return io.status;
            }
            if (io.peer_closed) {
                drop_peer(peer, Status{ErrorCode::connection_closed}, now, false);
                return Status::success();
            }
            if (io.would_block) break;
            if (io.bytes == 0) break;
            total += io.bytes;
            try {
                peer.receive_buffer.append(scratch.data(), io.bytes);
            } catch (...) {
                drop_peer(peer, Status{ErrorCode::capacity_exceeded}, now, true);
                return {ErrorCode::capacity_exceeded};
            }
            const auto processed = process_peer_buffer(peer, now);
            if (!processed.ok() || peer.closed) return processed;
            if (has_complete_buffered_frame(peer)) break;
            if (peer.receive_buffer.size() >= config.max_frame_bytes) {
                drop_peer(peer, Status{ErrorCode::too_large}, now, true);
                return {ErrorCode::too_large};
            }
        }
        return Status::success();
    }

    [[nodiscard]] Status read_pending(Pending& connection,
                                      std::uint64_t now) noexcept {
        std::array<char, read_chunk_size> scratch{};
        std::size_t total = 0;
        while (!connection.closed && total < io_byte_budget) {
            const auto high_water = config.max_frame_bytes + read_chunk_size;
            if (connection.receive_buffer.size() >= high_water) {
                connection.closed = true;
                connection.stream.close();
                report_error({}, Status{ErrorCode::too_large});
                return {ErrorCode::too_large};
            }
            const auto available = high_water - connection.receive_buffer.size();
            const auto budget = io_byte_budget - total;
            const auto read_size = std::min({scratch.size(), available, budget});
            if (read_size == 0) break;
            const auto io = connection.stream.read(std::span<char>{scratch.data(), read_size});
            if (!io.status.ok()) {
                connection.closed = true;
                connection.stream.close();
                report_error({}, io.status);
                return io.status;
            }
            if (io.peer_closed) {
                connection.closed = true;
                connection.stream.close();
                return Status::success();
            }
            if (io.would_block || io.bytes == 0) break;
            total += io.bytes;
            try {
                connection.receive_buffer.append(scratch.data(), io.bytes);
            } catch (...) {
                connection.closed = true;
                connection.stream.close();
                report_error({}, Status{ErrorCode::capacity_exceeded});
                return {ErrorCode::capacity_exceeded};
            }
            const auto processed = process_pending_buffer(connection, now);
            if (!processed.ok() || connection.closed) return processed;
            if (connection.receive_buffer.size() >= config.max_frame_bytes) {
                connection.closed = true;
                connection.stream.close();
                report_error({}, Status{ErrorCode::too_large});
                return {ErrorCode::too_large};
            }
        }
        return Status::success();
    }

    static WriteResult socket_writer(void* context, std::span<const char> bytes) noexcept {
        auto* stream = static_cast<TcpStream*>(context);
        const auto result = stream->write(bytes);
        const auto status = result.peer_closed
            ? Status{ErrorCode::connection_closed} : result.status;
        return {result.bytes, result.would_block, status};
    }

    [[nodiscard]] Status write_peer(Peer& peer, std::uint64_t now) noexcept {
        if (peer.closed || peer.connection != Connection::connected || !peer.driver ||
            !peer.driver->needs_write()) return Status::success();
        const auto status = peer.driver->flush(&socket_writer, &peer.stream, now, io_byte_budget);
        if (!status.ok()) {
            drop_peer(peer, status, now, false);
            return status;
        }
        return Status::success();
    }

    void tick_sessions(std::uint64_t now) noexcept {
        const auto wall = wall_timestamp();
        for (auto& peer_ptr : peers) {
            auto& peer = *peer_ptr;
            if (peer.closed) continue;
            if (peer.connection == Connection::connecting && now >= peer.connect_deadline) {
                drop_peer(peer, Status{ErrorCode::io_error}, now, true);
                continue;
            }
            if (peer.connection != Connection::connected || !peer.driver) continue;
            const auto status = peer.driver->tick(now, timestamp_view(wall));
            if (!status.ok()) {
                if (shutdown_started && shutdown_status.ok()) shutdown_status = status;
                drop_peer(peer, status, now, false);
                continue;
            }
            if (peer.driver->should_close()) {
                drop_peer(peer, Status{ErrorCode::connection_closed}, now, false);
            }
        }
    }

    void sweep_closed() noexcept {
        pending.erase(std::remove_if(pending.begin(), pending.end(),
            [](const auto& item) { return item->closed; }), pending.end());
        peers.erase(std::remove_if(peers.begin(), peers.end(),
            [](const auto& item) { return item->closed; }), peers.end());
    }

    [[nodiscard]] std::uint64_t compute_deadline(std::uint64_t now) const noexcept {
        std::uint64_t deadline = now + 1000;
        for (const auto& peer : peers) {
            if (peer->closed) continue;
            if (peer->connection == Connection::disconnected && peer->initiator)
                deadline = std::min(deadline, peer->reconnect_at);
            else if (peer->connection == Connection::connecting)
                deadline = std::min(deadline, peer->connect_deadline);
            else if (peer->connection == Connection::connected && peer->driver) {
                const auto session_deadline = peer->driver->next_deadline_ms();
                if (session_deadline != 0) deadline = std::min(deadline, session_deadline);
            }
        }
        for (const auto& item : pending)
            if (!item->closed) deadline = std::min(deadline, item->deadline);
        if (shutdown_started) deadline = std::min(deadline, shutdown_deadline);
        return deadline;
    }

    [[nodiscard]] Status begin_shutdown(std::uint64_t now) noexcept {
        if (shutdown_started) return Status::success();
        shutdown_started = true;
        shutdown_deadline = now + config.logout_timeout_ms;
        for (auto& listener : listeners) {
            if (listener.socket) listener.socket->close();
        }
        for (auto& item : pending) {
            item->closed = true;
            item->stream.close();
        }
        const auto wall = wall_timestamp();
        for (auto& peer_ptr : peers) {
            auto& peer = *peer_ptr;
            if (peer.connection == Connection::connected && peer.driver) {
                // A peer Logout may already have moved the session to
                // logout_pending (or completed it) just before request_stop()
                // was observed. Preserve that in-flight handshake and avoid
                // asking the core to start a second local Logout from a state
                // where it is not valid.
                if (peer.driver->should_close()) continue;
                const auto state = peer.driver->state();
                if (state != SessionState::active &&
                    state != SessionState::awaiting_logon) continue;
                const auto status = peer.driver->logout("engine shutdown", now, timestamp_view(wall));
                if (!status.ok()) {
                    if (shutdown_status.ok()) shutdown_status = status;
                    const auto reported = peer.driver->status();
                    if (reported.code != status.code || reported.tag != status.tag ||
                        reported.offset != status.offset) report_error(peer.name, status);
                }
            } else if (peer.connection == Connection::connecting) {
                drop_peer(peer, Status{ErrorCode::engine_stopped}, now, false);
            }
        }
        return Status::success();
    }

    bool shutdown_complete(std::uint64_t now) noexcept {
        if (!shutdown_started) return false;
        for (auto& peer_ptr : peers) {
            auto& peer = *peer_ptr;
            if (peer.connection != Connection::connected) continue;
            if (peer.driver && peer.driver->should_close()) {
                drop_peer(peer, Status{ErrorCode::engine_stopped}, now, false);
                continue;
            }
            if (now >= shutdown_deadline) {
                const Status timeout{ErrorCode::logout_timeout};
                if (shutdown_status.ok()) shutdown_status = timeout;
                drop_peer(peer, timeout, now, true);
                continue;
            }
            return false;
        }
        return true;
    }

    [[nodiscard]] Status run_loop() noexcept {
        const auto initial_validation = validate_config();
        if (!initial_validation.ok()) return initial_validation;
        try {
            peers.reserve(config.max_sessions);
            pending.reserve(config.max_pending_connections);
        } catch (...) {
            return {ErrorCode::capacity_exceeded};
        }
        auto status = create_storage_directory();
        if (!status.ok()) return status;
        status = bind_listeners();
        if (!status.ok()) return status;
        status = start_initiators();
        if (!status.ok()) return status;

        std::vector<pollfd> poll_fds;
        std::vector<PollTarget> targets;
        try {
            poll_fds.reserve(1 + listeners.size() + config.max_pending_connections + config.max_sessions);
            targets.reserve(poll_fds.capacity());
        } catch (...) {
            return {ErrorCode::capacity_exceeded};
        }
        std::size_t rotation = 0;
        std::array<char, sizeof(std::uint64_t)> wake_bytes{};

        while (true) {
            auto now = monotonic_ms();
            for (auto& peer : peers) {
                if (peer->initiator && peer->connection == Connection::disconnected &&
                    !stopping.load(std::memory_order_acquire))
                    (void)begin_connect(*peer, now);
            }
            tick_sessions(now);
            bool buffered_work = false;
            for (auto& peer_ptr : peers) {
                auto& peer = *peer_ptr;
                if (peer.closed || peer.connection != Connection::connected ||
                    peer.receive_buffer.empty()) continue;
                const auto buffered_status = process_peer_buffer(peer, now);
                if (!buffered_status.ok() || peer.closed) continue;
                if (has_complete_buffered_frame(peer)) buffered_work = true;
            }
            for (auto& item : pending) {
                if (!item->closed && now >= item->deadline) {
                    item->closed = true;
                    item->stream.close();
                    report_error({}, Status{ErrorCode::logon_timeout});
                }
            }
            if (stopping.load(std::memory_order_acquire)) (void)begin_shutdown(now);
            sweep_closed();
            if (observer_failed) return fatal_status;
            if (shutdown_complete(now)) return shutdown_status;

            poll_fds.clear();
            targets.clear();
            poll_fds.push_back(pollfd{wake_fd, POLLIN, 0});
            targets.push_back({TargetKind::wake, nullptr});
            for (auto& listener : listeners) {
                if (listener.socket && listener.socket->valid()) {
                    poll_fds.push_back(pollfd{listener.socket->native_handle(), POLLIN, 0});
                    targets.push_back({TargetKind::listener, &listener});
                }
            }
            for (auto& item : pending) {
                if (!item->closed && item->stream.valid()) {
                    poll_fds.push_back(pollfd{item->stream.native_handle(), POLLIN, 0});
                    targets.push_back({TargetKind::pending, item.get()});
                }
            }
            for (auto& peer : peers) {
                if (peer->closed || !peer->stream.valid()) continue;
                short events = 0;
                if (peer->connection == Connection::connecting) events = POLLOUT;
                else if (peer->connection == Connection::connected) {
                    events = POLLIN;
                    if (peer->driver && peer->driver->needs_write()) events |= POLLOUT;
                }
                if (events != 0) {
                    poll_fds.push_back(pollfd{peer->stream.native_handle(), events, 0});
                    targets.push_back({TargetKind::peer, peer.get()});
                }
            }
            now = monotonic_ms();
            const auto deadline = compute_deadline(now);
            const auto wait_ms = buffered_work || deadline <= now ? 0U : static_cast<unsigned>(
                std::min<std::uint64_t>(deadline - now, 1000U));
            const int ready = ::poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()),
                static_cast<int>(wait_ms));
            if (ready < 0) {
                if (errno == EINTR) continue;
                return {ErrorCode::io_error, 0, static_cast<std::size_t>(errno)};
            }
            if (ready == 0) continue;

            const auto count = poll_fds.size();
            const auto start = count == 0 ? 0 : rotation % count;
            for (std::size_t offset = 0; offset < count; ++offset) {
                const auto index = (start + offset) % count;
                const auto events = poll_fds[index].revents;
                if (events == 0) continue;
                auto* target = &targets[index];
                now = monotonic_ms();
                if (target->kind == TargetKind::wake) {
                    while (::read(wake_fd, wake_bytes.data(), wake_bytes.size()) > 0) {}
                    continue;
                }
                if (target->kind == TargetKind::listener) {
                    auto* listener = static_cast<Listener*>(target->pointer);
                    if (events & (POLLERR | POLLNVAL)) {
                        if (listener->socket) listener->socket->close();
                        report_error(listener->config.name, Status{ErrorCode::io_error});
                        continue;
                    }
                    const auto listener_index = static_cast<std::size_t>(listener - listeners.data());
                    const auto accepted = start_accepts(*listener, listener_index, now);
                    if (!accepted.ok() && observer_failed) return fatal_status;
                    continue;
                }
                if (target->kind == TargetKind::pending) {
                    auto* item = static_cast<Pending*>(target->pointer);
                    if (events & (POLLERR | POLLNVAL)) {
                        item->closed = true;
                        item->stream.close();
                        report_error({}, Status{ErrorCode::io_error});
                        if (observer_failed) return fatal_status;
                        continue;
                    }
                    // POLLHUP can accompany unread Logon bytes. Drain them
                    // through the same bounded read path before treating EOF
                    // as an incomplete pending connection.
                    if (events & (POLLIN | POLLHUP)) {
                        (void)read_pending(*item, now);
                        if (observer_failed) return fatal_status;
                    }
                    continue;
                }
                auto* peer = static_cast<Peer*>(target->pointer);
                if (peer->closed) continue;
                if (peer->connection == Connection::connecting &&
                    (events & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))) {
                    const auto completed = peer->stream.finish_connect();
                    if (!completed.ok()) {
                        drop_peer(*peer, completed, now, true);
                        continue;
                    }
                    const auto connected = connected_peer(*peer, now);
                    if (!connected.ok()) continue;
                }
                if (peer->connection != Connection::connected) continue;
                if (events & (POLLERR | POLLNVAL)) {
                    drop_peer(*peer, Status{ErrorCode::io_error}, now, true);
                    continue;
                }
                // TCP may report POLLHUP together with data that has not yet
                // been read. Let read_peer apply its byte/frame budgets and
                // observe recv()==0 only after the kernel receive queue has
                // been drained; its buffered complete frames are then
                // delivered by the normal ready-work path before disconnect.
                if (events & (POLLIN | POLLHUP)) (void)read_peer(*peer, now);
                if (!peer->closed && (events & POLLOUT)) (void)write_peer(*peer, now);
                if (observer_failed) return fatal_status;
            }
            if (count != 0) rotation = (rotation + 1) % count;
        }
    }

    EngineConfig config;
    std::vector<InitiatorConfig> initiators;
    std::vector<Listener> listeners;
    std::vector<Acceptor> acceptors;
    std::vector<std::unique_ptr<Peer>> peers;
    std::vector<std::unique_ptr<Pending>> pending;
    std::vector<FieldView> field_scratch;
    EngineMessageCallback message_callback;
    EngineActiveCallback active_callback;
    EngineErrorCallback error_callback;
    int wake_fd{-1};
    std::atomic<bool> stopping{false};
    std::atomic<bool> has_run{false};
    std::mutex endpoint_mutex;
    bool shutdown_started{};
    bool observer_failed{};
    std::uint64_t shutdown_deadline{};
    Status shutdown_status{};
    Status fatal_status{ErrorCode::callback_failed};
};

EngineRuntime::EngineRuntime(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
EngineRuntime::~EngineRuntime() = default;

Result<std::unique_ptr<EngineRuntime>> EngineRuntime::create(EngineConfig config) noexcept {
    try {
        auto impl = std::make_unique<Impl>(std::move(config));
        if (impl->wake_fd < 0) return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)};
        auto runtime = std::unique_ptr<EngineRuntime>(new EngineRuntime(std::move(impl)));
        return runtime;
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded};
    }
}

Status EngineRuntime::on_message(EngineMessageCallback callback) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire)) return {ErrorCode::invalid_config};
    if (!callback) return {ErrorCode::invalid_config};
    if (impl_->message_callback) return {ErrorCode::duplicate_field};
    try { impl_->message_callback = std::move(callback); }
    catch (...) { return {ErrorCode::capacity_exceeded}; }
    return Status::success();
}

Status EngineRuntime::on_session_active(EngineActiveCallback callback) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire)) return {ErrorCode::invalid_config};
    if (!callback) return {ErrorCode::invalid_config};
    if (impl_->active_callback) return {ErrorCode::duplicate_field};
    try { impl_->active_callback = std::move(callback); }
    catch (...) { return {ErrorCode::capacity_exceeded}; }
    return Status::success();
}

Status EngineRuntime::on_error(EngineErrorCallback callback) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire)) return {ErrorCode::invalid_config};
    if (!callback) return {ErrorCode::invalid_config};
    if (impl_->error_callback) return {ErrorCode::duplicate_field};
    try { impl_->error_callback = std::move(callback); }
    catch (...) { return {ErrorCode::capacity_exceeded}; }
    return Status::success();
}

Result<SessionHandle> EngineRuntime::add_initiator(InitiatorConfig config) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire) ||
        !plain_identifier(config.name, 128) ||
        !plain_identifier(config.sender_comp_id, 64) ||
        !plain_identifier(config.target_comp_id, 64) ||
        config.sender_comp_id == config.target_comp_id ||
        !is_numeric_host(config.endpoint.host) ||
        config.endpoint.port == 0 ||
        impl_->initiators.size() + impl_->acceptors.size() >= impl_->config.max_sessions)
        return Status{ErrorCode::invalid_config};
    for (const auto& current : impl_->initiators)
        if (current.name == config.name ||
            (current.sender_comp_id == config.sender_comp_id &&
             current.target_comp_id == config.target_comp_id))
            return Status{ErrorCode::invalid_config};
    for (const auto& current : impl_->acceptors)
        if (current.config.name == config.name ||
            (current.config.local_comp_id == config.sender_comp_id &&
             current.config.remote_comp_id == config.target_comp_id))
            return Status{ErrorCode::invalid_config};
    try {
        const auto name = config.name;
        impl_->initiators.push_back(std::move(config));
        return SessionHandle{name};
    } catch (...) { return Status{ErrorCode::capacity_exceeded}; }
}

Result<ListenerHandle> EngineRuntime::add_listener(ListenerConfig config) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire) ||
        !plain_identifier(config.name, 128) || !is_numeric_host(config.endpoint.host, true) ||
        config.backlog == 0 || config.backlog > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        impl_->listeners.size() >= impl_->config.max_sessions)
        return Status{ErrorCode::invalid_config};
    for (const auto& listener : impl_->listeners)
        if (listener.config.name == config.name) return Status{ErrorCode::invalid_config};
    try {
        const auto name = config.name;
        Impl::Listener listener;
        listener.config = std::move(config);
        impl_->listeners.push_back(std::move(listener));
        return ListenerHandle{name};
    } catch (...) { return Status{ErrorCode::capacity_exceeded}; }
}

Result<SessionHandle> EngineRuntime::add_acceptor(AcceptorConfig config) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire) ||
        !plain_identifier(config.name, 128) ||
        !plain_identifier(config.local_comp_id, 64) ||
        !plain_identifier(config.remote_comp_id, 64) ||
        config.local_comp_id == config.remote_comp_id ||
        impl_->initiators.size() + impl_->acceptors.size() >= impl_->config.max_sessions)
        return Status{ErrorCode::invalid_config};
    std::size_t listener_index = no_index;
    for (std::size_t i = 0; i < impl_->listeners.size(); ++i)
        if (impl_->listeners[i].config.name == config.listener) listener_index = i;
    if (listener_index == no_index) return Status{ErrorCode::invalid_config};
    for (const auto& current : impl_->initiators)
        if (current.name == config.name) return Status{ErrorCode::invalid_config};
    for (const auto& current : impl_->acceptors)
        if (current.config.name == config.name ||
            (current.config.local_comp_id == config.local_comp_id &&
             current.config.remote_comp_id == config.remote_comp_id))
            return Status{ErrorCode::invalid_config};
    for (const auto& current : impl_->initiators)
        if (current.name == config.name ||
            (current.sender_comp_id == config.local_comp_id &&
             current.target_comp_id == config.remote_comp_id))
            return Status{ErrorCode::invalid_config};
    try {
        const auto name = config.name;
        impl_->acceptors.push_back(Impl::Acceptor{std::move(config), listener_index, nullptr});
        return SessionHandle{name};
    } catch (...) { return Status{ErrorCode::capacity_exceeded}; }
}

Status EngineRuntime::validate() const noexcept {
    if (!impl_) return {ErrorCode::invalid_config};
    const auto status = impl_->validate_config();
    if (!status.ok()) return status;
    if (!impl_->initiators.empty() && impl_->initiators.size() > impl_->config.max_sessions)
        return {ErrorCode::invalid_config};
    for (const auto& acceptor : impl_->acceptors)
        if (acceptor.listener_index >= impl_->listeners.size()) return {ErrorCode::invalid_config};
    return Status::success();
}

Status EngineRuntime::run() noexcept {
    if (!impl_) return {ErrorCode::invalid_config};
    bool expected = false;
    if (!impl_->has_run.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return {ErrorCode::invalid_config};
    try {
        auto status = impl_->run_loop();
        const bool reported_shutdown_error = !impl_->shutdown_status.ok() &&
            status.code == impl_->shutdown_status.code && status.tag == impl_->shutdown_status.tag &&
            status.offset == impl_->shutdown_status.offset;
        if (!status.ok() && !impl_->observer_failed && !reported_shutdown_error)
            impl_->report_error({}, status);
        if (impl_->observer_failed) status = impl_->fatal_status;
        impl_->close_all();
        return status;
    } catch (...) {
        impl_->report_error({}, Status{ErrorCode::capacity_exceeded});
        impl_->close_all();
        return impl_->observer_failed ? impl_->fatal_status : Status{ErrorCode::capacity_exceeded};
    }
}

void EngineRuntime::request_stop() noexcept {
    if (!impl_) return;
    impl_->stopping.store(true, std::memory_order_release);
    impl_->wake();
}

Result<TcpEndpoint> EngineRuntime::listener_endpoint(std::string_view name) const noexcept {
    if (!impl_) return Status{ErrorCode::invalid_config};
    try {
        std::lock_guard lock(impl_->endpoint_mutex);
        for (const auto& listener : impl_->listeners) {
            if (listener.config.name != name) continue;
            if (listener.published.port == 0) return Status{ErrorCode::need_more};
            return listener.published;
        }
        return Status{ErrorCode::invalid_config};
    } catch (...) {
        return Status{ErrorCode::io_error};
    }
}

} // namespace detail

std::string ErrorEvent::format() const {
    std::string result;
    if (!session_name.empty()) {
        result.append(session_name);
        result.append(": ");
    }
    result.append(status.message());
    if (status.tag != 0) {
        result.append(" (tag ");
        result.append(std::to_string(status.tag));
        result.push_back(')');
    }
    if (status.offset != 0) {
        result.append(status.code == ErrorCode::io_error ? " (system error " : " at byte ");
        result.append(std::to_string(status.offset));
        if (status.code == ErrorCode::io_error) result.push_back(')');
    }
    return result;
}

} // namespace nimblefix
