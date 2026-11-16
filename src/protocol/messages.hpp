#pragma once

#include "src/protocol/cell.hpp"
#include "src/protocol/codec.hpp"
#include "src/protocol/compression.hpp"
#include "src/protocol/frame.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tmux2::protocol {

enum class MsgType : uint8_t {
    // Control RPCs (0x01 - 0x1F)
    ClientHello        = 0x01,
    ServerHello        = 0x02,
    ListSessionsReq    = 0x03,
    ListSessionsResp   = 0x04,
    NewSessionReq      = 0x05,
    NewSessionResp     = 0x06,
    AttachSessionReq   = 0x07,
    AttachSessionResp  = 0x08,
    SwitchSessionReq   = 0x09,
    SwitchSessionResp  = 0x0A,
    DetachReq          = 0x0B,
    DetachNotice       = 0x0C,
    KillSessionReq     = 0x0D,
    KillSessionResp    = 0x0E,
    CommandRpcReq      = 0x0F,
    CommandRpcResp     = 0x10,

    // Interactive Input & Terminal Events (0x20 - 0x2F)
    KeyEvent           = 0x20,
    MouseEvent         = 0x21,
    PasteEvent         = 0x22,
    ClientResize       = 0x23,

    // Rendering, Diff & Scrollback (0x30 - 0x3F)
    LayoutUpdate       = 0x30,
    ScreenSnapshot     = 0x31,
    FrameDiff          = 0x32,
    ScrollbackSliceReq = 0x33,
    ScrollbackSliceResp= 0x34,
    PassthroughEvent   = 0x35,

    // Heartbeat (0x40 - 0x4F)
    HeartbeatPing      = 0x40,
    HeartbeatPong      = 0x41,
};

constexpr std::string_view msg_type_to_string(MsgType type) noexcept {
    switch (type) {
        case MsgType::ClientHello: return "ClientHello";
        case MsgType::ServerHello: return "ServerHello";
        case MsgType::ListSessionsReq: return "ListSessionsReq";
        case MsgType::ListSessionsResp: return "ListSessionsResp";
        case MsgType::NewSessionReq: return "NewSessionReq";
        case MsgType::NewSessionResp: return "NewSessionResp";
        case MsgType::AttachSessionReq: return "AttachSessionReq";
        case MsgType::AttachSessionResp: return "AttachSessionResp";
        case MsgType::SwitchSessionReq: return "SwitchSessionReq";
        case MsgType::SwitchSessionResp: return "SwitchSessionResp";
        case MsgType::DetachReq: return "DetachReq";
        case MsgType::DetachNotice: return "DetachNotice";
        case MsgType::KillSessionReq: return "KillSessionReq";
        case MsgType::KillSessionResp: return "KillSessionResp";
        case MsgType::CommandRpcReq: return "CommandRpcReq";
        case MsgType::CommandRpcResp: return "CommandRpcResp";
        case MsgType::KeyEvent: return "KeyEvent";
        case MsgType::MouseEvent: return "MouseEvent";
        case MsgType::PasteEvent: return "PasteEvent";
        case MsgType::ClientResize: return "ClientResize";
        case MsgType::LayoutUpdate: return "LayoutUpdate";
        case MsgType::ScreenSnapshot: return "ScreenSnapshot";
        case MsgType::FrameDiff: return "FrameDiff";
        case MsgType::ScrollbackSliceReq: return "ScrollbackSliceReq";
        case MsgType::ScrollbackSliceResp: return "ScrollbackSliceResp";
        case MsgType::PassthroughEvent: return "PassthroughEvent";
        case MsgType::HeartbeatPing: return "HeartbeatPing";
        case MsgType::HeartbeatPong: return "HeartbeatPong";
    }
    return "UnknownMsgType";
}

struct CursorState {
    uint16_t col{0};
    uint16_t row{0};
    bool visible{true};
    uint8_t shape{0}; // 0 = Default/Block, 1 = Underline, 2 = Bar

    constexpr bool operator==(const CursorState&) const = default;
};

struct SessionSummary {
    uint32_t session_id{0};
    std::string name;
    uint64_t created_at{0};
    uint32_t window_count{0};
    uint32_t attached_clients{0};
    std::string preview_summary;

    bool operator==(const SessionSummary&) const = default;
};

// Protocol Message Definitions

struct MsgClientHello {
    uint32_t protocol_version{1};
    std::array<uint8_t, 16> client_instance_id{};
    uint32_t term_caps{0}; // bit 0: truecolor, bit 1: kitty_kbd, bit 2: sgr_mouse, bit 3: osc52
    std::string resume_token;
    uint64_t last_srv_seq{0};
    std::string target;

    bool operator==(const MsgClientHello&) const = default;
};

struct MsgServerHello {
    uint32_t server_version{1};
    uint64_t assigned_client_id{0};
    std::string resume_token;
    uint64_t last_client_seq_acked{0};
    std::vector<SessionSummary> session_list;

    bool operator==(const MsgServerHello&) const = default;
};

struct MsgListSessionsReq {
    std::string filter;

    bool operator==(const MsgListSessionsReq&) const = default;
};

struct MsgListSessionsResp {
    std::vector<SessionSummary> sessions;

    bool operator==(const MsgListSessionsResp&) const = default;
};

struct MsgNewSessionReq {
    std::string name;
    std::string initial_window_name;
    std::string cwd;
    std::string command;
    std::vector<std::pair<std::string, std::string>> env;
    bool detached{false};

    bool operator==(const MsgNewSessionReq&) const = default;
};

struct MsgNewSessionResp {
    bool success{false};
    uint32_t session_id{0};
    uint32_t window_id{0};
    uint32_t pane_id{0};
    std::string error_msg;

    bool operator==(const MsgNewSessionResp&) const = default;
};

struct MsgAttachSessionReq {
    uint32_t session_id{0};
    std::string session_name;
    bool read_only{false};
    uint16_t cols{80};
    uint16_t rows{24};

    bool operator==(const MsgAttachSessionReq&) const = default;
};

struct MsgAttachSessionResp {
    bool success{false};
    uint32_t session_id{0};
    uint32_t window_id{0};
    std::string layout_tree_json;
    std::string error_msg;

    bool operator==(const MsgAttachSessionResp&) const = default;
};

struct MsgSwitchSessionReq {
    uint32_t session_id{0};
    std::string session_name;

    bool operator==(const MsgSwitchSessionReq&) const = default;
};

struct MsgSwitchSessionResp {
    bool success{false};
    uint32_t session_id{0};
    uint32_t window_id{0};
    std::string error_msg;

    bool operator==(const MsgSwitchSessionResp&) const = default;
};

struct MsgDetachReq {
    uint32_t session_id{0};
    std::string reason;

    bool operator==(const MsgDetachReq&) const = default;
};

struct MsgDetachNotice {
    std::string reason;

    bool operator==(const MsgDetachNotice&) const = default;
};

struct MsgKillSessionReq {
    uint32_t session_id{0};

    bool operator==(const MsgKillSessionReq&) const = default;
};

struct MsgKillSessionResp {
    bool success{false};
    std::string error_msg;

    bool operator==(const MsgKillSessionResp&) const = default;
};

struct MsgCommandRpcReq {
    uint64_t rpc_id{0};
    std::string command_dsl;

    bool operator==(const MsgCommandRpcReq&) const = default;
};

struct MsgCommandRpcResp {
    uint64_t rpc_id{0};
    int32_t exit_code{0};
    std::string stdout_data;
    std::string stderr_data;

    bool operator==(const MsgCommandRpcResp&) const = default;
};

struct MsgKeyEvent {
    uint64_t client_seq{0};
    uint32_t target_pane{0};
    uint32_t key_code{0};
    uint32_t modifiers{0};
    std::vector<uint8_t> raw_bytes;

    bool operator==(const MsgKeyEvent&) const = default;
};

struct MsgMouseEvent {
    uint64_t client_seq{0};
    uint32_t pane_id{0};
    uint8_t kind{0};   // 0 = press, 1 = release, 2 = move, 3 = wheel_up, 4 = wheel_down
    uint8_t button{0}; // 0 = none, 1 = left, 2 = middle, 3 = right
    uint16_t col{0};
    uint16_t row{0};
    uint32_t modifiers{0};

    constexpr bool operator==(const MsgMouseEvent&) const = default;
};

struct MsgPasteEvent {
    uint64_t client_seq{0};
    uint32_t pane_id{0};
    bool bracketed{true};
    std::vector<uint8_t> data;

    bool operator==(const MsgPasteEvent&) const = default;
};

struct MsgClientResize {
    uint16_t cols{0};
    uint16_t rows{0};
    uint32_t pixel_width{0};
    uint32_t pixel_height{0};

    constexpr bool operator==(const MsgClientResize&) const = default;
};

struct MsgLayoutUpdate {
    uint64_t server_seq{0};
    uint32_t session_id{0};
    uint32_t window_id{0};
    std::string layout_tree_json;
    uint32_t active_pane_id{0};
    uint32_t zoomed_pane_id{0};

    bool operator==(const MsgLayoutUpdate&) const = default;
};

struct MsgScreenSnapshot {
    uint64_t server_seq{0};
    uint32_t pane_id{0};
    uint16_t cols{0};
    uint16_t rows{0};
    CursorState cursor{};
    uint32_t modes{0};
    std::vector<Cell> cells; // cols * rows cells, wire serialized with RLE

    bool operator==(const MsgScreenSnapshot&) const = default;
};

struct CellRowDiff {
    uint16_t row{0};
    uint16_t start_col{0};
    std::vector<Cell> cells; // RLE encoded

    bool operator==(const CellRowDiff&) const = default;
};

struct PaneRowDiff {
    uint32_t pane_id{0};
    CursorState cursor{};
    std::vector<CellRowDiff> rows;

    bool operator==(const PaneRowDiff&) const = default;
};

struct MsgFrameDiff {
    uint64_t server_seq{0};
    uint64_t acked_client_seq{0};
    std::string status_line;
    std::vector<PaneRowDiff> pane_diffs;

    bool operator==(const MsgFrameDiff&) const = default;
};

struct MsgScrollbackSliceReq {
    uint32_t pane_id{0};
    uint64_t start_line{0};
    uint32_t line_count{0};

    constexpr bool operator==(const MsgScrollbackSliceReq&) const = default;
};

struct ScrollbackLine {
    bool wrapped{false};
    std::vector<Cell> cells;

    bool operator==(const ScrollbackLine&) const = default;
};

struct MsgScrollbackSliceResp {
    uint32_t pane_id{0};
    uint64_t start_line{0};
    uint64_t total_available_lines{0};
    std::vector<ScrollbackLine> lines;

    bool operator==(const MsgScrollbackSliceResp&) const = default;
};

struct MsgPassthroughEvent {
    uint32_t pane_id{0};
    uint8_t kind{0}; // 1 = Osc52Clipboard, 2 = Bell, 3 = DesktopNotification, 4 = Hyperlink
    std::vector<uint8_t> payload;

    bool operator==(const MsgPassthroughEvent&) const = default;
};

struct MsgHeartbeatPing {
    uint64_t seq{0};
    uint64_t timestamp_ms{0};

    constexpr bool operator==(const MsgHeartbeatPing&) const = default;
};

struct MsgHeartbeatPong {
    uint64_t seq{0};
    uint64_t timestamp_ms{0};

    constexpr bool operator==(const MsgHeartbeatPong&) const = default;
};

using ProtocolMessage = std::variant<
    MsgClientHello,
    MsgServerHello,
    MsgListSessionsReq,
    MsgListSessionsResp,
    MsgNewSessionReq,
    MsgNewSessionResp,
    MsgAttachSessionReq,
    MsgAttachSessionResp,
    MsgSwitchSessionReq,
    MsgSwitchSessionResp,
    MsgDetachReq,
    MsgDetachNotice,
    MsgKillSessionReq,
    MsgKillSessionResp,
    MsgCommandRpcReq,
    MsgCommandRpcResp,
    MsgKeyEvent,
    MsgMouseEvent,
    MsgPasteEvent,
    MsgClientResize,
    MsgLayoutUpdate,
    MsgScreenSnapshot,
    MsgFrameDiff,
    MsgScrollbackSliceReq,
    MsgScrollbackSliceResp,
    MsgPassthroughEvent,
    MsgHeartbeatPing,
    MsgHeartbeatPong
>;

MsgType get_message_type(const ProtocolMessage& msg) noexcept;
uint16_t get_default_stream_id(MsgType type) noexcept;

void encode_message_payload(const ProtocolMessage& msg, BinaryWriter& writer);
std::expected<ProtocolMessage, CodecError> decode_message_payload(
    MsgType type, BinaryReader& reader);

// Framing: encode message into an 8-byte framed packet (with optional zstd compression)
std::vector<std::byte> encode_frame(
    const ProtocolMessage& msg,
    uint16_t stream_id = 0,
    bool compress = false,
    int zstd_level = 3);

// Framing: decode a framed packet (with automatic zstd decompression if flagged)
std::expected<std::pair<StreamHeader, ProtocolMessage>, CodecError> decode_frame(
    std::span<const std::byte> frame_data,
    ZstdDecompressor* decomp = nullptr);

} // namespace tmux2::protocol
