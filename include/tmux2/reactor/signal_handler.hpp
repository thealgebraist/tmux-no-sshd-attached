#pragma once

#include <tmux2/reactor/types.hpp>

#include <csignal>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <expected>
#include <system_error>

namespace tmux2::reactor {

class SignalHandler {
public:
    SignalHandler() {
        init_pipe();
    }

    ~SignalHandler() {
        restore_all();
        close_pipe();
    }

    SignalHandler(const SignalHandler&) = delete;
    SignalHandler& operator=(const SignalHandler&) = delete;
    SignalHandler(SignalHandler&&) = delete;
    SignalHandler& operator=(SignalHandler&&) = delete;

    [[nodiscard]] int read_fd() const noexcept {
        return m_pipe_read_fd;
    }

    [[nodiscard]] int write_fd() const noexcept {
        return m_pipe_write_fd;
    }

    // Register interest in a specific signal with a callback.
    // Automatically sets up SA_RESTART signal disposition.
    std::expected<void, std::error_code> register_signal(int signum, SignalCallback callback) {
        if (signum <= 0 || signum >= 64) {
            return std::unexpected(make_error_code(std::errc::invalid_argument));
        }

        // Install POSIX signal handler if not already installed for this signum
        if (m_registered_signals.find(signum) == m_registered_signals.end()) {
            struct sigaction sa{};
            sa.sa_handler = &SignalHandler::posix_signal_handler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_RESTART;
#ifdef SA_NOCLDSTOP
            if (signum == SIGCHLD) {
                sa.sa_flags |= SA_NOCLDSTOP;
            }
#endif

            struct sigaction old_sa{};
            if (::sigaction(signum, &sa, &old_sa) < 0) {
                return std::unexpected(std::error_code(errno, std::generic_category()));
            }

            m_saved_sigactions[signum] = old_sa;
        }

        m_registered_signals[signum] = std::move(callback);
        set_active_instance(this);
        return {};
    }

    // Unregister a previously registered signal and restore previous action.
    void unregister_signal(int signum) {
        auto it = m_registered_signals.find(signum);
        if (it != m_registered_signals.end()) {
            m_registered_signals.erase(it);
        }

        auto sa_it = m_saved_sigactions.find(signum);
        if (sa_it != m_saved_sigactions.end()) {
            ::sigaction(signum, &sa_it->second, nullptr);
            m_saved_sigactions.erase(sa_it);
        }
    }

    // Read pending signal notifications from the self-pipe and invoke callbacks.
    size_t process_pending_signals() {
        if (m_pipe_read_fd < 0) {
            return 0;
        }

        uint8_t buffer[128];
        size_t processed = 0;

        for (;;) {
            ssize_t n = ::read(m_pipe_read_fd, buffer, sizeof(buffer));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (n == 0) {
                break;
            }

            for (ssize_t i = 0; i < n; ++i) {
                int signum = static_cast<int>(buffer[i]);
                auto it = m_registered_signals.find(signum);
                if (it != m_registered_signals.end() && it->second) {
                    it->second(signum);
                    processed++;
                }
            }
        }

        return processed;
    }

    // Helper to register standard terminal multiplexer signals:
    // SIGCHLD, SIGWINCH, SIGTERM, SIGINT, SIGHUP
    std::expected<void, std::error_code> register_standard_signals(
        std::function<void(int signum)> global_callback) {
        const int standard_signals[] = {SIGCHLD, SIGWINCH, SIGTERM, SIGINT, SIGHUP};
        for (int sig : standard_signals) {
            auto res = register_signal(sig, global_callback);
            if (!res) {
                return res;
            }
        }
        return {};
    }

private:
    static void posix_signal_handler(int signum) {
        int saved_errno = errno;
        int write_fd = s_global_write_fd.load(std::memory_order_relaxed);
        if (write_fd >= 0) {
            uint8_t sig = static_cast<uint8_t>(signum);
            // write() to a pipe is async-signal-safe
            (void)::write(write_fd, &sig, sizeof(sig));
        }
        errno = saved_errno;
    }

    static void set_active_instance(SignalHandler* instance) {
        if (instance) {
            s_global_write_fd.store(instance->m_pipe_write_fd, std::memory_order_release);
        } else {
            s_global_write_fd.store(-1, std::memory_order_release);
        }
    }

    void init_pipe() {
        int pipe_fds[2] = {-1, -1};
#if defined(__linux__) && defined(O_CLOEXEC)
        if (::pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) == 0) {
            m_pipe_read_fd = pipe_fds[0];
            m_pipe_write_fd = pipe_fds[1];
            return;
        }
#endif
        if (::pipe(pipe_fds) == 0) {
            m_pipe_read_fd = pipe_fds[0];
            m_pipe_write_fd = pipe_fds[1];

            // Set CLOEXEC and NONBLOCK
            for (int fd : pipe_fds) {
                int flags = ::fcntl(fd, F_GETFD);
                if (flags != -1) {
                    ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
                }
                int fl = ::fcntl(fd, F_GETFL);
                if (fl != -1) {
                    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
                }
            }
        }
    }

    void close_pipe() {
        set_active_instance(nullptr);
        if (m_pipe_read_fd >= 0) {
            ::close(m_pipe_read_fd);
            m_pipe_read_fd = -1;
        }
        if (m_pipe_write_fd >= 0) {
            ::close(m_pipe_write_fd);
            m_pipe_write_fd = -1;
        }
    }

    void restore_all() {
        for (const auto& [signum, old_sa] : m_saved_sigactions) {
            ::sigaction(signum, &old_sa, nullptr);
        }
        m_saved_sigactions.clear();
        m_registered_signals.clear();
    }

    int m_pipe_read_fd{-1};
    int m_pipe_write_fd{-1};
    std::unordered_map<int, SignalCallback> m_registered_signals;
    std::unordered_map<int, struct sigaction> m_saved_sigactions;

    inline static std::atomic<int> s_global_write_fd{-1};
};

} // namespace tmux2::reactor
