#include "src/transport/unix_transport.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <thread>

namespace tmux2::transport {

// --- UnixStream ---

UnixStream::UnixStream(uint64_t stream_id, std::shared_ptr<UnixConnection> connection)
    : m_stream_id(stream_id), m_conn(std::move(connection)) {}

UnixStream::~UnixStream() {
    close();
}

uint64_t UnixStream::id() const noexcept {
    return m_stream_id;
}

bool UnixStream::is_closed() const noexcept {
    return m_closed;
}

void UnixStream::close() {
    if (!m_closed) {
        m_closed = true;
        auto conn = m_conn.lock();
        if (conn && conn->is_connected()) {
            (void)conn->queue_frame(
                static_cast<uint16_t>(m_stream_id),
                0,
                protocol::kFlagFinStream,
                {}
            );
            conn->flush_outbound();
        }
    }
}

std::expected<size_t, std::error_code> UnixStream::write(std::span<const std::byte> data) {
    if (is_closed()) {
        return std::unexpected(std::make_error_code(std::errc::not_connected));
    }

    auto conn = m_conn.lock();
    if (!conn || !conn->is_connected()) {
        return std::unexpected(std::make_error_code(std::errc::not_connected));
    }

    auto res = conn->queue_frame(
        static_cast<uint16_t>(m_stream_id),
        0,
        protocol::kFlagNone,
        data
    );
    if (!res) return std::unexpected(res.error());

    conn->flush_outbound();
    return data.size();
}

std::expected<size_t, std::error_code> UnixStream::read(std::span<std::byte> out) {
    if (out.empty()) return 0;

    auto conn = m_conn.lock();
    if (conn) {
        conn->poll_io();
    }

    if (!m_read_buffer.empty()) {
        size_t count = std::min(out.size(), m_read_buffer.size());
        std::memcpy(out.data(), m_read_buffer.data(), count);
        m_read_buffer.erase(m_read_buffer.begin(), m_read_buffer.begin() + count);
        return count;
    }

    if (is_closed()) {
        return 0; // EOF
    }

    return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
}

std::expected<void, std::error_code> UnixStream::write_frame(
    uint8_t msg_type,
    uint8_t flags,
    std::span<const std::byte> payload
) {
    if (is_closed()) {
        return std::unexpected(std::make_error_code(std::errc::not_connected));
    }

    auto conn = m_conn.lock();
    if (!conn || !conn->is_connected()) {
        return std::unexpected(std::make_error_code(std::errc::not_connected));
    }

    auto res = conn->queue_frame(
        static_cast<uint16_t>(m_stream_id),
        msg_type,
        flags,
        payload
    );
    if (!res) return std::unexpected(res.error());

    conn->flush_outbound();
    return {};
}

void UnixStream::set_on_frame(FrameCallback cb) {
    m_on_frame = std::move(cb);
}

void UnixStream::set_on_data(DataCallback cb) {
    m_on_data = std::move(cb);
    if (m_on_data && !m_read_buffer.empty()) {
        m_on_data(m_read_buffer);
        m_read_buffer.clear();
    }
}

void UnixStream::deliver_frame(const protocol::StreamHeader& hdr, std::span<const std::byte> payload) {
    if (hdr.is_fin()) {
        m_closed = true;
    }

    if (m_on_frame) {
        m_on_frame(hdr, payload);
    } else if (m_on_data) {
        m_on_data(payload);
    } else {
        m_read_buffer.insert(m_read_buffer.end(), payload.begin(), payload.end());
    }
}

void UnixStream::deliver_data(std::span<const std::byte> data) {
    if (m_on_data) {
        m_on_data(data);
    } else {
        m_read_buffer.insert(m_read_buffer.end(), data.begin(), data.end());
    }
}

// --- UnixConnection ---

UnixConnection::UnixConnection(int socket_fd, bool is_server)
    : m_fd(socket_fd), m_is_server(is_server) {
    if (m_fd >= 0) {
        ucred cred{};
        socklen_t len = sizeof(cred);
        if (::getsockopt(m_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
            m_peer_uid = cred.uid;
        }
    }
}

UnixConnection::~UnixConnection() {
    close();
}

bool UnixConnection::is_connected() const noexcept {
    return !m_closed && m_fd >= 0;
}

void UnixConnection::close() {
    if (m_closed) return;
    m_closed = true;

    for (auto& [id, stream] : m_streams) {
        if (stream) stream->close();
    }
    m_streams.clear();

    if (m_reactor) {
        unregister_from_reactor(*m_reactor);
    }

    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

std::optional<uid_t> UnixConnection::peer_uid() const {
    return m_peer_uid;
}

std::expected<std::shared_ptr<IStream>, std::error_code> UnixConnection::open_stream(
    uint64_t stream_id,
    bool /*bidirectional*/
) {
    if (!is_connected()) {
        return std::unexpected(std::make_error_code(std::errc::not_connected));
    }

    if (stream_id == 0 && m_next_stream_id != 0) {
        stream_id = m_next_stream_id;
        m_next_stream_id += 2;
    } else if (stream_id >= m_next_stream_id) {
        m_next_stream_id = stream_id + 2;
    }

    auto stream = std::make_shared<UnixStream>(stream_id, shared_from_this());
    m_streams[stream_id] = stream;
    return stream;
}

std::shared_ptr<IStream> UnixConnection::get_stream(uint64_t stream_id) {
    auto it = m_streams.find(stream_id);
    if (it != m_streams.end()) return it->second;

    auto stream = std::make_shared<UnixStream>(stream_id, shared_from_this());
    m_streams[stream_id] = stream;
    return stream;
}

void UnixConnection::set_on_stream(StreamCallback cb) {
    m_on_stream = std::move(cb);
}

void UnixConnection::register_with_reactor(reactor::Reactor& reactor) {
    m_reactor = &reactor;
    if (m_fd < 0) return;

    reactor::EventFlags flags = reactor::EventFlags::Readable;
    if (!m_out_buffer.empty()) {
        flags |= reactor::EventFlags::Writable;
    }

    (void)m_reactor->register_fd(
        m_fd,
        flags,
        [this](int, reactor::EventFlags) {
            poll_io();
        }
    );
}

void UnixConnection::unregister_from_reactor(reactor::Reactor& reactor) {
    if (m_fd >= 0) {
        (void)reactor.unregister_fd(m_fd);
    }
    m_reactor = nullptr;
}

std::expected<void, std::error_code> UnixConnection::queue_frame(
    uint16_t stream_id,
    uint8_t msg_type,
    uint8_t flags,
    std::span<const std::byte> payload
) {
    if (m_closed) {
        return std::unexpected(std::make_error_code(std::errc::not_connected));
    }

    protocol::StreamHeader hdr{
        .stream_id = stream_id,
        .msg_type = msg_type,
        .flags = flags,
        .payload_len = static_cast<uint32_t>(payload.size())
    };

    auto hdr_bytes = protocol::encode_header(hdr);
    m_out_buffer.insert(m_out_buffer.end(), hdr_bytes.begin(), hdr_bytes.end());
    m_out_buffer.insert(m_out_buffer.end(), payload.begin(), payload.end());

    if (m_reactor && m_fd >= 0) {
        (void)m_reactor->modify_fd(m_fd, reactor::EventFlags::Readable | reactor::EventFlags::Writable);
    }
    return {};
}

void UnixConnection::flush_outbound() {
    if (m_fd < 0 || m_out_buffer.empty()) return;

    while (!m_out_buffer.empty()) {
        ssize_t n = ::write(m_fd, m_out_buffer.data(), m_out_buffer.size());
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            close();
            break;
        }
        m_out_buffer.erase(m_out_buffer.begin(), m_out_buffer.begin() + n);
    }

    if (m_reactor && m_fd >= 0) {
        reactor::EventFlags flags = reactor::EventFlags::Readable;
        if (!m_out_buffer.empty()) {
            flags |= reactor::EventFlags::Writable;
        }
        (void)m_reactor->modify_fd(m_fd, flags);
    }
}

void UnixConnection::process_inbound_bytes() {
    while (m_in_buffer.size() >= protocol::kStreamHeaderSize) {
        auto hdr_res = protocol::decode_header(
            std::span<const std::byte>(m_in_buffer.data(), protocol::kStreamHeaderSize)
        );
        if (!hdr_res) {
            close();
            return;
        }

        const auto& hdr = *hdr_res;
        size_t total_len = protocol::kStreamHeaderSize + hdr.payload_len;
        if (m_in_buffer.size() < total_len) {
            break;
        }

        std::span<const std::byte> payload(
            m_in_buffer.data() + protocol::kStreamHeaderSize,
            hdr.payload_len
        );

        auto it = m_streams.find(hdr.stream_id);
        std::shared_ptr<UnixStream> stream;
        if (it == m_streams.end()) {
            stream = std::make_shared<UnixStream>(hdr.stream_id, shared_from_this());
            m_streams[hdr.stream_id] = stream;
            if (m_on_stream) {
                m_on_stream(stream);
            }
        } else {
            stream = it->second;
        }

        if (stream) {
            stream->deliver_frame(hdr, payload);
        }

        m_in_buffer.erase(m_in_buffer.begin(), m_in_buffer.begin() + total_len);
    }
}

void UnixConnection::poll_io() {
    if (m_fd < 0) return;

    flush_outbound();

    std::byte temp[4096];
    while (true) {
        ssize_t n = ::read(m_fd, temp, sizeof(temp));
        if (n <= 0) {
            if (n == 0) {
                close();
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                close();
            }
            break;
        }
        m_in_buffer.insert(m_in_buffer.end(), temp, temp + n);
    }

    process_inbound_bytes();
}

// --- UnixListener ---

UnixListener::UnixListener(int fd, std::filesystem::path path)
    : m_fd(fd), m_path(std::move(path)) {}

UnixListener::~UnixListener() {
    close();
}

std::expected<std::unique_ptr<UnixListener>, std::string> UnixListener::create(
    const std::filesystem::path& socket_path
) {
    if (socket_path.empty()) {
        return std::unexpected("Socket path cannot be empty");
    }

    // Ensure parent directory exists
    std::error_code ec;
    auto parent = socket_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    // Unlink old socket file if it exists
    ::unlink(socket_path.c_str());

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return std::unexpected("Failed to create AF_UNIX socket: " + std::string(strerror(errno)));
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.string().size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return std::unexpected("AF_UNIX socket path too long");
    }
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return std::unexpected("Failed to bind AF_UNIX socket: " + std::string(strerror(errno)));
    }

    // Enforce strict 0600 permissions
    if (::chmod(socket_path.c_str(), 0600) < 0) {
        ::close(fd);
        ::unlink(socket_path.c_str());
        return std::unexpected("Failed to chmod AF_UNIX socket to 0600: " + std::string(strerror(errno)));
    }

    if (::listen(fd, 128) < 0) {
        ::close(fd);
        ::unlink(socket_path.c_str());
        return std::unexpected("Failed to listen on AF_UNIX socket: " + std::string(strerror(errno)));
    }

    int flags = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return std::unique_ptr<UnixListener>(new UnixListener(fd, socket_path));
}

std::expected<std::shared_ptr<IConnection>, std::error_code> UnixListener::accept_connection() {
    if (m_fd < 0) {
        return std::unexpected(std::make_error_code(std::errc::not_connected));
    }

    sockaddr_un client_addr{};
    socklen_t len = sizeof(client_addr);
    int client_fd = ::accept(m_fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    int flags = ::fcntl(client_fd, F_GETFL);
    ::fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    auto conn = std::make_shared<UnixConnection>(client_fd, true);
    if (m_on_accept) {
        m_on_accept(conn);
    }
    return conn;
}

void UnixListener::set_on_accept(AcceptCallback cb) {
    m_on_accept = std::move(cb);
}

void UnixListener::register_with_reactor(reactor::Reactor& reactor) {
    m_reactor = &reactor;
    if (m_fd < 0) return;

    (void)m_reactor->register_fd(m_fd, reactor::EventFlags::Readable,
        [this](int, reactor::EventFlags) {
            auto res = accept_connection();
            (void)res;
        });
}

void UnixListener::unregister_from_reactor(reactor::Reactor& reactor) {
    if (m_fd >= 0) {
        (void)reactor.unregister_fd(m_fd);
    }
    m_reactor = nullptr;
}

void UnixListener::close() {
    if (m_closed) return;
    m_closed = true;

    if (m_reactor) {
        unregister_from_reactor(*m_reactor);
    }

    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }

    if (!m_path.empty()) {
        ::unlink(m_path.c_str());
    }
}

// --- UnixConnector ---

std::expected<std::shared_ptr<UnixConnection>, std::string> UnixConnector::connect(
    const std::filesystem::path& socket_path,
    std::chrono::milliseconds timeout,
    reactor::Reactor* reactor
) {
    if (socket_path.empty()) {
        return std::unexpected("Socket path cannot be empty");
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return std::unexpected("Failed to create AF_UNIX socket: " + std::string(strerror(errno)));
    }

    int flags = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.string().size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return std::unexpected("AF_UNIX socket path too long");
    }
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    int conn_ret = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (conn_ret < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return std::unexpected("Failed to connect to AF_UNIX socket: " + std::string(strerror(errno)));
    }

    if (conn_ret < 0 && errno == EINPROGRESS) {
        auto start_time = std::chrono::steady_clock::now();
        bool connected = false;
        while (std::chrono::steady_clock::now() - start_time < timeout) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            timeval tv{.tv_sec = 0, .tv_usec = 5000};
            int sel = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
            if (sel > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                    connected = true;
                    break;
                } else {
                    ::close(fd);
                    return std::unexpected("AF_UNIX connect failed: " + std::string(strerror(err)));
                }
            }
        }
        if (!connected) {
            ::close(fd);
            return std::unexpected("AF_UNIX connection timed out");
        }
    }

    auto conn = std::make_shared<UnixConnection>(fd, false);
    if (reactor) {
        conn->register_with_reactor(*reactor);
    }
    return conn;
}

} // namespace tmux2::transport
