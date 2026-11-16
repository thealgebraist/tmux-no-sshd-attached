#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <doctest/doctest.h>

#include "src/client/client.hpp"
#include "src/protocol/protocol.hpp"

#include <string>
#include <vector>

using namespace tmux2::client;
using namespace tmux2::protocol;

TEST_CASE("Client: POSIX raw-mode TTY RAII wrapper & WindowSize") {
    // WindowSize equality & defaults
    WindowSize ws1{80, 24, 0, 0};
    WindowSize ws2{80, 24, 0, 0};
    CHECK(ws1 == ws2);

    // TtyRaw object creation (using stdout_fd -1 / stdin_fd -1 for safe non-terminal testing)
    TtyRaw tty(-1, -1);
    CHECK_FALSE(tty.is_raw());
    CHECK_FALSE(tty.is_a_tty());
    CHECK_FALSE(tty.enable_raw_mode()); // fails gracefully on non-tty fd

    // Modes toggle state tracking
    CHECK_FALSE(tty.is_alt_screen());
    tty.enter_alt_screen();
    CHECK(tty.is_alt_screen());
    tty.leave_alt_screen();
    CHECK_FALSE(tty.is_alt_screen());

    CHECK_FALSE(tty.is_mouse_enabled());
    tty.enable_mouse();
    CHECK(tty.is_mouse_enabled());
    tty.disable_mouse();
    CHECK_FALSE(tty.is_mouse_enabled());

    CHECK_FALSE(tty.is_bracketed_paste_enabled());
    tty.enable_bracketed_paste();
    CHECK(tty.is_bracketed_paste_enabled());
    tty.disable_bracketed_paste();
    CHECK_FALSE(tty.is_bracketed_paste_enabled());

    CHECK_FALSE(tty.is_cursor_hidden());
    tty.hide_cursor();
    CHECK(tty.is_cursor_hidden());
    tty.show_cursor();
    CHECK_FALSE(tty.is_cursor_hidden());

    // Window size query fallback
    WindowSize sz = tty.get_window_size();
    CHECK(sz.cols >= 1);
    CHECK(sz.rows >= 1);

    // RAII move semantics
    TtyRaw moved = std::move(tty);
    CHECK_FALSE(moved.is_raw());
}

TEST_CASE("Client: InputParser - Keyboard, CSI, SS3, UTF-8 & Bracketed Paste") {
    InputParser parser;

    SUBCASE("Plain ASCII and UTF-8") {
        auto evs = parser.parse("hello \xf0\x9f\x91\x8b"); // "hello 👋"
        REQUIRE(evs.size() == 7);
        CHECK(std::get<ClientKeyEvent>(evs[0]).key_code == 'h');
        CHECK(std::get<ClientKeyEvent>(evs[1]).key_code == 'e');
        CHECK(std::get<ClientKeyEvent>(evs[2]).key_code == 'l');
        CHECK(std::get<ClientKeyEvent>(evs[3]).key_code == 'l');
        CHECK(std::get<ClientKeyEvent>(evs[4]).key_code == 'o');
        CHECK(std::get<ClientKeyEvent>(evs[5]).key_code == ' ');
        CHECK(std::get<ClientKeyEvent>(evs[6]).key_code == 0x1F44B); // 👋
    }

    SUBCASE("Control Characters") {
        auto evs = parser.parse(std::string_view("\x02\x01\x1a\r\t\x7f", 6));
        REQUIRE(evs.size() == 6);
        // Ctrl-b
        auto k0 = std::get<ClientKeyEvent>(evs[0]);
        CHECK(k0.key_code == 'b');
        CHECK((k0.modifiers & KeyModifiers::Ctrl) != 0);

        // Ctrl-a
        auto k1 = std::get<ClientKeyEvent>(evs[1]);
        CHECK(k1.key_code == 'a');
        CHECK((k1.modifiers & KeyModifiers::Ctrl) != 0);

        // Ctrl-z
        auto k2 = std::get<ClientKeyEvent>(evs[2]);
        CHECK(k2.key_code == 'z');
        CHECK((k2.modifiers & KeyModifiers::Ctrl) != 0);

        // Enter (\r)
        CHECK(std::get<ClientKeyEvent>(evs[3]).key_code == SpecialKey::Enter);
        // Tab (\t)
        CHECK(std::get<ClientKeyEvent>(evs[4]).key_code == SpecialKey::Tab);
        // Backspace (\x7f)
        CHECK(std::get<ClientKeyEvent>(evs[5]).key_code == SpecialKey::Backspace);
    }

    SUBCASE("VT CSI Arrow keys and modifiers") {
        auto evs = parser.parse("\x1b[A\x1b[B\x1b[C\x1b[D\x1b[1;5A");
        REQUIRE(evs.size() == 5);
        CHECK(std::get<ClientKeyEvent>(evs[0]).key_code == SpecialKey::Up);
        CHECK(std::get<ClientKeyEvent>(evs[1]).key_code == SpecialKey::Down);
        CHECK(std::get<ClientKeyEvent>(evs[2]).key_code == SpecialKey::Right);
        CHECK(std::get<ClientKeyEvent>(evs[3]).key_code == SpecialKey::Left);

        auto k4 = std::get<ClientKeyEvent>(evs[4]);
        CHECK(k4.key_code == SpecialKey::Up);
        CHECK((k4.modifiers & KeyModifiers::Ctrl) != 0);
    }

    SUBCASE("VT CSI Tilde & SS3 function keys") {
        auto evs = parser.parse("\x1b[5~\x1b[6~\x1b[2~\x1b[3~\x1bOP");
        REQUIRE(evs.size() == 5);
        CHECK(std::get<ClientKeyEvent>(evs[0]).key_code == SpecialKey::PageUp);
        CHECK(std::get<ClientKeyEvent>(evs[1]).key_code == SpecialKey::PageDown);
        CHECK(std::get<ClientKeyEvent>(evs[2]).key_code == SpecialKey::Insert);
        CHECK(std::get<ClientKeyEvent>(evs[3]).key_code == SpecialKey::Delete);
        CHECK(std::get<ClientKeyEvent>(evs[4]).key_code == SpecialKey::F1);
    }

    SUBCASE("Bracketed paste mode") {
        auto evs = parser.parse("\x1b[200~pasted payload text\x1b[201~");
        REQUIRE(evs.size() == 1);
        auto p = std::get<ClientPasteEvent>(evs[0]);
        CHECK(p.bracketed);
        std::string str(p.data.begin(), p.data.end());
        CHECK(str == "pasted payload text");
    }

    SUBCASE("Alt modifier keys") {
        auto evs = parser.parse("\x1bx");
        REQUIRE(evs.size() == 1);
        auto k = std::get<ClientKeyEvent>(evs[0]);
        CHECK(k.key_code == 'x');
        CHECK((k.modifiers & KeyModifiers::Alt) != 0);
    }

    SUBCASE("Standalone ESC flush") {
        auto evs = parser.parse("\x1b");
        CHECK(evs.empty()); // waiting for possible sequence
        auto flushed = parser.flush();
        REQUIRE(flushed.size() == 1);
        CHECK(std::get<ClientKeyEvent>(flushed[0]).key_code == SpecialKey::Escape);
    }
}

TEST_CASE("Client: InputParser - SGR Mouse Parsing") {
    InputParser parser;

    SUBCASE("SGR Mouse Press, Release, Move") {
        // Press left button at col 10, row 5 (1-based in VT: \x1b[<0;11;6M)
        auto evs1 = parser.parse("\x1b[<0;11;6M");
        REQUIRE(evs1.size() == 1);
        auto m1 = std::get<ClientMouseEvent>(evs1[0]);
        CHECK(m1.kind == ClientMouseEvent::Kind::Press);
        CHECK(m1.button == ClientMouseEvent::Button::Left);
        CHECK(m1.col == 10);
        CHECK(m1.row == 5);

        // Release at col 10, row 5 (\x1b[<0;11;6m)
        auto evs2 = parser.parse("\x1b[<0;11;6m");
        REQUIRE(evs2.size() == 1);
        auto m2 = std::get<ClientMouseEvent>(evs2[0]);
        CHECK(m2.kind == ClientMouseEvent::Kind::Release);
        CHECK(m2.col == 10);
        CHECK(m2.row == 5);

        // Drag / move with button 1 pressed (btn 32)
        auto evs3 = parser.parse("\x1b[<32;15;8M");
        REQUIRE(evs3.size() == 1);
        auto m3 = std::get<ClientMouseEvent>(evs3[0]);
        CHECK(m3.kind == ClientMouseEvent::Kind::Move);
        CHECK(m3.button == ClientMouseEvent::Button::Left);
        CHECK(m3.col == 14);
        CHECK(m3.row == 7);
    }

    SUBCASE("SGR Mouse Wheel") {
        // Wheel up: btn 64
        auto evs_up = parser.parse("\x1b[<64;20;10M");
        REQUIRE(evs_up.size() == 1);
        auto mu = std::get<ClientMouseEvent>(evs_up[0]);
        CHECK(mu.kind == ClientMouseEvent::Kind::WheelUp);
        CHECK(mu.col == 19);
        CHECK(mu.row == 9);

        // Wheel down: btn 65
        auto evs_dn = parser.parse("\x1b[<65;20;10M");
        REQUIRE(evs_dn.size() == 1);
        auto md = std::get<ClientMouseEvent>(evs_dn[0]);
        CHECK(md.kind == ClientMouseEvent::Kind::WheelDown);
        CHECK(md.col == 19);
        CHECK(md.row == 9);
    }

    SUBCASE("SGR Mouse with Modifiers") {
        // Shift+Left press: btn = 0 | 4 = 4
        auto evs_mod = parser.parse("\x1b[<4;5;5M");
        REQUIRE(evs_mod.size() == 1);
        auto mm = std::get<ClientMouseEvent>(evs_mod[0]);
        CHECK((mm.modifiers & KeyModifiers::Shift) != 0);
        CHECK(mm.button == ClientMouseEvent::Button::Left);
    }
}

TEST_CASE("Client: KeybindingsEngine - Prefix & Shortcuts") {
    KeybindingsEngine engine("C-b");
    CHECK(engine.prefix_str() == "C-b");
    CHECK(engine.prefix_key_code() == 'b');
    CHECK((engine.prefix_modifiers() & KeyModifiers::Ctrl) != 0);

    ClientKeyEvent key_ctrl_b{
        .key_code = 'b',
        .modifiers = KeyModifiers::Ctrl,
        .raw_bytes = {0x02}
    };

    SUBCASE("Normal key forwarding") {
        ClientKeyEvent key_x{.key_code = 'x', .modifiers = KeyModifiers::None, .raw_bytes = {'x'}};
        auto act = engine.handle_key(key_x);
        CHECK(act.type == KeyActionType::ForwardKey);
        REQUIRE(act.forwarded_key.has_value());
        CHECK(act.forwarded_key->key_code == 'x');
    }

    SUBCASE("Prefix activation and prefix repetition") {
        auto a1 = engine.handle_key(key_ctrl_b);
        CHECK(a1.type == KeyActionType::PrefixActivated);
        CHECK(engine.is_prefix_active());

        // Pressing prefix again emits literal Ctrl-b
        auto a2 = engine.handle_key(key_ctrl_b);
        CHECK(a2.type == KeyActionType::ForwardKey);
        REQUIRE(a2.forwarded_key.has_value());
        CHECK(a2.forwarded_key->key_code == 'b');
        CHECK_FALSE(engine.is_prefix_active());
    }

    SUBCASE("Core Prefix Shortcuts") {
        auto test_prefix_action = [&](uint32_t code, KeyActionType expected, int expected_param = 0) {
            engine.handle_key(key_ctrl_b);
            ClientKeyEvent k{.key_code = code, .modifiers = KeyModifiers::None, .raw_bytes = {static_cast<uint8_t>(code)}};
            auto act = engine.handle_key(k);
            CHECK(act.type == expected);
            if (expected_param != 0) {
                CHECK(act.param == expected_param);
            }
            CHECK_FALSE(engine.is_prefix_active());
        };

        test_prefix_action('c', KeyActionType::NewWindow);
        test_prefix_action('n', KeyActionType::NextWindow);
        test_prefix_action('p', KeyActionType::PreviousWindow);
        test_prefix_action('0', KeyActionType::SelectWindow, 0);
        test_prefix_action('3', KeyActionType::SelectWindow, 3);
        test_prefix_action('%', KeyActionType::SplitVertical);
        test_prefix_action('"', KeyActionType::SplitHorizontal);
        test_prefix_action('o', KeyActionType::FocusNextPane);
        test_prefix_action('z', KeyActionType::ToggleZoom);
        test_prefix_action('x', KeyActionType::KillPane);
        test_prefix_action('d', KeyActionType::Detach);
        test_prefix_action('(', KeyActionType::PreviousSession);
        test_prefix_action(')', KeyActionType::NextSession);
        test_prefix_action('[', KeyActionType::EnterCopyMode);
        test_prefix_action('s', KeyActionType::OpenSessionChooser);
    }

    SUBCASE("Arrow directions under prefix") {
        engine.handle_key(key_ctrl_b);
        ClientKeyEvent up_k{.key_code = SpecialKey::Up, .modifiers = KeyModifiers::None};
        auto act_up = engine.handle_key(up_k);
        CHECK(act_up.type == KeyActionType::FocusDirection);
        CHECK(act_up.direction == Direction::Up);

        engine.handle_key(key_ctrl_b);
        ClientKeyEvent down_k{.key_code = SpecialKey::Down, .modifiers = KeyModifiers::None};
        auto act_down = engine.handle_key(down_k);
        CHECK(act_down.type == KeyActionType::FocusDirection);
        CHECK(act_down.direction == Direction::Down);
    }

    SUBCASE("Prefix cancel on unmapped key") {
        engine.handle_key(key_ctrl_b);
        ClientKeyEvent unmapped{.key_code = 'q', .modifiers = KeyModifiers::None};
        auto act = engine.handle_key(unmapped);
        CHECK(act.type == KeyActionType::PrefixCancelled);
        CHECK_FALSE(engine.is_prefix_active());
    }

    SUBCASE("Command Prompt & Rename Session Prompt") {
        // : Command Prompt
        engine.handle_key(key_ctrl_b);
        ClientKeyEvent colon_k{.key_code = ':', .modifiers = KeyModifiers::None};
        auto a_col = engine.handle_key(colon_k);
        CHECK(a_col.type == KeyActionType::CommandPrompt);
        CHECK(engine.is_prompt_active());

        engine.handle_key(ClientKeyEvent{.key_code = 'l'});
        engine.handle_key(ClientKeyEvent{.key_code = 's'});
        CHECK(engine.prompt_buffer() == "ls");

        auto a_exec = engine.handle_key(ClientKeyEvent{.key_code = SpecialKey::Enter});
        CHECK(a_exec.type == KeyActionType::ExecuteCommand);
        CHECK(a_exec.str_param == "ls");
        CHECK_FALSE(engine.is_prompt_active());

        // $ Rename Session Prompt
        engine.handle_key(key_ctrl_b);
        ClientKeyEvent dollar_k{.key_code = '$', .modifiers = KeyModifiers::None};
        auto a_dol = engine.handle_key(dollar_k);
        CHECK(a_dol.type == KeyActionType::PromptRenameSession);

        engine.handle_key(ClientKeyEvent{.key_code = 'w'});
        engine.handle_key(ClientKeyEvent{.key_code = 'o'});
        engine.handle_key(ClientKeyEvent{.key_code = 'r'});
        engine.handle_key(ClientKeyEvent{.key_code = 'k'});
        auto a_ren = engine.handle_key(ClientKeyEvent{.key_code = SpecialKey::Enter});
        CHECK(a_ren.type == KeyActionType::RenameSession);
        CHECK(a_ren.str_param == "work");
    }
}

TEST_CASE("Client: Renderer - Shadow Screen, Snapshots, Diffs, DECSET 2026 & Borders") {
    Renderer renderer(80, 24);
    CHECK(renderer.term_cols() == 80);
    CHECK(renderer.term_rows() == 24);

    SUBCASE("Apply ScreenSnapshot") {
        MsgScreenSnapshot snap{
            .server_seq = 1,
            .pane_id = 1,
            .cols = 40,
            .rows = 23,
            .cursor = CursorState{.col = 5, .row = 2, .visible = true},
            .modes = 0,
            .cells = std::vector<Cell>(40 * 23, Cell{.codepoint = U'A', .width = 1})
        };
        renderer.apply_snapshot(snap);

        const auto* grid = renderer.get_pane_grid(1);
        REQUIRE(grid != nullptr);
        CHECK(grid->cols == 40);
        CHECK(grid->rows == 23);
        CHECK(grid->cell_at(0, 0)->codepoint == U'A');
        CHECK(grid->cursor.col == 5);
        CHECK(grid->cursor.row == 2);
    }

    SUBCASE("Apply FrameDiff") {
        MsgScreenSnapshot snap{
            .server_seq = 1,
            .pane_id = 1,
            .cols = 80,
            .rows = 23,
            .cursor = CursorState{},
            .modes = 0,
            .cells = std::vector<Cell>(80 * 23, Cell{.codepoint = U' ', .width = 1})
        };
        renderer.apply_snapshot(snap);

        MsgFrameDiff diff{
            .server_seq = 2,
            .acked_client_seq = 10,
            .status_line = "tmux2-status",
            .pane_diffs = {
                PaneRowDiff{
                    .pane_id = 1,
                    .cursor = CursorState{.col = 12, .row = 0, .visible = true},
                    .rows = {
                        CellRowDiff{
                            .row = 0,
                            .start_col = 0,
                            .cells = {
                                Cell{.codepoint = U'H'},
                                Cell{.codepoint = U'i'},
                                Cell{.codepoint = U'!'}
                            }
                        }
                    }
                }
            }
        };
        renderer.apply_diff(diff);

        const auto* grid = renderer.get_pane_grid(1);
        REQUIRE(grid != nullptr);
        CHECK(grid->cell_at(0, 0)->codepoint == U'H');
        CHECK(grid->cell_at(1, 0)->codepoint == U'i');
        CHECK(grid->cell_at(2, 0)->codepoint == U'!');
        CHECK(grid->cursor.col == 12);
    }

    SUBCASE("Split pane borders & active pane highlight") {
        // 2 panes split vertically: pane 1 on left (cols 0-38), border at 39, pane 2 on right (cols 40-79)
        std::vector<PaneRect> rects = {
            PaneRect{.pane_id = 1, .x = 0, .y = 0, .width = 39, .height = 23},
            PaneRect{.pane_id = 2, .x = 40, .y = 0, .width = 40, .height = 23}
        };
        renderer.set_pane_rects(rects);
        renderer.set_active_pane(1);

        // Populate grids
        renderer.apply_snapshot(MsgScreenSnapshot{
            .pane_id = 1, .cols = 39, .rows = 23,
            .cells = std::vector<Cell>(39 * 23, Cell{.codepoint = U'L'})
        });
        renderer.apply_snapshot(MsgScreenSnapshot{
            .pane_id = 2, .cols = 40, .rows = 23,
            .cells = std::vector<Cell>(40 * 23, Cell{.codepoint = U'R'})
        });

        renderer.compose_frame();

        // Left pane content
        CHECK(renderer.get_composite_cell(0, 0).codepoint == U'L');
        // Border column at 39
        const auto& border_cell = renderer.get_composite_cell(39, 0);
        CHECK(border_cell.codepoint == U'│');
        // Border adjacent to active pane 1 should be highlighted (Bold/Green)
        CHECK((border_cell.style.flags & StyleFlags::Bold) != 0);
        // Right pane content
        CHECK(renderer.get_composite_cell(40, 0).codepoint == U'R');
    }

    SUBCASE("Status bar tabs, prefix indicator & reconnect banner") {
        renderer.set_session_name("dev");
        renderer.set_window_tabs({
            WindowTabInfo{.index = 0, .name = "code", .is_active = true, .is_last = false},
            WindowTabInfo{.index = 1, .name = "logs", .is_active = false, .is_last = true}
        });
        renderer.set_prefix_indicator(true);
        renderer.set_reconnect_banner("[reconnecting 2s]");

        renderer.compose_frame();
        std::string status_text = renderer.get_row_text(23); // row 23 is status bar
        CHECK(status_text.find("[dev]") != std::string::npos);
        CHECK(status_text.find("0:code*") != std::string::npos);
        CHECK(status_text.find("1:logs-") != std::string::npos);
        CHECK(status_text.find("[prefix]") != std::string::npos);
        CHECK(status_text.find("[reconnecting 2s]") != std::string::npos);
    }

    SUBCASE("Atomic frame VT output wrapping in DECSET 2026") {
        renderer.compose_frame();
        std::string vt = renderer.render_frame_vt(true);
        CHECK(vt.starts_with("\x1b[?2026h"));
        CHECK(vt.ends_with("\x1b[?2026l"));
    }

    SUBCASE("OSC 52 Clipboard Formatting") {
        std::string osc52 = Renderer::make_osc52("hello clipboard", true);
        CHECK(osc52.starts_with("\x1b]52;c;"));
        CHECK(osc52.ends_with("\x07"));

        std::string b64 = Renderer::base64_encode("hello world");
        CHECK(Renderer::base64_decode(b64) == "hello world");
    }
}

TEST_CASE("Client: SessionChooser - Overlay & SwitchSessionReq") {
    SessionChooser chooser;
    CHECK_FALSE(chooser.is_active());

    std::vector<SessionSummary> sessions = {
        SessionSummary{.session_id = 1, .name = "main", .window_count = 2, .attached_clients = 1, .preview_summary = "bash"},
        SessionSummary{.session_id = 2, .name = "work", .window_count = 4, .attached_clients = 0, .preview_summary = "vim"},
        SessionSummary{.session_id = 3, .name = "deploy", .window_count = 1, .attached_clients = 0, .preview_summary = "cargo"}
    };

    chooser.open(sessions);
    CHECK(chooser.is_active());
    CHECK(chooser.selected_index() == 0);

    // Navigation down
    ClientKeyEvent key_down{.key_code = 'j', .modifiers = KeyModifiers::None};
    chooser.handle_key(key_down);
    CHECK(chooser.selected_index() == 1);

    // Navigation up
    ClientKeyEvent key_up{.key_code = 'k', .modifiers = KeyModifiers::None};
    chooser.handle_key(key_up);
    CHECK(chooser.selected_index() == 0);

    // Filter
    ClientKeyEvent key_w{.key_code = 'w', .modifiers = KeyModifiers::None};
    chooser.handle_key(key_w);
    CHECK(chooser.filter() == "w");
    auto filtered = chooser.filtered_sessions();
    REQUIRE(filtered.size() == 1);
    CHECK(filtered[0].name == "work");

    // Enter selection emits MsgSwitchSessionReq
    ClientKeyEvent key_enter{.key_code = SpecialKey::Enter, .modifiers = KeyModifiers::None};
    auto switch_req = chooser.handle_key(key_enter);
    REQUIRE(switch_req.has_value());
    CHECK(switch_req->session_id == 2);
    CHECK(switch_req->session_name == "work");
    CHECK_FALSE(chooser.is_active());
}

TEST_CASE("Client: CopyMode - Paging, Navigation, Search, Selection & OSC 52 Yank") {
    CopyMode copy;
    CHECK_FALSE(copy.is_active());

    copy.enter(1, 80, 24, 100);
    CHECK(copy.is_active());
    CHECK(copy.pane_id() == 1);
    CHECK(copy.total_lines() == 100);

    // Supply mock scrollback history slice
    std::vector<ScrollbackLine> slice;
    for (int i = 0; i < 24; ++i) {
        ScrollbackLine line;
        std::string txt = "line " + std::to_string(i) + " log output";
        for (char ch : txt) {
            line.cells.push_back(Cell{.codepoint = static_cast<char32_t>(ch)});
        }
        slice.push_back(std::move(line));
    }
    copy.add_local_history_lines(76, slice);

    SUBCASE("Cursor movement: h, j, k, l, g, G") {
        auto pos_init = copy.cursor();
        // Move left
        copy.handle_key(ClientKeyEvent{.key_code = 'h'});
        CHECK(copy.cursor().col == 0);

        // Move right
        copy.handle_key(ClientKeyEvent{.key_code = 'l'});
        CHECK(copy.cursor().col == 1);

        // Move up
        copy.handle_key(ClientKeyEvent{.key_code = 'k'});
        CHECK(copy.cursor().line == pos_init.line - 1);

        // Move down
        copy.handle_key(ClientKeyEvent{.key_code = 'j'});
        CHECK(copy.cursor().line == pos_init.line);

        // Jump to top: g
        copy.handle_key(ClientKeyEvent{.key_code = 'g'});
        CHECK(copy.cursor().line == 0);

        // Jump to bottom: G
        copy.handle_key(ClientKeyEvent{.key_code = 'G'});
        CHECK(copy.cursor().line == 99);
    }

    SUBCASE("Selection & Yank emits OSC 52") {
        copy.handle_key(ClientKeyEvent{.key_code = 'g'}); // jump to top (line 0)
        // Set line 76 content
        copy.enter(1, 80, 24, 100);

        // Position cursor at line 76, col 0
        while (copy.cursor().line > 76) {
            copy.handle_key(ClientKeyEvent{.key_code = 'k'});
        }
        CHECK(copy.cursor().line == 76);

        // Begin selection (Space)
        copy.handle_key(ClientKeyEvent{.key_code = ' '});
        CHECK(copy.is_selecting());

        // Move cursor 6 characters to select "line 0"
        for (int i = 0; i < 6; ++i) {
            copy.handle_key(ClientKeyEvent{.key_code = 'l'});
        }

        std::string selected = copy.get_selected_text();
        CHECK(selected.starts_with("line 0"));

        // Yank selection (y)
        auto res = copy.handle_key(ClientKeyEvent{.key_code = 'y'});
        CHECK(res.exited);
        CHECK_FALSE(copy.is_active());
        REQUIRE(res.yanked_osc52.has_value());
        CHECK(res.yanked_osc52->starts_with("\x1b]52;c;"));
        CHECK(res.yanked_osc52->ends_with("\x07"));

        // Verify the OSC 52 base64 payload decodes to the selected text
        std::string osc52_str = *res.yanked_osc52;
        size_t b64_start = osc52_str.rfind(';') + 1;
        size_t b64_end = osc52_str.find('\x07', b64_start);
        std::string b64_payload = osc52_str.substr(b64_start, b64_end - b64_start);
        CHECK(Renderer::base64_decode(b64_payload).starts_with("line 0"));
    }

    SUBCASE("Search in history") {
        copy.handle_key(ClientKeyEvent{.key_code = '/'});
        copy.handle_key(ClientKeyEvent{.key_code = 'l'});
        copy.handle_key(ClientKeyEvent{.key_code = 'o'});
        copy.handle_key(ClientKeyEvent{.key_code = 'g'});
        copy.handle_key(ClientKeyEvent{.key_code = SpecialKey::Enter});

        CHECK(copy.search_query() == "log");
        // Should have found "log" in lines 76-99
        CHECK(copy.cursor().line >= 76);
    }
}

TEST_CASE("Client: Mouse Hit-Testing & Split Border Drag Resizing") {
    ClientApp app(80, 24);

    std::vector<PaneRect> rects = {
        PaneRect{.pane_id = 1, .x = 0, .y = 0, .width = 39, .height = 23},
        PaneRect{.pane_id = 2, .x = 40, .y = 0, .width = 40, .height = 23}
    };
    app.renderer().set_pane_rects(rects);
    app.renderer().set_active_pane(1);
    app.renderer().set_window_tabs({
        WindowTabInfo{.index = 0, .name = "bash", .is_active = true},
        WindowTabInfo{.index = 1, .name = "vim", .is_active = false}
    });

    SUBCASE("Hit test pane area") {
        auto h1 = app.hit_test(10, 5);
        CHECK(h1.type == HitType::Pane);
        CHECK(h1.pane_id == 1);
        CHECK(h1.local_col == 10);
        CHECK(h1.local_row == 5);

        auto h2 = app.hit_test(45, 5);
        CHECK(h2.type == HitType::Pane);
        CHECK(h2.pane_id == 2);
        CHECK(h2.local_col == 5);
        CHECK(h2.local_row == 5);
    }

    SUBCASE("Hit test vertical split border") {
        auto hb = app.hit_test(39, 10);
        CHECK(hb.type == HitType::VerticalBorder);
        CHECK(hb.left_pane_id == 1);
        CHECK(hb.right_pane_id == 2);
    }

    SUBCASE("Split border drag resizing") {
        auto hb = app.hit_test(39, 10);
        bool resized = app.resize_border_drag(hb, 5, 0); // drag right by 5 cols
        CHECK(resized);

        const auto& new_rects = app.renderer().pane_rects();
        CHECK(new_rects[0].width == 44);
        CHECK(new_rects[1].x == 45);
        CHECK(new_rects[1].width == 35);
    }

    SUBCASE("Mouse wheel enters copy mode on normal pane") {
        ClientMouseEvent wheel_up{
            .kind = ClientMouseEvent::Kind::WheelUp,
            .button = ClientMouseEvent::Button::None,
            .col = 10,
            .row = 5
        };
        auto res = app.process_input_event(wheel_up);
        CHECK(res.should_redraw);
        CHECK(app.copy_mode().is_active());
    }
}

TEST_CASE("Client: ReconnectBuffer - Sequence Management, ACK & Replay") {
    ReconnectBuffer buf(100);
    CHECK(buf.size() == 0);

    uint64_t s1 = buf.next_seq();
    uint64_t s2 = buf.next_seq();
    uint64_t s3 = buf.next_seq();
    CHECK(s1 == 1);
    CHECK(s2 == 2);
    CHECK(s3 == 3);

    buf.push(s1, MsgKeyEvent{.client_seq = s1, .key_code = 'a'});
    buf.push(s2, MsgKeyEvent{.client_seq = s2, .key_code = 'b'});
    buf.push(s3, MsgKeyEvent{.client_seq = s3, .key_code = 'c'});
    CHECK(buf.size() == 3);

    // Server acknowledges seq 1
    buf.ack(1);
    CHECK(buf.size() == 2);

    // Replay unacknowledged messages after reconnect
    auto replay = buf.get_unacked(1);
    REQUIRE(replay.size() == 2);
    CHECK(replay[0].client_seq == 2);
    CHECK(replay[1].client_seq == 3);

    // Server acks everything
    buf.ack(3);
    CHECK(buf.size() == 0);
    CHECK(buf.get_unacked(3).empty());
}

TEST_CASE("Client: CLI Parser - Full Commands & Options") {
    SUBCASE("host:session shorthand") {
        const char* argv[] = {"tmux2", "dev:work"};
        auto opts = CliParser::parse(2, argv);
        CHECK(opts.command == CliCommandType::AttachSession);
        CHECK(opts.host == "dev");
        CHECK(opts.session_name == "work");
    }

    SUBCASE("-H flag with new session") {
        const char* argv[] = {"tmux2", "-H", "infra", "new", "-A", "-s", "main", "-n", "editor", "vim"};
        auto opts = CliParser::parse(10, argv);
        CHECK(opts.command == CliCommandType::NewSession);
        CHECK(opts.host == "infra");
        CHECK(opts.attach_if_exists);
        CHECK(opts.session_name == "main");
        CHECK(opts.window_name == "editor");
        REQUIRE(opts.args.size() == 1);
        CHECK(opts.args[0] == "vim");
    }

    SUBCASE("attach with target") {
        const char* argv[] = {"tmux2", "attach", "-d", "-t", "dev:logs"};
        auto opts = CliParser::parse(5, argv);
        CHECK(opts.command == CliCommandType::AttachSession);
        CHECK(opts.detached);
        CHECK(opts.host == "dev");
        CHECK(opts.session_name == "logs");
    }

    SUBCASE("ls / list-sessions") {
        const char* argv[] = {"tmux2", "-H", "dev", "ls"};
        auto opts = CliParser::parse(4, argv);
        CHECK(opts.command == CliCommandType::ListSessions);
        CHECK(opts.host == "dev");
    }

    SUBCASE("split-window") {
        const char* argv[] = {"tmux2", "split-window", "-h", "-c", "/var/log", "tail", "-f", "syslog"};
        auto opts = CliParser::parse(8, argv);
        CHECK(opts.command == CliCommandType::SplitWindow);
        CHECK(opts.horizontal_split);
        CHECK(opts.cwd == "/var/log");
        REQUIRE(opts.args.size() == 3);
        CHECK(opts.args[0] == "tail");
    }

    SUBCASE("send-keys & capture-pane") {
        const char* argv_sk[] = {"tmux2", "send-keys", "-t", "main:0.0", "echo hi", "Enter"};
        auto opts_sk = CliParser::parse(6, argv_sk);
        CHECK(opts_sk.command == CliCommandType::SendKeys);
        CHECK(opts_sk.target_pane == "main:0.0");
        REQUIRE(opts_sk.args.size() == 2);

        const char* argv_cp[] = {"tmux2", "capture-pane", "-p", "-S", "-50", "-E", "-1"};
        auto opts_cp = CliParser::parse(7, argv_cp);
        CHECK(opts_cp.command == CliCommandType::CapturePane);
        CHECK(opts_cp.print_to_stdout);
        CHECK(opts_cp.capture_start == -50);
        CHECK(opts_cp.capture_end == -1);
    }

    SUBCASE("pki init and export-client") {
        const char* argv_init[] = {"tmux2", "pki", "init", "--host", "dev.lan", "--user", "bob"};
        auto opts_init = CliParser::parse(7, argv_init);
        CHECK(opts_init.command == CliCommandType::PkiInit);
        CHECK(opts_init.pki_host == "dev.lan");
        CHECK(opts_init.pki_user == "bob");

        const char* argv_exp[] = {"tmux2", "pki", "export-client", "alice", "--server", "dev.lan:8443", "--out", "alice.profile"};
        auto opts_exp = CliParser::parse(8, argv_exp);
        CHECK(opts_exp.command == CliCommandType::PkiExportClient);
        CHECK(opts_exp.pki_user == "alice");
        CHECK(opts_exp.pki_server == "dev.lan:8443");
        CHECK(opts_exp.pki_out_file == "alice.profile");
    }

    SUBCASE("profile import") {
        const char* argv[] = {"tmux2", "profile", "import", "my.profile"};
        auto opts = CliParser::parse(4, argv);
        CHECK(opts.command == CliCommandType::ProfileImport);
        CHECK(opts.profile_file == "my.profile");
    }

    SUBCASE("server daemonize") {
        const char* argv[] = {"tmux2", "server", "--daemonize", "-c", "/etc/tmux2/server.toml"};
        auto opts = CliParser::parse(5, argv);
        CHECK(opts.command == CliCommandType::Server);
        CHECK(opts.daemonize);
        CHECK(opts.config_path == "/etc/tmux2/server.toml");
    }

    SUBCASE("CLI error handling and invalid arguments") {
        const char* argv_err1[] = {"tmux2", "-H"};
        auto opts1 = CliParser::parse(2, argv_err1);
        CHECK_FALSE(opts1.success);

        const char* argv_err2[] = {"tmux2", "pki", "invalid-sub"};
        auto opts2 = CliParser::parse(3, argv_err2);
        CHECK_FALSE(opts2.success);

        const char* argv_err3[] = {"tmux2", "unknown-command"};
        auto opts3 = CliParser::parse(2, argv_err3);
        CHECK_FALSE(opts3.success);

        CHECK_FALSE(CliParser::format_help().empty());
        CHECK_FALSE(CliParser::format_version().empty());
    }
}

TEST_CASE("Client: ClientApp - Server Message Handling & Reconnect Catch-Up") {
    ClientApp app(80, 24);

    SUBCASE("Handle ServerHello updates session list and acks client seq") {
        // Send a key first to increment client_seq
        ClientKeyEvent k{.key_code = 'a', .modifiers = KeyModifiers::None, .raw_bytes = {'a'}};
        auto r = app.process_input_event(k);
        CHECK(r.outbound_messages.size() == 1);
        CHECK(app.reconnect_buffer().size() == 1);

        // ServerHello arrives acking client seq 1
        MsgServerHello hello{
            .server_version = 1,
            .assigned_client_id = 42,
            .last_client_seq_acked = 1,
            .session_list = {
                SessionSummary{.session_id = 1, .name = "work"}
            }
        };
        auto s_res = app.handle_server_message(hello);
        CHECK(app.reconnect_buffer().size() == 0);
    }

    SUBCASE("Handle PassthroughEvent (OSC 52 and Bell)") {
        std::string raw_osc = "\x1b]52;c;dGVzdA==\x07";
        MsgPassthroughEvent p_ev{
            .pane_id = 1,
            .kind = 1,
            .payload = std::vector<uint8_t>(raw_osc.begin(), raw_osc.end())
        };
        auto r1 = app.handle_server_message(p_ev);
        CHECK(r1.terminal_output == raw_osc);

        MsgPassthroughEvent bell_ev{.pane_id = 1, .kind = 2};
        auto r2 = app.handle_server_message(bell_ev);
        CHECK(r2.terminal_output == "\x07");
    }

    SUBCASE("Reconnect replay produces unacknowledged messages in order") {
        ClientKeyEvent k1{.key_code = 'x', .raw_bytes = {'x'}};
        ClientKeyEvent k2{.key_code = 'y', .raw_bytes = {'y'}};
        ClientKeyEvent k3{.key_code = 'z', .raw_bytes = {'z'}};
        app.process_input_event(k1);
        app.process_input_event(k2);
        app.process_input_event(k3);

        // Reconnect after seq 1 was acked
        auto replay = app.prepare_reconnect_replay(1);
        REQUIRE(replay.size() == 2);
        auto key_ev1 = std::get<MsgKeyEvent>(replay[0]);
        auto key_ev2 = std::get<MsgKeyEvent>(replay[1]);
        CHECK(key_ev1.client_seq == 2);
        CHECK(key_ev2.client_seq == 3);
    }

    SUBCASE("Zoom pane eliminates split borders during render") {
        std::vector<PaneRect> rects = {
            PaneRect{.pane_id = 1, .x = 0, .y = 0, .width = 39, .height = 23},
            PaneRect{.pane_id = 2, .x = 40, .y = 0, .width = 40, .height = 23}
        };
        app.renderer().set_pane_rects(rects);
        app.renderer().set_active_pane(1);
        app.renderer().set_zoomed_pane(1);

        app.renderer().apply_snapshot(MsgScreenSnapshot{
            .pane_id = 1, .cols = 80, .rows = 23,
            .cells = std::vector<Cell>(80 * 23, Cell{.codepoint = U'Z'})
        });

        std::string frame = app.render_frame(false);
        // Column 39 should NOT be split border '│' because pane 1 is zoomed
        CHECK(app.renderer().get_composite_cell(39, 10).codepoint == U'Z');
    }

    SUBCASE("Alt-screen / mouse reporting pane forwards wheel event instead of entering CopyMode") {
        std::vector<PaneRect> rects = {
            PaneRect{.pane_id = 1, .x = 0, .y = 0, .width = 80, .height = 23}
        };
        app.renderer().set_pane_rects(rects);
        app.renderer().set_active_pane(1);
        // Set mode bit 0 (alt-screen/mouse reporting)
        app.renderer().apply_snapshot(MsgScreenSnapshot{
            .pane_id = 1, .cols = 80, .rows = 23,
            .modes = 1,
            .cells = std::vector<Cell>(80 * 23, Cell{.codepoint = U' '})
        });

        ClientMouseEvent wheel_up{
            .kind = ClientMouseEvent::Kind::WheelUp,
            .button = ClientMouseEvent::Button::None,
            .col = 10,
            .row = 5
        };
        auto res = app.process_input_event(wheel_up);
        // Should NOT enter copy mode
        CHECK_FALSE(app.copy_mode().is_active());
        // Should have forwarded MsgMouseEvent to server
        REQUIRE(res.outbound_messages.size() == 1);
        auto m_ev = std::get<MsgMouseEvent>(res.outbound_messages[0]);
        CHECK(m_ev.pane_id == 1);
        CHECK(m_ev.kind == static_cast<uint8_t>(ClientMouseEvent::Kind::WheelUp));
    }
}

TEST_CASE("Client: InputParser - Fragmented Stream Chunk Parsing") {
    InputParser parser;
    // Feed CSI escape sequence "\x1b[1;5A" byte by byte
    const std::string seq = "\x1b[1;5A";
    std::vector<InputEvent> evs;
    for (size_t i = 0; i < seq.size(); ++i) {
        uint8_t b = static_cast<uint8_t>(seq[i]);
        auto chunk = parser.parse(std::span<const uint8_t>(&b, 1));
        evs.insert(evs.end(), chunk.begin(), chunk.end());
    }
    REQUIRE(evs.size() == 1);
    auto k = std::get<ClientKeyEvent>(evs[0]);
    CHECK(k.key_code == SpecialKey::Up);
    CHECK((k.modifiers & KeyModifiers::Ctrl) != 0);
}

TEST_CASE("Client: ReconnectBuffer - Capacity Overflow Eviction") {
    ReconnectBuffer buf(3); // Small capacity of 3
    uint64_t s1 = buf.next_seq();
    uint64_t s2 = buf.next_seq();
    uint64_t s3 = buf.next_seq();
    uint64_t s4 = buf.next_seq();

    buf.push(s1, MsgKeyEvent{.client_seq = s1});
    buf.push(s2, MsgKeyEvent{.client_seq = s2});
    buf.push(s3, MsgKeyEvent{.client_seq = s3});
    CHECK(buf.size() == 3);

    // Push 4th: should evict s1
    buf.push(s4, MsgKeyEvent{.client_seq = s4});
    CHECK(buf.size() == 3);

    auto unacked = buf.get_unacked(0);
    REQUIRE(unacked.size() == 3);
    CHECK(unacked[0].client_seq == s2);
    CHECK(unacked[1].client_seq == s3);
    CHECK(unacked[2].client_seq == s4);
}

TEST_CASE("Client: InputParser - Escape sequence edge cases and malformed input") {
    InputParser parser;

    // Alt+Enter (\x1b\r)
    {
        std::string seq = "\x1b\r";
        auto evs = parser.parse(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(seq.data()), seq.size()));
        REQUIRE(evs.size() == 1);
        auto k = std::get<ClientKeyEvent>(evs[0]);
        CHECK(k.key_code == SpecialKey::Enter);
        CHECK((k.modifiers & KeyModifiers::Alt) != 0);
    }

    // Alt+Tab (\x1b\t)
    {
        std::string seq = "\x1b\t";
        auto evs = parser.parse(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(seq.data()), seq.size()));
        REQUIRE(evs.size() == 1);
        auto k = std::get<ClientKeyEvent>(evs[0]);
        CHECK(k.key_code == SpecialKey::Tab);
        CHECK((k.modifiers & KeyModifiers::Alt) != 0);
    }

    // Alt+Backspace (\x1b\x7f)
    {
        std::string seq = "\x1b\x7f";
        auto evs = parser.parse(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(seq.data()), seq.size()));
        REQUIRE(evs.size() == 1);
        auto k = std::get<ClientKeyEvent>(evs[0]);
        CHECK(k.key_code == SpecialKey::Backspace);
        CHECK((k.modifiers & KeyModifiers::Alt) != 0);
    }

    // Double ESC (\x1b\x1b) followed by 'x' -> ESC then Alt+x
    {
        std::string seq = "\x1b\x1b""x";
        auto evs = parser.parse(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(seq.data()), seq.size()));
        REQUIRE(evs.size() == 2);
        auto k1 = std::get<ClientKeyEvent>(evs[0]);
        CHECK(k1.key_code == SpecialKey::Escape);
        auto k2 = std::get<ClientKeyEvent>(evs[1]);
        CHECK(k2.key_code == 'x');
        CHECK((k2.modifiers & KeyModifiers::Alt) != 0);
    }

    // Malformed SGR mouse input: must consume malformed chunk without hanging parser
    {
        std::string seq = "\x1b[<;;;Mhello";
        auto evs = parser.parse(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(seq.data()), seq.size()));
        // "hello" should be parsed successfully as 5 key events
        REQUIRE(evs.size() == 5);
        CHECK(std::get<ClientKeyEvent>(evs[0]).key_code == 'h');
        CHECK(std::get<ClientKeyEvent>(evs[4]).key_code == 'o');

        // Unknown sequence with non-digits
        std::string seq2 = "\x1b[<xyz" "ok";
        auto evs2 = parser.parse(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(seq2.data()), seq2.size()));
        // Discards \x1b[<, then parses "xyzok" without hanging
        CHECK_FALSE(evs2.empty());
    }
}

TEST_CASE("Client: Renderer - Border intersection '+' and codepoint 0 handling") {
    Renderer r(80, 24);
    // 2x2 grid of panes:
    // Pane 1: (0, 0, 39, 11), Pane 2: (40, 0, 40, 11)
    // Pane 3: (0, 12, 39, 11), Pane 4: (40, 12, 40, 11)
    // Vertical border at x=39, Horizontal border at y=11
    // Intersection at (39, 11)
    std::vector<PaneRect> rects = {
        PaneRect{.pane_id = 1, .x = 0, .y = 0, .width = 39, .height = 11},
        PaneRect{.pane_id = 2, .x = 40, .y = 0, .width = 40, .height = 11},
        PaneRect{.pane_id = 3, .x = 0, .y = 12, .width = 39, .height = 11},
        PaneRect{.pane_id = 4, .x = 40, .y = 12, .width = 40, .height = 11}
    };
    r.set_pane_rects(rects);
    r.compose_frame();

    // Check intersection cell at (39, 11)
    const auto& cell_inter = r.get_composite_cell(39, 11);
    CHECK(cell_inter.codepoint == U'┼');

    // Check row text conversion on empty cells: codepoint 0 must output ' ', not '\0'
    // Row 0 has 79 ASCII spaces + 1 3-byte UTF-8 border '│', giving 82 bytes
    std::string row_text = r.get_row_text(0);
    CHECK(row_text.size() == 82);
    CHECK(row_text.find('\0') == std::string::npos);
}

TEST_CASE("Client: CopyMode - Backward search '?', line selection 'V', and whitespace trimming") {
    CopyMode cm;
    cm.enter(1, 40, 5, 5);

    // Populate lines:
    // Line 0: "first match string"
    // Line 1: "middle line"
    // Line 2: "second match string"
    // Line 3: "another line"
    // Line 4: "third match string"
    auto make_line = [](std::string_view s) {
        ScrollbackLine l;
        for (char ch : s) {
            l.cells.push_back(Cell{.codepoint = static_cast<char32_t>(ch), .width = 1});
        }
        return l;
    };

    cm.add_local_history_lines(0, std::vector<ScrollbackLine>{
        make_line("first match string    "),
        make_line("middle line           "),
        make_line("second match string   "),
        make_line("another line          "),
        make_line("third match string    ")
    });

    // Start cursor at line 4
    // Backward search with '?'
    cm.handle_key(ClientKeyEvent{.key_code = '?'});
    for (char c : std::string("match")) {
        cm.handle_key(ClientKeyEvent{.key_code = static_cast<uint32_t>(c)});
    }
    cm.handle_key(ClientKeyEvent{.key_code = SpecialKey::Enter});

    // Should have moved cursor backward to line 2
    CHECK(cm.cursor().line == 2);

    // Next match in same backward direction with 'n'
    cm.handle_key(ClientKeyEvent{.key_code = 'n'});
    CHECK(cm.cursor().line == 0);

    // Reverse search direction with 'N' (forward to line 2)
    cm.handle_key(ClientKeyEvent{.key_code = 'N'});
    CHECK(cm.cursor().line == 2);

    // Visual line selection 'V'
    cm.handle_key(ClientKeyEvent{.key_code = 'V'});
    CHECK(cm.is_selecting());
    CHECK(cm.is_line_selecting());

    // Move cursor down to line 3 with 'j'
    cm.handle_key(ClientKeyEvent{.key_code = 'j'});
    CHECK(cm.cursor().line == 3);

    // Yank selected lines
    std::string text = cm.get_selected_text();
    // Should have 2 lines, trimmed of trailing spaces, separated by newline
    CHECK(text == "second match string\nanother line");
}

TEST_CASE("Client: SessionChooser - Scrolling when sessions exceed box height") {
    SessionChooser chooser;
    std::vector<SessionSummary> sessions;
    for (int i = 0; i < 30; ++i) {
        sessions.push_back(SessionSummary{
            .session_id = static_cast<uint32_t>(i + 1),
            .name = std::format("session-{}", i),
            .window_count = 1,
            .attached_clients = 0,
            .preview_summary = "test"
        });
    }

    chooser.open(sessions);
    // Move down 25 times
    for (int i = 0; i < 25; ++i) {
        chooser.handle_key(ClientKeyEvent{.key_code = SpecialKey::Down});
    }
    CHECK(chooser.selected_index() == 25);

    // Render on 80x24 screen
    std::vector<Cell> screen(80 * 24, Cell{.codepoint = U' '});
    chooser.render_overlay(screen, 80, 24);

    // Verify session-25 text is rendered in the screen cells
    bool found_session_25 = false;
    for (const auto& cell : screen) {
        if (cell.codepoint == U'2' && (&cell + 1 < &screen.back()) && (&cell)[1].codepoint == U'5') {
            found_session_25 = true;
            break;
        }
    }
    CHECK(found_session_25);
}

TEST_CASE("Client: ClientApp::render_frame - Overlays rendered into VT output") {
    ClientApp app(80, 24);
    std::vector<PaneRect> rects = {
        PaneRect{.pane_id = 1, .x = 0, .y = 0, .width = 80, .height = 23}
    };
    app.renderer().set_pane_rects(rects);
    app.renderer().set_active_pane(1);

    // 1. Session Chooser overlay in render_frame
    app.session_chooser().open({
        SessionSummary{.session_id = 1, .name = "mysession", .window_count = 1, .attached_clients = 1}
    });
    CHECK(app.session_chooser().is_active());

    std::string vt_chooser = app.render_frame(false);
    CHECK(vt_chooser.find("Sessions") != std::string::npos);
    CHECK(vt_chooser.find("mysession") != std::string::npos);

    app.session_chooser().close();
    CHECK_FALSE(app.session_chooser().is_active());

    // 2. Copy Mode overlay in render_frame
    app.copy_mode().enter(1, 80, 23, 10);
    CHECK(app.copy_mode().is_active());

    std::string vt_copymode = app.render_frame(false);
    CHECK(vt_copymode.find("Copy Mode:") != std::string::npos);
}
