#include <tmux2/reactor/reactor.hpp>
#include <tmux2/reactor/frame_coalescer.hpp>

namespace tmux2::reactor {

// Explicit instantiation and anchor for translation unit
const std::error_category& get_reactor_error_category() noexcept {
    return reactor_error_category();
}

} // namespace tmux2::reactor
