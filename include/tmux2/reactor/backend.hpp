#pragma once

#include <tmux2/reactor/types.hpp>

#include <span>
#include <optional>
#include <chrono>
#include <expected>
#include <system_error>
#include <string_view>

namespace tmux2::reactor {

class ReactorBackend {
public:
    virtual ~ReactorBackend() = default;

    // Register a file descriptor with specific event interests.
    virtual std::expected<void, std::error_code> register_fd(int fd, EventFlags flags) = 0;

    // Modify event interests for a registered descriptor.
    virtual std::expected<void, std::error_code> modify_fd(int fd, EventFlags flags) = 0;

    // Unregister a file descriptor.
    virtual std::expected<void, std::error_code> unregister_fd(int fd) = 0;

    // Wait for events up to optional timeout. Returns number of events stored in out_events.
    virtual std::expected<size_t, std::error_code> poll_events(
        std::span<PollEvent> out_events,
        std::optional<std::chrono::microseconds> timeout) = 0;

    // Name of backend ("epoll", "kqueue", "poll").
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

} // namespace tmux2::reactor
