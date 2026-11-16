#include "src/transport/quic_transport.hpp"
#include "src/pki/identity_validator.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <thread>

namespace tmux2::transport {

// --- QuicStream ---

QuicStream::QuicStream(SSL* stream_ssl, uint64_t stream_id, std::shared_ptr<QuicConnection> connection)
    : m_ssl(stream_ssl), m_stream_id(stream_id), m_conn(std::move(connection)) {
    if (m_ssl) {
        SSL_set_blocking_mode(m_ssl, 0);
    }
}

QuicStream::~QuicStream() {
    close();
}

uint64_t QuicStream::id() const noexcept {
    return m_stream_id;
}

bool QuicStream::is_closed() const noexcept {
    return m_closed || !m_ssl;
}

void QuicStream::close() {
    if (!m_closed && m_ssl) {
        m_closed = true;
        SSL_stream_conclude(m_ssl, 0);
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }
}

std::expected<size_t, std::error_code> QuicStream::write(std::span<const std::byte> data) {
    if (is_closed()) {
        return std::unexpected(std::make_error_code(std::errc::not_connected));
    }

    size_t total_written = 0;
    while (total_written < data.size()) {
        size_t written = 0;
        int ret = SSL_write_ex(m_ssl, data.data() + total_written, data.size() - total_written, &written);
        if (ret <= 0) {
            int err = SSL_get_error(m_ssl, ret);
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                if (total_written > 0) return total_written;
                return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
            }
            return std::unexpected(std::make_error_code(std::errc::io_error));
        }
        total_written += written;
    }

    return total_written;
}

std::expected<size_t, std::error_code> QuicStream::read(std::span<std::byte> out) {
    if (out.empty()) return 0;

    // Check buffered data first
    if (!m_in_buffer.empty()) {
        size_t count = std::min(out.size(), m_in_buffer.size());
        std::memcpy(out.data(), m_in_buffer.data(), count);
        m_in_buffer.erase(m_in_buffer.begin(), m_in_buffer.begin() + count);
        return count;
    }

    if (is_closed()) {
        return std::unexpected(std::make_error_code(std::errc::not_connected));
    }

    size_t bytes_read = 0;
    int ret = SSL_read_ex(m_ssl, out.data(), out.size(), &bytes_read);
    if (ret <= 0) {
        int err = SSL_get_error(m_ssl, ret);
        if (err == SSL_ERROR_ZERO_RETURN) {
            m_closed = true;
            return 0; // EOF
        }
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    return bytes_read;
}

std::expected<void, std::error_code> QuicStream::write_frame(
    uint8_t msg_type,
    uint8_t flags,
    std::span<const std::byte> payload
) {
    protocol::StreamHeader hdr{
        .stream_id = static_cast<uint16_t>(m_stream_id),
        .msg_type = msg_type,
        .flags = flags,
        .payload_len = static_cast<uint32_t>(payload.size())
    };

    auto hdr_bytes = protocol::encode_header(hdr);
    std::vector<std::byte> packet;
    packet.reserve(protocol::kStreamHeaderSize + payload.size());
    packet.insert(packet.end(), hdr_bytes.begin(), hdr_bytes.end());
    packet.insert(packet.end(), payload.begin(), payload.end());

    auto res = write(packet);
    if (!res) return std::unexpected(res.error());
    return {};
}

void QuicStream::set_on_frame(FrameCallback cb) {
    m_on_frame = std::move(cb);
}

void QuicStream::set_on_data(DataCallback cb) {
    m_on_data = std::move(cb);
}

void QuicStream::poll_stream() {
    if (is_closed()) return;

    std::byte temp[4096];
    while (true) {
        size_t bytes_read = 0;
        int ret = SSL_read_ex(m_ssl, temp, sizeof(temp), &bytes_read);
        if (ret <= 0) {
            int err = SSL_get_error(m_ssl, ret);
            if (err == SSL_ERROR_ZERO_RETURN) {
                m_closed = true;
            }
            break;
        }
        m_in_buffer.insert(m_in_buffer.end(), temp, temp + bytes_read);
    }

    // Process callbacks
    if (m_on_frame) {
        while (m_in_buffer.size() >= protocol::kStreamHeaderSize) {
            auto hdr_res = protocol::decode_header(
                std::span<const std::byte>(m_in_buffer.data(), protocol::kStreamHeaderSize)
            );
            if (!hdr_res) {
                break;
            }
            auto hdr = *hdr_res;
            size_t total_len = protocol::kStreamHeaderSize + hdr.payload_len;
            if (m_in_buffer.size() < total_len) {
                break; // Incomplete payload
            }

            std::span<const std::byte> payload(
                m_in_buffer.data() + protocol::kStreamHeaderSize,
                hdr.payload_len
            );
            m_on_frame(hdr, payload);
            m_in_buffer.erase(m_in_buffer.begin(), m_in_buffer.begin() + total_len);
        }
    } else if (m_on_data && !m_in_buffer.empty()) {
        m_on_data(m_in_buffer);
        m_in_buffer.clear();
    }
}

// --- QuicConnection ---

QuicConnection::QuicConnection(
    UniqueSsl ssl,
    int socket_fd,
    bool owns_fd,
    std::shared_ptr<UniqueSslCtx> ctx_holder
)
    : m_ssl(std::move(ssl)),
      m_fd(socket_fd),
      m_owns_fd(owns_fd),
      m_ctx(std::move(ctx_holder)) {}

QuicConnection::~QuicConnection() {
    close();
}

bool QuicConnection::is_connected() const noexcept {
    return !m_closed && m_ssl && SSL_is_connection(m_ssl.get()) && SSL_is_init_finished(m_ssl.get());
}

void QuicConnection::close() {
    if (m_closed) return;
    m_closed = true;

    for (auto& [id, stream] : m_streams) {
        if (stream) stream->close();
    }
    m_streams.clear();

    if (m_reactor) {
        unregister_from_reactor(*m_reactor);
    }

    if (m_ssl) {
        SSL_shutdown(m_ssl.get());
        m_ssl.reset();
    }

    if (m_owns_fd && m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

std::optional<pki::ClientIdentity> QuicConnection::peer_identity() const {
    if (!m_ssl) return std::nullopt;
    X509* cert = SSL_get0_peer_certificate(m_ssl.get());
    if (!cert) {
        SSL_handle_events(m_ssl.get());
        cert = SSL_get0_peer_certificate(m_ssl.get());
    }
    if (!cert) return std::nullopt;

    auto res = pki::extract_client_identity(cert);
    if (res) return *res;
    return std::nullopt;
}

std::expected<std::shared_ptr<IStream>, std::error_code> QuicConnection::open_stream(
    uint64_t /*stream_id*/,
    bool bidirectional
) {
    if (!is_connected()) {
        return std::unexpected(std::make_error_code(std::errc::not_connected));
    }

    SSL* stream_ssl = SSL_new_stream(m_ssl.get(), bidirectional ? 0 : SSL_STREAM_FLAG_UNI);
    if (!stream_ssl) {
        return std::unexpected(std::make_error_code(std::errc::protocol_error));
    }

    uint64_t assigned_id = SSL_get_stream_id(stream_ssl);
    auto stream = std::make_shared<QuicStream>(stream_ssl, assigned_id, shared_from_this());
    m_streams[assigned_id] = stream;
    return stream;
}

std::shared_ptr<IStream> QuicConnection::get_stream(uint64_t stream_id) {
    auto it = m_streams.find(stream_id);
    if (it != m_streams.end()) return it->second;
    return nullptr;
}

void QuicConnection::set_on_stream(StreamCallback cb) {
    m_on_stream = std::move(cb);
}

void QuicConnection::register_with_reactor(reactor::Reactor& reactor) {
    m_reactor = &reactor;
    if (m_fd < 0 || !m_ssl) return;

    auto res = m_reactor->register_fd(m_fd, reactor::EventFlags::Readable | reactor::EventFlags::Writable,
        [this](int, reactor::EventFlags) {
            poll_io();
        });
    (void)res;

    m_reactor->sync_openssl_timeout(m_ssl.get(), m_timer_id);
}

void QuicConnection::unregister_from_reactor(reactor::Reactor& reactor) {
    if (m_timer_id != reactor::kInvalidTimerId) {
        reactor.timer_heap().cancel(m_timer_id);
        m_timer_id = reactor::kInvalidTimerId;
    }
    if (m_fd >= 0) {
        (void)reactor.unregister_fd(m_fd);
    }
    m_reactor = nullptr;
}

void QuicConnection::accept_new_streams() {
    if (!m_ssl) return;
    while (true) {
        SSL* stream_ssl = SSL_accept_stream(m_ssl.get(), SSL_ACCEPT_STREAM_NO_BLOCK);
        if (!stream_ssl) break;

        uint64_t sid = SSL_get_stream_id(stream_ssl);
        auto stream = std::make_shared<QuicStream>(stream_ssl, sid, shared_from_this());
        m_streams[sid] = stream;
        if (m_on_stream) {
            m_on_stream(stream);
        }
    }
}

void QuicConnection::poll_io() {
    if (!m_ssl) return;

    if (m_parent_tick) {
        m_parent_tick();
    }

    SSL_handle_events(m_ssl.get());
    accept_new_streams();

    for (auto it = m_streams.begin(); it != m_streams.end();) {
        if (it->second->is_closed()) {
            it = m_streams.erase(it);
        } else {
            it->second->poll_stream();
            ++it;
        }
    }

    if (m_reactor) {
        m_reactor->sync_openssl_timeout(m_ssl.get(), m_timer_id);
    }
}

// --- QuicListener ---

QuicListener::QuicListener(
    std::shared_ptr<UniqueSslCtx> ctx,
    UniqueSsl listener,
    int fd,
    uint16_t port
)
    : m_ctx(std::move(ctx)),
      m_listener(std::move(listener)),
      m_fd(fd),
      m_port(port) {}

QuicListener::~QuicListener() {
    close();
}

std::expected<std::unique_ptr<QuicListener>, std::string> QuicListener::create(
    const std::string& bind_ip,
    uint16_t port,
    const TlsConfig& config
) {
    auto ctx_res = create_quic_server_ctx(config);
    if (!ctx_res) return std::unexpected(ctx_res.error());
    auto ctx = std::make_shared<UniqueSslCtx>(std::move(*ctx_res));

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return std::unexpected("Failed to create UDP socket: " + std::string(strerror(errno)));
    }

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd);
        return std::unexpected("Invalid bind IP: " + bind_ip);
    }

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return std::unexpected("Failed to bind UDP socket: " + std::string(strerror(errno)));
    }

    // Retrieve bound port
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        port = ntohs(addr.sin_port);
    }

    int flags = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    UniqueSsl listener(SSL_new_listener(ctx->get(), 0));
    if (!listener) {
        ::close(fd);
        return std::unexpected("Failed to create QUIC listener");
    }

    if (!SSL_set_fd(listener.get(), fd)) {
        ::close(fd);
        return std::unexpected("Failed to set socket fd on QUIC listener");
    }

    if (!SSL_set_blocking_mode(listener.get(), 0)) {
        ::close(fd);
        return std::unexpected("Failed to set nonblocking mode on QUIC listener");
    }

    if (!SSL_listen(listener.get())) {
        ::close(fd);
        return std::unexpected("Failed to start QUIC listener");
    }

    return std::unique_ptr<QuicListener>(new QuicListener(ctx, std::move(listener), fd, port));
}

std::expected<std::shared_ptr<IConnection>, std::error_code> QuicListener::accept_connection() {
    if (!m_listener) {
        return std::unexpected(std::make_error_code(std::errc::not_connected));
    }

    SSL_handle_events(m_listener.get());
    SSL* conn_ssl = SSL_accept_connection(m_listener.get(), 0);
    if (!conn_ssl) {
        return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
    }

    SSL_set_blocking_mode(conn_ssl, 0);
    SSL_set_default_stream_mode(conn_ssl, SSL_DEFAULT_STREAM_MODE_NONE);
    SSL_set_incoming_stream_policy(conn_ssl, SSL_INCOMING_STREAM_POLICY_ACCEPT, 0);

    auto conn = std::make_shared<QuicConnection>(
        UniqueSsl(conn_ssl),
        m_fd,
        false, // Listener owns the UDP fd
        m_ctx
    );
    conn->set_parent_tick([listener = m_listener.get()]() {
        SSL_handle_events(listener);
    });

    if (m_on_accept) {
        m_on_accept(conn);
    }

    return conn;
}

void QuicListener::set_on_accept(AcceptCallback cb) {
    m_on_accept = std::move(cb);
}

void QuicListener::register_with_reactor(reactor::Reactor& reactor) {
    m_reactor = &reactor;
    if (m_fd < 0 || !m_listener) return;

    (void)m_reactor->register_fd(m_fd, reactor::EventFlags::Readable,
        [this](int, reactor::EventFlags) {
            auto res = accept_connection();
            (void)res;
        });
}

void QuicListener::unregister_from_reactor(reactor::Reactor& reactor) {
    if (m_fd >= 0) {
        (void)reactor.unregister_fd(m_fd);
    }
    m_reactor = nullptr;
}

void QuicListener::close() {
    if (m_closed) return;
    m_closed = true;

    if (m_reactor) {
        unregister_from_reactor(*m_reactor);
    }

    m_listener.reset();
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

// --- QuicConnector ---

std::expected<std::shared_ptr<QuicConnection>, std::string> QuicConnector::connect(
    const std::string& host,
    uint16_t port,
    const TlsConfig& config,
    std::chrono::milliseconds timeout,
    reactor::Reactor* reactor
) {
    auto ctx_res = create_quic_client_ctx(config);
    if (!ctx_res) return std::unexpected(ctx_res.error());
    auto ctx = std::make_shared<UniqueSslCtx>(std::move(*ctx_res));

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return std::unexpected("Failed to create UDP socket: " + std::string(strerror(errno)));
    }

    int flags = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in target_addr{};
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &target_addr.sin_addr) <= 0) {
        // Resolve host
        struct hostent* he = ::gethostbyname(host.c_str());
        if (!he) {
            ::close(fd);
            return std::unexpected("Failed to resolve host: " + host);
        }
        std::memcpy(&target_addr.sin_addr, he->h_addr_list[0], sizeof(target_addr.sin_addr));
    }

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&target_addr), sizeof(target_addr)) < 0) {
        ::close(fd);
        return std::unexpected("Failed to connect UDP socket: " + std::string(strerror(errno)));
    }

    UniqueSsl ssl(SSL_new(ctx->get()));
    if (!ssl) {
        ::close(fd);
        return std::unexpected("Failed to create client SSL object");
    }

    set_client_alpn(ssl.get(), config.alpn);
    std::string sni_host = config.expected_server_name.empty() ? host : config.expected_server_name;
    set_client_host(ssl.get(), sni_host);

    BIO* bio = BIO_new(BIO_s_datagram());
    BIO_set_fd(bio, fd, BIO_NOCLOSE);
    SSL_set_bio(ssl.get(), bio, bio);

    BIO_ADDR* peer_baddr = BIO_ADDR_new();
    BIO_ADDR_rawmake(peer_baddr, AF_INET, &target_addr.sin_addr, sizeof(target_addr.sin_addr), target_addr.sin_port);
    SSL_set1_initial_peer_addr(ssl.get(), peer_baddr);
    BIO_ADDR_free(peer_baddr);

    SSL_set_blocking_mode(ssl.get(), 0);
    SSL_set_default_stream_mode(ssl.get(), SSL_DEFAULT_STREAM_MODE_NONE);
    SSL_set_incoming_stream_policy(ssl.get(), SSL_INCOMING_STREAM_POLICY_ACCEPT, 0);

    auto start_time = std::chrono::steady_clock::now();
    bool connected = false;

    while (std::chrono::steady_clock::now() - start_time < timeout) {
        int ret = SSL_connect(ssl.get());
        if (ret == 1) {
            connected = true;
            break;
        }

        int err = SSL_get_error(ssl.get(), ret);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            ::close(fd);
            return std::unexpected("QUIC handshake failed with error: " + std::to_string(err));
        }

        SSL_handle_events(ssl.get());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (!connected) {
        ::close(fd);
        return std::unexpected("QUIC connection timed out");
    }

    // Verify peer certificate hostname if required
    if (config.verify_peer) {
        X509* server_cert = SSL_get0_peer_certificate(ssl.get());
        if (!server_cert) {
            ::close(fd);
            return std::unexpected("Server certificate missing during verification");
        }
        if (!pki::verify_server_hostname(server_cert, sni_host)) {
            ::close(fd);
            return std::unexpected("Server certificate hostname mismatch: expected " + sni_host);
        }
    }

    auto conn = std::make_shared<QuicConnection>(
        std::move(ssl),
        fd,
        true, // Client owns the UDP socket
        ctx
    );

    if (reactor) {
        conn->register_with_reactor(*reactor);
    }

    return conn;
}

} // namespace tmux2::transport
