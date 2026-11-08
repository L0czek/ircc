#include "ui/icons.hpp"

namespace ui {

namespace {

// 1 bpp, MSB = leftmost pixel, one byte per row
constexpr uint8_t kBolt[] = {
    0b00011000,   // ...XX
    0b00110000,   // ..XX.
    0b01100000,   // .XX..
    0b11111000,   // XXXXX
    0b00110000,   // ..XX.
    0b01100000,   // .XX..
    0b11000000,   // XX...
};

constexpr uint8_t kPlug[] = {
    0b01001000,   // .X..X.
    0b01001000,   // .X..X.
    0b11111100,   // XXXXXX
    0b11111100,   // XXXXXX
    0b01111000,   // .XXXX.
    0b00110000,   // ..XX..
    0b00110000,   // ..XX..
};

constexpr uint8_t kWarning[] = {
    0b00010000,   // ...X...
    0b00101000,   // ..X.X..
    0b00111000,   // ..XXX..
    0b01101100,   // .XX.XX.
    0b01111100,   // .XXXXX.
    0b11101110,   // XXX.XXX
    0b11111110,   // XXXXXXX
};

constexpr uint8_t kLink[] = {
    0b11111100,   // XXXXXX
    0b10000100,   // X....X
    0b10100100,   // X.X..X
    0b10010100,   // X..X.X
    0b10100100,   // X.X..X
    0b10000100,   // X....X
    0b11111100,   // XXXXXX
};

} // namespace

const Bitmap kIconBolt{5, 7, kBolt};
const Bitmap kIconPlug{6, 7, kPlug};
const Bitmap kIconWarning{7, 7, kWarning};
const Bitmap kIconLink{6, 7, kLink};

} // namespace ui
