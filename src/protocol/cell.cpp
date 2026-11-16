#include "src/protocol/cell.hpp"

namespace tmux2::protocol {

void write_color(BinaryWriter& writer, const Color& color) {
    writer.write_u8(static_cast<uint8_t>(color.type));
    writer.write_u8(color.r_or_idx);
    writer.write_u8(color.g);
    writer.write_u8(color.b);
}

std::expected<Color, CodecError> read_color(BinaryReader& reader) noexcept {
    auto t = reader.read_u8();
    if (!t) return std::unexpected(t.error());
    if (*t > static_cast<uint8_t>(ColorType::Rgb)) {
        return std::unexpected(CodecError::InvalidData);
    }
    auto r = reader.read_u8();
    if (!r) return std::unexpected(r.error());
    auto g = reader.read_u8();
    if (!g) return std::unexpected(g.error());
    auto b = reader.read_u8();
    if (!b) return std::unexpected(b.error());

    Color c;
    c.type = static_cast<ColorType>(*t);
    c.r_or_idx = *r;
    c.g = *g;
    c.b = *b;
    return c;
}

void write_cell_style(BinaryWriter& writer, const CellStyle& style) {
    writer.write_u16(style.flags);
    write_color(writer, style.fg);
    write_color(writer, style.bg);
}

std::expected<CellStyle, CodecError> read_cell_style(BinaryReader& reader) noexcept {
    auto flags = reader.read_u16();
    if (!flags) return std::unexpected(flags.error());

    auto fg = read_color(reader);
    if (!fg) return std::unexpected(fg.error());

    auto bg = read_color(reader);
    if (!bg) return std::unexpected(bg.error());

    CellStyle st;
    st.flags = *flags;
    st.fg = *fg;
    st.bg = *bg;
    return st;
}

void write_cell(BinaryWriter& writer, const Cell& cell) {
    writer.write_u32(static_cast<uint32_t>(cell.codepoint));
    writer.write_u8(cell.width);
    write_cell_style(writer, cell.style);
}

std::expected<Cell, CodecError> read_cell(BinaryReader& reader) noexcept {
    auto cp = reader.read_u32();
    if (!cp) return std::unexpected(cp.error());

    auto w = reader.read_u8();
    if (!w) return std::unexpected(w.error());

    auto st = read_cell_style(reader);
    if (!st) return std::unexpected(st.error());

    Cell cell;
    cell.codepoint = static_cast<char32_t>(*cp);
    cell.width = *w;
    cell.style = *st;
    return cell;
}

void write_cell_run(BinaryWriter& writer, const CellRun& run) {
    writer.write_u16(run.count);
    write_cell(writer, run.cell);
}

std::expected<CellRun, CodecError> read_cell_run(BinaryReader& reader) noexcept {
    auto cnt = reader.read_u16();
    if (!cnt) return std::unexpected(cnt.error());
    if (*cnt == 0) return std::unexpected(CodecError::InvalidData);

    auto cell = read_cell(reader);
    if (!cell) return std::unexpected(cell.error());

    return CellRun{.cell = *cell, .count = *cnt};
}

void encode_cell_runs(BinaryWriter& writer, std::span<const Cell> cells) {
    if (cells.empty()) {
        writer.write_u32(0);
        return;
    }

    // First pass or vector of runs
    std::vector<CellRun> runs;
    runs.reserve(std::min<size_t>(cells.size(), 256));

    Cell current_cell = cells[0];
    uint16_t current_count = 1;

    for (size_t i = 1; i < cells.size(); ++i) {
        if (cells[i] == current_cell && current_count < 0xFFFF) {
            ++current_count;
        } else {
            runs.push_back(CellRun{.cell = current_cell, .count = current_count});
            current_cell = cells[i];
            current_count = 1;
        }
    }
    runs.push_back(CellRun{.cell = current_cell, .count = current_count});

    writer.write_u32(static_cast<uint32_t>(runs.size()));
    for (const auto& run : runs) {
        write_cell_run(writer, run);
    }
}

std::expected<void, CodecError> decode_cell_runs_into(
    BinaryReader& reader, std::vector<Cell>& dst, size_t expected_total_cells) {
    auto run_count_res = reader.read_u32();
    if (!run_count_res) return std::unexpected(run_count_res.error());
    uint32_t run_count = *run_count_res;

    dst.clear();
    dst.reserve(expected_total_cells);

    size_t accumulated = 0;
    for (uint32_t i = 0; i < run_count; ++i) {
        auto run_res = read_cell_run(reader);
        if (!run_res) return std::unexpected(run_res.error());
        const auto& run = *run_res;

        if (accumulated + run.count > expected_total_cells) {
            return std::unexpected(CodecError::InvalidData);
        }

        dst.insert(dst.end(), run.count, run.cell);
        accumulated += run.count;
    }

    if (accumulated != expected_total_cells) {
        return std::unexpected(CodecError::InvalidData);
    }

    return {};
}

std::expected<std::vector<Cell>, CodecError> decode_cell_runs(
    BinaryReader& reader, size_t expected_total_cells) {
    if (expected_total_cells > kMaxCellCount) {
        return std::unexpected(CodecError::VectorTooLong);
    }
    std::vector<Cell> cells;
    auto res = decode_cell_runs_into(reader, cells, expected_total_cells);
    if (!res) return std::unexpected(res.error());
    return cells;
}

} // namespace tmux2::protocol
