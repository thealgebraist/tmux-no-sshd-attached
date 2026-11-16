#pragma once

#include "src/transport/transport.hpp"
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace tmux2::transport {

class UnixConnection;

class UnixStream : public IStream {
public:
    UnixStream(uint64_t stream_id, std::shared_ptr<UnixConnection> connection);
    ~UnixStream() override;

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

    void deliver_frame(const protocol::StreamHeader& hdr, std::span<const std::byte> payload);
    void deliver_data(std::span<const std::byte> data);

private:
    uint64_t m_stream_id{0};
    std::weak_ptr<UnixConnection> m_conn;
    bool m_closed{false};

    FrameCallback m_on_frame;
    DataCallback m_on_data;
    std::vector<std::byte> m_read_buffer;
};

class UnixConnection : public IConnection, public std::enable_shared_from_this<UnixConnection> {
public:
    UnixConnection(int socket_fd, bool is_server);
    ~UnixConnection() override;

    [[nodiscard]] TransportType type() const noexcept override {
        return TransportType::Unix;
    }

    [[nodiscard]] bool is_connected() const noexcept override;
    void close() override;

    [[nodiscard]] std::optional<pki::ClientIdentity> peer_identity() const override {
        return std::nullopt; // Unix domain socket uses peer_uid
    }

    [[nodiscard]] std::optional<uid_t> peer_uid() const override;

    std::expected<std::shared_ptr<IStream>, std::error_code> open_stream(
        uint64_t stream_id = 0,
        bool bidirectional = true
    ) override;

    std::shared_ptr<IStream> get_stream(uint64_t stream_id) override;

    void set_on_stream(StreamCallback cb) override;

    void register_with_reactor(reactor::Reactor& reactor) override;
    void unregister_from_reactor(reactor::Reactor& reactor) override;
    void poll_io() override;

    void flush_outbound();

    std::expected<void, std::error_code> queue_frame(
        uint16_t stream_id,
        uint8_t msg_type,
        uint8_t flags,
        std::span<const std::byte> payload
    );

    [[nodiscard]] int fd() const noexcept { return m_fd; }

private:
    void process_inbound_bytes();

    int m_fd{-1};
    bool m_is_server{false};
    bool m_closed{false};
    std::optional<uid_t> m_peer_uid;

    reactor::Reactor* m_reactor{nullptr};
    StreamCallback m_on_stream;

    std::unordered_map<uint64_t, std::shared_ptr<UnixStream>> m_streams;
    uint64_t m_next_stream_id{0};

    std::vector<std::byte> m_in_buffer;
    std::vector<std::byte> m_out_buffer;
    bool m_processing_inbound{false};
};

class UnixListener : public IListener {
public:
    static std::expected<std::unique_ptr<UnixListener>, std::string> create(
        const std::filesystem::path& socket_path
    );

    ~UnixListener() override;

    [[nodiscard]] TransportType type() const noexcept override {
        return TransportType::Unix;
    }

    [[nodiscard]] int listening_fd() const noexcept override {
        return m_fd;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return m_path;
    }

    std::expected<std::shared_ptr<IConnection>, std::error_code> accept_connection() override;

    void set_on_accept(AcceptCallback cb) override;

    void register_with_reactor(reactor::Reactor& reactor) override;
    void unregister_from_reactor(reactor::Reactor& reactor) override;
    void close() override;

private:
    UnixListener(int fd, std::filesystem::path path);

    int m_fd{-1};
    std::filesystem::path m_path;
    bool m_closed{false};

    reactor::Reactor* m_reactor{nullptr};
    AcceptCallback m_on_accept;
};

class UnixConnector {
public:
    static std::expected<std::shared_ptr<UnixConnection>, std::string> connect(
        const std::filesystem::path& socket_path,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
        reactor::Reactor* reactor = nullptr
    );
};

} // namespace tmux2::transport
