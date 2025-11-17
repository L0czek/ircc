#ifndef __MESSAGE_HPP__
#define __MESSAGE_HPP__

#include <cstddef>
#include <cstdint>

constexpr std::size_t MESSAGE_SIZE = 256;

union message {
    struct {
        std::uint32_t crc32;
        std::uint8_t size;
        std::uint8_t reserved[3];
        std::byte data[
            MESSAGE_SIZE - 
                sizeof(std::uint8_t) - 
                sizeof(std::uint32_t) -
                sizeof(std::uint8_t[3])
        ];
    } fields;

    std::byte raw[MESSAGE_SIZE];
};

#define MESSAGE_SIZE(msg) (msg.fields.size + sizeof(std::uint8_t) - sizeof(std::uint32_t) - sizeof(std::uint8_t[3]))

#endif
