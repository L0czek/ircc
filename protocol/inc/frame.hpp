#ifndef __PROTOCOL_FRAME_HPP__
#define __PROTOCOL_FRAME_HPP__

/**
 * @file frame.hpp
 * @brief Link framing shared by firmware and host (all links: USB CDC, ctrl UART, aux UART)
 *
 * Frame layout (little endian):
 *
 *   offset size field
 *   0      4    magic "QCW2"          resynchronisation point
 *   4      1    type                  Type below
 *   5      1    flags                 type specific (DATA: kFlagLast...)
 *   6      2    length                payload bytes, <= kMaxPayload
 *   8      2    length_check          ~length: a corrupted length is rejected at once
 *                                     instead of stalling the parser waiting for bytes
 *   10     2    seq                   REQUEST: id chosen by the host, echoed in the RESPONSE;
 *                                     EVENT: per-link counter; DATA: frame index within
 *                                     the transfer (gap detection)
 *   12     N    payload               protobuf (REQUEST/RESPONSE/EVENT) or binary (DATA)
 *   12+N   4    crc32                 CRC-32/MPEG-2 over bytes [0, 12+N): poly 0x04C11DB7,
 *                                     init 0xFFFFFFFF, no reflection, no final xor
 *                                     (= STM32 CRC unit defaults, byte input)
 *
 * Overhead is 16 bytes per frame. FrameParser reassembles frames from an arbitrary byte
 * stream (split or merged reads, garbage, bit errors) and resynchronises on the magic.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace frame {

constexpr std::array<uint8_t, 4> kMagic = {'Q', 'C', 'W', '2'};
constexpr size_t kHeaderSize = 12;
constexpr size_t kTrailerSize = 4;
constexpr size_t kOverhead = kHeaderSize + kTrailerSize;
constexpr size_t kMaxPayload = 2048;
constexpr size_t kMaxFrame = kOverhead + kMaxPayload;

enum class Type : uint8_t {
    Request = 1,    // host -> device, protobuf Request
    Response = 2,   // device -> host, protobuf Response answering the request with the same seq
    Event = 3,      // device -> host, unsolicited protobuf Response (e.g. transfer complete)
    Data = 4,       // device -> host, binary bulk data (see scope_data.hpp)
};

constexpr bool valid_type(uint8_t t) {
    return t >= static_cast<uint8_t>(Type::Request) && t <= static_cast<uint8_t>(Type::Data);
}

// ---------------------------------------------------------------------------
// CRC-32/MPEG-2 (software; the firmware may use the CRC peripheral instead)
// ---------------------------------------------------------------------------

namespace detail {
constexpr std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i << 24;
        for (int b = 0; b < 8; ++b) {
            c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
        }
        table[i] = c;
    }
    return table;
}
inline constexpr std::array<uint32_t, 256> kCrcTable = make_crc_table();
} // namespace detail

inline uint32_t crc32_mpeg2(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < len; ++i) {
        crc = (crc << 8) ^ detail::kCrcTable[((crc >> 24) ^ data[i]) & 0xFFu];
    }
    return crc;
}

/** @brief CRC over a contiguous buffer; context is passed through (e.g. a CRC peripheral) */
using CrcFn = uint32_t (*)(const uint8_t* data, size_t len, void* context);

inline uint32_t software_crc(const uint8_t* data, size_t len, void*) {
    return crc32_mpeg2(data, len);
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

inline void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

inline void put_u32(uint8_t* p, uint32_t v) {
    put_u16(p, static_cast<uint16_t>(v));
    put_u16(p + 2, static_cast<uint16_t>(v >> 16));
}

inline uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t get_u32(const uint8_t* p) {
    return get_u16(p) | (static_cast<uint32_t>(get_u16(p + 2)) << 16);
}

/** @brief Where the payload of a frame being built in `buffer` goes */
inline uint8_t* payload_area(uint8_t* buffer) {
    return buffer + kHeaderSize;
}

/**
 * @brief Finish a frame whose payload (payload_len bytes) is already at payload_area(buffer).
 * @return total frame size, or 0 if payload_len > kMaxPayload or buffer_size too small
 */
inline size_t finish(uint8_t* buffer, size_t buffer_size, Type type, uint8_t flags, uint16_t seq,
                     size_t payload_len, CrcFn crc = software_crc, void* crc_context = nullptr) {
    if (payload_len > kMaxPayload || buffer_size < kOverhead + payload_len) {
        return 0;
    }
    std::memcpy(buffer, kMagic.data(), kMagic.size());
    buffer[4] = static_cast<uint8_t>(type);
    buffer[5] = flags;
    put_u16(buffer + 6, static_cast<uint16_t>(payload_len));
    put_u16(buffer + 8, static_cast<uint16_t>(~payload_len));
    put_u16(buffer + 10, seq);
    put_u32(buffer + kHeaderSize + payload_len, crc(buffer, kHeaderSize + payload_len, crc_context));
    return kOverhead + payload_len;
}

/** @brief Build a frame from a separate payload buffer (copies the payload) */
inline size_t encode(uint8_t* buffer, size_t buffer_size, Type type, uint8_t flags, uint16_t seq,
                     const uint8_t* payload, size_t payload_len, CrcFn crc = software_crc,
                     void* crc_context = nullptr) {
    if (payload_len > kMaxPayload || buffer_size < kOverhead + payload_len) {
        return 0;
    }
    if (payload_len) {
        std::memmove(payload_area(buffer), payload, payload_len);
    }
    return finish(buffer, buffer_size, type, flags, seq, payload_len, crc, crc_context);
}

// ---------------------------------------------------------------------------
// Stream parser
// ---------------------------------------------------------------------------

struct View {
    Type type;
    uint8_t flags;
    uint16_t seq;
    const uint8_t* payload;
    size_t length;
};

struct ParserStats {
    uint32_t frames = 0;
    uint32_t crc_errors = 0;
    uint32_t header_errors = 0;
    uint32_t dropped_bytes = 0;   // bytes discarded while hunting for the magic
};

/**
 * @brief Incremental frame parser. feed() any chunking of the byte stream; the handler
 *        is called for every valid frame (the view is valid during the call only).
 *
 * Needs kMaxFrame bytes of buffer (member). Not thread safe.
 */
class Parser {
public:
    explicit Parser(CrcFn crc = software_crc, void* crc_context = nullptr) : crc_(crc), crc_context_(crc_context) {}

    template <typename Handler>
    void feed(const uint8_t* data, size_t len, Handler&& handler) {
        while (len > 0) {
            const size_t n = (len < kMaxFrame - fill_) ? len : kMaxFrame - fill_;
            std::memcpy(buffer_.data() + fill_, data, n);
            fill_ += n;
            data += n;
            len -= n;
            process(handler);
        }
    }

    /**
     * @brief Call when the link has been idle for a while (no bytes for a frame time).
     *        An incomplete frame is then abandoned and the buffered bytes rescanned, so a
     *        frame truncated by a sender reset cannot hide the frames after it until
     *        more traffic arrives.
     */
    template <typename Handler>
    void on_idle(Handler&& handler) {
        while (fill_ > 0) {
            ++stats_.header_errors;
            stats_.dropped_bytes += 1;
            drop(1);
            process(handler);
        }
    }

    bool idle() const { return fill_ == 0; }
    void reset() { fill_ = 0; }
    const ParserStats& stats() const { return stats_; }

private:
    std::array<uint8_t, kMaxFrame> buffer_{};
    size_t fill_ = 0;
    ParserStats stats_{};
    CrcFn crc_;
    void* crc_context_;

    void drop(size_t n) {
        std::memmove(buffer_.data(), buffer_.data() + n, fill_ - n);
        fill_ -= n;
    }

    // Number of leading bytes that cannot start a frame.
    size_t garbage_prefix() const {
        for (size_t i = 0; i < fill_; ++i) {
            const size_t avail = fill_ - i;
            const size_t cmp = avail < kMagic.size() ? avail : kMagic.size();
            if (std::memcmp(buffer_.data() + i, kMagic.data(), cmp) == 0) {
                return i;
            }
        }
        return fill_;
    }

    template <typename Handler>
    void process(Handler& handler) {
        for (;;) {
            if (const size_t skip = garbage_prefix(); skip > 0) {
                stats_.dropped_bytes += static_cast<uint32_t>(skip);
                drop(skip);
            }
            if (fill_ < kHeaderSize) {
                return;
            }
            const uint8_t* h = buffer_.data();
            const uint16_t length = get_u16(h + 6);
            const uint16_t check = get_u16(h + 8);
            if (static_cast<uint16_t>(~length) != check || length > kMaxPayload || !valid_type(h[4])) {
                ++stats_.header_errors;
                stats_.dropped_bytes += 1;
                drop(1);   // not a frame start: resync from the next byte
                continue;
            }
            const size_t total = kOverhead + length;
            if (fill_ < total) {
                return;
            }
            const uint32_t expected = get_u32(h + kHeaderSize + length);
            if (crc_(h, kHeaderSize + length, crc_context_) != expected) {
                ++stats_.crc_errors;
                stats_.dropped_bytes += 1;
                drop(1);
                continue;
            }
            ++stats_.frames;
            const View view{static_cast<Type>(h[4]), h[5], get_u16(h + 10), h + kHeaderSize, length};
            handler(view);
            drop(total);
        }
    }
};

} // namespace frame

#endif
