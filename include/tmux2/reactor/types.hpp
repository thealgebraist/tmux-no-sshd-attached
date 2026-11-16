#pragma once

#include <cstdint>
#include <chrono>
#include <functional>
#include <system_error>
#include <type_traits>
#include <string_view>

namespace tmux2::reactor {

// Monotonically increasing identifier for scheduled timers.
using TimerId = uint64_t;
inline constexpr TimerId kInvalidTimerId = 0;

// Bitmask representing reactor I/O events.
enum class EventFlags : uint32_t {
    None           = 0,
    Readable       = 1 << 0,
    Writable       = 1 << 1,
    EdgeTriggered  = 1 << 2,
    OneShot        = 1 << 3,
    Error          = 1 << 4,
    Hangup         = 1 << 5,
};

constexpr EventFlags operator|(EventFlags lhs, EventFlags rhs) noexcept {
    return static_cast<EventFlags>(
        static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs)
    );
}

constexpr EventFlags operator&(EventFlags lhs, EventFlags rhs) noexcept {
    return static_cast<EventFlags>(
        static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs)
    );
}

constexpr EventFlags operator^(EventFlags lhs, EventFlags rhs) noexcept {
    return static_cast<EventFlags>(
        static_cast<uint32_t>(lhs) ^ static_cast<uint32_t>(rhs)
    );
}

constexpr EventFlags operator~(EventFlags flags) noexcept {
    return static_cast<EventFlags>(~static_cast<uint32_t>(flags));
}

constexpr EventFlags& operator|=(EventFlags& lhs, EventFlags rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

constexpr EventFlags& operator&=(EventFlags& lhs, EventFlags rhs) noexcept {
    lhs = lhs & rhs;
    return lhs;
}

constexpr EventFlags& operator^=(EventFlags& lhs, EventFlags rhs) noexcept {
    lhs = lhs ^ rhs;
    return lhs;
}

constexpr bool has_flag(EventFlags flags, EventFlags flag) noexcept {
    return (flags & flag) != EventFlags::None;
}

// Callback types for reactor events.
using EventCallback = std::function<void(int fd, EventFlags events)>;
using TimerCallback = std::function<void(TimerId id)>;
using SignalCallback = std::function<void(int signum)>;

// Single event returned by backend poll.
struct PollEvent {
    int fd{-1};
    EventFlags events{EventFlags::None};
};

// Error codes for reactor operations.
enum class ReactorError {
    Success = 0,
    BackendCreateFailed,
    InvalidFileDescriptor,
    DescriptorAlreadyRegistered,
    DescriptorNotFound,
    OperationFailed,
    Interrupted,
    NotSupported,
};

class ReactorErrorCategory : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override {
        return "tmux2::reactor";
    }

    [[nodiscard]] std::string message(int ev) const override {
        switch (static_cast<ReactorError>(ev)) {
            case ReactorError::Success:
                return "Success";
            case ReactorError::BackendCreateFailed:
                return "Failed to initialize reactor backend";
            case ReactorError::InvalidFileDescriptor:
                return "Invalid file descriptor";
            case ReactorError::DescriptorAlreadyRegistered:
                return "File descriptor already registered in reactor";
            case ReactorError::DescriptorNotFound:
                return "File descriptor not found in reactor";
            case ReactorError::OperationFailed:
                return "Reactor operation failed";
            case ReactorError::Interrupted:
                return "Reactor wait call was interrupted";
            case ReactorError::NotSupported:
                return "Operation not supported on this platform/device";
            default:
                return "Unknown reactor error";
        }
    }
};

inline const std::error_category& reactor_error_category() noexcept {
    static const ReactorErrorCategory category;
    return category;
}

inline std::error_code make_error_code(ReactorError e) noexcept {
    return {static_cast<int>(e), reactor_error_category()};
}

} // namespace tmux2::reactor

namespace std {
template <>
struct is_error_code_enum<tmux2::reactor::ReactorError> : true_type {};
} // namespace std
