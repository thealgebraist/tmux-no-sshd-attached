#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "src/pty/pty.hpp"
#include "src/vte/vte.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ============================================================================
// 1. Terminfo Detection Tests
// ============================================================================

TEST_CASE("Terminfo: has_terminfo checks known terminal types") {
    // Standard terminal types that should exist on any standard Linux / Unix system
    CHECK(tmux2::pty::has_terminfo("xterm"));
    CHECK(tmux2::pty::has_terminfo("xterm-256color"));

    // Empty or non-existent terminal types must return false
    CHECK_FALSE(tmux2::pty::has_terminfo(""));
    CHECK_FALSE(tmux2::pty::has_terminfo("nonexistent_term_xyz_12345"));
}

TEST_CASE("Terminfo: detect_default_term selects viable terminal") {
    std::string term = tmux2::pty::detect_default_term();
    // Must be one of the known supported terminal types
    bool valid = (term == "tmux-256color" || term == "xterm-256color" || term == "xterm");
    CHECK(valid);
}

TEST_CASE("Terminfo: build_default_env sets required variables") {
    auto env = tmux2::pty::build_default_env();
    CHECK(env.contains("TERM"));
    CHECK_FALSE(env["TERM"].empty());
    CHECK(env.contains("COLORTERM"));
    CHECK(env["COLORTERM"] == "truecolor");
    CHECK(env.contains("LANG"));
    CHECK_FALSE(env["LANG"].empty());

    // Explicit override
    auto custom_env = tmux2::pty::build_default_env("custom-vt100");
    CHECK(custom_env["TERM"] == "custom-vt100");
    CHECK(custom_env["COLORTERM"] == "truecolor");
}

// ============================================================================
// 2. PtyProcess Lifecycle & I/O Tests
// ============================================================================

TEST_CASE("PtyProcess: spawn, read output, and clean exit") {
    tmux2::pty::PtyOptions options{
        .cols = 80,
        .rows = 24,
        .command = "echo 'hello from pty'"
    };

    auto spawn_res = tmux2::pty::PtyProcess::spawn(options);
    REQUIRE(spawn_res.has_value());

    auto proc = std::move(*spawn_res);
    CHECK(proc.master_fd() >= 0);
    CHECK(proc.child_pid() > 0);
    CHECK(proc.cols() == 80);
    CHECK(proc.rows() == 24);

    // Read until output is gathered or EOF
    std::string output;
    std::array<char, 256> buf{};
    auto start_time = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() - start_time < 3s) {
        auto read_res = proc.read_master(buf);
        if (read_res.has_value()) {
            output.append(buf.data(), *read_res);
        } else if (read_res.error() == tmux2::pty::PtyReadError::Eof) {
            break;
        } else if (read_res.error() == tmux2::pty::PtyReadError::WouldBlock) {
            std::this_thread::sleep_for(10ms);
        } else {
            FAIL("Unexpected PtyReadError");
        }
    }

    CHECK(output.find("hello from pty") != std::string::npos);

    // Verify exit polling
    for (int i = 0; i < 50; ++i) {
        if (proc.poll_exit()) break;
        std::this_thread::sleep_for(20ms);
    }

    CHECK_FALSE(proc.is_alive());
    REQUIRE(proc.exit_code().has_value());
    CHECK(*proc.exit_code() == 0);
}

TEST_CASE("PtyProcess: interactive bidirectional I/O with shell") {
    tmux2::pty::PtyOptions options{
        .cols = 80,
        .rows = 24,
        .command = "/bin/sh"
    };

    auto spawn_res = tmux2::pty::PtyProcess::spawn(options);
    REQUIRE(spawn_res.has_value());
    auto proc = std::move(*spawn_res);

    CHECK(proc.is_alive());

    // Send a command to the shell
    std::string test_cmd = "echo 'interactive_roundtrip_test'\n";
    auto write_res = proc.write_master(test_cmd);
    REQUIRE(write_res.has_value());
    CHECK(*write_res == test_cmd.size());

    // Read response
    std::string output;
    std::array<char, 512> buf{};
    auto start_time = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() - start_time < 3s) {
        auto read_res = proc.read_master(buf);
        if (read_res.has_value()) {
            output.append(buf.data(), *read_res);
            if (output.find("interactive_roundtrip_test") != std::string::npos) {
                break;
            }
        } else if (read_res.error() == tmux2::pty::PtyReadError::WouldBlock) {
            std::this_thread::sleep_for(10ms);
        } else if (read_res.error() == tmux2::pty::PtyReadError::Eof) {
            break;
        }
    }

    CHECK(output.find("interactive_roundtrip_test") != std::string::npos);

    // Terminate shell
    (void)proc.write_master("exit 42\n");

    for (int i = 0; i < 50; ++i) {
        if (proc.poll_exit()) break;
        std::this_thread::sleep_for(20ms);
    }

    CHECK_FALSE(proc.is_alive());
    REQUIRE(proc.exit_code().has_value());
    CHECK(*proc.exit_code() == 42);
}

TEST_CASE("PtyProcess: window resize propagation (TIOCSWINSZ)") {
    tmux2::pty::PtyOptions options{
        .cols = 80,
        .rows = 24,
        .command = "sleep 10"
    };

    auto spawn_res = tmux2::pty::PtyProcess::spawn(options);
    REQUIRE(spawn_res.has_value());
    auto proc = std::move(*spawn_res);

    CHECK(proc.cols() == 80);
    CHECK(proc.rows() == 24);

    // Resize
    bool resized = proc.resize(132, 43, 800, 600);
    CHECK(resized);
    CHECK(proc.cols() == 132);
    CHECK(proc.rows() == 43);

    // Check with ioctl TIOCGWINSZ on master descriptor
    struct winsize ws{};
    int rc = ioctl(proc.master_fd(), TIOCGWINSZ, &ws);
    CHECK(rc == 0);
    CHECK(ws.ws_col == 132);
    CHECK(ws.ws_row == 43);
    CHECK(ws.ws_xpixel == 800);
    CHECK(ws.ws_ypixel == 600);

    // Terminate child
    proc.send_signal(SIGKILL);
    proc.close_master();
}

TEST_CASE("PtyProcess: move semantics and resource cleanup") {
    tmux2::pty::PtyOptions options{
        .cols = 80,
        .rows = 24,
        .command = "sleep 5"
    };

    auto proc1 = tmux2::pty::PtyProcess::spawn(options);
    REQUIRE(proc1.has_value());

    int orig_fd = proc1->master_fd();
    pid_t orig_pid = proc1->child_pid();
    CHECK(orig_fd >= 0);
    CHECK(orig_pid > 0);

    // Move construct
    tmux2::pty::PtyProcess proc2(std::move(*proc1));
    CHECK(proc1->master_fd() == -1);
    CHECK(proc1->child_pid() == -1);
    CHECK_FALSE(proc1->is_alive());

    CHECK(proc2.master_fd() == orig_fd);
    CHECK(proc2.child_pid() == orig_pid);
    CHECK(proc2.is_alive());

    // Move assign
    tmux2::pty::PtyProcess proc3;
    proc3 = std::move(proc2);
    CHECK(proc2.master_fd() == -1);
    CHECK(proc3.master_fd() == orig_fd);
    CHECK(proc3.child_pid() == orig_pid);
    CHECK(proc3.is_alive());

    // Send signal and close
    proc3.send_signal(SIGKILL);
    proc3.close_master();
    CHECK(proc3.master_fd() == -1);
}

// ============================================================================
// 3. DirtyTracker Tests
// ============================================================================

TEST_CASE("DirtyTracker: initial state, row marking, and clear") {
    tmux2::vte::DirtyTracker tracker(24);

    CHECK(tracker.has_dirty());
    CHECK(tracker.count_dirty() == 24);
    for (uint16_t r = 0; r < 24; ++r) {
        CHECK(tracker.is_dirty(r));
    }

    auto initial_dirty = tracker.dirty_rows();
    CHECK(initial_dirty.size() == 24);

    tracker.clear();
    CHECK_FALSE(tracker.has_dirty());
    CHECK(tracker.count_dirty() == 0);
    CHECK(tracker.dirty_rows().empty());

    // Mark single row
    tracker.mark_dirty(5);
    CHECK(tracker.has_dirty());
    CHECK(tracker.count_dirty() == 1);
    CHECK(tracker.is_dirty(5));
    CHECK_FALSE(tracker.is_dirty(4));
    CHECK_FALSE(tracker.is_dirty(6));
    CHECK(tracker.dirty_rows() == std::vector<uint16_t>{5});

    // Mark range
    tracker.mark_range(10, 13); // marks rows 10, 11, 12
    CHECK(tracker.count_dirty() == 4);
    CHECK(tracker.is_dirty(10));
    CHECK(tracker.is_dirty(11));
    CHECK(tracker.is_dirty(12));
    CHECK_FALSE(tracker.is_dirty(13));

    // Out-of-bounds marking is safely ignored
    tracker.mark_dirty(100);
    CHECK_FALSE(tracker.is_dirty(100));

    // Mark all
    tracker.mark_all();
    CHECK(tracker.count_dirty() == 24);

    // Resize resets dirty flags
    tracker.resize(40);
    CHECK(tracker.count_dirty() == 40);
    CHECK(tracker.has_dirty());
}

// ============================================================================
// 4. ScrollbackBuffer Tests
// ============================================================================

TEST_CASE("ScrollbackBuffer: push, get_line, chunk compression, and slicing") {
    // 4 lines per chunk, 10 lines max capacity
    tmux2::vte::ScrollbackBuffer sb(10, 4);

    CHECK(sb.total_lines() == 0);
    CHECK(sb.max_lines() == 10);
    CHECK(sb.get_line(0) == std::nullopt);

    auto make_test_line = [](char32_t c, size_t count, bool wrapped = false) {
        tmux2::protocol::ScrollbackLine line;
        line.wrapped = wrapped;
        line.cells.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            tmux2::protocol::Cell cell;
            cell.codepoint = c;
            cell.width = 1;
            cell.style.fg = tmux2::protocol::Color::indexed(static_cast<uint8_t>(i % 256));
            line.cells.push_back(cell);
        }
        return line;
    };

    // Push 3 lines (fits inside active uncompressed chunk)
    sb.push_line(make_test_line(U'A', 10));
    sb.push_line(make_test_line(U'B', 10, true));
    sb.push_line(make_test_line(U'C', 10));

    CHECK(sb.total_lines() == 3);
    auto l0 = sb.get_line(0);
    REQUIRE(l0.has_value());
    CHECK(l0->cells.size() == 10);
    CHECK(l0->cells[0].codepoint == U'A');
    CHECK_FALSE(l0->wrapped);

    auto l1 = sb.get_line(1);
    REQUIRE(l1.has_value());
    CHECK(l1->cells[0].codepoint == U'B');
    CHECK(l1->wrapped);

    // Push 4th and 5th line -> triggers flush_active_chunk() of chunk 0
    sb.push_line(make_test_line(U'D', 10));
    sb.push_line(make_test_line(U'E', 10));

    CHECK(sb.total_lines() == 5);

    // Line 0 is now in compressed chunk; line 4 is in active chunk
    auto l0_decomp = sb.get_line(0);
    REQUIRE(l0_decomp.has_value());
    CHECK(l0_decomp->cells[0].codepoint == U'A');

    auto l4 = sb.get_line(4);
    REQUIRE(l4.has_value());
    CHECK(l4->cells[0].codepoint == U'E');

    // Slice spanning compressed and active chunks: lines 2..4 (count 3)
    auto slice = sb.get_slice(2, 3);
    REQUIRE(slice.size() == 3);
    CHECK(slice[0].cells[0].codepoint == U'C');
    CHECK(slice[1].cells[0].codepoint == U'D');
    CHECK(slice[2].cells[0].codepoint == U'E');

    // Slice boundary tests
    auto full_slice = sb.get_slice(0, 100);
    CHECK(full_slice.size() == 5);

    auto empty_slice = sb.get_slice(5, 10);
    CHECK(empty_slice.empty());

    // Push past max_lines (10) to test chunk eviction
    // Currently 5 lines: A, B, C, D, E.
    // Push F, G, H, I, J, K, L (7 more lines, total 12 > max 10)
    for (char c = 'F'; c <= 'L'; ++c) {
        sb.push_line(make_test_line(static_cast<char32_t>(c), 10));
    }

    CHECK(sb.total_lines() <= 10);
    // Oldest chunk containing A, B, C, D was evicted
    auto oldest = sb.get_line(0);
    REQUIRE(oldest.has_value());
    CHECK(oldest->cells[0].codepoint == U'E');

    // Pop lines
    auto popped = sb.pop_line();
    REQUIRE(popped.has_value());
    CHECK(popped->cells[0].codepoint == U'L');

    // Memory usage reporting
    CHECK(sb.memory_usage_bytes() > 0);

    // Clear
    sb.clear();
    CHECK(sb.total_lines() == 0);
    CHECK(sb.get_line(0) == std::nullopt);
}

// ============================================================================
// 5. VteScreen Terminal Emulation Tests
// ============================================================================

TEST_CASE("VteScreen: basic geometry, cursor, and plain text feed") {
    tmux2::vte::VteScreen vte(80, 24);

    CHECK(vte.cols() == 80);
    CHECK(vte.rows() == 24);
    CHECK(vte.cursor().col == 0);
    CHECK(vte.cursor().row == 0);
    CHECK(vte.cursor().visible);
    CHECK_FALSE(vte.is_altscreen());

    vte.feed("Hello World!");
    vte.flush_damage();

    CHECK(vte.cursor().col == 12);
    CHECK(vte.cursor().row == 0);

    // Verify cell contents
    CHECK(vte.get_cell(0, 0).codepoint == U'H');
    CHECK(vte.get_cell(4, 0).codepoint == U'o');
    CHECK(vte.get_cell(11, 0).codepoint == U'!');

    // Extracted text
    std::string text = vte.get_text(0, 0, 11, 0);
    CHECK(text == "Hello World!");
}

TEST_CASE("VteScreen: UTF-8 and Wide/Combining Characters") {
    tmux2::vte::VteScreen vte(80, 24);

    // "café 🚀 日本語"
    // 'c', 'a', 'f' (width 1)
    // 'é' (U+00E9, width 1)
    // ' ' (width 1)
    // '🚀' (U+1F680, width 2)
    // ' ' (width 1)
    // '日' (U+65E5, width 2)
    // '本' (U+672C, width 2)
    // '語' (U+8A9E, width 2)
    vte.feed("café 🚀 日本語");
    vte.flush_damage();

    CHECK(vte.get_cell(0, 0).codepoint == U'c');
    CHECK(vte.get_cell(3, 0).codepoint == 0x00E9);
    CHECK(vte.get_cell(3, 0).width == 1);

    // Emoji: col 5 has U+1F680 with width 2
    auto emoji_cell = vte.get_cell(5, 0);
    CHECK(emoji_cell.codepoint == 0x1F680);
    CHECK(emoji_cell.width == 2);

    // CJK: col 8 has '日' (0x65E5) with width 2
    auto cjk_cell = vte.get_cell(8, 0);
    CHECK(cjk_cell.codepoint == 0x65E5);
    CHECK(cjk_cell.width == 2);
}

TEST_CASE("VteScreen: 24-bit Truecolor & ANSI SGR Styling") {
    tmux2::vte::VteScreen vte(80, 24);

    // 24-bit RGB fg (255, 128, 64) and bg (10, 20, 30) + Bold + Underline
    vte.feed("\033[38;2;255;128;64m\033[48;2;10;20;30m\033[1;4mRGB\033[0m");
    vte.flush_damage();

    auto c0 = vte.get_cell(0, 0);
    CHECK(c0.codepoint == U'R');
    CHECK(c0.style.fg.type == tmux2::protocol::ColorType::Rgb);
    CHECK(c0.style.fg.r_or_idx == 255);
    CHECK(c0.style.fg.g == 128);
    CHECK(c0.style.fg.b == 64);
    CHECK(c0.style.bg.type == tmux2::protocol::ColorType::Rgb);
    CHECK(c0.style.bg.r_or_idx == 10);
    CHECK(c0.style.bg.g == 20);
    CHECK(c0.style.bg.b == 30);
    CHECK((c0.style.flags & tmux2::protocol::StyleFlags::Bold) != 0);
    CHECK((c0.style.flags & tmux2::protocol::StyleFlags::Underline) != 0);

    // Indexed color
    vte.feed("\033[38;5;196mIdx\033[0m");
    vte.flush_damage();

    auto c3 = vte.get_cell(3, 0);
    CHECK(c3.codepoint == U'I');
    CHECK(c3.style.fg.type == tmux2::protocol::ColorType::Indexed);
    CHECK(c3.style.fg.r_or_idx == 196);
}

TEST_CASE("VteScreen: Cursor Position, Visibility, and Shapes") {
    tmux2::vte::VteScreen vte(80, 24);

    // Move cursor to row 10, col 20 (CSI 10 ; 20 H -> 0-indexed row 9, col 19)
    vte.feed("\033[10;20H");
    vte.flush_damage();
    CHECK(vte.cursor().row == 9);
    CHECK(vte.cursor().col == 19);

    // Hide cursor (DECTCEM)
    vte.feed("\033[?25l");
    vte.flush_damage();
    CHECK_FALSE(vte.cursor().visible);

    // Show cursor
    vte.feed("\033[?25h");
    vte.flush_damage();
    CHECK(vte.cursor().visible);

    // Cursor shape: underline (DECSCUSR 3 or 4)
    vte.feed("\033[3 q");
    vte.flush_damage();
    CHECK(vte.cursor().shape == 1); // 1 = underline

    // Cursor shape: bar (DECSCUSR 5 or 6)
    vte.feed("\033[5 q");
    vte.flush_damage();
    CHECK(vte.cursor().shape == 2); // 2 = bar
}

TEST_CASE("VteScreen: Alternate Screen Switching") {
    tmux2::vte::VteScreen vte(80, 24);

    vte.feed("Primary Screen Content");
    vte.flush_damage();
    CHECK_FALSE(vte.is_altscreen());
    CHECK(vte.get_cell(0, 0).codepoint == U'P');

    // Switch to alternate screen
    vte.feed("\033[?1049h\033[H");
    vte.flush_damage();
    CHECK(vte.is_altscreen());

    vte.feed("Alt Screen Content");
    vte.flush_damage();
    CHECK(vte.get_cell(0, 0).codepoint == U'A');

    // Return to primary screen
    vte.feed("\033[?1049l");
    vte.flush_damage();
    CHECK_FALSE(vte.is_altscreen());
    CHECK(vte.get_cell(0, 0).codepoint == U'P');
}

TEST_CASE("VteScreen: Window Title and Bell Callbacks") {
    tmux2::vte::VteScreen vte(80, 24);

    std::string captured_title;
    vte.set_title_callback([&](std::string_view title) {
        captured_title = title;
    });

    bool bell_triggered = false;
    vte.set_bell_callback([&]() {
        bell_triggered = true;
    });

    // Set title via OSC 0
    vte.feed("\033]0;My Custom Window\007");
    vte.flush_damage();
    CHECK(vte.title() == "My Custom Window");
    CHECK(captured_title == "My Custom Window");

    // Trigger bell
    vte.feed("\007");
    vte.flush_damage();
    CHECK(bell_triggered);
}

TEST_CASE("VteScreen: OSC 52 Clipboard Callback") {
    tmux2::vte::VteScreen vte(80, 24);

    std::string captured_sel;
    std::string captured_payload;
    vte.set_osc52_callback([&](std::string_view sel, std::string_view payload) {
        captured_sel = sel;
        captured_payload = payload;
    });

    // OSC 52;c;SGVsbG8gV29ybGQ= BEL (Hello World in base64)
    vte.feed("\033]52;c;SGVsbG8gV29ybGQ=\007");
    vte.flush_damage();

    CHECK(captured_sel == "c");
    CHECK(captured_payload == "SGVsbG8gV29ybGQ=");

    // OSC 52 with ST (ESC \) terminator
    vte.feed("\033]52;p;VGVzdCBEYXRh\033\\");
    vte.flush_damage();

    CHECK(captured_sel == "p");
    CHECK(captured_payload == "VGVzdCBEYXRh");
}

TEST_CASE("VteScreen: Incremental Dirty Diffing and Snapshots") {
    tmux2::vte::VteScreen vte(80, 24);

    // Initial state diffs
    auto initial_diffs = vte.get_dirty_diffs();
    // After clear, next query should have no dirty diffs
    auto clean_diffs = vte.get_dirty_diffs();
    CHECK(clean_diffs.empty());

    // Write to row 0
    vte.feed("Row zero update\r\n");
    auto diffs1 = vte.get_dirty_diffs();
    REQUIRE(diffs1.size() >= 1);
    CHECK(diffs1[0].row == 0);
    CHECK(diffs1[0].start_col == 0);
    CHECK(diffs1[0].cells[0].codepoint == U'R');

    // Immediately querying again yields no diffs
    auto empty_diffs = vte.get_dirty_diffs();
    CHECK(empty_diffs.empty());

    // Screen snapshot verification
    auto snap = vte.get_snapshot(1234, 1);
    CHECK(snap.server_seq == 1234);
    CHECK(snap.pane_id == 1);
    CHECK(snap.cols == 80);
    CHECK(snap.rows == 24);
    CHECK(snap.cells.size() == 80 * 24);
    CHECK(snap.cells[0].codepoint == U'R');
}

TEST_CASE("VteScreen: Scrollback Buffer Integration on Row Overflow") {
    tmux2::vte::VteScreen vte(80, 5, 50); // 5 rows tall

    // Feed 10 lines
    for (int i = 1; i <= 10; ++i) {
        vte.feed("Line " + std::to_string(i) + "\n");
    }
    vte.flush_damage();

    // At least 5 lines must have scrolled into the scrollback buffer
    CHECK(vte.scrollback().total_lines() >= 5);

    auto oldest_line = vte.scrollback().get_line(0);
    REQUIRE(oldest_line.has_value());
    CHECK(oldest_line->cells[0].codepoint == U'L');
}
