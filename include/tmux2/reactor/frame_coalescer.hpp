#pragma once

#include <tmux2/reactor/types.hpp>
#include <tmux2/reactor/timer_heap.hpp>
#include <tmux2/reactor/reactor.hpp>

#include <chrono>
#include <functional>
#include <cstdint>
#include <unordered_set>

namespace tmux2::reactor {

// Frame coalescer for batching rapid terminal output into rate-limited display frames.
// For example, 60 FPS (16.6ms window) or low-latency interactive bursts (5ms).
class FrameCoalescer {
public:
    using RenderCallback = std::function<void(const std::unordered_set<uint32_t>& dirty_panes)>;

    explicit FrameCoalescer(Reactor& reactor,
                            std::chrono::microseconds coalesce_interval = std::chrono::microseconds(16666))
        : m_reactor(reactor)
        , m_interval(coalesce_interval) {}

    ~FrameCoalescer() {
        cancel();
    }

    FrameCoalescer(const FrameCoalescer&) = delete;
    FrameCoalescer& operator=(const FrameCoalescer&) = delete;

    void set_interval(std::chrono::microseconds interval) noexcept {
        m_interval = interval;
    }

    [[nodiscard]] std::chrono::microseconds interval() const noexcept {
        return m_interval;
    }

    // Mark a specific pane dirty and ensure a frame is scheduled within the coalescing window.
    void mark_dirty(uint32_t pane_id, RenderCallback on_render) {
        m_dirty_panes.insert(pane_id);
        schedule(std::move(on_render));
    }

    // Request a frame render without a specific pane ID.
    void request_frame(RenderCallback on_render) {
        schedule(std::move(on_render));
    }

    // Cancel pending coalesced frame.
    void cancel() {
        if (m_timer_id != kInvalidTimerId) {
            m_reactor.cancel_timer(m_timer_id);
            m_timer_id = kInvalidTimerId;
        }
        m_dirty_panes.clear();
        m_render_callback = nullptr;
    }

    [[nodiscard]] bool is_pending() const noexcept {
        return m_timer_id != kInvalidTimerId;
    }

    [[nodiscard]] const std::unordered_set<uint32_t>& dirty_panes() const noexcept {
        return m_dirty_panes;
    }

private:
    void schedule(RenderCallback on_render) {
        m_render_callback = std::move(on_render);
        if (m_timer_id != kInvalidTimerId) {
            // Already scheduled; coalescing within current window
            return;
        }

        m_timer_id = m_reactor.add_timer(m_interval, [this](TimerId) {
            m_timer_id = kInvalidTimerId;
            auto dirty = std::move(m_dirty_panes);
            m_dirty_panes.clear();
            auto cb = std::move(m_render_callback);
            m_render_callback = nullptr;

            if (cb) {
                cb(dirty);
            }
        });
    }

    Reactor& m_reactor;
    std::chrono::microseconds m_interval;
    TimerId m_timer_id{kInvalidTimerId};
    std::unordered_set<uint32_t> m_dirty_panes;
    RenderCallback m_render_callback;
};

} // namespace tmux2::reactor
