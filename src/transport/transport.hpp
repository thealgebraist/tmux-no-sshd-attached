#pragma once

#include "src/pki/identity_validator.hpp"
#include "src/protocol/frame.hpp"
#include "include/tmux2/reactor/reactor.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace tmux2::transport {

enum class TransportType {
    Quic,
    TcpTls,
    Unix
};

inline constexpr std::string_view to_string(TransportType type) noexcept {
    switch (type) {
        case TransportType::Quic: return "quic";
        case TransportType::TcpTls: return "tcp";
        case TransportType::Unix: return "unix";
    }
    return "unknown";
}

struct TlsConfig {
    std::string ca_cert_pem;
    std::string cert_pem;
    std::string key_pem;
    std::string expected_server_name;
    std::string alpn{"tmux2/1"};
    bool require_client_cert{true};
    bool verify_peer{true};
};

class IStream {
public:
    virtual ~IStream() = default;

    [[nodiscard]] virtual uint64_t id() const noexcept = 0;
    [[nodiscard]] virtual bool is_closed() const noexcept = 0;

    virtual std::expected<size_t, std::error_code> write(std::span<const std::byte> data) = 0;
    virtual std::expected<size_t, std::error_code> read(std::span<std::byte> out) = 0;

    virtual std::expected<void, std::error_code> write_frame(
        uint8_t msg_type,
        uint8_t flags,
        std::span<const std::byte> payload
    ) = 0;

    using FrameCallback = std::function<void(const protocol::StreamHeader& hdr, std::span<const std::byte> payload)>;
    using DataCallback = std::function<void(std::span<const std::byte> data)>;

    virtual void set_on_frame(FrameCallback cb) = 0;
    virtual void set_on_data(DataCallback cb) = 0;

    virtual void close() = 0;
};

class IConnection {
public:
    virtual ~IConnection() = default;

    [[nodiscard]] virtual TransportType type() const noexcept = 0;
    [[nodiscard]] virtual bool is_connected() const noexcept = 0;
    virtual void close() = 0;

    [[nodiscard]] virtual std::optional<pki::ClientIdentity> peer_identity() const = 0;
    [[nodiscard]] virtual std::optional<uid_t> peer_uid() const = 0;

    virtual std::expected<std::shared_ptr<IStream>, std::error_code> open_stream(
        uint64_t stream_id = 0,
        bool bidirectional = true
    ) = 0;

    virtual std::shared_ptr<IStream> get_stream(uint64_t stream_id) = 0;

    using StreamCallback = std::function<void(std::shared_ptr<IStream>)>;
    virtual void set_on_stream(StreamCallback cb) = 0;

    virtual void register_with_reactor(reactor::Reactor& reactor) = 0;
    virtual void unregister_from_reactor(reactor::Reactor& reactor) = 0;
    virtual void poll_io() = 0;
};

class IListener {
public:
    virtual ~IListener() = default;

    [[nodiscard]] virtual TransportType type() const noexcept = 0;
    [[nodiscard]] virtual int listening_fd() const noexcept = 0;

    virtual std::expected<std::shared_ptr<IConnection>, std::error_code> accept_connection() = 0;

    using AcceptCallback = std::function<void(std::shared_ptr<IConnection>)>;
    virtual void set_on_accept(AcceptCallback cb) = 0;

    virtual void register_with_reactor(reactor::Reactor& reactor) = 0;
    virtual void unregister_from_reactor(reactor::Reactor& reactor) = 0;
    virtual void close() = 0;
};

} // namespace tmux2::transport
