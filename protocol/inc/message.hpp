#ifndef __MESSAGE_HPP__
#define __MESSAGE_HPP__

#include <cstddef>
#include <cstdint>

constexpr std::size_t MESSAGE_SIZE = 256;
constexpr std::size_t MESSAGE_HEADER_SIZE = sizeof(char[4]) + sizeof(std::uint32_t) + sizeof(std::uint8_t) + sizeof(std::uint8_t[3]);
constexpr std::byte MESSAGE_HEADER_ID[4] = { std::byte('M'), std::byte('S'), std::byte('G'), std::byte('S') };

union message {
    struct {
        std::byte header[4];
        std::uint32_t crc32;
        std::uint8_t size;
        std::uint8_t reserved[3];
        std::byte data[
            MESSAGE_SIZE - 
                sizeof(std::byte[4]) -
                sizeof(std::uint8_t) - 
                sizeof(std::uint32_t) -
                sizeof(std::uint8_t[3])
        ];
    } fields;

    std::byte raw[MESSAGE_SIZE];
};

#define GET_MESSAGE_SIZE(msg) (msg.fields.size + MESSAGE_HEADER_SIZE)

#endif
