#pragma once

#include "src/protocol/codec.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

// Forward declaration of internal zstd context types
typedef struct ZSTD_CCtx_s ZSTD_CCtx;
typedef struct ZSTD_DCtx_s ZSTD_DCtx;

namespace tmux2::protocol {

inline constexpr size_t kDefaultMaxUncompressedSize = 64 * 1024 * 1024; // 64 MB
inline constexpr int kDefaultCompressionLevel = 3;

class ZstdCompressor {
public:
    explicit ZstdCompressor(int default_level = kDefaultCompressionLevel);
    ~ZstdCompressor();

    ZstdCompressor(const ZstdCompressor&) = delete;
    ZstdCompressor& operator=(const ZstdCompressor&) = delete;
    ZstdCompressor(ZstdCompressor&& other) noexcept;
    ZstdCompressor& operator=(ZstdCompressor&& other) noexcept;

    [[nodiscard]] std::expected<std::vector<std::byte>, CodecError> compress(
        std::span<const std::byte> src, int level = -1);

    [[nodiscard]] std::expected<size_t, CodecError> compress_to(
        std::span<const std::byte> src, std::span<std::byte> dst, int level = -1);

    [[nodiscard]] int default_level() const noexcept { return default_level_; }
    void set_default_level(int level) noexcept { default_level_ = level; }

private:
    ZSTD_CCtx* cctx_{nullptr};
    int default_level_{kDefaultCompressionLevel};
};

class ZstdDecompressor {
public:
    ZstdDecompressor();
    ~ZstdDecompressor();

    ZstdDecompressor(const ZstdDecompressor&) = delete;
    ZstdDecompressor& operator=(const ZstdDecompressor&) = delete;
    ZstdDecompressor(ZstdDecompressor&& other) noexcept;
    ZstdDecompressor& operator=(ZstdDecompressor&& other) noexcept;

    [[nodiscard]] std::expected<std::vector<std::byte>, CodecError> decompress(
        std::span<const std::byte> src,
        size_t max_uncompressed_size = kDefaultMaxUncompressedSize);

    [[nodiscard]] std::expected<size_t, CodecError> decompress_to(
        std::span<const std::byte> src, std::span<std::byte> dst);

private:
    ZSTD_DCtx* dctx_{nullptr};
};

// Stateless convenience helpers
std::expected<std::vector<std::byte>, CodecError> zstd_compress(
    std::span<const std::byte> src, int level = kDefaultCompressionLevel);

std::expected<std::vector<std::byte>, CodecError> zstd_decompress(
    std::span<const std::byte> src,
    size_t max_uncompressed_size = kDefaultMaxUncompressedSize);

} // namespace tmux2::protocol
