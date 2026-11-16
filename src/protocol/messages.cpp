#include "src/protocol/messages.hpp"
#include <algorithm>
#include <cstring>

namespace tmux2::protocol {

namespace {

void write_cursor_state(BinaryWriter& writer, const CursorState& cursor) {
    writer.write_u16(cursor.col);
    writer.write_u16(cursor.row);
    writer.write_bool(cursor.visible);
    writer.write_u8(cursor.shape);
}

std::expected<CursorState, CodecError> read_cursor_state(BinaryReader& reader) noexcept {
    auto col = reader.read_u16();
    if (!col) return std::unexpected(col.error());
    auto row = reader.read_u16();
    if (!row) return std::unexpected(row.error());
    auto vis = reader.read_bool();
    if (!vis) return std::unexpected(vis.error());
    auto shape = reader.read_u8();
    if (!shape) return std::unexpected(shape.error());

    return CursorState{
        .col = *col,
        .row = *row,
        .visible = *vis,
        .shape = *shape,
    };
}

void write_session_summary(BinaryWriter& writer, const SessionSummary& s) {
    writer.write_u32(s.session_id);
    writer.write_string(s.name);
    writer.write_u64(s.created_at);
    writer.write_u32(s.window_count);
    writer.write_u32(s.attached_clients);
    writer.write_string(s.preview_summary);
}

std::expected<SessionSummary, CodecError> read_session_summary(BinaryReader& reader) {
    auto sid = reader.read_u32();
    if (!sid) return std::unexpected(sid.error());
    auto name = reader.read_string();
    if (!name) return std::unexpected(name.error());
    auto created = reader.read_u64();
    if (!created) return std::unexpected(created.error());
    auto win_cnt = reader.read_u32();
    if (!win_cnt) return std::unexpected(win_cnt.error());
    auto att_cnt = reader.read_u32();
    if (!att_cnt) return std::unexpected(att_cnt.error());
    auto prev = reader.read_string();
    if (!prev) return std::unexpected(prev.error());

    return SessionSummary{
        .session_id = *sid,
        .name = std::move(*name),
        .created_at = *created,
        .window_count = *win_cnt,
        .attached_clients = *att_cnt,
        .preview_summary = std::move(*prev),
    };
}

void write_cell_row_diff(BinaryWriter& writer, const CellRowDiff& row_diff) {
    writer.write_u16(row_diff.row);
    writer.write_u16(row_diff.start_col);
    writer.write_u16(static_cast<uint16_t>(row_diff.cells.size()));
    encode_cell_runs(writer, row_diff.cells);
}

std::expected<CellRowDiff, CodecError> read_cell_row_diff(BinaryReader& reader) {
    auto row = reader.read_u16();
    if (!row) return std::unexpected(row.error());
    auto scol = reader.read_u16();
    if (!scol) return std::unexpected(scol.error());
    auto count = reader.read_u16();
    if (!count) return std::unexpected(count.error());

    auto cells = decode_cell_runs(reader, *count);
    if (!cells) return std::unexpected(cells.error());

    return CellRowDiff{
        .row = *row,
        .start_col = *scol,
        .cells = std::move(*cells),
    };
}

void write_pane_row_diff(BinaryWriter& writer, const PaneRowDiff& pane_diff) {
    writer.write_u32(pane_diff.pane_id);
    write_cursor_state(writer, pane_diff.cursor);
    writer.write_u32(static_cast<uint32_t>(pane_diff.rows.size()));
    for (const auto& r : pane_diff.rows) {
        write_cell_row_diff(writer, r);
    }
}

std::expected<PaneRowDiff, CodecError> read_pane_row_diff(BinaryReader& reader) {
    auto pid = reader.read_u32();
    if (!pid) return std::unexpected(pid.error());
    auto cursor = read_cursor_state(reader);
    if (!cursor) return std::unexpected(cursor.error());
    auto rcount = reader.read_u32();
    if (!rcount) return std::unexpected(rcount.error());
    if (*rcount > 100000) return std::unexpected(CodecError::VectorTooLong);

    std::vector<CellRowDiff> rows;
    rows.reserve(*rcount);
    for (uint32_t i = 0; i < *rcount; ++i) {
        auto r = read_cell_row_diff(reader);
        if (!r) return std::unexpected(r.error());
        rows.push_back(std::move(*r));
    }

    return PaneRowDiff{
        .pane_id = *pid,
        .cursor = *cursor,
        .rows = std::move(rows),
    };
}

void write_scrollback_line(BinaryWriter& writer, const ScrollbackLine& line) {
    writer.write_bool(line.wrapped);
    writer.write_u16(static_cast<uint16_t>(line.cells.size()));
    encode_cell_runs(writer, line.cells);
}

std::expected<ScrollbackLine, CodecError> read_scrollback_line(BinaryReader& reader) {
    auto wrapped = reader.read_bool();
    if (!wrapped) return std::unexpected(wrapped.error());
    auto count = reader.read_u16();
    if (!count) return std::unexpected(count.error());

    auto cells = decode_cell_runs(reader, *count);
    if (!cells) return std::unexpected(cells.error());

    return ScrollbackLine{
        .wrapped = *wrapped,
        .cells = std::move(*cells),
    };
}

} // anonymous namespace

MsgType get_message_type(const ProtocolMessage& msg) noexcept {
    return std::visit([](const auto& m) -> MsgType {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, MsgClientHello>) return MsgType::ClientHello;
        else if constexpr (std::is_same_v<T, MsgServerHello>) return MsgType::ServerHello;
        else if constexpr (std::is_same_v<T, MsgListSessionsReq>) return MsgType::ListSessionsReq;
        else if constexpr (std::is_same_v<T, MsgListSessionsResp>) return MsgType::ListSessionsResp;
        else if constexpr (std::is_same_v<T, MsgNewSessionReq>) return MsgType::NewSessionReq;
        else if constexpr (std::is_same_v<T, MsgNewSessionResp>) return MsgType::NewSessionResp;
        else if constexpr (std::is_same_v<T, MsgAttachSessionReq>) return MsgType::AttachSessionReq;
        else if constexpr (std::is_same_v<T, MsgAttachSessionResp>) return MsgType::AttachSessionResp;
        else if constexpr (std::is_same_v<T, MsgSwitchSessionReq>) return MsgType::SwitchSessionReq;
        else if constexpr (std::is_same_v<T, MsgSwitchSessionResp>) return MsgType::SwitchSessionResp;
        else if constexpr (std::is_same_v<T, MsgDetachReq>) return MsgType::DetachReq;
        else if constexpr (std::is_same_v<T, MsgDetachNotice>) return MsgType::DetachNotice;
        else if constexpr (std::is_same_v<T, MsgKillSessionReq>) return MsgType::KillSessionReq;
        else if constexpr (std::is_same_v<T, MsgKillSessionResp>) return MsgType::KillSessionResp;
        else if constexpr (std::is_same_v<T, MsgCommandRpcReq>) return MsgType::CommandRpcReq;
        else if constexpr (std::is_same_v<T, MsgCommandRpcResp>) return MsgType::CommandRpcResp;
        else if constexpr (std::is_same_v<T, MsgKeyEvent>) return MsgType::KeyEvent;
        else if constexpr (std::is_same_v<T, MsgMouseEvent>) return MsgType::MouseEvent;
        else if constexpr (std::is_same_v<T, MsgPasteEvent>) return MsgType::PasteEvent;
        else if constexpr (std::is_same_v<T, MsgClientResize>) return MsgType::ClientResize;
        else if constexpr (std::is_same_v<T, MsgLayoutUpdate>) return MsgType::LayoutUpdate;
        else if constexpr (std::is_same_v<T, MsgScreenSnapshot>) return MsgType::ScreenSnapshot;
        else if constexpr (std::is_same_v<T, MsgFrameDiff>) return MsgType::FrameDiff;
        else if constexpr (std::is_same_v<T, MsgScrollbackSliceReq>) return MsgType::ScrollbackSliceReq;
        else if constexpr (std::is_same_v<T, MsgScrollbackSliceResp>) return MsgType::ScrollbackSliceResp;
        else if constexpr (std::is_same_v<T, MsgPassthroughEvent>) return MsgType::PassthroughEvent;
        else if constexpr (std::is_same_v<T, MsgHeartbeatPing>) return MsgType::HeartbeatPing;
        else if constexpr (std::is_same_v<T, MsgHeartbeatPong>) return MsgType::HeartbeatPong;
    }, msg);
}

uint16_t get_default_stream_id(MsgType type) noexcept {
    switch (type) {
        case MsgType::LayoutUpdate:
        case MsgType::ScreenSnapshot:
        case MsgType::FrameDiff:
        case MsgType::ScrollbackSliceResp:
        case MsgType::PassthroughEvent:
            return kRenderStreamId;
        default:
            return kControlStreamId;
    }
}

void encode_message_payload(const ProtocolMessage& msg, BinaryWriter& writer) {
    std::visit([&writer](const auto& m) {
        using T = std::decay_t<decltype(m)>;

        if constexpr (std::is_same_v<T, MsgClientHello>) {
            writer.write_u32(m.protocol_version);
            writer.write_bytes(m.client_instance_id.data(), m.client_instance_id.size());
            writer.write_u32(m.term_caps);
            writer.write_string(m.resume_token);
            writer.write_u64(m.last_srv_seq);
            writer.write_string(m.target);
        }
        else if constexpr (std::is_same_v<T, MsgServerHello>) {
            writer.write_u32(m.server_version);
            writer.write_u64(m.assigned_client_id);
            writer.write_string(m.resume_token);
            writer.write_u64(m.last_client_seq_acked);
            writer.write_u32(static_cast<uint32_t>(m.session_list.size()));
            for (const auto& s : m.session_list) {
                write_session_summary(writer, s);
            }
        }
        else if constexpr (std::is_same_v<T, MsgListSessionsReq>) {
            writer.write_string(m.filter);
        }
        else if constexpr (std::is_same_v<T, MsgListSessionsResp>) {
            writer.write_u32(static_cast<uint32_t>(m.sessions.size()));
            for (const auto& s : m.sessions) {
                write_session_summary(writer, s);
            }
        }
        else if constexpr (std::is_same_v<T, MsgNewSessionReq>) {
            writer.write_string(m.name);
            writer.write_string(m.initial_window_name);
            writer.write_string(m.cwd);
            writer.write_string(m.command);
            writer.write_u32(static_cast<uint32_t>(m.env.size()));
            for (const auto& [k, v] : m.env) {
                writer.write_string(k);
                writer.write_string(v);
            }
            writer.write_bool(m.detached);
        }
        else if constexpr (std::is_same_v<T, MsgNewSessionResp>) {
            writer.write_bool(m.success);
            writer.write_u32(m.session_id);
            writer.write_u32(m.window_id);
            writer.write_u32(m.pane_id);
            writer.write_string(m.error_msg);
        }
        else if constexpr (std::is_same_v<T, MsgAttachSessionReq>) {
            writer.write_u32(m.session_id);
            writer.write_string(m.session_name);
            writer.write_bool(m.read_only);
            writer.write_u16(m.cols);
            writer.write_u16(m.rows);
        }
        else if constexpr (std::is_same_v<T, MsgAttachSessionResp>) {
            writer.write_bool(m.success);
            writer.write_u32(m.session_id);
            writer.write_u32(m.window_id);
            writer.write_string(m.layout_tree_json);
            writer.write_string(m.error_msg);
        }
        else if constexpr (std::is_same_v<T, MsgSwitchSessionReq>) {
            writer.write_u32(m.session_id);
            writer.write_string(m.session_name);
        }
        else if constexpr (std::is_same_v<T, MsgSwitchSessionResp>) {
            writer.write_bool(m.success);
            writer.write_u32(m.session_id);
            writer.write_u32(m.window_id);
            writer.write_string(m.error_msg);
        }
        else if constexpr (std::is_same_v<T, MsgDetachReq>) {
            writer.write_u32(m.session_id);
            writer.write_string(m.reason);
        }
        else if constexpr (std::is_same_v<T, MsgDetachNotice>) {
            writer.write_string(m.reason);
        }
        else if constexpr (std::is_same_v<T, MsgKillSessionReq>) {
            writer.write_u32(m.session_id);
        }
        else if constexpr (std::is_same_v<T, MsgKillSessionResp>) {
            writer.write_bool(m.success);
            writer.write_string(m.error_msg);
        }
        else if constexpr (std::is_same_v<T, MsgCommandRpcReq>) {
            writer.write_u64(m.rpc_id);
            writer.write_string(m.command_dsl);
        }
        else if constexpr (std::is_same_v<T, MsgCommandRpcResp>) {
            writer.write_u64(m.rpc_id);
            writer.write_i32(m.exit_code);
            writer.write_string(m.stdout_data);
            writer.write_string(m.stderr_data);
        }
        else if constexpr (std::is_same_v<T, MsgKeyEvent>) {
            writer.write_u64(m.client_seq);
            writer.write_u32(m.target_pane);
            writer.write_u32(m.key_code);
            writer.write_u32(m.modifiers);
            writer.write_byte_vector(m.raw_bytes);
        }
        else if constexpr (std::is_same_v<T, MsgMouseEvent>) {
            writer.write_u64(m.client_seq);
            writer.write_u32(m.pane_id);
            writer.write_u8(m.kind);
            writer.write_u8(m.button);
            writer.write_u16(m.col);
            writer.write_u16(m.row);
            writer.write_u32(m.modifiers);
        }
        else if constexpr (std::is_same_v<T, MsgPasteEvent>) {
            writer.write_u64(m.client_seq);
            writer.write_u32(m.pane_id);
            writer.write_bool(m.bracketed);
            writer.write_byte_vector(m.data);
        }
        else if constexpr (std::is_same_v<T, MsgClientResize>) {
            writer.write_u16(m.cols);
            writer.write_u16(m.rows);
            writer.write_u32(m.pixel_width);
            writer.write_u32(m.pixel_height);
        }
        else if constexpr (std::is_same_v<T, MsgLayoutUpdate>) {
            writer.write_u64(m.server_seq);
            writer.write_u32(m.session_id);
            writer.write_u32(m.window_id);
            writer.write_string(m.layout_tree_json);
            writer.write_u32(m.active_pane_id);
            writer.write_u32(m.zoomed_pane_id);
        }
        else if constexpr (std::is_same_v<T, MsgScreenSnapshot>) {
            writer.write_u64(m.server_seq);
            writer.write_u32(m.pane_id);
            writer.write_u16(m.cols);
            writer.write_u16(m.rows);
            write_cursor_state(writer, m.cursor);
            writer.write_u32(m.modes);
            encode_cell_runs(writer, m.cells);
        }
        else if constexpr (std::is_same_v<T, MsgFrameDiff>) {
            writer.write_u64(m.server_seq);
            writer.write_u64(m.acked_client_seq);
            writer.write_string(m.status_line);
            writer.write_u32(static_cast<uint32_t>(m.pane_diffs.size()));
            for (const auto& pd : m.pane_diffs) {
                write_pane_row_diff(writer, pd);
            }
        }
        else if constexpr (std::is_same_v<T, MsgScrollbackSliceReq>) {
            writer.write_u32(m.pane_id);
            writer.write_u64(m.start_line);
            writer.write_u32(m.line_count);
        }
        else if constexpr (std::is_same_v<T, MsgScrollbackSliceResp>) {
            writer.write_u32(m.pane_id);
            writer.write_u64(m.start_line);
            writer.write_u64(m.total_available_lines);
            writer.write_u32(static_cast<uint32_t>(m.lines.size()));
            for (const auto& line : m.lines) {
                write_scrollback_line(writer, line);
            }
        }
        else if constexpr (std::is_same_v<T, MsgPassthroughEvent>) {
            writer.write_u32(m.pane_id);
            writer.write_u8(m.kind);
            writer.write_byte_vector(m.payload);
        }
        else if constexpr (std::is_same_v<T, MsgHeartbeatPing>) {
            writer.write_u64(m.seq);
            writer.write_u64(m.timestamp_ms);
        }
        else if constexpr (std::is_same_v<T, MsgHeartbeatPong>) {
            writer.write_u64(m.seq);
            writer.write_u64(m.timestamp_ms);
        }
    }, msg);
}

std::expected<ProtocolMessage, CodecError> decode_message_payload(
    MsgType type, BinaryReader& reader) {
    switch (type) {
        case MsgType::ClientHello: {
            MsgClientHello msg;
            auto ver = reader.read_u32();
            if (!ver) return std::unexpected(ver.error());
            msg.protocol_version = *ver;

            auto id_bytes = reader.read_bytes(16);
            if (!id_bytes) return std::unexpected(id_bytes.error());
            std::memcpy(msg.client_instance_id.data(), id_bytes->data(), 16);

            auto caps = reader.read_u32();
            if (!caps) return std::unexpected(caps.error());
            msg.term_caps = *caps;

            auto tok = reader.read_string();
            if (!tok) return std::unexpected(tok.error());
            msg.resume_token = std::move(*tok);

            auto seq = reader.read_u64();
            if (!seq) return std::unexpected(seq.error());
            msg.last_srv_seq = *seq;

            auto tgt = reader.read_string();
            if (!tgt) return std::unexpected(tgt.error());
            msg.target = std::move(*tgt);

            return msg;
        }
        case MsgType::ServerHello: {
            MsgServerHello msg;
            auto ver = reader.read_u32();
            if (!ver) return std::unexpected(ver.error());
            msg.server_version = *ver;

            auto cid = reader.read_u64();
            if (!cid) return std::unexpected(cid.error());
            msg.assigned_client_id = *cid;

            auto tok = reader.read_string();
            if (!tok) return std::unexpected(tok.error());
            msg.resume_token = std::move(*tok);

            auto ack = reader.read_u64();
            if (!ack) return std::unexpected(ack.error());
            msg.last_client_seq_acked = *ack;

            auto slen = reader.read_u32();
            if (!slen) return std::unexpected(slen.error());
            if (*slen > 100000) return std::unexpected(CodecError::VectorTooLong);
            msg.session_list.reserve(*slen);
            for (uint32_t i = 0; i < *slen; ++i) {
                auto s = read_session_summary(reader);
                if (!s) return std::unexpected(s.error());
                msg.session_list.push_back(std::move(*s));
            }
            return msg;
        }
        case MsgType::ListSessionsReq: {
            MsgListSessionsReq msg;
            auto filt = reader.read_string();
            if (!filt) return std::unexpected(filt.error());
            msg.filter = std::move(*filt);
            return msg;
        }
        case MsgType::ListSessionsResp: {
            MsgListSessionsResp msg;
            auto slen = reader.read_u32();
            if (!slen) return std::unexpected(slen.error());
            if (*slen > 100000) return std::unexpected(CodecError::VectorTooLong);
            msg.sessions.reserve(*slen);
            for (uint32_t i = 0; i < *slen; ++i) {
                auto s = read_session_summary(reader);
                if (!s) return std::unexpected(s.error());
                msg.sessions.push_back(std::move(*s));
            }
            return msg;
        }
        case MsgType::NewSessionReq: {
            MsgNewSessionReq msg;
            auto name = reader.read_string();
            if (!name) return std::unexpected(name.error());
            msg.name = std::move(*name);

            auto win = reader.read_string();
            if (!win) return std::unexpected(win.error());
            msg.initial_window_name = std::move(*win);

            auto cwd = reader.read_string();
            if (!cwd) return std::unexpected(cwd.error());
            msg.cwd = std::move(*cwd);

            auto cmd = reader.read_string();
            if (!cmd) return std::unexpected(cmd.error());
            msg.command = std::move(*cmd);

            auto env_len = reader.read_u32();
            if (!env_len) return std::unexpected(env_len.error());
            if (*env_len > 10000) return std::unexpected(CodecError::VectorTooLong);
            msg.env.reserve(*env_len);
            for (uint32_t i = 0; i < *env_len; ++i) {
                auto k = reader.read_string();
                if (!k) return std::unexpected(k.error());
                auto v = reader.read_string();
                if (!v) return std::unexpected(v.error());
                msg.env.emplace_back(std::move(*k), std::move(*v));
            }

            auto det = reader.read_bool();
            if (!det) return std::unexpected(det.error());
            msg.detached = *det;

            return msg;
        }
        case MsgType::NewSessionResp: {
            MsgNewSessionResp msg;
            auto succ = reader.read_bool();
            if (!succ) return std::unexpected(succ.error());
            msg.success = *succ;

            auto sid = reader.read_u32();
            if (!sid) return std::unexpected(sid.error());
            msg.session_id = *sid;

            auto wid = reader.read_u32();
            if (!wid) return std::unexpected(wid.error());
            msg.window_id = *wid;

            auto pid = reader.read_u32();
            if (!pid) return std::unexpected(pid.error());
            msg.pane_id = *pid;

            auto err = reader.read_string();
            if (!err) return std::unexpected(err.error());
            msg.error_msg = std::move(*err);

            return msg;
        }
        case MsgType::AttachSessionReq: {
            MsgAttachSessionReq msg;
            auto sid = reader.read_u32();
            if (!sid) return std::unexpected(sid.error());
            msg.session_id = *sid;

            auto sname = reader.read_string();
            if (!sname) return std::unexpected(sname.error());
            msg.session_name = std::move(*sname);

            auto ro = reader.read_bool();
            if (!ro) return std::unexpected(ro.error());
            msg.read_only = *ro;

            auto cols = reader.read_u16();
            if (!cols) return std::unexpected(cols.error());
            msg.cols = *cols;

            auto rows = reader.read_u16();
            if (!rows) return std::unexpected(rows.error());
            msg.rows = *rows;

            return msg;
        }
        case MsgType::AttachSessionResp: {
            MsgAttachSessionResp msg;
            auto succ = reader.read_bool();
            if (!succ) return std::unexpected(succ.error());
            msg.success = *succ;

            auto sid = reader.read_u32();
            if (!sid) return std::unexpected(sid.error());
            msg.session_id = *sid;

            auto wid = reader.read_u32();
            if (!wid) return std::unexpected(wid.error());
            msg.window_id = *wid;

            auto tree = reader.read_string();
            if (!tree) return std::unexpected(tree.error());
            msg.layout_tree_json = std::move(*tree);

            auto err = reader.read_string();
            if (!err) return std::unexpected(err.error());
            msg.error_msg = std::move(*err);

            return msg;
        }
        case MsgType::SwitchSessionReq: {
            MsgSwitchSessionReq msg;
            auto sid = reader.read_u32();
            if (!sid) return std::unexpected(sid.error());
            msg.session_id = *sid;

            auto sname = reader.read_string();
            if (!sname) return std::unexpected(sname.error());
            msg.session_name = std::move(*sname);

            return msg;
        }
        case MsgType::SwitchSessionResp: {
            MsgSwitchSessionResp msg;
            auto succ = reader.read_bool();
            if (!succ) return std::unexpected(succ.error());
            msg.success = *succ;

            auto sid = reader.read_u32();
            if (!sid) return std::unexpected(sid.error());
            msg.session_id = *sid;

            auto wid = reader.read_u32();
            if (!wid) return std::unexpected(wid.error());
            msg.window_id = *wid;

            auto err = reader.read_string();
            if (!err) return std::unexpected(err.error());
            msg.error_msg = std::move(*err);

            return msg;
        }
        case MsgType::DetachReq: {
            MsgDetachReq msg;
            auto sid = reader.read_u32();
            if (!sid) return std::unexpected(sid.error());
            msg.session_id = *sid;

            auto reason = reader.read_string();
            if (!reason) return std::unexpected(reason.error());
            msg.reason = std::move(*reason);

            return msg;
        }
        case MsgType::DetachNotice: {
            MsgDetachNotice msg;
            auto reason = reader.read_string();
            if (!reason) return std::unexpected(reason.error());
            msg.reason = std::move(*reason);

            return msg;
        }
        case MsgType::KillSessionReq: {
            MsgKillSessionReq msg;
            auto sid = reader.read_u32();
            if (!sid) return std::unexpected(sid.error());
            msg.session_id = *sid;

            return msg;
        }
        case MsgType::KillSessionResp: {
            MsgKillSessionResp msg;
            auto succ = reader.read_bool();
            if (!succ) return std::unexpected(succ.error());
            msg.success = *succ;

            auto err = reader.read_string();
            if (!err) return std::unexpected(err.error());
            msg.error_msg = std::move(*err);

            return msg;
        }
        case MsgType::CommandRpcReq: {
            MsgCommandRpcReq msg;
            auto rpc = reader.read_u64();
            if (!rpc) return std::unexpected(rpc.error());
            msg.rpc_id = *rpc;

            auto dsl = reader.read_string();
            if (!dsl) return std::unexpected(dsl.error());
            msg.command_dsl = std::move(*dsl);

            return msg;
        }
        case MsgType::CommandRpcResp: {
            MsgCommandRpcResp msg;
            auto rpc = reader.read_u64();
            if (!rpc) return std::unexpected(rpc.error());
            msg.rpc_id = *rpc;

            auto code = reader.read_i32();
            if (!code) return std::unexpected(code.error());
            msg.exit_code = *code;

            auto out = reader.read_string();
            if (!out) return std::unexpected(out.error());
            msg.stdout_data = std::move(*out);

            auto err = reader.read_string();
            if (!err) return std::unexpected(err.error());
            msg.stderr_data = std::move(*err);

            return msg;
        }
        case MsgType::KeyEvent: {
            MsgKeyEvent msg;
            auto seq = reader.read_u64();
            if (!seq) return std::unexpected(seq.error());
            msg.client_seq = *seq;

            auto tp = reader.read_u32();
            if (!tp) return std::unexpected(tp.error());
            msg.target_pane = *tp;

            auto code = reader.read_u32();
            if (!code) return std::unexpected(code.error());
            msg.key_code = *code;

            auto mod = reader.read_u32();
            if (!mod) return std::unexpected(mod.error());
            msg.modifiers = *mod;

            auto raw = reader.read_byte_vector();
            if (!raw) return std::unexpected(raw.error());
            msg.raw_bytes = std::move(*raw);

            return msg;
        }
        case MsgType::MouseEvent: {
            MsgMouseEvent msg;
            auto seq = reader.read_u64();
            if (!seq) return std::unexpected(seq.error());
            msg.client_seq = *seq;

            auto pid = reader.read_u32();
            if (!pid) return std::unexpected(pid.error());
            msg.pane_id = *pid;

            auto kind = reader.read_u8();
            if (!kind) return std::unexpected(kind.error());
            msg.kind = *kind;

            auto btn = reader.read_u8();
            if (!btn) return std::unexpected(btn.error());
            msg.button = *btn;

            auto col = reader.read_u16();
            if (!col) return std::unexpected(col.error());
            msg.col = *col;

            auto row = reader.read_u16();
            if (!row) return std::unexpected(row.error());
            msg.row = *row;

            auto mod = reader.read_u32();
            if (!mod) return std::unexpected(mod.error());
            msg.modifiers = *mod;

            return msg;
        }
        case MsgType::PasteEvent: {
            MsgPasteEvent msg;
            auto seq = reader.read_u64();
            if (!seq) return std::unexpected(seq.error());
            msg.client_seq = *seq;

            auto pid = reader.read_u32();
            if (!pid) return std::unexpected(pid.error());
            msg.pane_id = *pid;

            auto br = reader.read_bool();
            if (!br) return std::unexpected(br.error());
            msg.bracketed = *br;

            auto data = reader.read_byte_vector();
            if (!data) return std::unexpected(data.error());
            msg.data = std::move(*data);

            return msg;
        }
        case MsgType::ClientResize: {
            MsgClientResize msg;
            auto cols = reader.read_u16();
            if (!cols) return std::unexpected(cols.error());
            msg.cols = *cols;

            auto rows = reader.read_u16();
            if (!rows) return std::unexpected(rows.error());
            msg.rows = *rows;

            auto pw = reader.read_u32();
            if (!pw) return std::unexpected(pw.error());
            msg.pixel_width = *pw;

            auto ph = reader.read_u32();
            if (!ph) return std::unexpected(ph.error());
            msg.pixel_height = *ph;

            return msg;
        }
        case MsgType::LayoutUpdate: {
            MsgLayoutUpdate msg;
            auto sseq = reader.read_u64();
            if (!sseq) return std::unexpected(sseq.error());
            msg.server_seq = *sseq;

            auto sid = reader.read_u32();
            if (!sid) return std::unexpected(sid.error());
            msg.session_id = *sid;

            auto wid = reader.read_u32();
            if (!wid) return std::unexpected(wid.error());
            msg.window_id = *wid;

            auto tree = reader.read_string();
            if (!tree) return std::unexpected(tree.error());
            msg.layout_tree_json = std::move(*tree);

            auto apid = reader.read_u32();
            if (!apid) return std::unexpected(apid.error());
            msg.active_pane_id = *apid;

            auto zpid = reader.read_u32();
            if (!zpid) return std::unexpected(zpid.error());
            msg.zoomed_pane_id = *zpid;

            return msg;
        }
        case MsgType::ScreenSnapshot: {
            MsgScreenSnapshot msg;
            auto sseq = reader.read_u64();
            if (!sseq) return std::unexpected(sseq.error());
            msg.server_seq = *sseq;

            auto pid = reader.read_u32();
            if (!pid) return std::unexpected(pid.error());
            msg.pane_id = *pid;

            auto cols = reader.read_u16();
            if (!cols) return std::unexpected(cols.error());
            msg.cols = *cols;

            auto rows = reader.read_u16();
            if (!rows) return std::unexpected(rows.error());
            msg.rows = *rows;

            auto cur = read_cursor_state(reader);
            if (!cur) return std::unexpected(cur.error());
            msg.cursor = *cur;

            auto modes = reader.read_u32();
            if (!modes) return std::unexpected(modes.error());
            msg.modes = *modes;

            size_t expected_cells = static_cast<size_t>(*cols) * static_cast<size_t>(*rows);
            auto cells = decode_cell_runs(reader, expected_cells);
            if (!cells) return std::unexpected(cells.error());
            msg.cells = std::move(*cells);

            return msg;
        }
        case MsgType::FrameDiff: {
            MsgFrameDiff msg;
            auto sseq = reader.read_u64();
            if (!sseq) return std::unexpected(sseq.error());
            msg.server_seq = *sseq;

            auto ack = reader.read_u64();
            if (!ack) return std::unexpected(ack.error());
            msg.acked_client_seq = *ack;

            auto status = reader.read_string();
            if (!status) return std::unexpected(status.error());
            msg.status_line = std::move(*status);

            auto plen = reader.read_u32();
            if (!plen) return std::unexpected(plen.error());
            if (*plen > 100000) return std::unexpected(CodecError::VectorTooLong);
            msg.pane_diffs.reserve(*plen);
            for (uint32_t i = 0; i < *plen; ++i) {
                auto pd = read_pane_row_diff(reader);
                if (!pd) return std::unexpected(pd.error());
                msg.pane_diffs.push_back(std::move(*pd));
            }

            return msg;
        }
        case MsgType::ScrollbackSliceReq: {
            MsgScrollbackSliceReq msg;
            auto pid = reader.read_u32();
            if (!pid) return std::unexpected(pid.error());
            msg.pane_id = *pid;

            auto start = reader.read_u64();
            if (!start) return std::unexpected(start.error());
            msg.start_line = *start;

            auto cnt = reader.read_u32();
            if (!cnt) return std::unexpected(cnt.error());
            msg.line_count = *cnt;

            return msg;
        }
        case MsgType::ScrollbackSliceResp: {
            MsgScrollbackSliceResp msg;
            auto pid = reader.read_u32();
            if (!pid) return std::unexpected(pid.error());
            msg.pane_id = *pid;

            auto start = reader.read_u64();
            if (!start) return std::unexpected(start.error());
            msg.start_line = *start;

            auto tot = reader.read_u64();
            if (!tot) return std::unexpected(tot.error());
            msg.total_available_lines = *tot;

            auto llen = reader.read_u32();
            if (!llen) return std::unexpected(llen.error());
            if (*llen > 100000) return std::unexpected(CodecError::VectorTooLong);
            msg.lines.reserve(*llen);
            for (uint32_t i = 0; i < *llen; ++i) {
                auto l = read_scrollback_line(reader);
                if (!l) return std::unexpected(l.error());
                msg.lines.push_back(std::move(*l));
            }

            return msg;
        }
        case MsgType::PassthroughEvent: {
            MsgPassthroughEvent msg;
            auto pid = reader.read_u32();
            if (!pid) return std::unexpected(pid.error());
            msg.pane_id = *pid;

            auto kind = reader.read_u8();
            if (!kind) return std::unexpected(kind.error());
            msg.kind = *kind;

            auto payload = reader.read_byte_vector();
            if (!payload) return std::unexpected(payload.error());
            msg.payload = std::move(*payload);

            return msg;
        }
        case MsgType::HeartbeatPing: {
            MsgHeartbeatPing msg;
            auto seq = reader.read_u64();
            if (!seq) return std::unexpected(seq.error());
            msg.seq = *seq;

            auto ts = reader.read_u64();
            if (!ts) return std::unexpected(ts.error());
            msg.timestamp_ms = *ts;

            return msg;
        }
        case MsgType::HeartbeatPong: {
            MsgHeartbeatPong msg;
            auto seq = reader.read_u64();
            if (!seq) return std::unexpected(seq.error());
            msg.seq = *seq;

            auto ts = reader.read_u64();
            if (!ts) return std::unexpected(ts.error());
            msg.timestamp_ms = *ts;

            return msg;
        }
    }
    return std::unexpected(CodecError::UnknownMessageType);
}

std::vector<std::byte> encode_frame(
    const ProtocolMessage& msg,
    uint16_t stream_id,
    bool compress,
    int zstd_level) {
    MsgType msg_type = get_message_type(msg);
    if (stream_id == 0) {
        stream_id = get_default_stream_id(msg_type);
    }

    BinaryWriter payload_writer;
    encode_message_payload(msg, payload_writer);

    StreamHeader header;
    header.stream_id = stream_id;
    header.msg_type = static_cast<uint8_t>(msg_type);
    header.flags = kFlagNone;

    std::vector<std::byte> final_payload;

    if (compress) {
        auto comp_res = zstd_compress(payload_writer.span(), zstd_level);
        // Only keep compressed if compression succeeded and actually saved space
        if (comp_res && comp_res->size() < payload_writer.size()) {
            final_payload = std::move(*comp_res);
            header.set_compressed(true);
        } else {
            final_payload = payload_writer.take();
        }
    } else {
        final_payload = payload_writer.take();
    }

    header.payload_len = static_cast<uint32_t>(final_payload.size());

    BinaryWriter frame_writer(kStreamHeaderSize + final_payload.size());
    append_header(frame_writer, header);
    frame_writer.write_bytes(final_payload);

    return frame_writer.take();
}

std::expected<std::pair<StreamHeader, ProtocolMessage>, CodecError> decode_frame(
    std::span<const std::byte> frame_data,
    ZstdDecompressor* decomp) {
    if (frame_data.size() < kStreamHeaderSize) {
        return std::unexpected(CodecError::Truncated);
    }

    auto hdr_res = decode_header(frame_data.subspan(0, kStreamHeaderSize));
    if (!hdr_res) return std::unexpected(hdr_res.error());
    StreamHeader hdr = *hdr_res;

    if (frame_data.size() < kStreamHeaderSize + hdr.payload_len) {
        return std::unexpected(CodecError::Truncated);
    }

    std::span<const std::byte> payload_span =
        frame_data.subspan(kStreamHeaderSize, hdr.payload_len);

    std::vector<std::byte> decompressed_buf;
    std::span<const std::byte> effective_payload;

    if (hdr.is_compressed()) {
        if (decomp) {
            auto dec_res = decomp->decompress(payload_span);
            if (!dec_res) return std::unexpected(dec_res.error());
            decompressed_buf = std::move(*dec_res);
        } else {
            auto dec_res = zstd_decompress(payload_span);
            if (!dec_res) return std::unexpected(dec_res.error());
            decompressed_buf = std::move(*dec_res);
        }
        effective_payload = decompressed_buf;
    } else {
        effective_payload = payload_span;
    }

    BinaryReader reader(effective_payload);
    auto msg_res = decode_message_payload(static_cast<MsgType>(hdr.msg_type), reader);
    if (!msg_res) return std::unexpected(msg_res.error());

    return std::pair<StreamHeader, ProtocolMessage>{hdr, std::move(*msg_res)};
}

} // namespace tmux2::protocol
