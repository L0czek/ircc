#ifndef __PROTOCOL_SCOPE_DATA_HPP__
#define __PROTOCOL_SCOPE_DATA_HPP__

/**
 * @file scope_data.hpp
 * @brief Payload of DATA frames carrying oscilloscope samples (frame::Type::Data)
 *
 * Payload layout (little endian):
 *
 *   offset size field
 *   0      2    transfer_id     low 16 bits of ScopeRead.transfer_id
 *   2      1    channel         scope channel index
 *   3      1    reserved (0)
 *   4      4    sample_offset   index of the first sample, relative to ScopeRead.start
 *   8      2    sample_count    samples in this frame (<= kSamplesPerFrame)
 *   10     2    reserved (0)
 *   12     2*n  samples         uint16, right-aligned ADC codes
 *
 * Frame flags: kFlagLastOfChannel on the last frame of each channel.
 * Frame seq: frame index within the transfer (mod 65536), for gap detection.
 *
 * Order: for each block of kSamplesPerFrame samples, one frame per requested channel
 * (ascending), so all channels arrive progressively in time order. Each frame stands
 * alone (own CRC, offset, channel): a lost or corrupt frame is re-requested with a
 * ScopeRead covering its range.
 */

#include "frame.hpp"
#include <cstddef>
#include <cstdint>

namespace scope_data {

constexpr size_t kHeaderSize = 12;
constexpr size_t kSamplesPerFrame = 1016;   // 12 + 2 * 1016 = 2044 <= frame::kMaxPayload
static_assert(kHeaderSize + 2 * kSamplesPerFrame <= frame::kMaxPayload);

constexpr uint8_t kFlagLastOfChannel = 0x01;

struct Header {
    uint16_t transfer_id;
    uint8_t channel;
    uint32_t sample_offset;
    uint16_t sample_count;
};

inline void write_header(uint8_t* payload, const Header& h) {
    frame::put_u16(payload + 0, h.transfer_id);
    payload[2] = h.channel;
    payload[3] = 0;
    frame::put_u32(payload + 4, h.sample_offset);
    frame::put_u16(payload + 8, h.sample_count);
    frame::put_u16(payload + 10, 0);
}

/** @return false if the payload is too short for its sample count */
inline bool read_header(const uint8_t* payload, size_t length, Header& h) {
    if (length < kHeaderSize) {
        return false;
    }
    h.transfer_id = frame::get_u16(payload + 0);
    h.channel = payload[2];
    h.sample_offset = frame::get_u32(payload + 4);
    h.sample_count = frame::get_u16(payload + 8);
    return h.sample_count <= kSamplesPerFrame && length == kHeaderSize + 2u * h.sample_count;
}

inline uint16_t sample(const uint8_t* payload, size_t index) {
    return frame::get_u16(payload + kHeaderSize + 2 * index);
}

} // namespace scope_data

#endif
