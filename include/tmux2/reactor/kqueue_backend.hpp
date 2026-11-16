#pragma once

#include <tmux2/reactor/backend.hpp>
#include <tmux2/reactor/poll_backend.hpp>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace tmux2::reactor {

class KqueueBackend : public ReactorBackend {
public:
    static std::expected<std::unique_ptr<KqueueBackend>, std::error_code> create() {
        int kq = ::kqueue();
        if (kq < 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        return std::unique_ptr<KqueueBackend>(new KqueueBackend(kq));
    }

    ~KqueueBackend() override {
        if (m_kq_fd >= 0) {
            ::close(m_kq_fd);
            m_kq_fd = -1;
        }
    }

    KqueueBackend(const KqueueBackend&) = delete;
    KqueueBackend& operator=(const KqueueBackend&) = delete;

    KqueueBackend(KqueueBackend&& other) noexcept
        : m_kq_fd(other.m_kq_fd)
        , m_registered_fds(std::move(other.m_registered_fds))
        , m_tty_fds(std::move(other.m_tty_fds))
        , m_tty_backend(std::move(other.m_tty_backend)) {
        other.m_kq_fd = -1;
    }

    KqueueBackend& operator=(KqueueBackend&& other) noexcept {
        if (this != &other) {
            if (m_kq_fd >= 0) {
                ::close(m_kq_fd);
            }
            m_kq_fd = other.m_kq_fd;
            m_registered_fds = std::move(other.m_registered_fds);
            m_tty_fds = std::move(other.m_tty_fds);
            m_tty_backend = std::move(other.m_tty_backend);
            other.m_kq_fd = -1;
        }
        return *this;
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "kqueue";
    }

    [[nodiscard]] int fd() const noexcept {
        return m_kq_fd;
    }

    std::expected<void, std::error_code> register_fd(int fd, EventFlags flags) override {
        if (fd < 0) {
            return std::unexpected(make_error_code(ReactorError::InvalidFileDescriptor));
        }
        if (m_registered_fds.find(fd) != m_registered_fds.end() || m_tty_fds.find(fd) != m_tty_fds.end()) {
            return std::unexpected(make_error_code(ReactorError::DescriptorAlreadyRegistered));
        }

        // On Darwin / macOS, kqueue has known issues with /dev/tty character devices
        // (returning ENODEV, hang, or spurious EOF on EVFILT_READ).
        // If the descriptor is a TTY, route it through the poll(2) fallback subsystem.
        if (::isatty(fd)) {
            auto res = m_tty_backend.register_fd(fd, flags);
            if (!res) {
                return res;
            }
            m_tty_fds.insert(fd);
            return {};
        }

        // Apply kevent filters
        auto res = apply_kevent(fd, EventFlags::None, flags);
        if (!res) {
            return res;
        }

        m_registered_fds[fd] = flags;
        return {};
    }

    std::expected<void, std::error_code> modify_fd(int fd, EventFlags flags) override {
        if (m_tty_fds.find(fd) != m_tty_fds.end()) {
            return m_tty_backend.modify_fd(fd, flags);
        }

        auto it = m_registered_fds.find(fd);
        if (it == m_registered_fds.end()) {
            return std::unexpected(make_error_code(ReactorError::DescriptorNotFound));
        }

        auto res = apply_kevent(fd, it->second, flags);
        if (!res) {
            return res;
        }

        it->second = flags;
        return {};
    }

    std::expected<void, std::error_code> unregister_fd(int fd) override {
        if (m_tty_fds.find(fd) != m_tty_fds.end()) {
            m_tty_fds.erase(fd);
            return m_tty_backend.unregister_fd(fd);
        }

        auto it = m_registered_fds.find(fd);
        if (it == m_registered_fds.end()) {
            return std::unexpected(make_error_code(ReactorError::DescriptorNotFound));
        }

        // Remove active kevent filters
        (void)apply_kevent(fd, it->second, EventFlags::None);
        m_registered_fds.erase(it);
        return {};
    }

    std::expected<size_t, std::error_code> poll_events(
        std::span<PollEvent> out_events,
        std::optional<std::chrono::microseconds> timeout) override {
        if (out_events.empty()) {
            return 0;
        }

        // If no TTY descriptors registered, pure kqueue path
        if (m_tty_fds.empty()) {
            return poll_kqueue(out_events, timeout);
        }

        // Hybrid kqueue + poll for Darwin TTYs:
        // First check kqueue non-blockingly (timeout 0)
        size_t kq_count = 0;
        {
            auto res = poll_kqueue(out_events, std::chrono::microseconds(0));
            if (!res) {
                return res;
            }
            kq_count = *res;
        }

        if (kq_count > 0) {
            // Already have ready events from sockets/pipes; poll TTY non-blockingly
            if (kq_count < out_events.size()) {
                auto poll_res = m_tty_backend.poll_events(
                    out_events.subspan(kq_count),
                    std::chrono::microseconds(0)
                );
                if (poll_res) {
                    kq_count += *poll_res;
                }
            }
            return kq_count;
        }

        // No kqueue events ready. If timeout is 0, do immediate TTY poll
        if (timeout.has_value() && timeout->count() <= 0) {
            return m_tty_backend.poll_events(out_events, std::chrono::microseconds(0));
        }

        // Wait up to timeout (or a small 10ms slice if timeout is large, to keep kqueue responsive)
        std::chrono::microseconds wait_slice = timeout.value_or(std::chrono::microseconds(10000));
        if (wait_slice.count() > 10000) {
            wait_slice = std::chrono::microseconds(10000);
        }

        auto poll_res = m_tty_backend.poll_events(out_events, wait_slice);
        if (!poll_res) {
            return poll_res;
        }

        return *poll_res;
    }

    [[nodiscard]] size_t descriptor_count() const noexcept {
        return m_registered_fds.size() + m_tty_fds.size();
    }

    [[nodiscard]] bool contains(int fd) const noexcept {
        return m_registered_fds.find(fd) != m_registered_fds.end() ||
               m_tty_fds.find(fd) != m_tty_fds.end();
    }

private:
    explicit KqueueBackend(int kq_fd) : m_kq_fd(kq_fd) {}

    std::expected<void, std::error_code> apply_kevent(int fd, EventFlags old_flags, EventFlags new_flags) {
        struct kevent changes[4];
        int nchanges = 0;

        uint16_t extra_flags = 0;
        if (has_flag(new_flags, EventFlags::EdgeTriggered)) {
            extra_flags |= EV_CLEAR;
        }
        if (has_flag(new_flags, EventFlags::OneShot)) {
            extra_flags |= EV_ONESHOT;
        }

        // READ filter changes
        bool old_r = has_flag(old_flags, EventFlags::Readable);
        bool new_r = has_flag(new_flags, EventFlags::Readable);
        if (!old_r && new_r) {
            EV_SET(&changes[nchanges++], fd, EVFILT_READ, EV_ADD | EV_ENABLE | extra_flags, 0, 0, nullptr);
        } else if (old_r && !new_r) {
            EV_SET(&changes[nchanges++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        }

        // WRITE filter changes
        bool old_w = has_flag(old_flags, EventFlags::Writable);
        bool new_w = has_flag(new_flags, EventFlags::Writable);
        if (!old_w && new_w) {
            EV_SET(&changes[nchanges++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | extra_flags, 0, 0, nullptr);
        } else if (old_w && !new_w) {
            EV_SET(&changes[nchanges++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        }

        if (nchanges > 0) {
            int ret = ::kevent(m_kq_fd, changes, nchanges, nullptr, 0, nullptr);
            if (ret < 0 && errno != ENOENT) {
                return std::unexpected(std::error_code(errno, std::generic_category()));
            }
        }

        return {};
    }

    std::expected<size_t, std::error_code> poll_kqueue(
        std::span<PollEvent> out_events,
        std::optional<std::chrono::microseconds> timeout) {
        constexpr size_t kMaxBatch = 64;
        size_t batch_size = std::min(out_events.size(), kMaxBatch);
        struct kevent eventlist[kMaxBatch];

        struct timespec ts{};
        struct timespec* pts = nullptr;

        if (timeout.has_value()) {
            auto us = timeout->count();
            if (us < 0) {
                us = 0;
            }
            ts.tv_sec = static_cast<time_t>(us / 1000000);
            ts.tv_nsec = static_cast<long>((us % 1000000) * 1000);
            pts = &ts;
        }

        int nevents = ::kevent(m_kq_fd, nullptr, 0, eventlist, static_cast<int>(batch_size), pts);
        if (nevents < 0) {
            if (errno == EINTR) {
                return 0;
            }
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }

        size_t count = 0;
        for (int i = 0; i < nevents; ++i) {
            int ev_fd = static_cast<int>(eventlist[i].ident);
            EventFlags flags = EventFlags::None;

            if (eventlist[i].filter == EVFILT_READ) {
                flags |= EventFlags::Readable;
            } else if (eventlist[i].filter == EVFILT_WRITE) {
                flags |= EventFlags::Writable;
            }

            if (eventlist[i].flags & EV_ERROR) {
                flags |= EventFlags::Error;
            }
            if (eventlist[i].flags & EV_EOF) {
                flags |= EventFlags::Hangup;
            }

            if (flags != EventFlags::None) {
                out_events[count++] = PollEvent{
                    .fd = ev_fd,
                    .events = flags
                };
            }
        }

        return count;
    }

    int m_kq_fd{-1};
    std::unordered_map<int, EventFlags> m_registered_fds;
    std::unordered_set<int> m_tty_fds;
    PollBackend m_tty_backend;
};

} // namespace tmux2::reactor

#endif // defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
