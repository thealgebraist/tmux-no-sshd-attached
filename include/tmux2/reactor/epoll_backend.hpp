#pragma once

#include <tmux2/reactor/backend.hpp>

#if defined(__linux__)

#include <sys/epoll.h>
#include <unistd.h>
#include <cerrno>
#include <vector>
#include <unordered_map>

namespace tmux2::reactor {

class EpollBackend : public ReactorBackend {
public:
    static std::expected<std::unique_ptr<EpollBackend>, std::error_code> create() {
        int epfd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epfd < 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        return std::unique_ptr<EpollBackend>(new EpollBackend(epfd));
    }

    ~EpollBackend() override {
        if (m_epoll_fd >= 0) {
            ::close(m_epoll_fd);
            m_epoll_fd = -1;
        }
    }

    EpollBackend(const EpollBackend&) = delete;
    EpollBackend& operator=(const EpollBackend&) = delete;

    EpollBackend(EpollBackend&& other) noexcept
        : m_epoll_fd(other.m_epoll_fd)
        , m_registered_fds(std::move(other.m_registered_fds)) {
        other.m_epoll_fd = -1;
    }

    EpollBackend& operator=(EpollBackend&& other) noexcept {
        if (this != &other) {
            if (m_epoll_fd >= 0) {
                ::close(m_epoll_fd);
            }
            m_epoll_fd = other.m_epoll_fd;
            m_registered_fds = std::move(other.m_registered_fds);
            other.m_epoll_fd = -1;
        }
        return *this;
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "epoll";
    }

    [[nodiscard]] int fd() const noexcept {
        return m_epoll_fd;
    }

    std::expected<void, std::error_code> register_fd(int fd, EventFlags flags) override {
        if (fd < 0) {
            return std::unexpected(make_error_code(ReactorError::InvalidFileDescriptor));
        }
        if (m_registered_fds.find(fd) != m_registered_fds.end()) {
            return std::unexpected(make_error_code(ReactorError::DescriptorAlreadyRegistered));
        }

        struct epoll_event ev{};
        ev.events = to_epoll_events(flags);
        ev.data.fd = fd;

        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }

        m_registered_fds[fd] = flags;
        return {};
    }

    std::expected<void, std::error_code> modify_fd(int fd, EventFlags flags) override {
        if (m_registered_fds.find(fd) == m_registered_fds.end()) {
            return std::unexpected(make_error_code(ReactorError::DescriptorNotFound));
        }

        struct epoll_event ev{};
        ev.events = to_epoll_events(flags);
        ev.data.fd = fd;

        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }

        m_registered_fds[fd] = flags;
        return {};
    }

    std::expected<void, std::error_code> unregister_fd(int fd) override {
        auto it = m_registered_fds.find(fd);
        if (it == m_registered_fds.end()) {
            return std::unexpected(make_error_code(ReactorError::DescriptorNotFound));
        }

        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr) < 0) {
            // If the fd was already closed, epoll_ctl might return ENOENT or EBADF;
            // treat ENOENT as non-fatal when unregistering.
            if (errno != ENOENT && errno != EBADF) {
                return std::unexpected(std::error_code(errno, std::generic_category()));
            }
        }

        m_registered_fds.erase(it);
        return {};
    }

    std::expected<size_t, std::error_code> poll_events(
        std::span<PollEvent> out_events,
        std::optional<std::chrono::microseconds> timeout) override {
        if (out_events.empty()) {
            return 0;
        }

        int timeout_ms = -1;
        if (timeout.has_value()) {
            auto us = timeout->count();
            if (us <= 0) {
                timeout_ms = 0;
            } else {
                timeout_ms = static_cast<int>((us + 999) / 1000);
            }
        }

        constexpr size_t kMaxBatch = 64;
        size_t batch_size = std::min(out_events.size(), kMaxBatch);
        struct epoll_event raw_events[kMaxBatch];

        int nfds = ::epoll_wait(m_epoll_fd, raw_events, static_cast<int>(batch_size), timeout_ms);
        if (nfds < 0) {
            if (errno == EINTR) {
                return 0; // Interrupted by signal
            }
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }

        size_t count = static_cast<size_t>(nfds);
        for (size_t i = 0; i < count; ++i) {
            out_events[i] = PollEvent{
                .fd = raw_events[i].data.fd,
                .events = from_epoll_events(raw_events[i].events)
            };
        }

        return count;
    }

    [[nodiscard]] size_t descriptor_count() const noexcept {
        return m_registered_fds.size();
    }

    [[nodiscard]] bool contains(int fd) const noexcept {
        return m_registered_fds.find(fd) != m_registered_fds.end();
    }

private:
    explicit EpollBackend(int epoll_fd) : m_epoll_fd(epoll_fd) {}

    static uint32_t to_epoll_events(EventFlags flags) noexcept {
        uint32_t ev = 0;
        if (has_flag(flags, EventFlags::Readable)) {
            ev |= EPOLLIN | EPOLLPRI;
        }
        if (has_flag(flags, EventFlags::Writable)) {
            ev |= EPOLLOUT;
        }
        if (has_flag(flags, EventFlags::EdgeTriggered)) {
            ev |= EPOLLET;
        }
        if (has_flag(flags, EventFlags::OneShot)) {
            ev |= EPOLLONESHOT;
        }
        return ev;
    }

    static EventFlags from_epoll_events(uint32_t epoll_ev) noexcept {
        EventFlags flags = EventFlags::None;
        if (epoll_ev & (EPOLLIN | EPOLLPRI)) {
            flags |= EventFlags::Readable;
        }
        if (epoll_ev & EPOLLOUT) {
            flags |= EventFlags::Writable;
        }
        if (epoll_ev & EPOLLERR) {
            flags |= EventFlags::Error;
        }
        if (epoll_ev & (EPOLLHUP | EPOLLRDHUP)) {
            flags |= EventFlags::Hangup;
        }
        return flags;
    }

    int m_epoll_fd{-1};
    std::unordered_map<int, EventFlags> m_registered_fds;
};

} // namespace tmux2::reactor

#endif // defined(__linux__)
