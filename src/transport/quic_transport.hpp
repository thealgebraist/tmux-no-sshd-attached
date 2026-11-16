#pragma once

#include "src/transport/transport.hpp"
#include "src/transport/tls_utils.hpp"
#include <openssl/ssl.h>
#include <openssl/quic.h>
#include <memory>
#include <unordered_map>
#include <chrono>
#include <atomic>

namespace tmux2::transport {

class QuicConnection;

class QuicStream : public IStream {
public:
    QuicStream(SSL* stream_ssl, uint64_t stream_id, std::shared_ptr<QuicConnection> connection);
    ~QuicStream() override;

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

    // Internal poll to read available bytes and dispatch callbacks
    void poll_stream();
    void flush_outbound();

private:
    SSL* m_ssl{nullptr};
    uint64_t m_stream_id{0};
    std::weak_ptr<QuicConnection> m_conn;
    bool m_closed{false};

    FrameCallback m_on_frame;
    DataCallback m_on_data;
    std::vector<std::byte> m_in_buffer;
    std::vector<std::byte> m_out_buffer;
};

class QuicConnection : public IConnection, public std::enable_shared_from_this<QuicConnection> {
public:
    QuicConnection(
        UniqueSsl ssl,
        int socket_fd,
        bool owns_fd,
        std::shared_ptr<UniqueSslCtx> ctx_holder
    );
    ~QuicConnection() override;

    [[nodiscard]] TransportType type() const noexcept override {
        return TransportType::Quic;
    }

    [[nodiscard]] bool is_connected() const noexcept override;
    void close() override;

    [[nodiscard]] std::optional<pki::ClientIdentity> peer_identity() const override;
    [[nodiscard]] std::optional<uid_t> peer_uid() const override {
        return std::nullopt; // QUIC peer is network authenticated via PKI
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

    [[nodiscard]] SSL* ssl() const noexcept { return m_ssl.get(); }
    [[nodiscard]] int fd() const noexcept { return m_fd; }
    void set_parent_tick(std::function<void()> tick) { m_parent_tick = std::move(tick); }

private:
    void accept_new_streams();

    UniqueSsl m_ssl;
    int m_fd{-1};
    bool m_owns_fd{false};
    std::shared_ptr<UniqueSslCtx> m_ctx;
    bool m_closed{false};
    std::function<void()> m_parent_tick;

    reactor::Reactor* m_reactor{nullptr};
    reactor::TimerId m_timer_id{reactor::kInvalidTimerId};

    StreamCallback m_on_stream;
    std::unordered_map<uint64_t, std::shared_ptr<QuicStream>> m_streams;
};

class QuicListener : public IListener {
public:
    static std::expected<std::unique_ptr<QuicListener>, std::string> create(
        const std::string& bind_ip,
        uint16_t port,
        const TlsConfig& config
    );

    ~QuicListener() override;

    [[nodiscard]] TransportType type() const noexcept override {
        return TransportType::Quic;
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
    QuicListener(
        std::shared_ptr<UniqueSslCtx> ctx,
        UniqueSsl listener,
        int fd,
        uint16_t port,
        TlsConfig config
    );

    std::shared_ptr<UniqueSslCtx> m_ctx;
    UniqueSsl m_listener;
    int m_fd{-1};
    uint16_t m_port{0};
    TlsConfig m_config;
    bool m_closed{false};

    reactor::Reactor* m_reactor{nullptr};
    reactor::TimerId m_timer_id{reactor::kInvalidTimerId};
    AcceptCallback m_on_accept;
};

class QuicConnector {
public:
    static std::expected<std::shared_ptr<QuicConnection>, std::string> connect(
        const std::string& host,
        uint16_t port,
        const TlsConfig& config,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
        reactor::Reactor* reactor = nullptr,
        std::atomic<bool>* cancel = nullptr
    );
};

} // namespace tmux2::transport
