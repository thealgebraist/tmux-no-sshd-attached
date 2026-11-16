#include "src/transport/happy_eyeballs.hpp"
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>

namespace tmux2::transport {

std::expected<std::shared_ptr<IConnection>, std::string> HappyEyeballsConnector::connect(
    const std::string& host,
    uint16_t quic_port,
    uint16_t tcp_port,
    const TlsConfig& config,
    const HappyEyeballsOptions& options,
    reactor::Reactor* reactor
) {
    auto start_time = std::chrono::steady_clock::now();

    struct RaceState {
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> cancelled{false};
        std::shared_ptr<IConnection> winner{nullptr};
        std::string quic_error;
        std::string tcp_error;
        bool quic_done{false};
        bool tcp_done{false};
    };

    auto state = std::make_shared<RaceState>();

    // 1. Start QUIC attempt in a background thread
    std::thread quic_thread([state, host, quic_port, config, options]() {
        auto res = QuicConnector::connect(
            host,
            quic_port,
            config,
            options.total_timeout,
            nullptr,
            &state->cancelled
        );

        {
            std::lock_guard<std::mutex> lock(state->mtx);
            state->quic_done = true;
            if (res) {
                if (!state->winner && !state->cancelled.load()) {
                    state->winner = *res;
                    state->cancelled.store(true);
                } else {
                    (*res)->close();
                }
            } else {
                state->quic_error = res.error();
            }
        }
        state->cv.notify_all();
    });

    bool tcp_started = false;
    std::thread tcp_thread;

    // 2. Wait up to fallback_delay for QUIC to complete or win
    {
        std::unique_lock<std::mutex> lock(state->mtx);
        state->cv.wait_for(lock, options.fallback_delay, [&]() {
            return state->winner != nullptr || state->quic_done;
        });

        // If QUIC didn't win yet and total timeout has not expired, start TCP fallback concurrently
        if (!state->winner) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time
            );
            if (elapsed < options.total_timeout) {
                auto remaining = options.total_timeout - elapsed;
                tcp_started = true;
                tcp_thread = std::thread([state, host, tcp_port, config, remaining]() {
                    auto res = TcpTlsConnector::connect(
                        host,
                        tcp_port,
                        config,
                        remaining,
                        nullptr,
                        &state->cancelled
                    );

                    {
                        std::lock_guard<std::mutex> lock(state->mtx);
                        state->tcp_done = true;
                        if (res) {
                            if (!state->winner && !state->cancelled.load()) {
                                state->winner = *res;
                                state->cancelled.store(true);
                            } else {
                                (*res)->close();
                            }
                        } else {
                            state->tcp_error = res.error();
                        }
                    }
                    state->cv.notify_all();
                });
            }
        }

        // 3. Wait for winner or for all started attempts to finish (or total timeout)
        while (!state->winner) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time
            );
            if (elapsed >= options.total_timeout) {
                break;
            }
            if (state->quic_done && (!tcp_started || state->tcp_done)) {
                break;
            }
            auto remaining = options.total_timeout - elapsed;
            state->cv.wait_for(lock, remaining, [&]() {
                return state->winner != nullptr || (state->quic_done && (!tcp_started || state->tcp_done));
            });
        }
    }

    // Cancel any still-running attempt
    state->cancelled.store(true);

    // Cleanly join all threads
    if (quic_thread.joinable()) {
        quic_thread.join();
    }
    if (tcp_started && tcp_thread.joinable()) {
        tcp_thread.join();
    }

    std::lock_guard<std::mutex> lock(state->mtx);
    if (state->winner) {
        if (reactor) {
            state->winner->register_with_reactor(*reactor);
        }
        return state->winner;
    }

    std::string err = "Happy Eyeballs failed. QUIC: " +
        (state->quic_error.empty() ? "timed out" : state->quic_error);
    if (tcp_started) {
        err += "; TCP TLS: " + (state->tcp_error.empty() ? "timed out" : state->tcp_error);
    } else {
        err += "; TCP TLS: not started (timeout before fallback)";
    }
    return std::unexpected(err);
}

} // namespace tmux2::transport
