#include "src/pty/pty_process.hpp"
#include "src/pty/terminfo.hpp"

#if defined(__APPLE__)
#  include <util.h>
#elif defined(__linux__)
#  include <pty.h>
#  include <utmp.h>
#endif

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace tmux2::pty {

PtyProcess::~PtyProcess() {
    close_master();
    if (child_pid_ > 0 && alive_) {
        // Send SIGHUP to the child process group to gracefully terminate shells
        send_signal(SIGHUP);
        poll_exit();
        if (alive_) {
            // Give brief opportunity then force kill
            send_signal(SIGKILL);
            int status = 0;
            waitpid(child_pid_, &status, WNOHANG);
        }
    }
}

PtyProcess::PtyProcess(PtyProcess&& other) noexcept
    : master_fd_(std::exchange(other.master_fd_, -1)),
      child_pid_(std::exchange(other.child_pid_, -1)),
      alive_(std::exchange(other.alive_, false)),
      exit_code_(std::exchange(other.exit_code_, std::nullopt)),
      cols_(other.cols_),
      rows_(other.rows_) {}

PtyProcess& PtyProcess::operator=(PtyProcess&& other) noexcept {
    if (this != &other) {
        close_master();
        if (child_pid_ > 0 && alive_) {
            send_signal(SIGHUP);
            int status = 0;
            waitpid(child_pid_, &status, WNOHANG);
        }
        master_fd_ = std::exchange(other.master_fd_, -1);
        child_pid_ = std::exchange(other.child_pid_, -1);
        alive_ = std::exchange(other.alive_, false);
        exit_code_ = std::exchange(other.exit_code_, std::nullopt);
        cols_ = other.cols_;
        rows_ = other.rows_;
    }
    return *this;
}

std::expected<PtyProcess, std::string> PtyProcess::spawn(const PtyOptions& options) {
    int master_fd = -1;
    int slave_fd = -1;

    struct winsize ws{};
    ws.ws_col = options.cols > 0 ? options.cols : 80;
    ws.ws_row = options.rows > 0 ? options.rows : 24;

    struct termios tio{};
    // Sensible default termios: sane input/output processing with UTF-8 support
    tio.c_iflag = ICRNL | IXON;
#if defined(IUTF8)
    tio.c_iflag |= IUTF8;
#endif
    tio.c_oflag = OPOST | ONLCR;
    tio.c_cflag = CREAD | CS8 | HUPCL;
    tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOKE | ECHOCTL | IEXTEN;
    tio.c_cc[VINTR]  = 0x03; // Ctrl-C
    tio.c_cc[VQUIT]  = 0x1C; // Ctrl-Backslash
    tio.c_cc[VERASE] = 0x7F; // DEL / Backspace
    tio.c_cc[VKILL]  = 0x15; // Ctrl-U
    tio.c_cc[VEOF]   = 0x04; // Ctrl-D
    tio.c_cc[VTIME]  = 0;
    tio.c_cc[VMIN]   = 1;
#if defined(VSWTC)
    tio.c_cc[VSWTC]  = 0;
#endif
    tio.c_cc[VSTART] = 0x11; // Ctrl-Q
    tio.c_cc[VSTOP]  = 0x13; // Ctrl-S
    tio.c_cc[VSUSP]  = 0x1A; // Ctrl-Z

    if (openpty(&master_fd, &slave_fd, nullptr, &tio, &ws) < 0) {
        return std::unexpected(std::string("openpty failed: ") + std::strerror(errno));
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::string err = std::string("fork failed: ") + std::strerror(errno);
        close(master_fd);
        close(slave_fd);
        return std::unexpected(err);
    }

    if (pid == 0) {
        // Child process
        close(master_fd);

        // Create new session & process group
        if (setsid() < 0) {
            // setsid failure is ignored if already session leader
        }

        // Acquire controlling terminal on the slave PTY
#if defined(TIOCSCTTY)
        ioctl(slave_fd, TIOCSCTTY, 0);
#endif

        // Dup slave descriptor to stdin, stdout, stderr
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);

        if (slave_fd > STDERR_FILENO) {
            close(slave_fd);
        }

        // Reset signal dispositions
        sigset_t set;
        sigemptyset(&set);
        sigprocmask(SIG_SETMASK, &set, nullptr);

        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGQUIT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGCHLD, &sa, nullptr);
        sigaction(SIGHUP, &sa, nullptr);
        sigaction(SIGPIPE, &sa, nullptr);
        sigaction(SIGWINCH, &sa, nullptr);

        // Change working directory if specified
        if (!options.cwd.empty()) {
            if (chdir(options.cwd.c_str()) < 0) {
                // If requested cwd fails, continue in current directory
            }
        }

        // Configure environment
        auto env_vars = build_default_env(options.term);
        for (const auto& [k, v] : env_vars) {
            setenv(k.c_str(), v.c_str(), 1);
        }
        for (const auto& [k, v] : options.env) {
            setenv(k.c_str(), v.c_str(), 1);
        }

        // Execute command
        if (!options.args.empty()) {
            std::vector<char*> argv_ptrs;
            argv_ptrs.reserve(options.args.size() + 1);
            for (const auto& arg : options.args) {
                argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
            }
            argv_ptrs.push_back(nullptr);
            execvp(argv_ptrs[0], argv_ptrs.data());
        } else if (!options.command.empty()) {
            execl("/bin/sh", "sh", "-c", options.command.c_str(), static_cast<char*>(nullptr));
        } else {
            // Default user shell
            const char* shell = std::getenv("SHELL");
            if (!shell || *shell == '\0') {
                struct passwd* pw = getpwuid(getuid());
                if (pw && pw->pw_shell && *pw->pw_shell != '\0') {
                    shell = pw->pw_shell;
                } else {
                    shell = "/bin/sh";
                }
            }
            execl(shell, shell, static_cast<char*>(nullptr));
        }

        _exit(127);
    }

    // Parent process
    close(slave_fd);

    // Make master descriptor non-blocking
    int fl = fcntl(master_fd, F_GETFL, 0);
    if (fl >= 0) {
        fcntl(master_fd, F_SETFL, fl | O_NONBLOCK);
    }
    // Set close-on-exec
    fcntl(master_fd, F_SETFD, FD_CLOEXEC);

    PtyProcess proc;
    proc.master_fd_ = master_fd;
    proc.child_pid_ = pid;
    proc.alive_ = true;
    proc.cols_ = ws.ws_col;
    proc.rows_ = ws.ws_row;

    return proc;
}

std::expected<size_t, PtyReadError> PtyProcess::read_master(std::span<char> buffer) {
    if (master_fd_ < 0) {
        return std::unexpected(PtyReadError::Eof);
    }
    if (buffer.empty()) {
        return static_cast<size_t>(0);
    }

    ssize_t n = ::read(master_fd_, buffer.data(), buffer.size());
    if (n > 0) {
        return static_cast<size_t>(n);
    }

    if (n == 0) {
        // Darwin / BSD zero-byte read indicates EOF on slave close
        poll_exit();
        return std::unexpected(PtyReadError::Eof);
    }

    // n < 0
    if (errno == EIO) {
        // Linux returns EIO when all slave descriptors are closed: this is standard PTY EOF
        poll_exit();
        return std::unexpected(PtyReadError::Eof);
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return std::unexpected(PtyReadError::WouldBlock);
    }
    if (errno == EINTR) {
        return std::unexpected(PtyReadError::WouldBlock);
    }

    return std::unexpected(PtyReadError::IoError);
}

std::expected<size_t, PtyWriteError> PtyProcess::write_master(std::string_view data) {
    if (master_fd_ < 0) {
        return std::unexpected(PtyWriteError::BrokenPipe);
    }
    if (data.empty()) {
        return static_cast<size_t>(0);
    }

    ssize_t n = ::write(master_fd_, data.data(), data.size());
    if (n >= 0) {
        return static_cast<size_t>(n);
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return std::unexpected(PtyWriteError::WouldBlock);
    }
    if (errno == EPIPE || errno == EIO) {
        poll_exit();
        return std::unexpected(PtyWriteError::BrokenPipe);
    }

    return std::unexpected(PtyWriteError::IoError);
}

std::expected<size_t, PtyWriteError> PtyProcess::write_master(std::span<const std::byte> data) {
    return write_master(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

bool PtyProcess::resize(uint16_t cols, uint16_t rows, uint16_t xpixel, uint16_t ypixel) {
    if (master_fd_ < 0) {
        return false;
    }

    struct winsize ws{};
    ws.ws_col = cols;
    ws.ws_row = rows;
    ws.ws_xpixel = xpixel;
    ws.ws_ypixel = ypixel;

    if (ioctl(master_fd_, TIOCSWINSZ, &ws) < 0) {
        return false;
    }

    cols_ = cols;
    rows_ = rows;

    // Propagate SIGWINCH to the child process group
    if (child_pid_ > 0 && alive_) {
        kill(-child_pid_, SIGWINCH);
    }

    return true;
}

bool PtyProcess::poll_exit() {
    if (!alive_ || child_pid_ <= 0) {
        return true;
    }

    int status = 0;
    pid_t ret = waitpid(child_pid_, &status, WNOHANG);
    if (ret == child_pid_) {
        alive_ = false;
        if (WIFEXITED(status)) {
            exit_code_ = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            exit_code_ = 128 + WTERMSIG(status);
        } else {
            exit_code_ = 0;
        }
        return true;
    } else if (ret < 0 && errno == ECHILD) {
        alive_ = false;
        return true;
    }

    return false;
}

std::optional<int> PtyProcess::wait_for_exit(std::chrono::milliseconds timeout) {
    if (!alive_ || child_pid_ <= 0) {
        return exit_code_;
    }

    if (timeout < std::chrono::milliseconds(0)) {
        // Blocking wait
        int status = 0;
        pid_t ret = 0;
        do {
            ret = waitpid(child_pid_, &status, 0);
        } while (ret < 0 && errno == EINTR);

        if (ret == child_pid_) {
            alive_ = false;
            if (WIFEXITED(status)) {
                exit_code_ = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                exit_code_ = 128 + WTERMSIG(status);
            } else {
                exit_code_ = 0;
            }
            return exit_code_;
        } else if (ret < 0 && errno == ECHILD) {
            alive_ = false;
            return exit_code_.has_value() ? exit_code_ : std::make_optional(0);
        }
        return std::nullopt;
    }

    auto start = std::chrono::steady_clock::now();
    auto sleep_duration = std::chrono::milliseconds(1);
    while (true) {
        if (poll_exit()) {
            return exit_code_;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed >= timeout) {
            break;
        }
        auto remaining = timeout - elapsed;
        auto to_sleep = std::min(sleep_duration, remaining);
        std::this_thread::sleep_for(to_sleep);
        sleep_duration = std::min(sleep_duration * 2, std::chrono::milliseconds(20));
    }

    return poll_exit() ? exit_code_ : std::nullopt;
}

bool PtyProcess::send_signal(int sig) {
    if (!alive_ || child_pid_ <= 0) {
        return false;
    }

    // Attempt process group signaling first, fallback to individual PID
    if (kill(-child_pid_, sig) == 0) {
        return true;
    }
    return kill(child_pid_, sig) == 0;
}

void PtyProcess::close_master() noexcept {
    if (master_fd_ >= 0) {
        close(master_fd_);
        master_fd_ = -1;
    }
}

} // namespace tmux2::pty
