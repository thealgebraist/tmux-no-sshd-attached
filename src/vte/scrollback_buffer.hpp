#pragma once

#include "src/protocol/cell.hpp"
#include "src/protocol/compression.hpp"
#include "src/protocol/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace tmux2::vte {

inline constexpr size_t kDefaultScrollbackCapacity = 50000;
inline constexpr size_t kDefaultLinesPerChunk = 64;

class ScrollbackBuffer {
public:
    explicit ScrollbackBuffer(
        size_t max_lines = kDefaultScrollbackCapacity,
        size_t lines_per_chunk = kDefaultLinesPerChunk);

    ~ScrollbackBuffer() = default;

    // Non-copyable, movable
    ScrollbackBuffer(const ScrollbackBuffer&) = delete;
    ScrollbackBuffer& operator=(const ScrollbackBuffer&) = delete;
    ScrollbackBuffer(ScrollbackBuffer&&) noexcept = default;
    ScrollbackBuffer& operator=(ScrollbackBuffer&&) noexcept = default;

    /**
     * Appends a line to the scrollback ring buffer.
     * When uncompressed staging fills lines_per_chunk_, it is compressed via zstd.
     * If total stored lines exceeds max_lines_, the oldest chunks are evicted.
     */
    void push_line(protocol::ScrollbackLine line);

    /**
     * Attempts to pop the most recently pushed line (used when libvterm reflows lines back on resize).
     */
    std::optional<protocol::ScrollbackLine> pop_line();

    /**
     * Retrieves a single line by index in [0, total_lines()).
     * Index 0 is the oldest available line.
     */
    [[nodiscard]] std::optional<protocol::ScrollbackLine> get_line(size_t index) const;

    /**
     * Retrieves a contiguous range of lines starting at start_index up to count lines.
     * Returns all available lines within the requested range.
     */
    [[nodiscard]] std::vector<protocol::ScrollbackLine> get_slice(size_t start_index, size_t count) const;

    /**
     * Clears all lines from the scrollback buffer.
     */
    void clear() noexcept;

    [[nodiscard]] size_t total_lines() const noexcept { return total_lines_; }
    [[nodiscard]] size_t max_lines() const noexcept { return max_lines_; }
    void set_max_lines(size_t new_max);

    /**
     * Returns total memory currently consumed by compressed and active chunks in bytes.
     */
    [[nodiscard]] size_t memory_usage_bytes() const noexcept;

private:
    struct CompressedChunk {
        size_t start_line_offset{0};
        size_t line_count{0};
        bool is_compressed{false};
        std::vector<std::byte> compressed_data;
    };

    struct CachedDecompressedChunk {
        size_t chunk_index{static_cast<size_t>(-1)};
        std::vector<protocol::ScrollbackLine> lines;
    };

    void flush_active_chunk();
    void evict_oldest_if_needed();
    std::vector<protocol::ScrollbackLine> decompress_chunk(size_t chunk_idx) const;

    size_t max_lines_{kDefaultScrollbackCapacity};
    size_t lines_per_chunk_{kDefaultLinesPerChunk};
    size_t total_lines_{0};
    size_t chunk_lines_count_{0}; // Lines stored in chunks_ (excluding active_chunk_)

    std::deque<CompressedChunk> chunks_;
    std::vector<protocol::ScrollbackLine> active_chunk_;

    mutable protocol::ZstdCompressor compressor_;
    mutable protocol::ZstdDecompressor decompressor_;
    mutable CachedDecompressedChunk decompressed_cache_;
};

} // namespace tmux2::vte
