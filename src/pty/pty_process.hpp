#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <sys/types.h>

namespace tmux2::pty {

enum class PtyReadError {
    Eof,
    WouldBlock,
    IoError,
};

enum class PtyWriteError {
    WouldBlock,
    BrokenPipe,
    IoError,
};

struct PtyOptions {
    uint16_t cols{80};
    uint16_t rows{24};
    std::string term;                        // Empty = auto-detect via detect_default_term()
    std::string cwd;                         // Empty = inherit current directory
    std::string command;                     // If non-empty and args empty, run via /bin/sh -c
    std::vector<std::string> args;           // If non-empty, exec directly
    std::unordered_map<std::string, std::string> env; // Custom environment variables
};

class PtyProcess {
public:
    PtyProcess() noexcept = default;
    ~PtyProcess();

    // Move-only RAII semantics
    PtyProcess(const PtyProcess&) = delete;
    PtyProcess& operator=(const PtyProcess&) = delete;
    PtyProcess(PtyProcess&& other) noexcept;
    PtyProcess& operator=(PtyProcess&& other) noexcept;

    /**
     * Spawns a child process inside a new pseudo-terminal session.
     * Sets up process group isolation, controlling TTY, slave redirection,
     * non-blocking master descriptor, and environment.
     */
    static std::expected<PtyProcess, std::string> spawn(const PtyOptions& options = {});

    /**
     * Non-blocking read from master PTY descriptor.
     * Transparently normalizes EOF across platforms:
     * - Returns PtyReadError::Eof on 0 bytes read (Darwin/BSD)
     * - Returns PtyReadError::Eof on -1 with errno == EIO (Linux slave-close EOF)
     * - Returns PtyReadError::WouldBlock on EAGAIN / EWOULDBLOCK
     */
    [[nodiscard]] std::expected<size_t, PtyReadError> read_master(std::span<char> buffer);

    /**
     * Non-blocking write to master PTY descriptor.
     */
    [[nodiscard]] std::expected<size_t, PtyWriteError> write_master(std::string_view data);
    [[nodiscard]] std::expected<size_t, PtyWriteError> write_master(std::span<const std::byte> data);

    template <size_t N = std::dynamic_extent>
    [[nodiscard]] std::expected<size_t, PtyWriteError> write_master(std::span<const char, N> data) {
        return write_master(std::string_view(data.data(), data.size()));
    }

    template <size_t N = std::dynamic_extent>
    [[nodiscard]] std::expected<size_t, PtyWriteError> write_master(std::span<char, N> data) {
        return write_master(std::string_view(data.data(), data.size()));
    }

    /**
     * Propagates window size change to master PTY via TIOCSWINSZ
     * and signals the child process group with SIGWINCH.
     */
    bool resize(uint16_t cols, uint16_t rows, uint16_t xpixel = 0, uint16_t ypixel = 0);

    /**
     * Checks whether the child process has exited without blocking.
     * Updates exit code if child has terminated.
     */
    bool poll_exit();

    /**
     * Waits for the child process to terminate within the given timeout.
     * If timeout < 0ms (default), blocks until child exits.
     * Returns exit code if terminated, or std::nullopt on timeout.
     */
    std::optional<int> wait_for_exit(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

    /**
     * Sends a signal to the child process group (or child PID if group unavailable).
     */
    bool send_signal(int sig);

    /**
     * Closes the master descriptor and terminates child if alive.
     */
    void close_master() noexcept;

    [[nodiscard]] int master_fd() const noexcept { return master_fd_; }
    [[nodiscard]] pid_t child_pid() const noexcept { return child_pid_; }
    [[nodiscard]] bool is_alive() const noexcept { return alive_; }
    [[nodiscard]] std::optional<int> exit_code() const noexcept { return exit_code_; }
    [[nodiscard]] uint16_t cols() const noexcept { return cols_; }
    [[nodiscard]] uint16_t rows() const noexcept { return rows_; }

private:
    int master_fd_{-1};
    pid_t child_pid_{-1};
    bool alive_{false};
    std::optional<int> exit_code_{std::nullopt};
    uint16_t cols_{80};
    uint16_t rows_{24};
};

} // namespace tmux2::pty
