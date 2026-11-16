#include "src/transport/tls_tcp_transport.hpp"
#include "src/pki/identity_validator.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <thread>

namespace tmux2::transport {

// --- TcpStream ---

TcpStream::TcpStream(uint64_t stream_id, std::shared_ptr<TcpTlsConnection> connection)
    : m_stream_id(stream_id), m_conn(std::move(connection)) {}

TcpStream::~TcpStream() {
    close();
}

uint64_t TcpStream::id() const noexcept {
    return m_stream_id;
}

bool TcpStream::is_closed() const noexcept {
    return m_closed;
}

void TcpStream::close() {
    if (!m_closed) {
        m_closed = true;
        auto conn = m_conn.lock();
        if (conn && conn->is_connected()) {
            // Send FIN frame
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

std::expected<size_t, std::error_code> TcpStream::write(std::span<const std::byte> data) {
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

std::expected<size_t, std::error_code> TcpStream::read(std::span<std::byte> out) {
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

std::expected<void, std::error_code> TcpStream::write_frame(
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

void TcpStream::set_on_frame(FrameCallback cb) {
    m_on_frame = std::move(cb);
}

void TcpStream::set_on_data(DataCallback cb) {
    m_on_data = std::move(cb);
    if (m_on_data && !m_read_buffer.empty()) {
        m_on_data(m_read_buffer);
        m_read_buffer.clear();
    }
}

void TcpStream::deliver_frame(const protocol::StreamHeader& hdr, std::span<const std::byte> payload) {
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

void TcpStream::deliver_data(std::span<const std::byte> data) {
    if (m_on_data) {
        m_on_data(data);
    } else {
        m_read_buffer.insert(m_read_buffer.end(), data.begin(), data.end());
    }
}

// --- TcpTlsConnection ---

TcpTlsConnection::TcpTlsConnection(
    UniqueSsl ssl,
    int socket_fd,
    std::shared_ptr<UniqueSslCtx> ctx_holder,
    bool is_server
)
    : m_ssl(std::move(ssl)),
      m_fd(socket_fd),
      m_ctx(std::move(ctx_holder)),
      m_is_server(is_server) {}

TcpTlsConnection::~TcpTlsConnection() {
    close();
}

bool TcpTlsConnection::is_connected() const noexcept {
    return !m_closed && m_ssl && m_handshake_done && m_fd >= 0;
}

void TcpTlsConnection::close() {
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

    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

std::optional<pki::ClientIdentity> TcpTlsConnection::peer_identity() const {
    if (!m_ssl) return std::nullopt;
    X509* cert = SSL_get0_peer_certificate(m_ssl.get());
    if (!cert) return std::nullopt;

    auto res = pki::extract_client_identity(cert);
    if (res) return *res;
    return std::nullopt;
}

std::expected<std::shared_ptr<IStream>, std::error_code> TcpTlsConnection::open_stream(
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

    auto stream = std::make_shared<TcpStream>(stream_id, shared_from_this());
    m_streams[stream_id] = stream;
    return stream;
}

std::shared_ptr<IStream> TcpTlsConnection::get_stream(uint64_t stream_id) {
    auto it = m_streams.find(stream_id);
    if (it != m_streams.end()) return it->second;

    // Create stream on demand
    auto stream = std::make_shared<TcpStream>(stream_id, shared_from_this());
    m_streams[stream_id] = stream;
    return stream;
}

void TcpTlsConnection::set_on_stream(StreamCallback cb) {
    m_on_stream = std::move(cb);
}

void TcpTlsConnection::register_with_reactor(reactor::Reactor& reactor) {
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

void TcpTlsConnection::unregister_from_reactor(reactor::Reactor& reactor) {
    if (m_fd >= 0) {
        (void)reactor.unregister_fd(m_fd);
    }
    m_reactor = nullptr;
}

bool TcpTlsConnection::do_handshake() {
    if (m_handshake_done) return true;
    if (!m_ssl) return false;

    int ret = m_is_server ? SSL_accept(m_ssl.get()) : SSL_connect(m_ssl.get());
    if (ret == 1) {
        m_handshake_done = true;
        return true;
    }

    int err = SSL_get_error(m_ssl.get(), ret);
    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
        close();
        return false;
    }
    return false;
}

std::expected<void, std::error_code> TcpTlsConnection::queue_frame(
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

void TcpTlsConnection::flush_outbound() {
    if (!m_ssl || m_out_buffer.empty() || !m_handshake_done) return;

    while (!m_out_buffer.empty()) {
        size_t written = 0;
        int ret = SSL_write_ex(m_ssl.get(), m_out_buffer.data(), m_out_buffer.size(), &written);
        if (ret <= 0) {
            int err = SSL_get_error(m_ssl.get(), ret);
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                break;
            }
            close();
            break;
        }
        m_out_buffer.erase(m_out_buffer.begin(), m_out_buffer.begin() + written);
    }

    if (m_reactor && m_fd >= 0) {
        reactor::EventFlags flags = reactor::EventFlags::Readable;
        if (!m_out_buffer.empty()) {
            flags |= reactor::EventFlags::Writable;
        }
        (void)m_reactor->modify_fd(m_fd, flags);
    }
}

void TcpTlsConnection::process_inbound_bytes() {
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
            break; // Frame not yet fully buffered
        }

        std::span<const std::byte> payload(
            m_in_buffer.data() + protocol::kStreamHeaderSize,
            hdr.payload_len
        );

        auto it = m_streams.find(hdr.stream_id);
        std::shared_ptr<TcpStream> stream;
        if (it == m_streams.end()) {
            stream = std::make_shared<TcpStream>(hdr.stream_id, shared_from_this());
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

void TcpTlsConnection::poll_io() {
    if (!m_ssl) return;

    if (!m_handshake_done) {
        if (!do_handshake()) return;
    }

    // Flush any pending outbound frames
    flush_outbound();

    // Read inbound data
    std::byte temp[4096];
    while (true) {
        size_t bytes_read = 0;
        int ret = SSL_read_ex(m_ssl.get(), temp, sizeof(temp), &bytes_read);
        if (ret <= 0) {
            int err = SSL_get_error(m_ssl.get(), ret);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                close();
            }
            break;
        }
        m_in_buffer.insert(m_in_buffer.end(), temp, temp + bytes_read);
    }

    process_inbound_bytes();
}

// --- TcpTlsListener ---

TcpTlsListener::TcpTlsListener(
    std::shared_ptr<UniqueSslCtx> ctx,
    int fd,
    uint16_t port
)
    : m_ctx(std::move(ctx)),
      m_fd(fd),
      m_port(port) {}

TcpTlsListener::~TcpTlsListener() {
    close();
}

std::expected<std::unique_ptr<TcpTlsListener>, std::string> TcpTlsListener::create(
    const std::string& bind_ip,
    uint16_t port,
    const TlsConfig& config
) {
    auto ctx_res = create_tls_server_ctx(config);
    if (!ctx_res) return std::unexpected(ctx_res.error());
    auto ctx = std::make_shared<UniqueSslCtx>(std::move(*ctx_res));

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return std::unexpected("Failed to create TCP socket: " + std::string(strerror(errno)));
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
        return std::unexpected("Failed to bind TCP socket: " + std::string(strerror(errno)));
    }

    // Retrieve bound port
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        port = ntohs(addr.sin_port);
    }

    if (::listen(fd, 128) < 0) {
        ::close(fd);
        return std::unexpected("Failed to listen on TCP socket: " + std::string(strerror(errno)));
    }

    int flags = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return std::unique_ptr<TcpTlsListener>(new TcpTlsListener(ctx, fd, port));
}

std::expected<std::shared_ptr<IConnection>, std::error_code> TcpTlsListener::accept_connection() {
    if (m_fd < 0) {
        return std::unexpected(std::make_error_code(std::errc::not_connected));
    }

    sockaddr_in client_addr{};
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

    int nodelay = 1;
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    UniqueSsl ssl(SSL_new(m_ctx->get()));
    if (!ssl) {
        ::close(client_fd);
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }

    SSL_set_fd(ssl.get(), client_fd);

    auto conn = std::make_shared<TcpTlsConnection>(
        std::move(ssl),
        client_fd,
        m_ctx,
        true // is_server
    );

    // Initial handshake attempt
    conn->do_handshake();

    if (m_on_accept) {
        m_on_accept(conn);
    }

    return conn;
}

void TcpTlsListener::set_on_accept(AcceptCallback cb) {
    m_on_accept = std::move(cb);
}

void TcpTlsListener::register_with_reactor(reactor::Reactor& reactor) {
    m_reactor = &reactor;
    if (m_fd < 0) return;

    (void)m_reactor->register_fd(m_fd, reactor::EventFlags::Readable,
        [this](int, reactor::EventFlags) {
            auto res = accept_connection();
            (void)res;
        });
}

void TcpTlsListener::unregister_from_reactor(reactor::Reactor& reactor) {
    if (m_fd >= 0) {
        (void)reactor.unregister_fd(m_fd);
    }
    m_reactor = nullptr;
}

void TcpTlsListener::close() {
    if (m_closed) return;
    m_closed = true;

    if (m_reactor) {
        unregister_from_reactor(*m_reactor);
    }

    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

// --- TcpTlsConnector ---

std::expected<std::shared_ptr<TcpTlsConnection>, std::string> TcpTlsConnector::connect(
    const std::string& host,
    uint16_t port,
    const TlsConfig& config,
    std::chrono::milliseconds timeout,
    reactor::Reactor* reactor,
    std::atomic<bool>* cancel
) {
    auto ctx_res = create_tls_client_ctx(config);
    if (!ctx_res) return std::unexpected(ctx_res.error());
    auto ctx = std::make_shared<UniqueSslCtx>(std::move(*ctx_res));

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return std::unexpected("Failed to create TCP socket: " + std::string(strerror(errno)));
    }

    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    sockaddr_in target_addr{};
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &target_addr.sin_addr) <= 0) {
        struct hostent* he = ::gethostbyname(host.c_str());
        if (!he) {
            ::close(fd);
            return std::unexpected("Failed to resolve host: " + host);
        }
        std::memcpy(&target_addr.sin_addr, he->h_addr_list[0], sizeof(target_addr.sin_addr));
    }

    int flags = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int conn_ret = ::connect(fd, reinterpret_cast<const sockaddr*>(&target_addr), sizeof(target_addr));
    if (conn_ret < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return std::unexpected("Failed to connect TCP socket: " + std::string(strerror(errno)));
    }

    // Wait for TCP socket connect completion
    auto start_time = std::chrono::steady_clock::now();
    bool socket_connected = false;
    while (std::chrono::steady_clock::now() - start_time < timeout) {
        if (cancel && cancel->load()) {
            ::close(fd);
            return std::unexpected("TCP connection cancelled");
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        timeval tv{.tv_sec = 0, .tv_usec = 5000};
        int sel = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (sel > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                socket_connected = true;
                break;
            } else {
                ::close(fd);
                return std::unexpected("TCP connection failed: " + std::string(strerror(err)));
            }
        }
    }

    if (!socket_connected) {
        ::close(fd);
        return std::unexpected("TCP connection timed out");
    }

    UniqueSsl ssl(SSL_new(ctx->get()));
    if (!ssl) {
        ::close(fd);
        return std::unexpected("Failed to create client SSL object");
    }

    std::string sni_host = config.expected_server_name.empty() ? host : config.expected_server_name;
    set_client_host(ssl.get(), sni_host);
    set_client_alpn(ssl.get(), config.alpn);

    SSL_set_fd(ssl.get(), fd);

    auto conn = std::make_shared<TcpTlsConnection>(
        std::move(ssl),
        fd,
        ctx,
        false // is_client
    );

    // Drive TLS handshake
    while (std::chrono::steady_clock::now() - start_time < timeout) {
        if (cancel && cancel->load()) {
            conn->close();
            return std::unexpected("TCP connection cancelled");
        }
        if (conn->do_handshake()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!conn->is_handshake_done()) {
        conn->close();
        return std::unexpected("TLS 1.3 handshake timed out or failed");
    }

    // In TLS 1.3, the client's handshake finishes when it sends its Finished flight.
    // If the server rejects the client's certificate (e.g. missing cert or untrusted CA),
    // the server sends a fatal TLS alert immediately. We poll briefly to check for any
    // immediate rejection alert or socket closure.
    auto verify_start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - verify_start < std::chrono::milliseconds(30)) {
        if (cancel && cancel->load()) {
            conn->close();
            return std::unexpected("TCP connection cancelled");
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{.tv_sec = 0, .tv_usec = 2000};
        int sel = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (sel > 0) {
            char peek_buf[1];
            size_t peek_read = 0;
            int ret = SSL_peek_ex(conn->ssl(), peek_buf, sizeof(peek_buf), &peek_read);
            if (ret <= 0) {
                int err = SSL_get_error(conn->ssl(), ret);
                if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                    conn->close();
                    return std::unexpected("TLS 1.3 handshake rejected by server");
                }
            }
        }
        if (!conn->is_connected()) {
            return std::unexpected("TLS 1.3 connection closed by server");
        }
    }

    // Host verification
    if (config.verify_peer) {
        X509* server_cert = SSL_get0_peer_certificate(conn->ssl());
        if (!server_cert) {
            conn->close();
            return std::unexpected("Server certificate missing during verification");
        }
        if (!pki::verify_server_hostname(server_cert, sni_host)) {
            conn->close();
            return std::unexpected("Server certificate hostname mismatch: expected " + sni_host);
        }
    }

    if (reactor) {
        conn->register_with_reactor(*reactor);
    }

    return conn;
}

} // namespace tmux2::transport
