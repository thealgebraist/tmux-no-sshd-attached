#include "src/protocol/compression.hpp"
#include <zstd.h>
#include <utility>

namespace tmux2::protocol {

ZstdCompressor::ZstdCompressor(int default_level)
    : default_level_(default_level) {
    cctx_ = ZSTD_createCCtx();
}

ZstdCompressor::~ZstdCompressor() {
    if (cctx_) {
        ZSTD_freeCCtx(cctx_);
        cctx_ = nullptr;
    }
}

ZstdCompressor::ZstdCompressor(ZstdCompressor&& other) noexcept
    : cctx_(std::exchange(other.cctx_, nullptr)),
      default_level_(other.default_level_) {}

ZstdCompressor& ZstdCompressor::operator=(ZstdCompressor&& other) noexcept {
    if (this != &other) {
        if (cctx_) {
            ZSTD_freeCCtx(cctx_);
        }
        cctx_ = std::exchange(other.cctx_, nullptr);
        default_level_ = other.default_level_;
    }
    return *this;
}

std::expected<std::vector<std::byte>, CodecError> ZstdCompressor::compress(
    std::span<const std::byte> src, int level) {
    if (!cctx_) {
        cctx_ = ZSTD_createCCtx();
        if (!cctx_) return std::unexpected(CodecError::CompressionError);
    }

    int actual_level = (level < 0) ? default_level_ : level;
    size_t bound = ZSTD_compressBound(src.size());
    std::vector<std::byte> dst(bound);

    size_t const ret = ZSTD_compressCCtx(
        cctx_,
        dst.data(),
        dst.size(),
        src.data(),
        src.size(),
        actual_level);

    if (ZSTD_isError(ret)) {
        return std::unexpected(CodecError::CompressionError);
    }

    dst.resize(ret);
    return dst;
}

std::expected<size_t, CodecError> ZstdCompressor::compress_to(
    std::span<const std::byte> src, std::span<std::byte> dst, int level) {
    if (!cctx_) {
        cctx_ = ZSTD_createCCtx();
        if (!cctx_) return std::unexpected(CodecError::CompressionError);
    }

    int actual_level = (level < 0) ? default_level_ : level;
    size_t const ret = ZSTD_compressCCtx(
        cctx_,
        dst.data(),
        dst.size(),
        src.data(),
        src.size(),
        actual_level);

    if (ZSTD_isError(ret)) {
        return std::unexpected(CodecError::CompressionError);
    }

    return ret;
}

ZstdDecompressor::ZstdDecompressor() {
    dctx_ = ZSTD_createDCtx();
}

ZstdDecompressor::~ZstdDecompressor() {
    if (dctx_) {
        ZSTD_freeDCtx(dctx_);
        dctx_ = nullptr;
    }
}

ZstdDecompressor::ZstdDecompressor(ZstdDecompressor&& other) noexcept
    : dctx_(std::exchange(other.dctx_, nullptr)) {}

ZstdDecompressor& ZstdDecompressor::operator=(ZstdDecompressor&& other) noexcept {
    if (this != &other) {
        if (dctx_) {
            ZSTD_freeDCtx(dctx_);
        }
        dctx_ = std::exchange(other.dctx_, nullptr);
    }
    return *this;
}

std::expected<std::vector<std::byte>, CodecError> ZstdDecompressor::decompress(
    std::span<const std::byte> src, size_t max_uncompressed_size) {
    if (src.empty()) {
        return std::vector<std::byte>{};
    }

    if (!dctx_) {
        dctx_ = ZSTD_createDCtx();
        if (!dctx_) return std::unexpected(CodecError::DecompressionError);
    }

    unsigned long long const content_size = ZSTD_getFrameContentSize(src.data(), src.size());
    if (content_size == ZSTD_CONTENTSIZE_ERROR) {
        return std::unexpected(CodecError::DecompressionError);
    }

    if (content_size != ZSTD_CONTENTSIZE_UNKNOWN) {
        if (content_size > max_uncompressed_size) {
            return std::unexpected(CodecError::VectorTooLong);
        }
        std::vector<std::byte> dst(content_size);
        size_t const ret = ZSTD_decompressDCtx(
            dctx_,
            dst.data(),
            dst.size(),
            src.data(),
            src.size());

        if (ZSTD_isError(ret) || ret != content_size) {
            return std::unexpected(CodecError::DecompressionError);
        }
        return dst;
    }

    // Stream-growth fallback if frame content size is unknown
    size_t chunk_size = std::min<size_t>(src.size() * 4, 64 * 1024);
    std::vector<std::byte> dst;
    ZSTD_inBuffer input = {src.data(), src.size(), 0};

    while (input.pos < input.size) {
        size_t old_size = dst.size();
        if (old_size + chunk_size > max_uncompressed_size) {
            return std::unexpected(CodecError::VectorTooLong);
        }
        dst.resize(old_size + chunk_size);
        ZSTD_outBuffer output = {dst.data(), dst.size(), old_size};

        size_t const ret = ZSTD_decompressStream(dctx_, &output, &input);
        if (ZSTD_isError(ret)) {
            return std::unexpected(CodecError::DecompressionError);
        }

        if (output.pos < dst.size()) {
            dst.resize(output.pos);
        }
        if (ret == 0) {
            // Frame completed
            break;
        }
        chunk_size = std::min<size_t>(chunk_size * 2, 1024 * 1024);
    }

    return dst;
}

std::expected<size_t, CodecError> ZstdDecompressor::decompress_to(
    std::span<const std::byte> src, std::span<std::byte> dst) {
    if (!dctx_) {
        dctx_ = ZSTD_createDCtx();
        if (!dctx_) return std::unexpected(CodecError::DecompressionError);
    }

    size_t const ret = ZSTD_decompressDCtx(
        dctx_,
        dst.data(),
        dst.size(),
        src.data(),
        src.size());

    if (ZSTD_isError(ret)) {
        return std::unexpected(CodecError::DecompressionError);
    }

    return ret;
}

std::expected<std::vector<std::byte>, CodecError> zstd_compress(
    std::span<const std::byte> src, int level) {
    ZstdCompressor comp(level);
    return comp.compress(src, level);
}

std::expected<std::vector<std::byte>, CodecError> zstd_decompress(
    std::span<const std::byte> src, size_t max_uncompressed_size) {
    ZstdDecompressor decomp;
    return decomp.decompress(src, max_uncompressed_size);
}

} // namespace tmux2::protocol
