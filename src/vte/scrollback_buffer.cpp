#include "src/vte/scrollback_buffer.hpp"

#include <algorithm>
#include <stdexcept>

namespace tmux2::vte {

ScrollbackBuffer::ScrollbackBuffer(size_t max_lines, size_t lines_per_chunk)
    : max_lines_(max_lines),
      lines_per_chunk_(lines_per_chunk > 0 ? lines_per_chunk : kDefaultLinesPerChunk) {}

void ScrollbackBuffer::push_line(protocol::ScrollbackLine line) {
    active_chunk_.push_back(std::move(line));
    ++total_lines_;

    if (active_chunk_.size() >= lines_per_chunk_) {
        flush_active_chunk();
    }

    evict_oldest_if_needed();
}

std::optional<protocol::ScrollbackLine> ScrollbackBuffer::pop_line() {
    if (total_lines_ == 0) {
        return std::nullopt;
    }

    if (!active_chunk_.empty()) {
        protocol::ScrollbackLine line = std::move(active_chunk_.back());
        active_chunk_.pop_back();
        --total_lines_;
        return line;
    }

    if (!chunks_.empty()) {
        size_t last_idx = chunks_.size() - 1;
        auto lines = decompress_chunk(last_idx);
        chunk_lines_count_ -= chunks_.back().line_count;
        chunks_.pop_back();
        if (decompressed_cache_.chunk_index == last_idx) {
            decompressed_cache_.chunk_index = static_cast<size_t>(-1);
        }

        active_chunk_ = std::move(lines);
        protocol::ScrollbackLine line = std::move(active_chunk_.back());
        active_chunk_.pop_back();
        --total_lines_;
        return line;
    }

    return std::nullopt;
}

std::optional<protocol::ScrollbackLine> ScrollbackBuffer::get_line(size_t index) const {
    if (index >= total_lines_) {
        return std::nullopt;
    }

    // Check if within active_chunk_
    if (index >= chunk_lines_count_) {
        size_t active_idx = index - chunk_lines_count_;
        if (active_idx < active_chunk_.size()) {
            return active_chunk_[active_idx];
        }
        return std::nullopt;
    }

    // Search within chunks_
    size_t current_offset = 0;
    for (size_t c = 0; c < chunks_.size(); ++c) {
        const auto& chunk = chunks_[c];
        if (index < current_offset + chunk.line_count) {
            size_t in_chunk_idx = index - current_offset;
            const auto& lines = decompress_chunk(c);
            if (in_chunk_idx < lines.size()) {
                return lines[in_chunk_idx];
            }
            return std::nullopt;
        }
        current_offset += chunk.line_count;
    }

    return std::nullopt;
}

std::vector<protocol::ScrollbackLine> ScrollbackBuffer::get_slice(size_t start_index, size_t count) const {
    std::vector<protocol::ScrollbackLine> result;
    if (start_index >= total_lines_ || count == 0) {
        return result;
    }

    size_t end_index = std::min(total_lines_, start_index + count);
    result.reserve(end_index - start_index);

    size_t current_offset = 0;
    for (size_t c = 0; c < chunks_.size() && current_offset < end_index; ++c) {
        const auto& chunk = chunks_[c];
        size_t chunk_end = current_offset + chunk.line_count;

        if (chunk_end > start_index && current_offset < end_index) {
            const auto& lines = decompress_chunk(c);
            size_t take_start = (start_index > current_offset) ? (start_index - current_offset) : 0;
            size_t take_end = (end_index < chunk_end) ? (end_index - current_offset) : lines.size();
            take_end = std::min(take_end, lines.size());

            for (size_t i = take_start; i < take_end; ++i) {
                result.push_back(lines[i]);
            }
        }
        current_offset = chunk_end;
    }

    // Lines in active_chunk_
    if (end_index > chunk_lines_count_) {
        size_t active_start = (start_index > chunk_lines_count_) ? (start_index - chunk_lines_count_) : 0;
        size_t active_end = end_index - chunk_lines_count_;
        active_end = std::min(active_end, active_chunk_.size());

        for (size_t i = active_start; i < active_end; ++i) {
            result.push_back(active_chunk_[i]);
        }
    }

    return result;
}

void ScrollbackBuffer::clear() noexcept {
    chunks_.clear();
    active_chunk_.clear();
    total_lines_ = 0;
    chunk_lines_count_ = 0;
    decompressed_cache_.chunk_index = static_cast<size_t>(-1);
    decompressed_cache_.lines.clear();
}

void ScrollbackBuffer::set_max_lines(size_t new_max) {
    max_lines_ = new_max;
    evict_oldest_if_needed();
}

size_t ScrollbackBuffer::memory_usage_bytes() const noexcept {
    size_t mem = sizeof(*this);
    for (const auto& chunk : chunks_) {
        mem += sizeof(CompressedChunk) + chunk.compressed_data.capacity();
    }
    for (const auto& line : active_chunk_) {
        mem += sizeof(line) + line.cells.capacity() * sizeof(protocol::Cell);
    }
    for (const auto& line : decompressed_cache_.lines) {
        mem += sizeof(line) + line.cells.capacity() * sizeof(protocol::Cell);
    }
    return mem;
}

void ScrollbackBuffer::flush_active_chunk() {
    if (active_chunk_.empty()) {
        return;
    }

    protocol::BinaryWriter writer;
    writer.write_u32(static_cast<uint32_t>(active_chunk_.size()));

    for (const auto& line : active_chunk_) {
        writer.write_u8(line.wrapped ? 1 : 0);
        writer.write_u32(static_cast<uint32_t>(line.cells.size()));
        protocol::encode_cell_runs(writer, line.cells);
    }

    auto comp_res = compressor_.compress(writer.span());
    if (comp_res) {
        CompressedChunk chunk;
        chunk.start_line_offset = chunk_lines_count_;
        chunk.line_count = active_chunk_.size();
        chunk.compressed_data = std::move(*comp_res);

        chunk_lines_count_ += chunk.line_count;
        chunks_.push_back(std::move(chunk));
    } else {
        // Fallback: store uncompressed if compression fails
        CompressedChunk chunk;
        chunk.start_line_offset = chunk_lines_count_;
        chunk.line_count = active_chunk_.size();
        chunk.compressed_data = writer.take();

        chunk_lines_count_ += chunk.line_count;
        chunks_.push_back(std::move(chunk));
    }

    active_chunk_.clear();
}

void ScrollbackBuffer::evict_oldest_if_needed() {
    while (total_lines_ > max_lines_ && !chunks_.empty()) {
        size_t evicted = chunks_.front().line_count;
        chunks_.pop_front();
        total_lines_ -= evicted;
        chunk_lines_count_ -= evicted;
        decompressed_cache_.chunk_index = static_cast<size_t>(-1);
        decompressed_cache_.lines.clear();
    }

    while (total_lines_ > max_lines_ && !active_chunk_.empty()) {
        active_chunk_.erase(active_chunk_.begin());
        --total_lines_;
    }
}

std::vector<protocol::ScrollbackLine> ScrollbackBuffer::decompress_chunk(size_t chunk_idx) const {
    if (chunk_idx >= chunks_.size()) {
        return {};
    }

    if (decompressed_cache_.chunk_index == chunk_idx) {
        return decompressed_cache_.lines;
    }

    const auto& chunk = chunks_[chunk_idx];
    std::vector<std::byte> raw_data;

    auto decomp_res = decompressor_.decompress(chunk.compressed_data);
    if (decomp_res) {
        raw_data = std::move(*decomp_res);
    } else {
        // May have been stored raw
        raw_data = chunk.compressed_data;
    }

    std::vector<protocol::ScrollbackLine> lines;
    protocol::BinaryReader reader(raw_data);

    auto count_opt = reader.read_u32();
    if (!count_opt) {
        return lines;
    }
    uint32_t line_count = *count_opt;
    lines.reserve(line_count);

    for (uint32_t i = 0; i < line_count; ++i) {
        auto wrapped_opt = reader.read_u8();
        auto cell_count_opt = reader.read_u32();
        if (!wrapped_opt || !cell_count_opt) break;

        protocol::ScrollbackLine line;
        line.wrapped = (*wrapped_opt != 0);
        uint32_t cell_count = *cell_count_opt;

        auto decode_res = protocol::decode_cell_runs_into(reader, line.cells, cell_count);
        if (!decode_res) break;

        lines.push_back(std::move(line));
    }

    decompressed_cache_.chunk_index = chunk_idx;
    decompressed_cache_.lines = lines;

    return lines;
}

} // namespace tmux2::vte
