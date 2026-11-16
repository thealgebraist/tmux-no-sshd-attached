#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "src/platform/platform.hpp"
#include "src/server/layout.hpp"
#include "src/server/pane.hpp"
#include "src/server/window.hpp"
#include "src/server/session.hpp"
#include "src/server/client_state.hpp"
#include "src/server/session_manager.hpp"
#include "src/server/command_dsl.hpp"
#include "src/server/server_engine.hpp"
#include "src/protocol/messages.hpp"
#include "src/protocol/cell.hpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace tmux2;
using namespace tmux2::server;
using namespace tmux2::platform;
using namespace tmux2::protocol;

// ============================================================================
// Platform Helpers Test Cases
// ============================================================================
TEST_CASE("Platform Helpers") {
    SUBCASE("self_exe_path") {
        auto exe_path = self_exe_path();
        REQUIRE(exe_path.has_value());
        CHECK(!exe_path->empty());
        CHECK(fs::exists(*exe_path));
    }

    SUBCASE("Runtime directory & socket path") {
        auto rdir = default_runtime_dir();
        REQUIRE(rdir.has_value());
        CHECK(!rdir->empty());

        auto sock = default_socket_path();
        REQUIRE(sock.has_value());
        CHECK(!sock->empty());
        CHECK(sock->string().find("tmux2") != std::string::npos);
        CHECK(sock->string().find("default.sock") != std::string::npos);

        // Test ensure_runtime_dir
        fs::path test_dir = fs::temp_directory_path() / ("tmux2_test_perm_" + std::to_string(getpid()));
        fs::remove_all(test_dir);
        
        auto ok = ensure_runtime_dir(test_dir);
        REQUIRE(ok.has_value());
        CHECK(fs::exists(test_dir));

        struct ::stat st;
        CHECK(::stat(test_dir.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == 0700);

        // Test set_file_permissions
        fs::path test_file = test_dir / "test.sock";
        {
            std::ofstream ofs(test_file);
            ofs << "dummy";
        }
        auto perm_ok = set_file_permissions(test_file, 0600);
        REQUIRE(perm_ok.has_value());
        CHECK(::stat(test_file.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == 0600);

        fs::remove_all(test_dir);
    }

    SUBCASE("systemd service unit generation") {
        SystemdServiceOptions opts;
        opts.description = "tmux2 daemon test";
        opts.exec_start = "/usr/local/bin/tmux2 --daemonize";
        std::string unit = generate_systemd_service_unit(opts);
        CHECK(unit.find("[Unit]") != std::string::npos);
        CHECK(unit.find("Description=tmux2 daemon test") != std::string::npos);
        CHECK(unit.find("ExecStart=/usr/local/bin/tmux2 --daemonize") != std::string::npos);
        CHECK(unit.find("Restart=always") != std::string::npos);
        CHECK(unit.find("[Install]") != std::string::npos);
        CHECK(unit.find("WantedBy=default.target") != std::string::npos);
    }

    SUBCASE("launchagent plist generation") {
        LaunchAgentOptions opts;
        opts.label = "com.tmux2.daemon";
        opts.program_arguments = {"/usr/local/bin/tmux2", "--foreground"};
        std::string plist = generate_launchagent_plist(opts);
        CHECK(plist.find("<?xml") != std::string::npos);
        CHECK(plist.find("<key>Label</key>") != std::string::npos);
        CHECK(plist.find("<string>com.tmux2.daemon</string>") != std::string::npos);
        CHECK(plist.find("<string>/usr/local/bin/tmux2</string>") != std::string::npos);
        CHECK(plist.find("<key>RunAtLoad</key>") != std::string::npos);
        CHECK(plist.find("<key>KeepAlive</key>") != std::string::npos);
    }

    SUBCASE("POSIX daemonization double fork and pidfile") {
        auto tmp_pid = fs::temp_directory_path() / "test_tmux2_daemon.pid";
        fs::remove(tmp_pid);

        pid_t p = ::fork();
        REQUIRE(p >= 0);
        if (p == 0) {
            DaemonizeOptions opts;
            opts.pid_file = tmp_pid;
            opts.close_fds = true;
            auto res = daemonize(opts);
            if (!res) ::_exit(1);
            ::usleep(50000);
            ::_exit(0);
        }

        int status = 0;
        ::waitpid(p, &status, 0);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);

        for (int i = 0; i < 50; ++i) {
            if (fs::exists(tmp_pid) && fs::file_size(tmp_pid) > 0) break;
            ::usleep(10000);
        }
        CHECK(fs::exists(tmp_pid));
        std::ifstream pid_in(tmp_pid);
        pid_t daemon_pid = 0;
        pid_in >> daemon_pid;
        CHECK(daemon_pid > 0);
        fs::remove(tmp_pid);
    }
}

// ============================================================================
// Layout Tree Test Cases
// ============================================================================
TEST_CASE("Split Layout Tree") {
    SUBCASE("Single pane layout") {
        LayoutTree tree(1);
        tree.calculate(80, 24);

        auto r1 = tree.get_pane_rect(1);
        REQUIRE(r1.has_value());
        CHECK(r1->x == 0);
        CHECK(r1->y == 0);
        CHECK(r1->width == 80);
        CHECK(r1->height == 24);
    }

    SUBCASE("Horizontal split (left / right)") {
        LayoutTree tree(1);
        bool split_ok = tree.split_pane(1, 2, SplitType::Horizontal, 0.5);
        REQUIRE(split_ok);

        tree.calculate(80, 24);

        auto r1 = tree.get_pane_rect(1);
        auto r2 = tree.get_pane_rect(2);
        REQUIRE(r1.has_value());
        REQUIRE(r2.has_value());

        // SplitType::Horizontal divides width left and right
        CHECK(r1->x == 0);
        CHECK(r1->width == 40);
        CHECK(r1->height == 24);

        CHECK(r2->x == 40);
        CHECK(r2->width == 40);
        CHECK(r2->height == 24);
    }

    SUBCASE("Vertical split (top / bottom)") {
        LayoutTree tree(1);
        bool split_ok = tree.split_pane(1, 2, SplitType::Vertical, 0.5);
        REQUIRE(split_ok);

        tree.calculate(80, 24);

        auto r1 = tree.get_pane_rect(1);
        auto r2 = tree.get_pane_rect(2);
        REQUIRE(r1.has_value());
        REQUIRE(r2.has_value());

        // SplitType::Vertical divides height top and bottom
        CHECK(r1->y == 0);
        CHECK(r1->height == 12);
        CHECK(r1->width == 80);

        CHECK(r2->y == 12);
        CHECK(r2->height == 12);
        CHECK(r2->width == 80);
    }

    SUBCASE("Pane Zoom") {
        LayoutTree tree(1);
        tree.split_pane(1, 2, SplitType::Horizontal, 0.5);
        
        // Calculate with pane 2 zoomed
        tree.calculate(80, 24, 2);

        auto r1 = tree.get_pane_rect(1);
        auto r2 = tree.get_pane_rect(2);
        REQUIRE(r1.has_value());
        REQUIRE(r2.has_value());

        CHECK(r2->x == 0);
        CHECK(r2->y == 0);
        CHECK(r2->width == 80);
        CHECK(r2->height == 24);

        CHECK(r1->width == 0);
        CHECK(r1->height == 0);

        // Calculate without zoom
        tree.calculate(80, 24, 0);
        auto r1_unzoomed = tree.get_pane_rect(1);
        CHECK(r1_unzoomed->width == 40);
    }

    SUBCASE("Remove pane") {
        LayoutTree tree(1);
        tree.split_pane(1, 2, SplitType::Vertical, 0.5);
        tree.split_pane(2, 3, SplitType::Horizontal, 0.5);

        CHECK(tree.get_pane_rect(1).has_value());
        CHECK(tree.get_pane_rect(2).has_value());
        CHECK(tree.get_pane_rect(3).has_value());

        bool removed = tree.remove_pane(2);
        CHECK(removed);
        CHECK(!tree.get_pane_rect(2).has_value());
        CHECK(tree.get_pane_rect(1).has_value());
        CHECK(tree.get_pane_rect(3).has_value());

        tree.calculate(80, 24);
        auto r3 = tree.get_pane_rect(3);
        REQUIRE(r3.has_value());
        CHECK(r3->width == 80); // pane 3 reclaimed full width
    }

    SUBCASE("Resize pane") {
        LayoutTree tree(1);
        tree.split_pane(1, 2, SplitType::Vertical, 0.5);
        tree.calculate(80, 24);

        int orig_h = tree.get_pane_rect(1)->height;
        bool resized = tree.resize_pane(1, ResizeDirection::Down, 4, 80, 24);
        CHECK(resized);

        tree.calculate(80, 24);
        int new_h = tree.get_pane_rect(1)->height;
        CHECK(new_h > orig_h);
    }

    SUBCASE("JSON Serialization") {
        LayoutTree tree(1);
        tree.split_pane(1, 2, SplitType::Horizontal, 0.5);
        std::string json = tree.to_json();
        CHECK(json.find("\"direction\":\"horizontal\"") != std::string::npos);
        CHECK(json.find("\"pane_id\":1") != std::string::npos);
        CHECK(json.find("\"pane_id\":2") != std::string::npos);
    }
}

// ============================================================================
// Pane & Terminal ANSI Grid Test Cases
// ============================================================================
TEST_CASE("Pane and Terminal Grid") {
    auto pane = Pane::create_mock(1, 80, 24);
    REQUIRE(pane != nullptr);
    CHECK(pane->pane_id() == 1);
    CHECK(pane->width() == 80);
    CHECK(pane->height() == 24);

    SUBCASE("Plain text writing") {
        pane->feed_output("Hello, World!");
        auto snap = pane->get_snapshot();
        CHECK(snap.cols == 80);
        CHECK(snap.rows == 24);
        REQUIRE(snap.cells.size() == 80 * 24);
        CHECK(snap.cells[0].codepoint == 'H');
        CHECK(snap.cells[1].codepoint == 'e');
        CHECK(snap.cells[11].codepoint == 'd');
        CHECK(snap.cells[12].codepoint == '!');
        CHECK(snap.cursor.col == 13);
        CHECK(snap.cursor.row == 0);
    }

    SUBCASE("Newlines and carriage return") {
        pane->feed_output("Line 1\r\nLine 2");
        auto snap = pane->get_snapshot();
        CHECK(snap.cells[0].codepoint == 'L');
        CHECK(snap.cells[5].codepoint == '1');
        CHECK(snap.cells[80 + 0].codepoint == 'L');
        CHECK(snap.cells[80 + 5].codepoint == '2');
        CHECK(snap.cursor.row == 1);
        CHECK(snap.cursor.col == 6);
    }

    SUBCASE("ANSI SGR Color & Attributes") {
        // Red bold text: \x1b[31;1mRedBold\x1b[0mNormal
        pane->feed_output("\x1b[31;1mRedBold\x1b[0mNormal");
        auto snap = pane->get_snapshot();
        
        // 'R' in RedBold
        const auto& c_red = snap.cells[0];
        CHECK(c_red.codepoint == 'R');
        CHECK((c_red.style.flags & StyleFlags::Bold) != 0);
        CHECK(c_red.style.fg.type == ColorType::Indexed);
        CHECK(c_red.style.fg.r_or_idx == 1); // 1 = red

        // 'N' in Normal
        const auto& c_norm = snap.cells[7];
        CHECK(c_norm.codepoint == 'N');
        CHECK((c_norm.style.flags & StyleFlags::Bold) == 0);
        CHECK(c_norm.style.fg.type == ColorType::Default);
    }

    SUBCASE("ANSI 24-bit TrueColor") {
        pane->feed_output("\x1b[38;2;100;150;200mTrueColor");
        auto snap = pane->get_snapshot();
        const auto& c = snap.cells[0];
        CHECK(c.codepoint == 'T');
        CHECK(c.style.fg.type == ColorType::Rgb);
        CHECK(c.style.fg.r_or_idx == 100);
        CHECK(c.style.fg.g == 150);
        CHECK(c.style.fg.b == 200);
    }

    SUBCASE("Cursor positioning and clear screen") {
        pane->feed_output("\x1b[5;10HTest");
        auto snap = pane->get_snapshot();
        CHECK(snap.cursor.row == 4);
        CHECK(snap.cursor.col == 13);
        CHECK(snap.cells[4 * 80 + 9].codepoint == 'T');

        // Clear screen \x1b[2J
        pane->feed_output("\x1b[2J");
        auto snap_cleared = pane->get_snapshot();
        CHECK(snap_cleared.cells[4 * 80 + 9].codepoint == ' ');
    }

    SUBCASE("Scrolling into scrollback buffer and capture") {
        for (int i = 0; i < 30; ++i) {
            pane->feed_output("Line " + std::to_string(i) + "\r\n");
        }

        auto slice = pane->get_scrollback_slice(0, 3);
        REQUIRE(slice.lines.size() == 3);

        std::string captured = pane->capture_text(false);
        CHECK(captured.find("Line 28") != std::string::npos);

        std::string captured_with_history = pane->capture_text(true);
        CHECK(captured_with_history.find("Line 0") != std::string::npos);
        CHECK(captured_with_history.find("Line 28") != std::string::npos);
    }

    SUBCASE("Incremental Dirty Diff") {
        pane->feed_output("Initial Text");
        auto diff1 = pane->get_dirty_diff();
        CHECK(!diff1.rows.empty());

        // Calling again without modifications returns empty diff
        auto diff2 = pane->get_dirty_diff();
        CHECK(diff2.rows.empty());

        // Modify line 0
        pane->feed_output("\rUpdated");
        auto diff3 = pane->get_dirty_diff();
        REQUIRE(!diff3.rows.empty());
        CHECK(diff3.rows[0].row == 0);
    }

    SUBCASE("4-byte UTF-8 emoji and VteScreen integration") {
        pane->feed_output("Rocket \xF0\x9F\x9A\x80 test\r\n");
        std::string cap = pane->capture_text(false);
        CHECK(cap.find("Rocket") != std::string::npos);
        CHECK(cap.find("\xF0\x9F\x9A\x80") != std::string::npos);
        CHECK(cap.find("test") != std::string::npos);
        CHECK(pane->vte() != nullptr);
    }
}

// ============================================================================
// Window & Pane Management Test Cases
// ============================================================================
TEST_CASE("Window Management") {
    auto win = Window::create_mock(1, 0, "main", 1, 80, 24);
    REQUIRE(win != nullptr);
    CHECK(win->window_id() == 1);
    CHECK(win->name() == "main");
    CHECK(win->pane_count() == 1);
    CHECK(win->active_pane_id() == 1);

    SUBCASE("Split pane horizontally and vertically") {
        // split_pane_mock(target_pane_id, new_pane_id, vertical)
        auto p2 = win->split_pane_mock(1, 2, false); // horizontal
        REQUIRE(p2 != nullptr);
        CHECK(win->pane_count() == 2);
        CHECK(win->active_pane_id() == 2);

        auto p3 = win->split_pane_mock(2, 3, true); // vertical
        REQUIRE(p3 != nullptr);
        CHECK(win->pane_count() == 3);
        CHECK(win->active_pane_id() == 3);

        // Pane navigation
        win->select_previous_pane();
        CHECK(win->active_pane_id() == 2);
        win->select_previous_pane();
        CHECK(win->active_pane_id() == 1);
        win->select_next_pane();
        CHECK(win->active_pane_id() == 2);

        // Zoom toggle
        CHECK(!win->is_zoomed());
        win->toggle_zoom();
        CHECK(win->is_zoomed());
        CHECK(win->zoomed_pane_id() == 2);
        CHECK(win->layout().get_pane_rect(2)->width == 80);
        CHECK(win->layout().get_pane_rect(2)->height == 24);
        win->toggle_zoom();
        CHECK(!win->is_zoomed());

        // Layout update message
        auto update = win->get_layout_update(1, 10);
        CHECK(update.window_id == 1);
        CHECK(update.active_pane_id == 2);
        CHECK(!update.layout_tree_json.empty());

        // Kill pane
        bool killed = win->kill_pane(3);
        CHECK(killed);
        CHECK(win->pane_count() == 2);
    }

    SUBCASE("Resize Window") {
        win->split_pane_mock(1, 2, false);
        win->resize(120, 40);
        CHECK(win->cols() == 120);
        CHECK(win->rows() == 40);

        auto r1 = win->layout().get_pane_rect(1);
        auto r2 = win->layout().get_pane_rect(2);
        REQUIRE(r1.has_value());
        REQUIRE(r2.has_value());
        CHECK(r1->width + r2->width == 120);
    }
}

// ============================================================================
// Session & Multi-Client Attachment Test Cases
// ============================================================================
TEST_CASE("Session & Multi-Client Attachment") {
    auto sess = Session::create_mock(1, "my-session", 1, 1, "0", 80, 24);
    REQUIRE(sess != nullptr);
    CHECK(sess->session_id() == 1);
    CHECK(sess->name() == "my-session");
    CHECK(sess->window_count() == 1);

    SUBCASE("Window operations") {
        auto w2 = sess->create_window_mock(2, "dev", 2);
        REQUIRE(w2 != nullptr);
        CHECK(sess->window_count() == 2);
        CHECK(sess->active_window()->window_id() == 2);

        sess->select_window(1);
        CHECK(sess->active_window()->window_id() == 1);

        sess->next_window();
        CHECK(sess->active_window()->window_id() == 2);

        sess->previous_window();
        CHECK(sess->active_window()->window_id() == 1);
    }

    SUBCASE("Multi-client attachment and latest size policy") {
        // Attach Client 100 (80x24)
        sess->attach_client(100, false, 80, 24, false);
        CHECK(sess->attached_client_count() == 1);
        CHECK(sess->has_client(100));

        // Attach Client 200 (100x30)
        sess->attach_client(200, false, 100, 30, false);
        CHECK(sess->attached_client_count() == 2);

        // Interaction from Client 200 resizes window to 100x30
        sess->client_interacted(200, 100, 30);
        CHECK(sess->active_window()->cols() == 100);
        CHECK(sess->active_window()->rows() == 30);

        // Interaction from Client 100 resizes window back to 80x24
        sess->client_interacted(100, 80, 24);
        CHECK(sess->active_window()->cols() == 80);
        CHECK(sess->active_window()->rows() == 24);

        // Attach Client 300 with detach_others = true
        sess->attach_client(300, false, 90, 25, true);
        CHECK(sess->attached_client_count() == 1);
        CHECK(!sess->has_client(100));
        CHECK(!sess->has_client(200));
        CHECK(sess->has_client(300));

        // Read-only client
        sess->attach_client(400, true, 90, 25, false);
        CHECK(sess->attached_client_count() == 2);
        const auto& clients = sess->attached_clients();
        auto it = std::find_if(clients.begin(), clients.end(), [](const auto& c) { return c.client_id == 400; });
        REQUIRE(it != clients.end());
        CHECK(it->read_only == true);
    }

    SUBCASE("Window lookup strictly returns nullptr for nonexistent window") {
        CHECK(sess->find_window(9999) == nullptr);
        CHECK(sess->select_window(9999) == false);
        CHECK(sess->active_window() != nullptr);
    }
}

// ============================================================================
// ClientState & Section 5 Resumption Test Cases
// ============================================================================
TEST_CASE("ClientState & Section 5 Resumption") {
    std::string cert_fp = "SHA256:1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";
    std::array<uint8_t, 16> inst_id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    std::vector<uint8_t> hmac_key = {'s', 'e', 'c', 'r', 'e', 't'};

    ClientState client(101, inst_id, cert_fp);
    CHECK(client.client_id() == 101);
    CHECK(client.cert_fingerprint() == cert_fp);

    SUBCASE("Resume token generation and validation") {
        std::string token = ClientState::generate_resume_token(101, cert_fp, hmac_key, std::chrono::seconds(60));
        CHECK(!token.empty());

        // Valid verification
        CHECK(ClientState::validate_resume_token(token, 101, cert_fp, hmac_key));

        // Wrong client id
        CHECK(!ClientState::validate_resume_token(token, 999, cert_fp, hmac_key));

        // Wrong secret
        std::vector<uint8_t> wrong_key = {'w', 'r', 'o', 'n', 'g'};
        CHECK(!ClientState::validate_resume_token(token, 101, cert_fp, wrong_key));

        // Wrong cert fingerprint
        CHECK(!ClientState::validate_resume_token(token, 101, "SHA256:wrong", hmac_key));

        // Tampered token
        std::string tampered = token;
        tampered[0] = (tampered[0] == 'a' ? 'b' : 'a');
        CHECK(!ClientState::validate_resume_token(tampered, 101, cert_fp, hmac_key));

        // Expired token (ttl = 0 seconds)
        std::string exp_token = ClientState::generate_resume_token(101, cert_fp, hmac_key, std::chrono::seconds(0));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(!ClientState::validate_resume_token(exp_token, 101, cert_fp, hmac_key));
    }

    SUBCASE("Input deduplication (client_seq)") {
        CHECK(client.deduplicate_client_seq(1) == true);
        CHECK(client.deduplicate_client_seq(1) == false); // duplicate
        CHECK(client.deduplicate_client_seq(2) == true);
        CHECK(client.deduplicate_client_seq(2) == false); // duplicate
        CHECK(client.deduplicate_client_seq(5) == true);
        CHECK(client.deduplicate_client_seq(3) == false); // out of order / old
    }

    SUBCASE("FrameDiff Ring Buffer delta replay & aged out snapshot fallback") {
        // Push 10 frames (seq 1 to 10)
        for (uint64_t seq = 1; seq <= 10; ++seq) {
            MsgFrameDiff diff;
            diff.server_seq = seq;
            client.record_frame_diff(diff);
        }

        CHECK(client.last_srv_seq() == 10);
        CHECK(client.can_delta_replay(5) == true);
        CHECK(client.can_delta_replay(10) == true);
        CHECK(client.can_delta_replay(0) == true);

        // Replay from seq 7 (should return 8, 9, 10)
        auto replayed = client.get_replay_diffs(7);
        REQUIRE(replayed.size() == 3);
        CHECK(replayed[0].server_seq == 8);
        CHECK(replayed[1].server_seq == 9);
        CHECK(replayed[2].server_seq == 10);

        // Push 300 frames to exceed ring buffer capacity of 256
        for (uint64_t seq = 11; seq <= 300; ++seq) {
            MsgFrameDiff diff;
            diff.server_seq = seq;
            client.record_frame_diff(diff);
        }

        CHECK(client.last_srv_seq() == 300);
        // seq 10 is no longer in ring buffer
        CHECK(client.can_delta_replay(10) == false);
        // Resumption fallback to full snapshot is required
        auto diffs_aged = client.get_replay_diffs(10);
        CHECK(diffs_aged.empty());
    }
}

// ============================================================================
// SessionManager Test Cases
// ============================================================================
TEST_CASE("SessionManager") {
    SessionManager mgr;
    MsgNewSessionReq req1;
    req1.name = "alpha";
    auto s1 = mgr.new_session(req1, 80, 24, true);
    REQUIRE(s1 != nullptr);
    CHECK(s1->name() == "alpha");

    MsgNewSessionReq req2;
    req2.name = "beta";
    auto s2 = mgr.new_session(req2, 100, 30, true);
    REQUIRE(s2 != nullptr);
    CHECK(s2->name() == "beta");

    CHECK(mgr.session_count() == 2);
    CHECK(mgr.find_session(s1->session_id()) == s1);
    CHECK(mgr.find_session_by_name("alpha") == s1);
    CHECK(mgr.find_session_by_name("beta") == s2);

    auto list = mgr.list_sessions();
    REQUIRE(list.size() == 2);

    bool closed = mgr.kill_session(s1->session_id());
    CHECK(closed);
    CHECK(mgr.session_count() == 1);
    CHECK(mgr.find_session(s1->session_id()) == nullptr);
}

// ============================================================================
// Command DSL Test Cases
// ============================================================================
TEST_CASE("Command DSL") {
    SessionManager mgr;

    SUBCASE("new-session and list-sessions") {
        auto res = CommandDsl::execute(mgr, "new-session -s dsl-sess -x 80 -y 24", 1, 0, true);
        CHECK(res.exit_code == 0);
        CHECK(mgr.find_session_by_name("dsl-sess") != nullptr);

        auto ls_res = CommandDsl::execute(mgr, "list-sessions", 2, 0, true);
        CHECK(ls_res.exit_code == 0);
        CHECK(ls_res.stdout_data.find("dsl-sess") != std::string::npos);
    }

    SUBCASE("rename-session") {
        CommandDsl::execute(mgr, "new-session -s original", 1, 0, true);
        auto ren = CommandDsl::execute(mgr, "rename-session -t original renamed", 2, 0, true);
        CHECK(ren.exit_code == 0);
        CHECK(mgr.find_session_by_name("original") == nullptr);
        CHECK(mgr.find_session_by_name("renamed") != nullptr);
    }

    SUBCASE("window commands (new, select, rename)") {
        CommandDsl::execute(mgr, "new-session -s w-sess", 1, 0, true);
        auto s = mgr.find_session_by_name("w-sess");
        REQUIRE(s != nullptr);

        auto w_res = CommandDsl::execute(mgr, "new-window -n editor", 2, s->session_id(), true);
        CHECK(w_res.exit_code == 0);
        CHECK(s->window_count() == 2);

        auto sel_res = CommandDsl::execute(mgr, "select-window -t 0", 3, s->session_id(), true);
        CHECK(sel_res.exit_code == 0);
        CHECK(s->active_window()->window_id() == 1);

        auto next_res = CommandDsl::execute(mgr, "next-window", 4, s->session_id(), true);
        CHECK(next_res.exit_code == 0);
        CHECK(s->active_window()->window_id() == 2);

        auto prev_res = CommandDsl::execute(mgr, "previous-window", 5, s->session_id(), true);
        CHECK(prev_res.exit_code == 0);
        CHECK(s->active_window()->window_id() == 1);

        auto ren_res = CommandDsl::execute(mgr, "rename-window -t 0 main-win", 6, s->session_id(), true);
        CHECK(ren_res.exit_code == 0);
        CHECK(s->find_window(1)->name() == "main-win");
    }

    SUBCASE("split, zoom, and resize pane") {
        CommandDsl::execute(mgr, "new-session -s p-sess", 1, 0, true);
        auto s = mgr.find_session_by_name("p-sess");
        REQUIRE(s != nullptr);

        auto split_h = CommandDsl::execute(mgr, "split-window -h", 2, s->session_id(), true);
        CHECK(split_h.exit_code == 0);
        CHECK(s->active_window()->pane_count() == 2);

        auto split_v = CommandDsl::execute(mgr, "split-window -v", 3, s->session_id(), true);
        CHECK(split_v.exit_code == 0);
        CHECK(s->active_window()->pane_count() == 3);

        // Zoom pane
        auto zoom_res = CommandDsl::execute(mgr, "resize-pane -Z", 4, s->session_id(), true);
        CHECK(zoom_res.exit_code == 0);
        CHECK(s->active_window()->is_zoomed());

        // Toggle zoom back
        auto zoom_toggle = CommandDsl::execute(mgr, "zoom-pane", 5, s->session_id(), true);
        CHECK(zoom_toggle.exit_code == 0);
        CHECK(!s->active_window()->is_zoomed());

        // Select pane
        auto sel_p = CommandDsl::execute(mgr, "select-pane -t 1", 6, s->session_id(), true);
        CHECK(sel_p.exit_code == 0);
        CHECK(s->active_window()->active_pane_id() == 1);

        // Kill pane
        auto kill_p = CommandDsl::execute(mgr, "kill-pane -t 3", 7, s->session_id(), true);
        CHECK(kill_p.exit_code == 0);
        CHECK(s->active_window()->pane_count() == 2);
    }

    SUBCASE("send-keys and capture-pane") {
        CommandDsl::execute(mgr, "new-session -s io-sess", 1, 0, true);
        auto s = mgr.find_session_by_name("io-sess");
        REQUIRE(s != nullptr);

        auto sk = CommandDsl::execute(mgr, "send-keys \"echo hello\" Enter", 2, s->session_id(), true);
        CHECK(sk.exit_code == 0);

        auto cap = CommandDsl::execute(mgr, "capture-pane -p", 3, s->session_id(), true);
        CHECK(cap.exit_code == 0);
    }

    SUBCASE("kill-session") {
        CommandDsl::execute(mgr, "new-session -s doomed", 1, 0, true);
        CHECK(mgr.find_session_by_name("doomed") != nullptr);

        auto kill_s = CommandDsl::execute(mgr, "kill-session -t doomed", 2, 0, true);
        CHECK(kill_s.exit_code == 0);
        CHECK(mgr.find_session_by_name("doomed") == nullptr);
    }

    SUBCASE("attach-session and detach") {
        CommandDsl::execute(mgr, "new-session -s att-sess", 1, 0, true);
        auto s = mgr.find_session_by_name("att-sess");
        REQUIRE(s != nullptr);
        s->attach_client(10, false, 80, 24);
        s->attach_client(20, false, 80, 24);
        CHECK(s->attached_client_count() == 2);

        // attach with -d detaches existing clients
        auto att_d = CommandDsl::execute(mgr, "attach-session -t att-sess -d", 2, 0, true);
        CHECK(att_d.exit_code == 0);
        CHECK(s->attached_client_count() == 0);

        auto att_bad = CommandDsl::execute(mgr, "attach-session -t nonexistent", 3, 0, true);
        CHECK(att_bad.exit_code == 1);
    }
}

// ============================================================================
// ServerEngine Protocol & End-to-End Test Cases
// ============================================================================
TEST_CASE("ServerEngine Protocol Integration") {
    std::vector<std::pair<uint64_t, ProtocolMessage>> outgoing;
    ServerEngine engine([&](uint64_t cid, const ProtocolMessage& msg) {
        outgoing.emplace_back(cid, msg);
    }, /*mock_pty=*/true);

    SUBCASE("ClientHello handshake") {
        outgoing.clear();
        MsgClientHello hello;
        hello.protocol_version = 1;
        hello.term_caps = 0;
        hello.resume_token = "";
        hello.last_srv_seq = 0;

        engine.handle_message(1, hello);
        REQUIRE(!outgoing.empty());
        CHECK(outgoing.back().first == 1);

        const auto* s_hello = std::get_if<MsgServerHello>(&outgoing.back().second);
        REQUIRE(s_hello != nullptr);
        CHECK(s_hello->server_version == 1);
        CHECK(s_hello->assigned_client_id == 1);
        CHECK(!s_hello->resume_token.empty());

        auto client = engine.get_client(1);
        REQUIRE(client != nullptr);
    }

    SUBCASE("NewSession and ListSessions protocol handling") {
        // Register & hello
        MsgClientHello hello;
        engine.handle_message(2, hello);

        outgoing.clear();
        MsgNewSessionReq new_s;
        new_s.name = "proto-sess";
        engine.handle_message(2, new_s);
        REQUIRE(!outgoing.empty());

        const auto* resp = std::get_if<MsgNewSessionResp>(&outgoing[0].second);
        REQUIRE(resp != nullptr);
        CHECK(resp->success == true);
        CHECK(resp->session_id > 0);
        uint32_t created_sid = resp->session_id;

        outgoing.clear();
        MsgListSessionsReq list_req;
        engine.handle_message(2, list_req);
        REQUIRE(!outgoing.empty());

        const auto* list_resp = std::get_if<MsgListSessionsResp>(&outgoing[0].second);
        REQUIRE(list_resp != nullptr);
        REQUIRE(list_resp->sessions.size() == 1);
        CHECK(list_resp->sessions[0].name == "proto-sess");
        CHECK(list_resp->sessions[0].session_id == created_sid);
    }

    SUBCASE("AttachSession, SwitchSession, and ClientResize") {
        MsgClientHello hello;
        engine.handle_message(3, hello);

        MsgNewSessionReq new_s_a;
        new_s_a.name = "sess-a";
        engine.handle_message(3, new_s_a);

        MsgNewSessionReq new_s_b;
        new_s_b.name = "sess-b";
        engine.handle_message(3, new_s_b);

        auto sess_a = engine.session_manager().find_session_by_name("sess-a");
        auto sess_b = engine.session_manager().find_session_by_name("sess-b");
        REQUIRE(sess_a != nullptr);
        REQUIRE(sess_b != nullptr);

        outgoing.clear();
        MsgAttachSessionReq att;
        att.session_name = "sess-a";
        att.read_only = false;
        att.cols = 80;
        att.rows = 24;
        engine.handle_message(3, att);

        REQUIRE(!outgoing.empty());
        const auto* att_resp = std::get_if<MsgAttachSessionResp>(&outgoing[0].second);
        REQUIRE(att_resp != nullptr);
        CHECK(att_resp->success == true);
        CHECK(att_resp->session_id == sess_a->session_id());

        // In-band SwitchSession to sess-b
        outgoing.clear();
        MsgSwitchSessionReq sw;
        sw.session_name = "sess-b";
        engine.handle_message(3, sw);

        REQUIRE(!outgoing.empty());
        const auto* sw_resp = std::get_if<MsgSwitchSessionResp>(&outgoing[0].second);
        REQUIRE(sw_resp != nullptr);
        CHECK(sw_resp->success == true);
        CHECK(sw_resp->session_id == sess_b->session_id());

        // ClientResize
        outgoing.clear();
        MsgClientResize resize;
        resize.cols = 120;
        resize.rows = 40;
        engine.handle_message(3, resize);

        REQUIRE(!outgoing.empty());
        const auto* layout_up = std::get_if<MsgLayoutUpdate>(&outgoing[0].second);
        REQUIRE(layout_up != nullptr);
        CHECK(sess_b->cols() == 120);
        CHECK(sess_b->rows() == 40);

        // Detach
        outgoing.clear();
        MsgDetachReq det;
        engine.handle_message(3, det);
        CHECK(engine.get_client(3)->attached_session_id() == 0);
    }

    SUBCASE("Session Resumption with HMAC token") {
        outgoing.clear();
        MsgClientHello hello;
        hello.protocol_version = 1;
        engine.handle_message(4, hello);

        REQUIRE(!outgoing.empty());
        const auto* hello_resp = std::get_if<MsgServerHello>(&outgoing[0].second);
        REQUIRE(hello_resp != nullptr);
        std::string token = hello_resp->resume_token;
        CHECK(!token.empty());

        // Attach to session
        MsgNewSessionReq new_s;
        new_s.name = "resume-sess";
        engine.handle_message(4, new_s);

        auto* client = engine.get_client(4).get();
        REQUIRE(client != nullptr);

        // Push some frames into client's ring buffer
        for (uint64_t i = 1; i <= 5; ++i) {
            MsgFrameDiff fd;
            fd.server_seq = i;
            client->record_frame_diff(fd);
        }

        // Reconnect with resume token and last_srv_seq = 3
        outgoing.clear();
        MsgClientHello resume_hello;
        resume_hello.protocol_version = 1;
        resume_hello.resume_token = token;
        resume_hello.last_srv_seq = 3;

        engine.handle_message(4, resume_hello);
        REQUIRE(!outgoing.empty());

        // First message is ServerHello
        const auto* s_hello = std::get_if<MsgServerHello>(&outgoing[0].second);
        REQUIRE(s_hello != nullptr);

        // Subsequent messages are replayed frames (4 and 5)
        REQUIRE(outgoing.size() == 3);
        const auto* fd4 = std::get_if<MsgFrameDiff>(&outgoing[1].second);
        const auto* fd5 = std::get_if<MsgFrameDiff>(&outgoing[2].second);
        REQUIRE(fd4 != nullptr);
        REQUIRE(fd5 != nullptr);
        CHECK(fd4->server_seq == 4);
        CHECK(fd5->server_seq == 5);
    }

    SUBCASE("CommandRpcReq dispatch") {
        MsgClientHello hello;
        engine.handle_message(5, hello);

        outgoing.clear();
        MsgCommandRpcReq rpc;
        rpc.rpc_id = 42;
        rpc.command_dsl = "new-session -s rpc-sess";
        engine.handle_message(5, rpc);

        REQUIRE(!outgoing.empty());
        const auto* rpc_resp = std::get_if<MsgCommandRpcResp>(&outgoing[0].second);
        REQUIRE(rpc_resp != nullptr);
        CHECK(rpc_resp->rpc_id == 42);
        CHECK(rpc_resp->exit_code == 0);
        CHECK(engine.session_manager().find_session_by_name("rpc-sess") != nullptr);
    }

    SUBCASE("Reactor and PTY polling integration") {
        auto reactor_res = tmux2::reactor::Reactor::create_default();
        REQUIRE(reactor_res.has_value());
        auto reactor = std::move(*reactor_res);

        // Attach engine to reactor
        engine.attach_to_reactor(*reactor);

        // Poll panes returns false when no pty output in mock mode
        CHECK_FALSE(engine.poll_panes());
    }
}
