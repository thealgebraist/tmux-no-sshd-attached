#pragma once

#include "src/protocol/codec.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace tmux2::protocol {

inline constexpr size_t kStreamHeaderSize = 8;
inline constexpr uint32_t kMaxPayloadLength = 64 * 1024 * 1024; // 64 MB maximum frame payload

// Stream Framing Flags (spec section 4.1)
inline constexpr uint8_t kFlagNone            = 0x00;
inline constexpr uint8_t kFlagZstdCompressed  = 0x01;
inline constexpr uint8_t kFlagFinStream       = 0x02;
inline constexpr uint8_t kFlagPriorityUrgent  = 0x04;

// Standard QUIC / Multiplex Stream IDs
inline constexpr uint16_t kControlStreamId = 0; // Bidirectional Control & Input Stream
inline constexpr uint16_t kRenderStreamId  = 2; // Server -> Client Render Stream

struct StreamHeader {
    uint16_t stream_id{0};
    uint8_t msg_type{0};
    uint8_t flags{0};
    uint32_t payload_len{0};

    [[nodiscard]] constexpr bool is_compressed() const noexcept {
        return (flags & kFlagZstdCompressed) != 0;
    }
    [[nodiscard]] constexpr bool is_fin() const noexcept {
        return (flags & kFlagFinStream) != 0;
    }
    [[nodiscard]] constexpr bool is_urgent() const noexcept {
        return (flags & kFlagPriorityUrgent) != 0;
    }

    constexpr void set_compressed(bool enabled = true) noexcept {
        if (enabled) flags |= kFlagZstdCompressed;
        else flags &= ~kFlagZstdCompressed;
    }

    constexpr void set_fin(bool enabled = true) noexcept {
        if (enabled) flags |= kFlagFinStream;
        else flags &= ~kFlagFinStream;
    }

    constexpr void set_urgent(bool enabled = true) noexcept {
        if (enabled) flags |= kFlagPriorityUrgent;
        else flags &= ~kFlagPriorityUrgent;
    }

    constexpr bool operator==(const StreamHeader&) const = default;
};

inline void encode_header_to(const StreamHeader& hdr, std::span<std::byte, kStreamHeaderSize> out) noexcept {
    uint16_t sid_le = to_little_endian(hdr.stream_id);
    uint32_t len_le = to_little_endian(hdr.payload_len);

    std::memcpy(out.data(), &sid_le, 2);
    out[2] = static_cast<std::byte>(hdr.msg_type);
    out[3] = static_cast<std::byte>(hdr.flags);
    std::memcpy(out.data() + 4, &len_le, 4);
}

inline std::array<std::byte, kStreamHeaderSize> encode_header(const StreamHeader& hdr) noexcept {
    std::array<std::byte, kStreamHeaderSize> buf{};
    encode_header_to(hdr, std::span<std::byte, kStreamHeaderSize>(buf));
    return buf;
}

inline void append_header(BinaryWriter& writer, const StreamHeader& hdr) {
    auto buf = encode_header(hdr);
    writer.write_bytes(buf);
}

inline std::expected<StreamHeader, CodecError> decode_header(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kStreamHeaderSize) {
        return std::unexpected(CodecError::Truncated);
    }

    StreamHeader hdr;
    uint16_t sid_raw;
    std::memcpy(&sid_raw, bytes.data(), 2);
    hdr.stream_id = from_little_endian(sid_raw);

    hdr.msg_type = static_cast<uint8_t>(bytes[2]);
    hdr.flags = static_cast<uint8_t>(bytes[3]);

    uint32_t len_raw;
    std::memcpy(&len_raw, bytes.data() + 4, 4);
    hdr.payload_len = from_little_endian(len_raw);

    if (hdr.payload_len > kMaxPayloadLength) {
        return std::unexpected(CodecError::VectorTooLong);
    }

    return hdr;
}

// Partial stream buffering helper: returns nullopt if header is not yet complete in the buffer
inline std::expected<std::optional<StreamHeader>, CodecError> try_decode_header(
    std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kStreamHeaderSize) {
        return std::nullopt;
    }
    auto res = decode_header(bytes.subspan(0, kStreamHeaderSize));
    if (!res) return std::unexpected(res.error());
    return *res;
}

} // namespace tmux2::protocol
