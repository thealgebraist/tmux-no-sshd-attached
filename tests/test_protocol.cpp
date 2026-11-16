#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "src/protocol/protocol.hpp"

#include <vector>
#include <string>
#include <string_view>
#include <cstring>

using namespace tmux2::protocol;

TEST_CASE("Codec: BinaryReader & BinaryWriter Primitives & Endianness") {
    BinaryWriter writer;
    writer.write_u8(0x42);
    writer.write_u16(0x1234);
    writer.write_u32(0x89ABCDEF);
    writer.write_u64(0x0123456789ABCDEFULL);
    writer.write_i8(-42);
    writer.write_i16(-1234);
    writer.write_i32(-12345678);
    writer.write_i64(-9876543210LL);
    writer.write_bool(true);
    writer.write_bool(false);

    // Verify explicit little-endian bytes in the buffer
    auto bytes = writer.byte_span();
    // u16: 0x1234 -> 0x34, 0x12
    CHECK(bytes[1] == 0x34);
    CHECK(bytes[2] == 0x12);
    // u32: 0x89ABCDEF -> 0xEF, 0xCD, 0xAB, 0x89
    CHECK(bytes[3] == 0xEF);
    CHECK(bytes[4] == 0xCD);
    CHECK(bytes[5] == 0xAB);
    CHECK(bytes[6] == 0x89);

    BinaryReader reader(writer.span());
    auto u8_val = reader.read_u8();
    REQUIRE(u8_val.has_value());
    CHECK(*u8_val == 0x42);

    auto u16_val = reader.read_u16();
    REQUIRE(u16_val.has_value());
    CHECK(*u16_val == 0x1234);

    auto u32_val = reader.read_u32();
    REQUIRE(u32_val.has_value());
    CHECK(*u32_val == 0x89ABCDEF);

    auto u64_val = reader.read_u64();
    REQUIRE(u64_val.has_value());
    CHECK(*u64_val == 0x0123456789ABCDEFULL);

    auto i8_val = reader.read_i8();
    REQUIRE(i8_val.has_value());
    CHECK(*i8_val == -42);

    auto i16_val = reader.read_i16();
    REQUIRE(i16_val.has_value());
    CHECK(*i16_val == -1234);

    auto i32_val = reader.read_i32();
    REQUIRE(i32_val.has_value());
    CHECK(*i32_val == -12345678);

    auto i64_val = reader.read_i64();
    REQUIRE(i64_val.has_value());
    CHECK(*i64_val == -9876543210LL);

    auto b1 = reader.read_bool();
    REQUIRE(b1.has_value());
    CHECK(*b1 == true);

    auto b2 = reader.read_bool();
    REQUIRE(b2.has_value());
    CHECK(*b2 == false);

    CHECK(reader.empty());
    CHECK(!reader.read_u8().has_value());
    CHECK(reader.read_u8().error() == CodecError::Truncated);
}

TEST_CASE("Codec: Strings, Byte Vectors & Zero-Copy StringViews") {
    BinaryWriter writer;
    std::string str1 = "Hello, tmux2 over mTLS!";
    std::string str2 = ""; // empty string
    std::string str3 = "Special UTF-8: 🚀 \u00A9 \u4E16\u754C";
    std::vector<uint8_t> blob = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF};

    writer.write_string(str1);
    writer.write_string(str2);
    writer.write_string(str3);
    writer.write_byte_vector(blob);

    BinaryReader reader(writer.span());

    // Zero-copy string view test
    auto sv1 = reader.read_string_view();
    REQUIRE(sv1.has_value());
    CHECK(*sv1 == str1);

    auto s2 = reader.read_string();
    REQUIRE(s2.has_value());
    CHECK(*s2 == str2);

    auto s3 = reader.read_string();
    REQUIRE(s3.has_value());
    CHECK(*s3 == str3);

    auto b_res = reader.read_byte_vector();
    REQUIRE(b_res.has_value());
    CHECK(*b_res == blob);

    CHECK(reader.empty());
}

TEST_CASE("Codec: Varint Encoding and Truncation / Overflow Handling") {
    BinaryWriter writer;
    std::vector<uint64_t> test_vals = {
        0, 1, 127, 128, 255, 300, 16384, 1000000, 0xFFFFFFFFFFFFFFFFULL
    };

    for (uint64_t val : test_vals) {
        writer.write_varint(val);
    }

    BinaryReader reader(writer.span());
    for (uint64_t expected : test_vals) {
        auto res = reader.read_varint();
        REQUIRE(res.has_value());
        CHECK(*res == expected);
    }
    CHECK(reader.empty());

    // Truncated varint
    std::array<std::byte, 2> truncated_varint{std::byte{0x80}, std::byte{0x80}};
    BinaryReader trunc_reader(truncated_varint);
    CHECK(trunc_reader.read_varint().error() == CodecError::Truncated);

    // Overflow varint (11 bytes of 0xFF)
    std::vector<std::byte> overflow_varint(11, std::byte{0xFF});
    BinaryReader ovf_reader(overflow_varint);
    CHECK(ovf_reader.read_varint().error() == CodecError::Overflow);
}

TEST_CASE("Codec: Patching in place") {
    BinaryWriter writer;
    writer.write_u32(0); // placeholder for payload len
    writer.write_string("payload");
    uint32_t payload_len = static_cast<uint32_t>(writer.size() - 4);
    writer.write_u32_at(0, payload_len);

    BinaryReader reader(writer.span());
    auto len = reader.read_u32();
    REQUIRE(len.has_value());
    CHECK(*len == payload_len);
    auto s = reader.read_string();
    REQUIRE(s.has_value());
    CHECK(*s == "payload");
}

TEST_CASE("Frame: 8-Byte Stream Framing Header") {
    StreamHeader hdr{
        .stream_id = 42,
        .msg_type = static_cast<uint8_t>(MsgType::KeyEvent),
        .flags = kFlagZstdCompressed | kFlagPriorityUrgent,
        .payload_len = 1024,
    };

    CHECK(hdr.is_compressed());
    CHECK(hdr.is_urgent());
    CHECK(!hdr.is_fin());

    auto encoded = encode_header(hdr);
    CHECK(encoded.size() == 8);

    // Verify raw wire layout:
    // stream_id (42 le) -> 42, 0
    CHECK(static_cast<uint8_t>(encoded[0]) == 42);
    CHECK(static_cast<uint8_t>(encoded[1]) == 0);
    // msg_type (0x20)
    CHECK(static_cast<uint8_t>(encoded[2]) == static_cast<uint8_t>(MsgType::KeyEvent));
    // flags (0x01 | 0x04 = 0x05)
    CHECK(static_cast<uint8_t>(encoded[3]) == 0x05);
    // payload_len (1024 = 0x0400 le) -> 0x00, 0x04, 0x00, 0x00
    CHECK(static_cast<uint8_t>(encoded[4]) == 0x00);
    CHECK(static_cast<uint8_t>(encoded[5]) == 0x04);
    CHECK(static_cast<uint8_t>(encoded[6]) == 0x00);
    CHECK(static_cast<uint8_t>(encoded[7]) == 0x00);

    auto decoded_res = decode_header(encoded);
    REQUIRE(decoded_res.has_value());
    CHECK(*decoded_res == hdr);

    // Truncated header (< 8 bytes)
    CHECK(decode_header(std::span<const std::byte>(encoded.data(), 7)).error() == CodecError::Truncated);

    // Partial buffer helper try_decode_header
    auto try_res1 = try_decode_header(std::span<const std::byte>(encoded.data(), 5));
    REQUIRE(try_res1.has_value());
    CHECK(!try_res1->has_value()); // Not ready yet

    auto try_res2 = try_decode_header(encoded);
    REQUIRE(try_res2.has_value());
    REQUIRE(try_res2->has_value());
    CHECK(**try_res2 == hdr);
}

TEST_CASE("Cell & RLE: Run-Length Encoding and Style Attributes") {
    Cell default_space{
        .codepoint = U' ',
        .width = 1,
        .style = CellStyle{
            .flags = StyleFlags::None,
            .fg = Color::default_color(),
            .bg = Color::default_color(),
        }
    };

    Cell styled_x{
        .codepoint = U'X',
        .width = 1,
        .style = CellStyle{
            .flags = StyleFlags::Bold | StyleFlags::Underline,
            .fg = Color::rgb(255, 128, 0),
            .bg = Color::indexed(235),
        }
    };

    Cell wide_kanji{
        .codepoint = U'漢',
        .width = 2,
        .style = CellStyle{
            .flags = StyleFlags::Italic,
            .fg = Color::indexed(12),
            .bg = Color::default_color(),
        }
    };

    // Construct a simulated terminal row: 10 spaces, 20 styled_x, 2 wide_kanji, 48 spaces (total 80 cells)
    std::vector<Cell> row;
    row.insert(row.end(), 10, default_space);
    row.insert(row.end(), 20, styled_x);
    row.insert(row.end(), 2, wide_kanji);
    row.insert(row.end(), 48, default_space);
    CHECK(row.size() == 80);

    BinaryWriter writer;
    encode_cell_runs(writer, row);

    // RLE compaction verification: 80 cells encoded as 4 runs
    BinaryReader reader(writer.span());
    auto decoded_cells_res = decode_cell_runs(reader, 80);
    REQUIRE(decoded_cells_res.has_value());
    CHECK(decoded_cells_res->size() == 80);
    CHECK(*decoded_cells_res == row);

    // Corrupted expected count should return InvalidData
    BinaryReader reader_mismatch(writer.span());
    auto mismatch_res = decode_cell_runs(reader_mismatch, 79);
    CHECK(!mismatch_res.has_value());
    CHECK(mismatch_res.error() == CodecError::InvalidData);
}

TEST_CASE("Zstd: Compression & Decompression Roundtrip") {
    ZstdCompressor compressor(3);
    ZstdDecompressor decompressor;

    std::string sample = "tmux2 high performance terminal multiplexer over TLS 1.3 mTLS! ";
    for (int i = 0; i < 100; ++i) {
        sample += "Line " + std::to_string(i) + ": repeating cell and scrollback payload data.\n";
    }

    std::span<const std::byte> sample_bytes(
        reinterpret_cast<const std::byte*>(sample.data()), sample.size());

    auto comp_res = compressor.compress(sample_bytes);
    REQUIRE(comp_res.has_value());
    CHECK(comp_res->size() < sample.size()); // Expect significant compression

    auto decomp_res = decompressor.decompress(*comp_res);
    REQUIRE(decomp_res.has_value());
    CHECK(decomp_res->size() == sample.size());
    CHECK(std::memcmp(decomp_res->data(), sample.data(), sample.size()) == 0);

    // Decompress corrupted data
    std::vector<std::byte> corrupt_data = *comp_res;
    for (size_t i = 10; i < std::min<size_t>(corrupt_data.size(), 30); ++i) {
        corrupt_data[i] = std::byte{0xFF};
    }
    auto corrupt_res = decompressor.decompress(corrupt_data);
    CHECK(!corrupt_res.has_value());
    CHECK(corrupt_res.error() == CodecError::DecompressionError);
}

TEST_CASE("Messages: Control RPCs Serialization & Deserialization") {
    // 1. ClientHello
    MsgClientHello client_hello{
        .protocol_version = 1,
        .client_instance_id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
        .term_caps = 0x0F,
        .resume_token = "hmac-token-secret-xyz",
        .last_srv_seq = 42,
        .target = "dev:main",
    };

    BinaryWriter w_ch;
    encode_message_payload(client_hello, w_ch);
    BinaryReader r_ch(w_ch.span());
    auto dec_ch = decode_message_payload(MsgType::ClientHello, r_ch);
    REQUIRE(dec_ch.has_value());
    CHECK(std::get<MsgClientHello>(*dec_ch) == client_hello);

    // 2. ServerHello
    MsgServerHello server_hello{
        .server_version = 1,
        .assigned_client_id = 9999,
        .resume_token = "new-resume-token-abc",
        .last_client_seq_acked = 105,
        .session_list = {
            SessionSummary{
                .session_id = 1,
                .name = "work",
                .created_at = 1700000000,
                .window_count = 3,
                .attached_clients = 1,
                .preview_summary = "nvim (3 panes)",
            },
            SessionSummary{
                .session_id = 2,
                .name = "build",
                .created_at = 1700001000,
                .window_count = 1,
                .attached_clients = 0,
                .preview_summary = "cargo test",
            }
        }
    };

    BinaryWriter w_sh;
    encode_message_payload(server_hello, w_sh);
    BinaryReader r_sh(w_sh.span());
    auto dec_sh = decode_message_payload(MsgType::ServerHello, r_sh);
    REQUIRE(dec_sh.has_value());
    CHECK(std::get<MsgServerHello>(*dec_sh) == server_hello);

    // 3. NewSessionReq & Resp
    MsgNewSessionReq new_sess{
        .name = "dev",
        .initial_window_name = "editor",
        .cwd = "/home/agent1/project",
        .command = "zsh",
        .env = {{"TERM", "xterm-256color"}, {"COLORTERM", "truecolor"}},
        .detached = false,
    };
    BinaryWriter w_ns;
    encode_message_payload(new_sess, w_ns);
    BinaryReader r_ns(w_ns.span());
    auto dec_ns = decode_message_payload(MsgType::NewSessionReq, r_ns);
    REQUIRE(dec_ns.has_value());
    CHECK(std::get<MsgNewSessionReq>(*dec_ns) == new_sess);

    MsgNewSessionResp new_sess_resp{
        .success = true,
        .session_id = 10,
        .window_id = 20,
        .pane_id = 30,
        .error_msg = "",
    };
    BinaryWriter w_nsr;
    encode_message_payload(new_sess_resp, w_nsr);
    BinaryReader r_nsr(w_nsr.span());
    auto dec_nsr = decode_message_payload(MsgType::NewSessionResp, r_nsr);
    REQUIRE(dec_nsr.has_value());
    CHECK(std::get<MsgNewSessionResp>(*dec_nsr) == new_sess_resp);

    // 4. AttachSessionReq & Resp
    MsgAttachSessionReq attach_req{
        .session_id = 10,
        .session_name = "dev",
        .read_only = false,
        .cols = 120,
        .rows = 40,
    };
    BinaryWriter w_att;
    encode_message_payload(attach_req, w_att);
    BinaryReader r_att(w_att.span());
    auto dec_att = decode_message_payload(MsgType::AttachSessionReq, r_att);
    REQUIRE(dec_att.has_value());
    CHECK(std::get<MsgAttachSessionReq>(*dec_att) == attach_req);

    // 5. CommandRpcReq & Resp
    MsgCommandRpcReq rpc_req{
        .rpc_id = 12345,
        .command_dsl = "split-window -h -c /tmp",
    };
    BinaryWriter w_rpc;
    encode_message_payload(rpc_req, w_rpc);
    BinaryReader r_rpc(w_rpc.span());
    auto dec_rpc = decode_message_payload(MsgType::CommandRpcReq, r_rpc);
    REQUIRE(dec_rpc.has_value());
    CHECK(std::get<MsgCommandRpcReq>(*dec_rpc) == rpc_req);
}

TEST_CASE("Messages: Interactive Input Events Serialization") {
    // KeyEvent
    MsgKeyEvent key_ev{
        .client_seq = 101,
        .target_pane = 2,
        .key_code = 0x1B, // ESC
        .modifiers = 0x01,
        .raw_bytes = {0x1B, '[', 'A'}, // Up arrow
    };
    BinaryWriter w_key;
    encode_message_payload(key_ev, w_key);
    BinaryReader r_key(w_key.span());
    auto dec_key = decode_message_payload(MsgType::KeyEvent, r_key);
    REQUIRE(dec_key.has_value());
    CHECK(std::get<MsgKeyEvent>(*dec_key) == key_ev);

    // MouseEvent
    MsgMouseEvent mouse_ev{
        .client_seq = 102,
        .pane_id = 1,
        .kind = 0,
        .button = 1,
        .col = 45,
        .row = 15,
        .modifiers = 0,
    };
    BinaryWriter w_mouse;
    encode_message_payload(mouse_ev, w_mouse);
    BinaryReader r_mouse(w_mouse.span());
    auto dec_mouse = decode_message_payload(MsgType::MouseEvent, r_mouse);
    REQUIRE(dec_mouse.has_value());
    CHECK(std::get<MsgMouseEvent>(*dec_mouse) == mouse_ev);

    // PasteEvent
    MsgPasteEvent paste_ev{
        .client_seq = 103,
        .pane_id = 1,
        .bracketed = true,
        .data = {'g', 'i', 't', ' ', 's', 't', 'a', 't', 'u', 's', '\n'},
    };
    BinaryWriter w_paste;
    encode_message_payload(paste_ev, w_paste);
    BinaryReader r_paste(w_paste.span());
    auto dec_paste = decode_message_payload(MsgType::PasteEvent, r_paste);
    REQUIRE(dec_paste.has_value());
    CHECK(std::get<MsgPasteEvent>(*dec_paste) == paste_ev);

    // ClientResize
    MsgClientResize resize_ev{
        .cols = 160,
        .rows = 50,
        .pixel_width = 1920,
        .pixel_height = 1080,
    };
    BinaryWriter w_res;
    encode_message_payload(resize_ev, w_res);
    BinaryReader r_res(w_res.span());
    auto dec_res = decode_message_payload(MsgType::ClientResize, r_res);
    REQUIRE(dec_res.has_value());
    CHECK(std::get<MsgClientResize>(*dec_res) == resize_ev);
}

TEST_CASE("Messages: ScreenSnapshot, FrameDiff & ScrollbackSlice with RLE and Libzstd") {
    // Build a 80x24 ScreenSnapshot
    uint16_t cols = 80;
    uint16_t rows = 24;
    std::vector<Cell> cells(cols * rows);

    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            size_t idx = r * cols + c;
            if (r == 0) {
                // Header line
                cells[idx] = Cell{
                    .codepoint = static_cast<char32_t>('A' + (c % 26)),
                    .width = 1,
                    .style = CellStyle{.flags = StyleFlags::Bold, .fg = Color::rgb(0, 200, 255), .bg = Color::default_color()}
                };
            } else {
                // Mostly empty cells with background
                cells[idx] = Cell{
                    .codepoint = U' ',
                    .width = 1,
                    .style = CellStyle{.flags = StyleFlags::None, .fg = Color::default_color(), .bg = Color::default_color()}
                };
            }
        }
    }

    MsgScreenSnapshot snapshot{
        .server_seq = 500,
        .pane_id = 1,
        .cols = cols,
        .rows = rows,
        .cursor = CursorState{.col = 15, .row = 5, .visible = true, .shape = 0},
        .modes = 0x01,
        .cells = cells,
    };

    // Encode into a framed packet with zstd compression enabled
    auto frame_bytes = encode_frame(snapshot, kRenderStreamId, /*compress=*/true, /*zstd_level=*/3);
    REQUIRE(frame_bytes.size() >= kStreamHeaderSize);

    // Check header
    auto hdr_res = decode_header(frame_bytes);
    REQUIRE(hdr_res.has_value());
    CHECK(hdr_res->stream_id == kRenderStreamId);
    CHECK(hdr_res->msg_type == static_cast<uint8_t>(MsgType::ScreenSnapshot));
    CHECK(hdr_res->is_compressed());

    // Decode full frame
    auto decoded_frame_res = decode_frame(frame_bytes);
    REQUIRE(decoded_frame_res.has_value());
    const auto& [hdr, msg] = *decoded_frame_res;
    CHECK(hdr == *hdr_res);

    REQUIRE(std::holds_alternative<MsgScreenSnapshot>(msg));
    const auto& dec_snapshot = std::get<MsgScreenSnapshot>(msg);
    CHECK(dec_snapshot == snapshot);

    // FrameDiff testing
    MsgFrameDiff frame_diff{
        .server_seq = 501,
        .acked_client_seq = 105,
        .status_line = "[0] 0:editor* 1:zsh- (120x40)",
        .pane_diffs = {
            PaneRowDiff{
                .pane_id = 1,
                .cursor = CursorState{.col = 20, .row = 5, .visible = true, .shape = 0},
                .rows = {
                    CellRowDiff{
                        .row = 5,
                        .start_col = 10,
                        .cells = {
                            Cell{.codepoint = U'l', .width = 1, .style = {}},
                            Cell{.codepoint = U's', .width = 1, .style = {}},
                        }
                    }
                }
            }
        }
    };

    auto diff_frame = encode_frame(frame_diff, kRenderStreamId, /*compress=*/true);
    auto dec_diff_res = decode_frame(diff_frame);
    REQUIRE(dec_diff_res.has_value());
    CHECK(std::get<MsgFrameDiff>(dec_diff_res->second) == frame_diff);

    // ScrollbackSlice testing
    MsgScrollbackSliceResp scrollback_resp{
        .pane_id = 1,
        .start_line = 1000,
        .total_available_lines = 50000,
        .lines = {
            ScrollbackLine{
                .wrapped = false,
                .cells = {
                    Cell{.codepoint = U'#', .width = 1, .style = {}},
                    Cell{.codepoint = U'!', .width = 1, .style = {}},
                    Cell{.codepoint = U'/', .width = 1, .style = {}},
                    Cell{.codepoint = U'b', .width = 1, .style = {}},
                    Cell{.codepoint = U'i', .width = 1, .style = {}},
                    Cell{.codepoint = U'n', .width = 1, .style = {}},
                    Cell{.codepoint = U'/', .width = 1, .style = {}},
                    Cell{.codepoint = U's', .width = 1, .style = {}},
                    Cell{.codepoint = U'h', .width = 1, .style = {}},
                }
            }
        }
    };

    auto sb_frame = encode_frame(scrollback_resp, kRenderStreamId, /*compress=*/true);
    auto dec_sb_res = decode_frame(sb_frame);
    REQUIRE(dec_sb_res.has_value());
    CHECK(std::get<MsgScrollbackSliceResp>(dec_sb_res->second) == scrollback_resp);
}

TEST_CASE("Messages: Heartbeats & Passthrough Events") {
    MsgHeartbeatPing ping{.seq = 777, .timestamp_ms = 1710000000123ULL};
    auto ping_frame = encode_frame(ping);
    auto dec_ping = decode_frame(ping_frame);
    REQUIRE(dec_ping.has_value());
    CHECK(std::get<MsgHeartbeatPing>(dec_ping->second) == ping);

    MsgHeartbeatPong pong{.seq = 777, .timestamp_ms = 1710000000125ULL};
    auto pong_frame = encode_frame(pong);
    auto dec_pong = decode_frame(pong_frame);
    REQUIRE(dec_pong.has_value());
    CHECK(std::get<MsgHeartbeatPong>(dec_pong->second) == pong);

    // OSC 52 Clipboard Passthrough
    MsgPassthroughEvent osc52{
        .pane_id = 1,
        .kind = 1, // Osc52Clipboard
        .payload = {'c', 'o', 'p', 'i', 'e', 'd', ' ', 't', 'e', 'x', 't'},
    };
    auto osc_frame = encode_frame(osc52);
    auto dec_osc = decode_frame(osc_frame);
    REQUIRE(dec_osc.has_value());
    CHECK(std::get<MsgPassthroughEvent>(dec_osc->second) == osc52);
}

TEST_CASE("Messages: Full Coverage of Remaining Message Types") {
    // ListSessionsReq & Resp
    MsgListSessionsReq list_req{.filter = "tmux*"};
    auto f_lreq = encode_frame(list_req);
    auto d_lreq = decode_frame(f_lreq);
    REQUIRE(d_lreq.has_value());
    CHECK(std::get<MsgListSessionsReq>(d_lreq->second) == list_req);

    MsgListSessionsResp list_resp{
        .sessions = {
            SessionSummary{
                .session_id = 42,
                .name = "tmux2-dev",
                .created_at = 1710000000,
                .window_count = 5,
                .attached_clients = 2,
                .preview_summary = "bash (active)",
            }
        }
    };
    auto f_lresp = encode_frame(list_resp);
    auto d_lresp = decode_frame(f_lresp);
    REQUIRE(d_lresp.has_value());
    CHECK(std::get<MsgListSessionsResp>(d_lresp->second) == list_resp);

    // SwitchSessionReq & Resp
    MsgSwitchSessionReq sw_req{.session_id = 5, .session_name = "prod"};
    auto f_swr = encode_frame(sw_req);
    auto d_swr = decode_frame(f_swr);
    REQUIRE(d_swr.has_value());
    CHECK(std::get<MsgSwitchSessionReq>(d_swr->second) == sw_req);

    MsgSwitchSessionResp sw_resp{.success = true, .session_id = 5, .window_id = 1, .error_msg = ""};
    auto f_swresp = encode_frame(sw_resp);
    auto d_swresp = decode_frame(f_swresp);
    REQUIRE(d_swresp.has_value());
    CHECK(std::get<MsgSwitchSessionResp>(d_swresp->second) == sw_resp);

    // DetachReq & Notice
    MsgDetachReq det_req{.session_id = 5, .reason = "client disconnected"};
    auto f_det = encode_frame(det_req);
    auto d_det = decode_frame(f_det);
    REQUIRE(d_det.has_value());
    CHECK(std::get<MsgDetachReq>(d_det->second) == det_req);

    MsgDetachNotice det_not{.reason = "session killed by admin"};
    auto f_dnot = encode_frame(det_not);
    auto d_dnot = decode_frame(f_dnot);
    REQUIRE(d_dnot.has_value());
    CHECK(std::get<MsgDetachNotice>(d_dnot->second) == det_not);

    // KillSessionReq & Resp
    MsgKillSessionReq kill_req{.session_id = 99};
    auto f_kill = encode_frame(kill_req);
    auto d_kill = decode_frame(f_kill);
    REQUIRE(d_kill.has_value());
    CHECK(std::get<MsgKillSessionReq>(d_kill->second) == kill_req);

    MsgKillSessionResp kill_resp{.success = true, .error_msg = ""};
    auto f_kresp = encode_frame(kill_resp);
    auto d_kresp = decode_frame(f_kresp);
    REQUIRE(d_kresp.has_value());
    CHECK(std::get<MsgKillSessionResp>(d_kresp->second) == kill_resp);

    // LayoutUpdate
    MsgLayoutUpdate layout_up{
        .server_seq = 888,
        .session_id = 1,
        .window_id = 2,
        .layout_tree_json = R"({"type":"split_v","panes":[1,2]})",
        .active_pane_id = 2,
        .zoomed_pane_id = 0,
    };
    auto f_layout = encode_frame(layout_up);
    auto d_layout = decode_frame(f_layout);
    REQUIRE(d_layout.has_value());
    CHECK(std::get<MsgLayoutUpdate>(d_layout->second) == layout_up);

    // ScrollbackSliceReq
    MsgScrollbackSliceReq sb_req{.pane_id = 3, .start_line = 500, .line_count = 50};
    auto f_sbreq = encode_frame(sb_req);
    auto d_sbreq = decode_frame(f_sbreq);
    REQUIRE(d_sbreq.has_value());
    CHECK(std::get<MsgScrollbackSliceReq>(d_sbreq->second) == sb_req);

    // Passthrough events of all kinds
    for (uint8_t k = 1; k <= 4; ++k) {
        MsgPassthroughEvent pt{.pane_id = 1, .kind = k, .payload = {k, 0xAA, 0xBB}};
        auto f_pt = encode_frame(pt);
        auto d_pt = decode_frame(f_pt);
        REQUIRE(d_pt.has_value());
        CHECK(std::get<MsgPassthroughEvent>(d_pt->second) == pt);
    }
}

TEST_CASE("Streaming: Back-to-Back Frames in Continuous Stream Buffer") {
    // Write multiple frames into a single continuous stream
    std::vector<std::byte> stream_buffer;

    MsgKeyEvent k1{.client_seq = 1, .target_pane = 1, .key_code = 'a', .modifiers = 0, .raw_bytes = {'a'}};
    MsgKeyEvent k2{.client_seq = 2, .target_pane = 1, .key_code = 'b', .modifiers = 0, .raw_bytes = {'b'}};
    MsgKeyEvent k3{.client_seq = 3, .target_pane = 1, .key_code = 'c', .modifiers = 0, .raw_bytes = {'c'}};

    auto f1 = encode_frame(k1);
    auto f2 = encode_frame(k2);
    auto f3 = encode_frame(k3);

    stream_buffer.insert(stream_buffer.end(), f1.begin(), f1.end());
    stream_buffer.insert(stream_buffer.end(), f2.begin(), f2.end());
    stream_buffer.insert(stream_buffer.end(), f3.begin(), f3.end());

    // Stream decoding loop
    std::vector<ProtocolMessage> decoded_messages;
    size_t offset = 0;
    while (offset < stream_buffer.size()) {
        std::span<const std::byte> remaining(stream_buffer.data() + offset, stream_buffer.size() - offset);
        auto hdr_res = try_decode_header(remaining);
        REQUIRE(hdr_res.has_value());
        REQUIRE(hdr_res->has_value()); // Header is present

        StreamHeader hdr = **hdr_res;
        size_t total_frame_len = kStreamHeaderSize + hdr.payload_len;
        REQUIRE(remaining.size() >= total_frame_len);

        auto frame_res = decode_frame(remaining.subspan(0, total_frame_len));
        REQUIRE(frame_res.has_value());
        decoded_messages.push_back(frame_res->second);

        offset += total_frame_len;
    }

    REQUIRE(decoded_messages.size() == 3);
    CHECK(std::get<MsgKeyEvent>(decoded_messages[0]) == k1);
    CHECK(std::get<MsgKeyEvent>(decoded_messages[1]) == k2);
    CHECK(std::get<MsgKeyEvent>(decoded_messages[2]) == k3);
}

TEST_CASE("Robustness: Truncation at every byte boundary") {
    MsgScreenSnapshot snapshot{
        .server_seq = 123,
        .pane_id = 1,
        .cols = 10,
        .rows = 5,
        .cursor = CursorState{.col = 2, .row = 1, .visible = true, .shape = 0},
        .modes = 0,
        .cells = std::vector<Cell>(50, Cell{.codepoint = U'.', .width = 1, .style = {}}),
    };

    auto frame = encode_frame(snapshot, kRenderStreamId, /*compress=*/false);
    REQUIRE(frame.size() > kStreamHeaderSize);

    // Verify every prefix length < frame.size() safely fails without crash
    for (size_t len = 0; len < frame.size(); ++len) {
        auto res = decode_frame(std::span<const std::byte>(frame.data(), len));
        CHECK(!res.has_value());
        CHECK(res.error() == CodecError::Truncated);
    }
}

TEST_CASE("Robustness: Large 200x60 Grid RLE and Zstd Compaction") {
    // 200 columns x 60 rows = 12,000 cells
    uint16_t cols = 200;
    uint16_t rows = 60;
    std::vector<Cell> cells(cols * rows, Cell{.codepoint = U' ', .width = 1, .style = {}});

    // Populate a simulated shell prompt and command output
    std::string prompt = "user@workstation:~/tmux2$ ls -la";
    for (size_t i = 0; i < prompt.size(); ++i) {
        cells[i] = Cell{.codepoint = static_cast<char32_t>(prompt[i]), .width = 1, .style = {}};
    }

    MsgScreenSnapshot large_snap{
        .server_seq = 999,
        .pane_id = 1,
        .cols = cols,
        .rows = rows,
        .cursor = CursorState{.col = static_cast<uint16_t>(prompt.size()), .row = 0, .visible = true, .shape = 0},
        .modes = 0,
        .cells = cells,
    };

    auto uncompressed_frame = encode_frame(large_snap, kRenderStreamId, /*compress=*/false);
    auto compressed_frame = encode_frame(large_snap, kRenderStreamId, /*compress=*/true, /*zstd_level=*/3);

    // Raw uncompressed cells would be 12000 * 20 bytes ~ 240 KB
    // RLE alone reduces it dramatically; zstd reduces it even further
    CHECK(compressed_frame.size() < uncompressed_frame.size());
    CHECK(compressed_frame.size() < 1000); // Should easily be well under 1 KB!

    auto dec_snap = decode_frame(compressed_frame);
    REQUIRE(dec_snap.has_value());
    CHECK(std::get<MsgScreenSnapshot>(dec_snap->second) == large_snap);
}

TEST_CASE("Fuzzing / Corruption: Corrupted Payload Handling") {
    MsgScreenSnapshot snap{
        .server_seq = 42,
        .pane_id = 1,
        .cols = 20,
        .rows = 10,
        .cursor = CursorState{},
        .modes = 0,
        .cells = std::vector<Cell>(200, Cell{.codepoint = U'#', .width = 1, .style = {}}),
    };

    auto frame = encode_frame(snap, kRenderStreamId, /*compress=*/true);

    // Bit-flip fuzzing across payload
    for (size_t offset = kStreamHeaderSize; offset < frame.size(); offset += 3) {
        auto mutated = frame;
        mutated[offset] = static_cast<std::byte>(static_cast<uint8_t>(mutated[offset]) ^ 0xAA);

        // Must never crash or segfault; should return either success (if uncritical padding) or CodecError
        auto res = decode_frame(mutated);
        if (!res.has_value()) {
            CHECK((res.error() == CodecError::DecompressionError ||
                   res.error() == CodecError::InvalidData ||
                   res.error() == CodecError::Truncated ||
                   res.error() == CodecError::VectorTooLong));
        }
    }
}

