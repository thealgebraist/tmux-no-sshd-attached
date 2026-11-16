#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tmux2::protocol {

enum class CodecError : uint8_t {
    Success = 0,
    Truncated,
    InvalidData,
    StringTooLong,
    VectorTooLong,
    InvalidHeader,
    InvalidMagic,
    CompressionError,
    DecompressionError,
    UnknownMessageType,
    InvalidUtf8,
    Overflow
};

constexpr std::string_view codec_error_to_string(CodecError err) noexcept {
    switch (err) {
        case CodecError::Success: return "Success";
        case CodecError::Truncated: return "Truncated binary payload";
        case CodecError::InvalidData: return "Invalid binary data or corrupted field";
        case CodecError::StringTooLong: return "String length exceeds safety threshold";
        case CodecError::VectorTooLong: return "Vector element count exceeds safety threshold";
        case CodecError::InvalidHeader: return "Malformed framing header";
        case CodecError::InvalidMagic: return "Invalid magic identifier";
        case CodecError::CompressionError: return "zstd compression failed";
        case CodecError::DecompressionError: return "zstd decompression failed";
        case CodecError::UnknownMessageType: return "Unknown message type discriminator";
        case CodecError::InvalidUtf8: return "Invalid UTF-8 sequence in string";
        case CodecError::Overflow: return "Integer arithmetic overflow during decoding";
    }
    return "Unknown codec error";
}

// Endian conversions using C++23 <bit>
template <typename T>
    requires std::integral<T>
constexpr T to_little_endian(T val) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return val;
    } else {
        return std::byteswap(val);
    }
}

template <typename T>
    requires std::integral<T>
constexpr T from_little_endian(T val) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return val;
    } else {
        return std::byteswap(val);
    }
}

class BinaryReader {
public:
    constexpr explicit BinaryReader(std::span<const std::byte> buffer) noexcept
        : buffer_(buffer), offset_(0) {}

    constexpr explicit BinaryReader(std::span<const uint8_t> buffer) noexcept
        : buffer_(std::as_bytes(buffer)), offset_(0) {}

    constexpr explicit BinaryReader(std::string_view sv) noexcept
        : buffer_(std::as_bytes(std::span<const char>(sv.data(), sv.size()))), offset_(0) {}

    BinaryReader(const void* data, size_t size) noexcept
        : buffer_(static_cast<const std::byte*>(data), size), offset_(0) {}

    [[nodiscard]] constexpr size_t offset() const noexcept { return offset_; }
    [[nodiscard]] constexpr size_t remaining() const noexcept { return buffer_.size() - offset_; }
    [[nodiscard]] constexpr bool has_remaining(size_t n) const noexcept { return remaining() >= n; }
    [[nodiscard]] constexpr bool empty() const noexcept { return remaining() == 0; }

    constexpr std::expected<void, CodecError> skip(size_t n) noexcept {
        if (!has_remaining(n)) {
            return std::unexpected(CodecError::Truncated);
        }
        offset_ += n;
        return {};
    }

    template <typename T>
        requires std::integral<T>
    std::expected<T, CodecError> read_int() noexcept {
        if (!has_remaining(sizeof(T))) {
            return std::unexpected(CodecError::Truncated);
        }
        T raw_val;
        std::memcpy(&raw_val, buffer_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return from_little_endian(raw_val);
    }

    std::expected<uint8_t, CodecError> read_u8() noexcept { return read_int<uint8_t>(); }
    std::expected<uint16_t, CodecError> read_u16() noexcept { return read_int<uint16_t>(); }
    std::expected<uint32_t, CodecError> read_u32() noexcept { return read_int<uint32_t>(); }
    std::expected<uint64_t, CodecError> read_u64() noexcept { return read_int<uint64_t>(); }

    std::expected<int8_t, CodecError> read_i8() noexcept { return read_int<int8_t>(); }
    std::expected<int16_t, CodecError> read_i16() noexcept { return read_int<int16_t>(); }
    std::expected<int32_t, CodecError> read_i32() noexcept { return read_int<int32_t>(); }
    std::expected<int64_t, CodecError> read_i64() noexcept { return read_int<int64_t>(); }

    std::expected<bool, CodecError> read_bool() noexcept {
        auto val = read_u8();
        if (!val) return std::unexpected(val.error());
        if (*val > 1) return std::unexpected(CodecError::InvalidData);
        return *val != 0;
    }

    std::expected<std::span<const std::byte>, CodecError> read_bytes(size_t len) noexcept {
        if (!has_remaining(len)) {
            return std::unexpected(CodecError::Truncated);
        }
        std::span<const std::byte> result = buffer_.subspan(offset_, len);
        offset_ += len;
        return result;
    }

    std::expected<std::string_view, CodecError> read_string_view(size_t max_len = 16 * 1024 * 1024) noexcept {
        auto len_res = read_u32();
        if (!len_res) return std::unexpected(len_res.error());
        uint32_t len = *len_res;
        if (len > max_len) {
            return std::unexpected(CodecError::StringTooLong);
        }
        if (!has_remaining(len)) {
            return std::unexpected(CodecError::Truncated);
        }
        const char* p = reinterpret_cast<const char*>(buffer_.data() + offset_);
        offset_ += len;
        return std::string_view(p, len);
    }

    std::expected<std::string, CodecError> read_string(size_t max_len = 16 * 1024 * 1024) {
        auto sv_res = read_string_view(max_len);
        if (!sv_res) return std::unexpected(sv_res.error());
        return std::string(*sv_res);
    }

    std::expected<std::vector<uint8_t>, CodecError> read_byte_vector(size_t max_len = 64 * 1024 * 1024) {
        auto len_res = read_u32();
        if (!len_res) return std::unexpected(len_res.error());
        uint32_t len = *len_res;
        if (len > max_len) {
            return std::unexpected(CodecError::VectorTooLong);
        }
        auto bytes_res = read_bytes(len);
        if (!bytes_res) return std::unexpected(bytes_res.error());

        std::vector<uint8_t> result(len);
        if (len > 0) {
            std::memcpy(result.data(), bytes_res->data(), len);
        }
        return result;
    }

    // Unsigned LEB128 varint
    std::expected<uint64_t, CodecError> read_varint() noexcept {
        uint64_t result = 0;
        unsigned int shift = 0;
        while (shift < 64) {
            auto b_res = read_u8();
            if (!b_res) return std::unexpected(b_res.error());
            uint8_t byte = *b_res;
            result |= static_cast<uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                return result;
            }
            shift += 7;
        }
        return std::unexpected(CodecError::Overflow);
    }

    [[nodiscard]] constexpr std::span<const std::byte> remaining_span() const noexcept {
        return buffer_.subspan(offset_);
    }

private:
    std::span<const std::byte> buffer_;
    size_t offset_{0};
};

class BinaryWriter {
public:
    BinaryWriter() = default;
    explicit BinaryWriter(size_t reserve_bytes) {
        buffer_.reserve(reserve_bytes);
    }

    template <typename T>
        requires std::integral<T>
    void write_int(T val) {
        T le_val = to_little_endian(val);
        const auto* ptr = reinterpret_cast<const std::byte*>(&le_val);
        buffer_.insert(buffer_.end(), ptr, ptr + sizeof(T));
    }

    void write_u8(uint8_t val) { write_int<uint8_t>(val); }
    void write_u16(uint16_t val) { write_int<uint16_t>(val); }
    void write_u32(uint32_t val) { write_int<uint32_t>(val); }
    void write_u64(uint64_t val) { write_int<uint64_t>(val); }

    void write_i8(int8_t val) { write_int<int8_t>(val); }
    void write_i16(int16_t val) { write_int<int16_t>(val); }
    void write_i32(int32_t val) { write_int<int32_t>(val); }
    void write_i64(int64_t val) { write_int<int64_t>(val); }

    void write_bool(bool val) { write_u8(val ? 1 : 0); }

    void write_bytes(std::span<const std::byte> bytes) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    void write_bytes(std::span<const uint8_t> bytes) {
        const auto* ptr = reinterpret_cast<const std::byte*>(bytes.data());
        buffer_.insert(buffer_.end(), ptr, ptr + bytes.size());
    }

    void write_bytes(const void* data, size_t size) {
        const auto* ptr = static_cast<const std::byte*>(data);
        buffer_.insert(buffer_.end(), ptr, ptr + size);
    }

    void write_string(std::string_view sv) {
        write_u32(static_cast<uint32_t>(sv.size()));
        const auto* ptr = reinterpret_cast<const std::byte*>(sv.data());
        buffer_.insert(buffer_.end(), ptr, ptr + sv.size());
    }

    void write_byte_vector(std::span<const uint8_t> vec) {
        write_u32(static_cast<uint32_t>(vec.size()));
        write_bytes(vec);
    }

    void write_varint(uint64_t val) {
        while (val >= 0x80) {
            write_u8(static_cast<uint8_t>((val & 0x7F) | 0x80));
            val >>= 7;
        }
        write_u8(static_cast<uint8_t>(val & 0x7F));
    }

    // In-place patching
    void write_u8_at(size_t offset, uint8_t val) {
        if (offset < buffer_.size()) {
            buffer_[offset] = static_cast<std::byte>(val);
        }
    }

    void write_u16_at(size_t offset, uint16_t val) {
        if (offset + sizeof(uint16_t) <= buffer_.size()) {
            uint16_t le = to_little_endian(val);
            std::memcpy(buffer_.data() + offset, &le, sizeof(uint16_t));
        }
    }

    void write_u32_at(size_t offset, uint32_t val) {
        if (offset + sizeof(uint32_t) <= buffer_.size()) {
            uint32_t le = to_little_endian(val);
            std::memcpy(buffer_.data() + offset, &le, sizeof(uint32_t));
        }
    }

    void write_u64_at(size_t offset, uint64_t val) {
        if (offset + sizeof(uint64_t) <= buffer_.size()) {
            uint64_t le = to_little_endian(val);
            std::memcpy(buffer_.data() + offset, &le, sizeof(uint64_t));
        }
    }

    [[nodiscard]] std::span<const std::byte> span() const noexcept {
        return {buffer_.data(), buffer_.size()};
    }

    [[nodiscard]] std::span<const uint8_t> byte_span() const noexcept {
        return {reinterpret_cast<const uint8_t*>(buffer_.data()), buffer_.size()};
    }

    [[nodiscard]] const std::byte* data() const noexcept { return buffer_.data(); }
    [[nodiscard]] size_t size() const noexcept { return buffer_.size(); }
    [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }

    void clear() noexcept { buffer_.clear(); }
    void reserve(size_t cap) { buffer_.reserve(cap); }

    [[nodiscard]] const std::vector<std::byte>& buffer() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<std::byte>& buffer() noexcept { return buffer_; }

    [[nodiscard]] std::vector<std::byte> take() noexcept { return std::move(buffer_); }

    [[nodiscard]] std::vector<uint8_t> take_bytes() {
        std::vector<uint8_t> out(buffer_.size());
        if (!buffer_.empty()) {
            std::memcpy(out.data(), buffer_.data(), buffer_.size());
        }
        return out;
    }

private:
    std::vector<std::byte> buffer_;
};

} // namespace tmux2::protocol
