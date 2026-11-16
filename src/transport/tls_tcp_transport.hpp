#pragma once

#include "src/transport/transport.hpp"
#include "src/transport/tls_utils.hpp"
#include <openssl/ssl.h>
#include <memory>
#include <unordered_map>
#include <vector>
#include <deque>
#include <chrono>
#include <atomic>

namespace tmux2::transport {

class TcpTlsConnection;

class TcpStream : public IStream {
public:
    TcpStream(uint64_t stream_id, std::shared_ptr<TcpTlsConnection> connection);
    ~TcpStream() override;

    [[nodiscard]] uint64_t id() const noexcept override;
    [[nodiscard]] bool is_closed() const noexcept override;

    std::expected<size_t, std::error_code> write(std::span<const std::byte> data) override;
    std::expected<size_t, std::error_code> read(std::span<std::byte> out) override;

    std::expected<void, std::error_code> write_frame(
        uint8_t msg_type,
        uint8_t flags,
        std::span<const std::byte> payload
    ) override;

    void set_on_frame(FrameCallback cb) override;
    void set_on_data(DataCallback cb) override;

    void close() override;

    // Delivery methods invoked by multiplexer
    void deliver_frame(const protocol::StreamHeader& hdr, std::span<const std::byte> payload);
    void deliver_data(std::span<const std::byte> data);

private:
    uint64_t m_stream_id{0};
    std::weak_ptr<TcpTlsConnection> m_conn;
    bool m_closed{false};

    FrameCallback m_on_frame;
    DataCallback m_on_data;
    std::vector<std::byte> m_read_buffer;
};

class TcpTlsConnection : public IConnection, public std::enable_shared_from_this<TcpTlsConnection> {
public:
    TcpTlsConnection(
        UniqueSsl ssl,
        int socket_fd,
        std::shared_ptr<UniqueSslCtx> ctx_holder,
        bool is_server
    );
    ~TcpTlsConnection() override;

    [[nodiscard]] TransportType type() const noexcept override {
        return TransportType::TcpTls;
    }

    [[nodiscard]] bool is_connected() const noexcept override;
    void close() override;

    [[nodiscard]] std::optional<pki::ClientIdentity> peer_identity() const override;
    [[nodiscard]] std::optional<uid_t> peer_uid() const override {
        return std::nullopt;
    }

    std::expected<std::shared_ptr<IStream>, std::error_code> open_stream(
        uint64_t stream_id = 0,
        bool bidirectional = true
    ) override;

    std::shared_ptr<IStream> get_stream(uint64_t stream_id) override;

    void set_on_stream(StreamCallback cb) override;

    void register_with_reactor(reactor::Reactor& reactor) override;
    void unregister_from_reactor(reactor::Reactor& reactor) override;
    void poll_io() override;

    // Flush pending frames into TLS stream
    void flush_outbound();

    // Enqueue a multiplexed frame for transmission
    std::expected<void, std::error_code> queue_frame(
        uint16_t stream_id,
        uint8_t msg_type,
        uint8_t flags,
        std::span<const std::byte> payload
    );

    [[nodiscard]] int fd() const noexcept { return m_fd; }
    [[nodiscard]] SSL* ssl() const noexcept { return m_ssl.get(); }
    [[nodiscard]] bool is_handshake_done() const noexcept { return m_handshake_done; }
    bool do_handshake();

private:
    void process_inbound_bytes();

    UniqueSsl m_ssl;
    int m_fd{-1};
    std::shared_ptr<UniqueSslCtx> m_ctx;
    bool m_is_server{false};
    bool m_handshake_done{false};
    bool m_closed{false};

    reactor::Reactor* m_reactor{nullptr};
    StreamCallback m_on_stream;

    std::unordered_map<uint64_t, std::shared_ptr<TcpStream>> m_streams;
    uint64_t m_next_stream_id{0};

    std::vector<std::byte> m_in_buffer;
    std::vector<std::byte> m_out_buffer;
};

class TcpTlsListener : public IListener {
public:
    static std::expected<std::unique_ptr<TcpTlsListener>, std::string> create(
        const std::string& bind_ip,
        uint16_t port,
        const TlsConfig& config
    );

    ~TcpTlsListener() override;

    [[nodiscard]] TransportType type() const noexcept override {
        return TransportType::TcpTls;
    }

    [[nodiscard]] int listening_fd() const noexcept override {
        return m_fd;
    }

    [[nodiscard]] uint16_t bound_port() const noexcept {
        return m_port;
    }

    std::expected<std::shared_ptr<IConnection>, std::error_code> accept_connection() override;

    void set_on_accept(AcceptCallback cb) override;

    void register_with_reactor(reactor::Reactor& reactor) override;
    void unregister_from_reactor(reactor::Reactor& reactor) override;
    void close() override;

private:
    TcpTlsListener(
        std::shared_ptr<UniqueSslCtx> ctx,
        int fd,
        uint16_t port
    );

    std::shared_ptr<UniqueSslCtx> m_ctx;
    int m_fd{-1};
    uint16_t m_port{0};
    bool m_closed{false};

    reactor::Reactor* m_reactor{nullptr};
    AcceptCallback m_on_accept;
};

class TcpTlsConnector {
public:
    static std::expected<std::shared_ptr<TcpTlsConnection>, std::string> connect(
        const std::string& host,
        uint16_t port,
        const TlsConfig& config,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
        reactor::Reactor* reactor = nullptr,
        std::atomic<bool>* cancel = nullptr
    );
};

} // namespace tmux2::transport
