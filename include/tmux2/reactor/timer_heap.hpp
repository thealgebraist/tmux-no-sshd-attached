#pragma once

#include <tmux2/reactor/types.hpp>

#include <chrono>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include <algorithm>
#include <cstdint>

namespace tmux2::reactor {

class TimerHeap {
public:
    struct TimerEntry {
        TimerId id{kInvalidTimerId};
        std::chrono::steady_clock::time_point deadline;
        std::chrono::microseconds interval{0}; // 0 = one-shot
        TimerCallback callback;
        bool cancelled{false};
    };

    TimerHeap() = default;
    ~TimerHeap() = default;

    TimerHeap(const TimerHeap&) = delete;
    TimerHeap& operator=(const TimerHeap&) = delete;

    TimerHeap(TimerHeap&&) noexcept = default;
    TimerHeap& operator=(TimerHeap&&) noexcept = default;

    // Schedule a one-shot timer with relative delay.
    TimerId add_timer(std::chrono::microseconds delay, TimerCallback callback) {
        return add_repeating_timer(delay, std::chrono::microseconds(0), std::move(callback));
    }

    // Schedule a repeating timer with initial delay and periodic interval.
    TimerId add_repeating_timer(std::chrono::microseconds initial_delay,
                                std::chrono::microseconds interval,
                                TimerCallback callback) {
        auto now = std::chrono::steady_clock::now();
        auto deadline = now + initial_delay;
        return add_timer_at(deadline, interval, std::move(callback));
    }

    // Schedule a timer with an absolute deadline.
    TimerId add_timer_at(std::chrono::steady_clock::time_point deadline,
                         TimerCallback callback) {
        return add_timer_at(deadline, std::chrono::microseconds(0), std::move(callback));
    }

    TimerId add_timer_at(std::chrono::steady_clock::time_point deadline,
                         std::chrono::microseconds interval,
                         TimerCallback callback) {
        TimerId id = ++m_next_id;
        auto entry = std::make_shared<TimerEntry>(TimerEntry{
            .id = id,
            .deadline = deadline,
            .interval = interval,
            .callback = std::move(callback),
            .cancelled = false
        });

        m_active_timers[id] = entry;
        m_heap.push_back(entry);
        std::push_heap(m_heap.begin(), m_heap.end(), CompareTimer{});
        return id;
    }

    // Cancel an existing timer. Returns true if the timer was active and cancelled.
    bool cancel(TimerId id) {
        auto it = m_active_timers.find(id);
        if (it == m_active_timers.end()) {
            return false;
        }
        it->second->cancelled = true;
        m_active_timers.erase(it);
        return true;
    }

    // Check if timer is active.
    [[nodiscard]] bool is_active(TimerId id) const noexcept {
        return m_active_timers.find(id) != m_active_timers.end();
    }

    // Returns the number of currently active timers.
    [[nodiscard]] size_t active_count() const noexcept {
        return m_active_timers.size();
    }

    // Returns true if there are no active timers.
    [[nodiscard]] bool empty() const noexcept {
        return m_active_timers.empty();
    }

    // Return next deadline if any active timers exist.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_deadline() {
        prune_cancelled_top();
        if (m_heap.empty()) {
            return std::nullopt;
        }
        return m_heap.front()->deadline;
    }

    // Return relative duration until the next timer deadline, or nullopt if none.
    // If the next deadline is in the past, returns 0 microseconds.
    [[nodiscard]] std::optional<std::chrono::microseconds> time_to_next(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        prune_cancelled_top();
        if (m_heap.empty()) {
            return std::nullopt;
        }
        auto deadline = m_heap.front()->deadline;
        if (deadline <= now) {
            return std::chrono::microseconds(0);
        }
        return std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    }

    // Process all expired timers up to the given time point.
    // Re-arms repeating timers. Returns the number of timers dispatched.
    size_t process_expired(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        size_t dispatched = 0;
        std::vector<std::shared_ptr<TimerEntry>> to_reschedule;

        while (!m_heap.empty()) {
            prune_cancelled_top();
            if (m_heap.empty()) {
                break;
            }

            if (m_heap.front()->deadline > now) {
                // Not expired yet
                break;
            }

            // Pop the earliest timer
            std::pop_heap(m_heap.begin(), m_heap.end(), CompareTimer{});
            auto entry = std::move(m_heap.back());
            m_heap.pop_back();

            if (entry->cancelled) {
                continue;
            }

            // Remove from active set before invoking callback (if one-shot)
            bool is_repeating = (entry->interval.count() > 0);
            if (!is_repeating) {
                m_active_timers.erase(entry->id);
            }

            // Fire callback
            if (entry->callback) {
                entry->callback(entry->id);
                dispatched++;
            }

            // If repeating and not cancelled during callback, reschedule
            if (is_repeating && !entry->cancelled) {
                // Advance deadline
                entry->deadline += entry->interval;
                if (entry->deadline <= now) {
                    // Prevent falling behind under system stall
                    entry->deadline = now + entry->interval;
                }
                to_reschedule.push_back(std::move(entry));
            }
        }

        // Re-insert repeating timers
        for (auto& entry : to_reschedule) {
            if (!entry->cancelled) {
                m_heap.push_back(entry);
                std::push_heap(m_heap.begin(), m_heap.end(), CompareTimer{});
            }
        }

        return dispatched;
    }

    // Clear all timers.
    void clear() {
        m_active_timers.clear();
        m_heap.clear();
    }

private:
    struct CompareTimer {
        // Min-heap: top element has smallest deadline
        bool operator()(const std::shared_ptr<TimerEntry>& a,
                        const std::shared_ptr<TimerEntry>& b) const noexcept {
            if (a->deadline != b->deadline) {
                return a->deadline > b->deadline;
            }
            return a->id > b->id; // Tie-breaker for stable FIFO ordering
        }
    };

    void prune_cancelled_top() {
        while (!m_heap.empty() && m_heap.front()->cancelled) {
            std::pop_heap(m_heap.begin(), m_heap.end(), CompareTimer{});
            m_heap.pop_back();
        }
    }

    TimerId m_next_id{0};
    std::vector<std::shared_ptr<TimerEntry>> m_heap;
    std::unordered_map<TimerId, std::shared_ptr<TimerEntry>> m_active_timers;
};

} // namespace tmux2::reactor
