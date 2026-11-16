#pragma once

#include <tmux2/reactor/types.hpp>
#include <tmux2/reactor/backend.hpp>
#include <tmux2/reactor/poll_backend.hpp>
#include <tmux2/reactor/timer_heap.hpp>
#include <tmux2/reactor/signal_handler.hpp>

#if defined(__linux__)
#include <tmux2/reactor/epoll_backend.hpp>
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <tmux2/reactor/kqueue_backend.hpp>
#endif

#include <memory>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <optional>
#include <system_error>
#include <expected>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>

// Forward declare OpenSSL SSL structure so <openssl/ssl.h> is optional in header
struct ssl_st;
typedef struct ssl_st SSL;

extern "C" {
int SSL_get_event_timeout(SSL *s, struct timeval *tv, int *is_infinite);
int SSL_handle_events(SSL *s);
}

namespace tmux2::reactor {

class Reactor {
public:
    // Factory method to create reactor with default platform backend:
    // EpollBackend on Linux, KqueueBackend on macOS/BSD, PollBackend otherwise.
    static std::expected<std::unique_ptr<Reactor>, std::error_code> create_default() {
#if defined(__linux__)
        auto epoll_res = EpollBackend::create();
        if (epoll_res) {
            return create_with_backend(std::move(*epoll_res));
        }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        auto kq_res = KqueueBackend::create();
        if (kq_res) {
            return create_with_backend(std::move(*kq_res));
        }
#endif
        auto poll_be = std::make_unique<PollBackend>();
        return create_with_backend(std::move(poll_be));
    }

    // Factory method to explicitly create reactor with POSIX poll(2) backend.
    static std::expected<std::unique_ptr<Reactor>, std::error_code> create_poll() {
        auto poll_be = std::make_unique<PollBackend>();
        return create_with_backend(std::move(poll_be));
    }

#if defined(__linux__)
    // Factory method to explicitly create reactor with Linux epoll backend.
    static std::expected<std::unique_ptr<Reactor>, std::error_code> create_epoll() {
        auto epoll_res = EpollBackend::create();
        if (!epoll_res) {
            return std::unexpected(epoll_res.error());
        }
        return create_with_backend(std::move(*epoll_res));
    }
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    // Factory method to explicitly create reactor with macOS/BSD kqueue backend.
    static std::expected<std::unique_ptr<Reactor>, std::error_code> create_kqueue() {
        auto kq_res = KqueueBackend::create();
        if (!kq_res) {
            return std::unexpected(kq_res.error());
        }
        return create_with_backend(std::move(*kq_res));
    }
#endif

    static std::expected<std::unique_ptr<Reactor>, std::error_code> create_with_backend(
        std::unique_ptr<ReactorBackend> backend) {
        auto reactor = std::unique_ptr<Reactor>(new Reactor(std::move(backend)));
        auto init_res = reactor->init();
        if (!init_res) {
            return std::unexpected(init_res.error());
        }
        return reactor;
    }

    ~Reactor() {
        stop();
        close_wakeup_pipe();
    }

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;
    Reactor(Reactor&&) = delete;
    Reactor& operator=(Reactor&&) = delete;

    [[nodiscard]] std::string_view backend_name() const noexcept {
        return m_backend ? m_backend->name() : "unknown";
    }

    // --- File Descriptor Management ---

    // Register a file descriptor with event flags and callback.
    std::expected<void, std::error_code> register_fd(int fd, EventFlags events, EventCallback callback) {
        if (fd < 0) {
            return std::unexpected(make_error_code(ReactorError::InvalidFileDescriptor));
        }
        if (m_callbacks.find(fd) != m_callbacks.end()) {
            return std::unexpected(make_error_code(ReactorError::DescriptorAlreadyRegistered));
        }

        auto res = m_backend->register_fd(fd, events);
        if (!res) {
            return res;
        }

        m_callbacks[fd] = CallbackEntry{
            .events = events,
            .callback = std::move(callback)
        };
        return {};
    }

    // Modify event flags and optionally callback for a registered descriptor.
    std::expected<void, std::error_code> modify_fd(
        int fd,
        EventFlags events,
        std::optional<EventCallback> callback = std::nullopt) {
        auto it = m_callbacks.find(fd);
        if (it == m_callbacks.end()) {
            return std::unexpected(make_error_code(ReactorError::DescriptorNotFound));
        }

        auto res = m_backend->modify_fd(fd, events);
        if (!res) {
            return res;
        }

        it->second.events = events;
        if (callback.has_value()) {
            it->second.callback = std::move(*callback);
        }
        return {};
    }

    // Unregister a file descriptor.
    std::expected<void, std::error_code> unregister_fd(int fd) {
        auto it = m_callbacks.find(fd);
        if (it == m_callbacks.end()) {
            return std::unexpected(make_error_code(ReactorError::DescriptorNotFound));
        }

        auto res = m_backend->unregister_fd(fd);
        m_callbacks.erase(it);
        return res;
    }

    [[nodiscard]] bool has_fd(int fd) const noexcept {
        return m_callbacks.find(fd) != m_callbacks.end();
    }

    // --- Timer Management ---

    // Schedule a one-shot timer after delay microseconds.
    TimerId add_timer(std::chrono::microseconds delay, TimerCallback callback) {
        return m_timer_heap.add_timer(delay, std::move(callback));
    }

    // Schedule a repeating timer with initial delay and periodic interval.
    TimerId add_repeating_timer(std::chrono::microseconds initial_delay,
                                std::chrono::microseconds interval,
                                TimerCallback callback) {
        return m_timer_heap.add_repeating_timer(initial_delay, interval, std::move(callback));
    }

    // Schedule a repeating timer with equal initial delay and interval.
    TimerId add_repeating_timer(std::chrono::microseconds interval, TimerCallback callback) {
        return m_timer_heap.add_repeating_timer(interval, interval, std::move(callback));
    }

    // Cancel a scheduled timer.
    bool cancel_timer(TimerId id) {
        return m_timer_heap.cancel(id);
    }

    [[nodiscard]] bool is_timer_active(TimerId id) const noexcept {
        return m_timer_heap.is_active(id);
    }

    [[nodiscard]] size_t active_timer_count() const noexcept {
        return m_timer_heap.active_count();
    }

    // --- Signal Handling ---

    // Register a POSIX signal handler.
    std::expected<void, std::error_code> register_signal(int signum, SignalCallback callback) {
        return m_signal_handler.register_signal(signum, std::move(callback));
    }

    // Unregister a POSIX signal handler.
    void unregister_signal(int signum) {
        m_signal_handler.unregister_signal(signum);
    }

    // Register standard terminal multiplexer signals:
    // SIGCHLD, SIGWINCH, SIGTERM, SIGINT, SIGHUP
    std::expected<void, std::error_code> register_standard_signals(
        std::function<void(int signum)> callback) {
        return m_signal_handler.register_standard_signals(std::move(callback));
    }

    // --- OpenSSL 3.5 QUIC State Machine Integration ---

    // Synchronize pending event timeout for OpenSSL 3.5 QUIC/TLS connection.
    // If SSL has a pending timer deadline, registers/updates a reactor timer.
    // When the timer expires, it invokes SSL_handle_events(ssl) and resyncs.
    void sync_openssl_timeout(SSL* ssl, TimerId& timer_id_storage) {
        if (!ssl) {
            return;
        }

        struct timeval tv{};
        int is_infinite = 1;
        int ret = SSL_get_event_timeout(ssl, &tv, &is_infinite);
        if (ret == 1 && is_infinite == 0) {
            auto us = std::chrono::seconds(tv.tv_sec) + std::chrono::microseconds(tv.tv_usec);
            if (timer_id_storage != kInvalidTimerId) {
                m_timer_heap.cancel(timer_id_storage);
            }
            timer_id_storage = add_timer(us, [this, ssl, &timer_id_storage](TimerId) {
                SSL_handle_events(ssl);
                timer_id_storage = kInvalidTimerId;
                sync_openssl_timeout(ssl, timer_id_storage);
            });
        } else {
            if (timer_id_storage != kInvalidTimerId) {
                m_timer_heap.cancel(timer_id_storage);
                timer_id_storage = kInvalidTimerId;
            }
        }
    }

    // --- Event Loop Control ---

    // Run the reactor loop until stop() is invoked.
    void run() {
        m_running = true;
        while (m_running) {
            (void)run_once(std::nullopt);
        }
    }

    // Execute a single iteration of the reactor loop.
    // max_timeout limits the maximum wait time even if no timers are pending sooner.
    // Returns number of I/O events, timers, and signals dispatched.
    size_t run_once(std::optional<std::chrono::microseconds> max_timeout = std::nullopt) {
        // 1. Determine wait timeout from timer heap and max_timeout
        auto timer_timeout = m_timer_heap.time_to_next();
        std::optional<std::chrono::microseconds> effective_timeout;

        if (timer_timeout.has_value() && max_timeout.has_value()) {
            effective_timeout = std::min(*timer_timeout, *max_timeout);
        } else if (timer_timeout.has_value()) {
            effective_timeout = timer_timeout;
        } else if (max_timeout.has_value()) {
            effective_timeout = max_timeout;
        } else {
            effective_timeout = std::nullopt; // Block indefinitely
        }

        // 2. Poll backend for I/O events
        constexpr size_t kMaxEvents = 64;
        PollEvent ready_events[kMaxEvents];
        auto poll_res = m_backend->poll_events(ready_events, effective_timeout);

        // 3. Process expired timers
        size_t timers_dispatched = m_timer_heap.process_expired();

        // 4. Process pending signals
        size_t signals_dispatched = m_signal_handler.process_pending_signals();

        // 5. Dispatch I/O callbacks
        size_t io_dispatched = 0;
        if (poll_res) {
            size_t n_events = *poll_res;
            for (size_t i = 0; i < n_events; ++i) {
                const auto& ev = ready_events[i];
                if (ev.fd == m_wakeup_read_fd) {
                    drain_wakeup_pipe();
                    continue;
                }
                if (ev.fd == m_signal_handler.read_fd()) {
                    signals_dispatched += m_signal_handler.process_pending_signals();
                    continue;
                }

                auto it = m_callbacks.find(ev.fd);
                if (it != m_callbacks.end() && it->second.callback) {
                    it->second.callback(ev.fd, ev.events);
                    io_dispatched++;
                }
            }
        }

        return io_dispatched + timers_dispatched + signals_dispatched;
    }

    // Request the loop to terminate.
    void stop() noexcept {
        m_running = false;
        wakeup();
    }

    [[nodiscard]] bool is_running() const noexcept {
        return m_running;
    }

    // Wake up the reactor if blocked inside poll_events().
    void wakeup() noexcept {
        if (m_wakeup_write_fd >= 0) {
            uint8_t byte = 1;
            (void)::write(m_wakeup_write_fd, &byte, sizeof(byte));
        }
    }

    [[nodiscard]] TimerHeap& timer_heap() noexcept {
        return m_timer_heap;
    }

    [[nodiscard]] SignalHandler& signal_handler() noexcept {
        return m_signal_handler;
    }

private:
    explicit Reactor(std::unique_ptr<ReactorBackend> backend)
        : m_backend(std::move(backend)) {}

    std::expected<void, std::error_code> init() {
        // Setup wakeup pipe
        int p[2] = {-1, -1};
#if defined(__linux__) && defined(O_CLOEXEC)
        if (::pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0) {
            m_wakeup_read_fd = p[0];
            m_wakeup_write_fd = p[1];
        } else
#endif
        if (::pipe(p) == 0) {
            m_wakeup_read_fd = p[0];
            m_wakeup_write_fd = p[1];
            for (int fd : p) {
                int flags = ::fcntl(fd, F_GETFD);
                if (flags != -1) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
                int fl = ::fcntl(fd, F_GETFL);
                if (fl != -1) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            }
        } else {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }

        // Register wakeup read fd in backend
        auto res = m_backend->register_fd(m_wakeup_read_fd, EventFlags::Readable);
        if (!res) {
            return res;
        }

        // Register signal pipe read fd in backend
        int sig_fd = m_signal_handler.read_fd();
        if (sig_fd >= 0) {
            auto sig_res = m_backend->register_fd(sig_fd, EventFlags::Readable);
            if (!sig_res) {
                return sig_res;
            }
        }

        return {};
    }

    void drain_wakeup_pipe() noexcept {
        if (m_wakeup_read_fd < 0) return;
        uint8_t buf[64];
        while (::read(m_wakeup_read_fd, buf, sizeof(buf)) > 0) {}
    }

    void close_wakeup_pipe() noexcept {
        if (m_wakeup_read_fd >= 0) {
            (void)m_backend->unregister_fd(m_wakeup_read_fd);
            ::close(m_wakeup_read_fd);
            m_wakeup_read_fd = -1;
        }
        if (m_wakeup_write_fd >= 0) {
            ::close(m_wakeup_write_fd);
            m_wakeup_write_fd = -1;
        }
    }

    struct CallbackEntry {
        EventFlags events{EventFlags::None};
        EventCallback callback;
    };

    std::unique_ptr<ReactorBackend> m_backend;
    TimerHeap m_timer_heap;
    SignalHandler m_signal_handler;
    std::unordered_map<int, CallbackEntry> m_callbacks;

    int m_wakeup_read_fd{-1};
    int m_wakeup_write_fd{-1};
    bool m_running{false};
};

} // namespace tmux2::reactor
