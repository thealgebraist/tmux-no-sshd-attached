#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <tmux2/reactor/types.hpp>
#include <tmux2/reactor/timer_heap.hpp>
#include <tmux2/reactor/signal_handler.hpp>
#include <tmux2/reactor/poll_backend.hpp>
#include <tmux2/reactor/reactor.hpp>
#include <tmux2/reactor/frame_coalescer.hpp>

#if defined(__linux__)
#include <tmux2/reactor/epoll_backend.hpp>
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <csignal>
#include <thread>
#include <atomic>
#include <chrono>

using namespace tmux2::reactor;
using namespace std::chrono_literals;

TEST_CASE("EventFlags bitwise operations") {
    EventFlags f = EventFlags::Readable | EventFlags::Writable;
    CHECK(has_flag(f, EventFlags::Readable));
    CHECK(has_flag(f, EventFlags::Writable));
    CHECK_FALSE(has_flag(f, EventFlags::EdgeTriggered));
    CHECK_FALSE(has_flag(f, EventFlags::OneShot));

    f |= EventFlags::EdgeTriggered;
    CHECK(has_flag(f, EventFlags::EdgeTriggered));

    f &= ~EventFlags::Writable;
    CHECK_FALSE(has_flag(f, EventFlags::Writable));
    CHECK(has_flag(f, EventFlags::Readable));
    CHECK(has_flag(f, EventFlags::EdgeTriggered));

    EventFlags xor_f = EventFlags::Readable ^ EventFlags::Readable;
    CHECK(xor_f == EventFlags::None);
}

TEST_CASE("TimerHeap basic insertion, ordering, and expiration") {
    TimerHeap heap;
    CHECK(heap.empty());
    CHECK(heap.active_count() == 0);
    CHECK_FALSE(heap.time_to_next().has_value());

    std::vector<int> fired_order;

    auto t3 = heap.add_timer(50ms, [&](TimerId) { fired_order.push_back(3); });
    auto t1 = heap.add_timer(10ms, [&](TimerId) { fired_order.push_back(1); });
    auto t2 = heap.add_timer(30ms, [&](TimerId) { fired_order.push_back(2); });

    CHECK(heap.active_count() == 3);
    CHECK(heap.is_active(t1));
    CHECK(heap.is_active(t2));
    CHECK(heap.is_active(t3));

    // Next deadline should be around 10ms
    auto ttn = heap.time_to_next();
    REQUIRE(ttn.has_value());
    CHECK(ttn->count() <= 10000);

    // Sleep 15ms: only t1 should fire
    std::this_thread::sleep_for(15ms);
    size_t count = heap.process_expired();
    CHECK(count == 1);
    REQUIRE(fired_order.size() == 1);
    CHECK(fired_order[0] == 1);
    CHECK_FALSE(heap.is_active(t1));
    CHECK(heap.is_active(t2));
    CHECK(heap.is_active(t3));

    // Sleep 40ms: t2 and t3 should fire in order
    std::this_thread::sleep_for(45ms);
    count = heap.process_expired();
    CHECK(count == 2);
    REQUIRE(fired_order.size() == 3);
    CHECK(fired_order[1] == 2);
    CHECK(fired_order[2] == 3);
    CHECK(heap.empty());
}

TEST_CASE("TimerHeap cancellation") {
    TimerHeap heap;
    bool fired = false;

    auto t1 = heap.add_timer(10ms, [&](TimerId) { fired = true; });
    CHECK(heap.is_active(t1));

    CHECK(heap.cancel(t1));
    CHECK_FALSE(heap.is_active(t1));
    CHECK_FALSE(heap.cancel(t1)); // Cancelling again returns false

    // Next deadline should be nullopt since t1 was cancelled
    CHECK_FALSE(heap.time_to_next().has_value());

    std::this_thread::sleep_for(15ms);
    size_t count = heap.process_expired();
    CHECK(count == 0);
    CHECK_FALSE(fired);
}

TEST_CASE("TimerHeap repeating timer") {
    TimerHeap heap;
    int tick_count = 0;

    TimerId rep_id = heap.add_repeating_timer(5ms, 10ms, [&](TimerId id) {
        tick_count++;
        if (tick_count >= 3) {
            heap.cancel(id);
        }
    });

    CHECK(heap.is_active(rep_id));

    // Process first tick
    std::this_thread::sleep_for(8ms);
    heap.process_expired();
    CHECK(tick_count == 1);
    CHECK(heap.is_active(rep_id));

    // Process second tick
    std::this_thread::sleep_for(12ms);
    heap.process_expired();
    CHECK(tick_count == 2);
    CHECK(heap.is_active(rep_id));

    // Process third tick (cancels itself)
    std::this_thread::sleep_for(12ms);
    heap.process_expired();
    CHECK(tick_count == 3);
    CHECK_FALSE(heap.is_active(rep_id));
}

TEST_CASE("TimerHeap microsecond precision") {
    TimerHeap heap;
    bool fired = false;

    // 500 microseconds
    auto id = heap.add_timer(500us, [&](TimerId) { fired = true; });
    CHECK(heap.is_active(id));

    std::this_thread::sleep_for(1ms);
    size_t n = heap.process_expired();
    CHECK(n == 1);
    CHECK(fired);
}

TEST_CASE("PollBackend basic I/O") {
    PollBackend backend;
    CHECK(backend.name() == "poll");

    int p[2];
    REQUIRE(::pipe(p) == 0);

    // Make read end non-blocking
    int flags = ::fcntl(p[0], F_GETFL);
    ::fcntl(p[0], F_SETFL, flags | O_NONBLOCK);

    auto reg_res = backend.register_fd(p[0], EventFlags::Readable);
    CHECK(reg_res.has_value());
    CHECK(backend.contains(p[0]));

    // Initially nothing readable
    PollEvent evs[4];
    auto count_res = backend.poll_events(evs, 0us);
    REQUIRE(count_res.has_value());
    CHECK(*count_res == 0);

    // Write to pipe
    char c = 'A';
    REQUIRE(::write(p[1], &c, 1) == 1);

    // Now should be readable
    count_res = backend.poll_events(evs, 10ms);
    REQUIRE(count_res.has_value());
    CHECK(*count_res == 1);
    CHECK(evs[0].fd == p[0]);
    CHECK(has_flag(evs[0].events, EventFlags::Readable));

    // Drain
    char buf[1];
    REQUIRE(::read(p[0], buf, 1) == 1);
    CHECK(buf[0] == 'A');

    // Unregister
    CHECK(backend.unregister_fd(p[0]).has_value());
    CHECK_FALSE(backend.contains(p[0]));

    ::close(p[0]);
    ::close(p[1]);
}

#if defined(__linux__)
TEST_CASE("EpollBackend basic I/O") {
    auto be_res = EpollBackend::create();
    REQUIRE(be_res.has_value());
    auto backend = std::move(*be_res);
    CHECK(backend->name() == "epoll");

    int p[2];
    REQUIRE(::pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);

    auto reg_res = backend->register_fd(p[0], EventFlags::Readable | EventFlags::EdgeTriggered);
    CHECK(reg_res.has_value());
    CHECK(backend->contains(p[0]));

    PollEvent evs[4];
    auto count_res = backend->poll_events(evs, 0us);
    REQUIRE(count_res.has_value());
    CHECK(*count_res == 0);

    char c = 'Z';
    REQUIRE(::write(p[1], &c, 1) == 1);

    count_res = backend->poll_events(evs, 10ms);
    REQUIRE(count_res.has_value());
    CHECK(*count_res == 1);
    CHECK(evs[0].fd == p[0]);
    CHECK(has_flag(evs[0].events, EventFlags::Readable));

    char buf[1];
    REQUIRE(::read(p[0], buf, 1) == 1);
    CHECK(buf[0] == 'Z');

    CHECK(backend->unregister_fd(p[0]).has_value());
    ::close(p[0]);
    ::close(p[1]);
}
#endif

TEST_CASE("SignalHandler self-pipe and dispatch") {
    SignalHandler sh;
    REQUIRE(sh.read_fd() >= 0);
    REQUIRE(sh.write_fd() >= 0);

    std::atomic<int> received_sig{0};
    auto res = sh.register_signal(SIGUSR1, [&](int s) {
        received_sig.store(s);
    });
    REQUIRE(res.has_value());

    // Send SIGUSR1 to self
    ::kill(::getpid(), SIGUSR1);

    // Allow signal handler to run and write to pipe
    std::this_thread::sleep_for(5ms);

    size_t processed = sh.process_pending_signals();
    CHECK(processed == 1);
    CHECK(received_sig.load() == SIGUSR1);

    sh.unregister_signal(SIGUSR1);
}

TEST_CASE("SignalHandler standard signals registration") {
    SignalHandler sh;
    std::atomic<int> sig_count{0};
    auto res = sh.register_standard_signals([&](int) {
        sig_count++;
    });
    REQUIRE(res.has_value());

    // Send SIGWINCH
    ::kill(::getpid(), SIGWINCH);
    std::this_thread::sleep_for(5ms);

    size_t n = sh.process_pending_signals();
    CHECK(n >= 1);
    CHECK(sig_count.load() >= 1);
}

TEST_CASE("Reactor full event loop with I/O and timers") {
    auto r_res = Reactor::create_default();
    REQUIRE(r_res.has_value());
    auto reactor = std::move(*r_res);

    int p[2];
    REQUIRE(::pipe(p) == 0);
    int flags = ::fcntl(p[0], F_GETFL);
    ::fcntl(p[0], F_SETFL, flags | O_NONBLOCK);

    bool io_fired = false;
    char read_char = '\0';
    auto reg_res = reactor->register_fd(p[0], EventFlags::Readable, [&](int fd, EventFlags ev) {
        if (has_flag(ev, EventFlags::Readable)) {
            io_fired = true;
            (void)::read(fd, &read_char, 1);
        }
    });
    REQUIRE(reg_res.has_value());

    bool timer_fired = false;
    reactor->add_timer(10ms, [&](TimerId) {
        timer_fired = true;
    });

    // Write to pipe
    char c = 'K';
    REQUIRE(::write(p[1], &c, 1) == 1);

    // Run loop until both fire (with timeout)
    auto start = std::chrono::steady_clock::now();
    while ((!io_fired || !timer_fired) && (std::chrono::steady_clock::now() - start < 100ms)) {
        reactor->run_once(20ms);
    }

    CHECK(io_fired);
    CHECK(read_char == 'K');
    CHECK(timer_fired);

    reactor->unregister_fd(p[0]);
    ::close(p[0]);
    ::close(p[1]);
}

TEST_CASE("Reactor wakeup and stop from background thread") {
    auto r_res = Reactor::create_default();
    REQUIRE(r_res.has_value());
    auto reactor = std::move(*r_res);

    std::atomic<bool> loop_started{false};
    std::atomic<bool> loop_finished{false};

    std::thread th([&]() {
        loop_started.store(true);
        reactor->run();
        loop_finished.store(true);
    });

    while (!loop_started.load()) {
        std::this_thread::yield();
    }

    std::this_thread::sleep_for(10ms);
    CHECK(reactor->is_running());
    CHECK_FALSE(loop_finished.load());

    // Stop and wakeup
    reactor->stop();
    th.join();

    CHECK(loop_finished.load());
    CHECK_FALSE(reactor->is_running());
}

TEST_CASE("Reactor explicit poll backend creation") {
    auto r_res = Reactor::create_poll();
    REQUIRE(r_res.has_value());
    auto reactor = std::move(*r_res);
    CHECK(reactor->backend_name() == "poll");

    bool timer_fired = false;
    reactor->add_timer(5ms, [&](TimerId) { timer_fired = true; });

    reactor->run_once(15ms);
    CHECK(timer_fired);
}

TEST_CASE("FrameCoalescer batches rapid dirty events") {
    auto r_res = Reactor::create_default();
    REQUIRE(r_res.has_value());
    auto reactor = std::move(*r_res);

    FrameCoalescer coalescer(*reactor, 10ms);
    int render_calls = 0;
    std::unordered_set<uint32_t> rendered_panes;

    auto on_render = [&](const std::unordered_set<uint32_t>& dirty) {
        render_calls++;
        rendered_panes = dirty;
    };

    // Mark panes dirty in rapid succession
    coalescer.mark_dirty(1, on_render);
    coalescer.mark_dirty(2, on_render);
    coalescer.mark_dirty(1, on_render); // duplicate
    coalescer.mark_dirty(42, on_render);

    CHECK(coalescer.is_pending());
    CHECK(coalescer.dirty_panes().size() == 3);

    // Run reactor for coalescing interval
    reactor->run_once(20ms);

    CHECK(render_calls == 1);
    CHECK(rendered_panes.size() == 3);
    CHECK(rendered_panes.count(1) == 1);
    CHECK(rendered_panes.count(2) == 1);
    CHECK(rendered_panes.count(42) == 1);
    CHECK_FALSE(coalescer.is_pending());
}

TEST_CASE("TimerHeap edge cases") {
    TimerHeap heap;
    CHECK(heap.empty());
    CHECK(heap.active_count() == 0);
    CHECK_FALSE(heap.time_to_next().has_value());
    CHECK_FALSE(heap.next_deadline().has_value());
    CHECK(heap.process_expired() == 0);

    // Cancel nonexistent
    CHECK_FALSE(heap.cancel(999999));

    // FIFO tie-breaking for equal deadlines
    auto now = std::chrono::steady_clock::now() + 5ms;
    std::vector<TimerId> fired_ids;
    auto id1 = heap.add_timer_at(now, [&](TimerId id) { fired_ids.push_back(id); });
    auto id2 = heap.add_timer_at(now, [&](TimerId id) { fired_ids.push_back(id); });
    auto id3 = heap.add_timer_at(now, [&](TimerId id) { fired_ids.push_back(id); });

    CHECK(heap.active_count() == 3);
    CHECK(id1 < id2);
    CHECK(id2 < id3);

    std::this_thread::sleep_for(10ms);
    size_t count = heap.process_expired();
    CHECK(count == 3);
    REQUIRE(fired_ids.size() == 3);
    CHECK(fired_ids[0] == id1);
    CHECK(fired_ids[1] == id2);
    CHECK(fired_ids[2] == id3);

    // Clear
    heap.add_timer(100ms, [](TimerId) {});
    CHECK(heap.active_count() == 1);
    heap.clear();
    CHECK(heap.empty());
    CHECK(heap.active_count() == 0);
}

TEST_CASE("PollBackend error and boundary handling") {
    PollBackend backend;

    // Negative fd
    auto res1 = backend.register_fd(-1, EventFlags::Readable);
    CHECK_FALSE(res1.has_value());

    // Valid pipe
    int p[2];
    REQUIRE(::pipe(p) == 0);
    auto res2 = backend.register_fd(p[0], EventFlags::Readable);
    CHECK(res2.has_value());

    // Duplicate registration
    auto res3 = backend.register_fd(p[0], EventFlags::Readable);
    CHECK_FALSE(res3.has_value());

    // Modify nonexistent fd
    auto res4 = backend.modify_fd(99999, EventFlags::Writable);
    CHECK_FALSE(res4.has_value());

    // Unregister nonexistent fd
    auto res5 = backend.unregister_fd(99999);
    CHECK_FALSE(res5.has_value());

    // Unregister valid
    CHECK(backend.unregister_fd(p[0]).has_value());

    ::close(p[0]);
    ::close(p[1]);
}

#if defined(__linux__)
TEST_CASE("EpollBackend error and boundary handling") {
    auto be_res = EpollBackend::create();
    REQUIRE(be_res.has_value());
    auto backend = std::move(*be_res);

    // Negative fd
    CHECK_FALSE(backend->register_fd(-1, EventFlags::Readable).has_value());

    // Duplicate registration
    int p[2];
    REQUIRE(::pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
    CHECK(backend->register_fd(p[0], EventFlags::Readable).has_value());
    CHECK_FALSE(backend->register_fd(p[0], EventFlags::Readable).has_value());

    // Modify nonexistent
    CHECK_FALSE(backend->modify_fd(99999, EventFlags::Writable).has_value());

    // Unregister nonexistent
    CHECK_FALSE(backend->unregister_fd(99999).has_value());

    CHECK(backend->unregister_fd(p[0]).has_value());
    ::close(p[0]);
    ::close(p[1]);
}
#endif

TEST_CASE("SignalHandler error and boundary handling") {
    SignalHandler sh;

    // Invalid signal numbers
    CHECK_FALSE(sh.register_signal(-1, [](int) {}).has_value());
    CHECK_FALSE(sh.register_signal(0, [](int) {}).has_value());
    CHECK_FALSE(sh.register_signal(64, [](int) {}).has_value());

    // Unregister nonexistent signal should not crash
    sh.unregister_signal(SIGUSR2);
}

TEST_CASE("Reactor event modification and multiple descriptors") {
    auto r_res = Reactor::create_default();
    REQUIRE(r_res.has_value());
    auto reactor = std::move(*r_res);

    int p1[2], p2[2];
    REQUIRE(::pipe(p1) == 0);
    REQUIRE(::pipe(p2) == 0);
    int flags1 = ::fcntl(p1[0], F_GETFL);
    ::fcntl(p1[0], F_SETFL, flags1 | O_NONBLOCK);
    int flags2 = ::fcntl(p2[0], F_GETFL);
    ::fcntl(p2[0], F_SETFL, flags2 | O_NONBLOCK);

    int p1_read_count = 0;
    int p2_read_count = 0;

    REQUIRE(reactor->register_fd(p1[0], EventFlags::Readable, [&](int fd, EventFlags) {
        char buf[16];
        if (::read(fd, buf, sizeof(buf)) > 0) {
            p1_read_count++;
        }
    }).has_value());

    REQUIRE(reactor->register_fd(p2[0], EventFlags::Readable, [&](int fd, EventFlags) {
        char buf[16];
        if (::read(fd, buf, sizeof(buf)) > 0) {
            p2_read_count++;
        }
    }).has_value());

    // Duplicate registration should fail
    CHECK_FALSE(reactor->register_fd(p1[0], EventFlags::Readable, [](int, EventFlags) {}).has_value());

    // Modify p1 callback
    bool modified_p1_called = false;
    REQUIRE(reactor->modify_fd(p1[0], EventFlags::Readable, [&](int fd, EventFlags) {
        char buf[16];
        if (::read(fd, buf, sizeof(buf)) > 0) {
            modified_p1_called = true;
        }
    }).has_value());

    // Write to p1 and p2
    char c = 'X';
    REQUIRE(::write(p1[1], &c, 1) == 1);
    REQUIRE(::write(p2[1], &c, 1) == 1);

    reactor->run_once(20ms);

    CHECK(modified_p1_called);
    CHECK(p1_read_count == 0); // old callback wasn't called
    CHECK(p2_read_count == 1);

    reactor->unregister_fd(p1[0]);
    reactor->unregister_fd(p2[0]);

    ::close(p1[0]);
    ::close(p1[1]);
    ::close(p2[0]);
    ::close(p2[1]);
}

TEST_CASE("OpenSSL 3.5 QUIC / TLS event timeout synchronization") {
    auto r_res = Reactor::create_default();
    REQUIRE(r_res.has_value());
    auto reactor = std::move(*r_res);

    // Initialize OpenSSL context
    const SSL_METHOD* method = TLS_server_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    REQUIRE(ctx != nullptr);

    SSL* ssl = SSL_new(ctx);
    REQUIRE(ssl != nullptr);

    TimerId quic_timer = kInvalidTimerId;
    // Initial sync on fresh TLS SSL object (usually infinite timeout unless QUIC handshake active)
    reactor->sync_openssl_timeout(ssl, quic_timer);

    // If infinite, timer is not scheduled
    struct timeval tv{};
    int is_inf = 1;
    int ret = SSL_get_event_timeout(ssl, &tv, &is_inf);
    if (ret == 1 && is_inf == 0) {
        CHECK(quic_timer != kInvalidTimerId);
    } else {
        CHECK(quic_timer == kInvalidTimerId);
    }

    SSL_free(ssl);
    SSL_CTX_free(ctx);
}
