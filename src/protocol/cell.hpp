#pragma once

#include "src/protocol/codec.hpp"
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tmux2::protocol {

inline constexpr size_t kMaxCellCount = 16 * 1024 * 1024; // 16 million cells safety limit

enum class ColorType : uint8_t {
    Default = 0,
    Indexed = 1,
    Rgb     = 2,
};

struct Color {
    ColorType type{ColorType::Default};
    uint8_t r_or_idx{0}; // 0-255 palette index when Indexed, Red channel when Rgb
    uint8_t g{0};        // Green channel when Rgb
    uint8_t b{0};        // Blue channel when Rgb

    static constexpr Color default_color() noexcept {
        return Color{ColorType::Default, 0, 0, 0};
    }

    static constexpr Color indexed(uint8_t idx) noexcept {
        return Color{ColorType::Indexed, idx, 0, 0};
    }

    static constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) noexcept {
        return Color{ColorType::Rgb, r, g, b};
    }

    constexpr bool operator==(const Color&) const = default;
};

// Terminal cell styling attribute bitflags
namespace StyleFlags {
    inline constexpr uint16_t None          = 0;
    inline constexpr uint16_t Bold          = 1 << 0;
    inline constexpr uint16_t Dim           = 1 << 1;
    inline constexpr uint16_t Italic        = 1 << 2;
    inline constexpr uint16_t Underline     = 1 << 3;
    inline constexpr uint16_t Blink         = 1 << 4;
    inline constexpr uint16_t Reverse       = 1 << 5;
    inline constexpr uint16_t Hidden        = 1 << 6;
    inline constexpr uint16_t Strikethrough = 1 << 7;
}

struct CellStyle {
    uint16_t flags{StyleFlags::None};
    Color fg{Color::default_color()};
    Color bg{Color::default_color()};

    constexpr bool operator==(const CellStyle&) const = default;
};

struct Cell {
    char32_t codepoint{U' '};
    uint8_t width{1}; // 0 = combining/zero-width, 1 = normal, 2 = wide CJK/emoji
    CellStyle style{};

    constexpr bool operator==(const Cell&) const = default;
};

struct CellRun {
    Cell cell{};
    uint16_t count{1};

    constexpr bool operator==(const CellRun&) const = default;
};

// Single cell serialization
void write_color(BinaryWriter& writer, const Color& color);
std::expected<Color, CodecError> read_color(BinaryReader& reader) noexcept;

void write_cell_style(BinaryWriter& writer, const CellStyle& style);
std::expected<CellStyle, CodecError> read_cell_style(BinaryReader& reader) noexcept;

void write_cell(BinaryWriter& writer, const Cell& cell);
std::expected<Cell, CodecError> read_cell(BinaryReader& reader) noexcept;

void write_cell_run(BinaryWriter& writer, const CellRun& run);
std::expected<CellRun, CodecError> read_cell_run(BinaryReader& reader) noexcept;

// High-performance Run-Length Encoding / Decoding for Cell slices
void encode_cell_runs(BinaryWriter& writer, std::span<const Cell> cells);

std::expected<std::vector<Cell>, CodecError> decode_cell_runs(
    BinaryReader& reader, size_t expected_total_cells);

std::expected<void, CodecError> decode_cell_runs_into(
    BinaryReader& reader, std::vector<Cell>& dst, size_t expected_total_cells);

} // namespace tmux2::protocol
