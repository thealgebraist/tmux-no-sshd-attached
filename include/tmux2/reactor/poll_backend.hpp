#pragma once

#include <tmux2/reactor/backend.hpp>

#include <poll.h>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cerrno>

namespace tmux2::reactor {

class PollBackend : public ReactorBackend {
public:
    PollBackend() = default;
    ~PollBackend() override = default;

    PollBackend(const PollBackend&) = delete;
    PollBackend& operator=(const PollBackend&) = delete;
    PollBackend(PollBackend&&) noexcept = default;
    PollBackend& operator=(PollBackend&&) noexcept = default;

    [[nodiscard]] std::string_view name() const noexcept override {
        return "poll";
    }

    std::expected<void, std::error_code> register_fd(int fd, EventFlags flags) override {
        if (fd < 0) {
            return std::unexpected(make_error_code(ReactorError::InvalidFileDescriptor));
        }
        if (m_fd_to_index.find(fd) != m_fd_to_index.end()) {
            return std::unexpected(make_error_code(ReactorError::DescriptorAlreadyRegistered));
        }

        short events = to_poll_events(flags);
        size_t idx = m_poll_fds.size();
        m_poll_fds.push_back(::pollfd{
            .fd = fd,
            .events = events,
            .revents = 0
        });
        m_fd_to_index[fd] = idx;
        m_fd_flags[fd] = flags;
        return {};
    }

    std::expected<void, std::error_code> modify_fd(int fd, EventFlags flags) override {
        auto it = m_fd_to_index.find(fd);
        if (it == m_fd_to_index.end()) {
            return std::unexpected(make_error_code(ReactorError::DescriptorNotFound));
        }

        m_poll_fds[it->second].events = to_poll_events(flags);
        m_fd_flags[fd] = flags;
        return {};
    }

    std::expected<void, std::error_code> unregister_fd(int fd) override {
        auto it = m_fd_to_index.find(fd);
        if (it == m_fd_to_index.end()) {
            return std::unexpected(make_error_code(ReactorError::DescriptorNotFound));
        }

        size_t idx = it->second;
        size_t last_idx = m_poll_fds.size() - 1;

        if (idx != last_idx) {
            // Swap with last element for O(1) removal
            int last_fd = m_poll_fds[last_idx].fd;
            m_poll_fds[idx] = m_poll_fds[last_idx];
            m_fd_to_index[last_fd] = idx;
        }

        m_poll_fds.pop_back();
        m_fd_to_index.erase(it);
        m_fd_flags.erase(fd);
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
                // Round up to avoid spinning
                timeout_ms = static_cast<int>((us + 999) / 1000);
            }
        }

        int ret = ::poll(m_poll_fds.data(), static_cast<nfds_t>(m_poll_fds.size()), timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) {
                return 0; // Signal interrupt is normal in event loops
            }
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }

        if (ret == 0) {
            return 0; // Timeout
        }

        size_t collected = 0;
        for (const auto& pfd : m_poll_fds) {
            if (pfd.revents == 0) {
                continue;
            }

            EventFlags events = from_poll_revents(pfd.revents);
            if (events != EventFlags::None) {
                out_events[collected++] = PollEvent{
                    .fd = pfd.fd,
                    .events = events
                };
                if (collected >= out_events.size()) {
                    break;
                }
            }
        }

        return collected;
    }

    [[nodiscard]] size_t descriptor_count() const noexcept {
        return m_poll_fds.size();
    }

    [[nodiscard]] bool contains(int fd) const noexcept {
        return m_fd_to_index.find(fd) != m_fd_to_index.end();
    }

private:
    static short to_poll_events(EventFlags flags) noexcept {
        short ev = 0;
        if (has_flag(flags, EventFlags::Readable)) {
            ev |= POLLIN | POLLPRI;
        }
        if (has_flag(flags, EventFlags::Writable)) {
            ev |= POLLOUT;
        }
        return ev;
    }

    static EventFlags from_poll_revents(short revents) noexcept {
        EventFlags flags = EventFlags::None;
        if (revents & (POLLIN | POLLPRI)) {
            flags |= EventFlags::Readable;
        }
        if (revents & POLLOUT) {
            flags |= EventFlags::Writable;
        }
        if (revents & POLLERR) {
            flags |= EventFlags::Error;
        }
        if (revents & (POLLHUP | POLLNVAL)) {
            flags |= EventFlags::Hangup;
        }
        return flags;
    }

    std::vector<struct pollfd> m_poll_fds;
    std::unordered_map<int, size_t> m_fd_to_index;
    std::unordered_map<int, EventFlags> m_fd_flags;
};

} // namespace tmux2::reactor
