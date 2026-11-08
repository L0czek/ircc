#ifndef UI_GFX_HPP
#define UI_GFX_HPP

#include "ui/font.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace ui {

constexpr int kWidth = 128;
constexpr int kHeight = 128;
constexpr size_t kRowBytes = kWidth / 2;
constexpr size_t kFrameBytes = kRowBytes * kHeight;

/** @brief Gray level 0 (black) .. 15 (full) */
using Gray = uint8_t;

namespace gray {
constexpr Gray kOff = 0;
constexpr Gray kGrid = 2;
constexpr Gray kDim = 4;       // hints, disabled items, grid lines
constexpr Gray kLabel = 8;     // labels, units
constexpr Gray kText = 12;     // normal text
constexpr Gray kValue = 15;    // values, selection
}

struct Rect {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;

    int16_t right() const noexcept { return static_cast<int16_t>(x + w); }    // exclusive
    int16_t bottom() const noexcept { return static_cast<int16_t>(y + h); }   // exclusive
    bool empty() const noexcept { return w <= 0 || h <= 0; }
    Rect intersect(const Rect& o) const noexcept;
    Rect inset(int16_t dx, int16_t dy) const noexcept {
        return Rect{static_cast<int16_t>(x + dx), static_cast<int16_t>(y + dy),
                    static_cast<int16_t>(w - 2 * dx), static_cast<int16_t>(h - 2 * dy)};
    }
};

/**
 * @brief 128x128 4 bpp frame in the SSD1327 GDDRAM layout: 64 bytes per row, high nibble =
 *        left (even x) pixel. Sent to the panel unchanged.
 */
class Framebuffer {
public:
    uint8_t* data() noexcept { return pixels_.data(); }
    const uint8_t* data() const noexcept { return pixels_.data(); }
    const uint8_t* row(int y) const noexcept { return pixels_.data() + static_cast<size_t>(y) * kRowBytes; }

    Gray get(int x, int y) const noexcept {
        if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) {
            return 0;
        }
        const uint8_t b = pixels_[static_cast<size_t>(y) * kRowBytes + static_cast<size_t>(x / 2)];
        return (x & 1) ? static_cast<Gray>(b & 0x0F) : static_cast<Gray>(b >> 4);
    }

    void set(int x, int y, Gray g) noexcept {
        uint8_t& b = pixels_[static_cast<size_t>(y) * kRowBytes + static_cast<size_t>(x / 2)];
        if (x & 1) {
            b = static_cast<uint8_t>((b & 0xF0) | (g & 0x0F));
        } else {
            b = static_cast<uint8_t>((b & 0x0F) | (g << 4));
        }
    }

    void fill(Gray g) noexcept;

private:
    alignas(4) std::array<uint8_t, kFrameBytes> pixels_{};
};

/** @brief Monochrome bitmap (1 bpp, MSB first, rows padded to bytes) */
struct Bitmap {
    uint8_t width;
    uint8_t height;
    const uint8_t* bits;
};

enum class Align : uint8_t { Left, Center, Right };

/**
 * @brief Drawing on a Framebuffer: everything is clipped to the clip rectangle and offset
 *        by the origin (used for the burn-in pixel shift).
 */
class Canvas {
public:
    explicit Canvas(Framebuffer& fb) noexcept : fb_(fb) {}

    Framebuffer& framebuffer() noexcept { return fb_; }

    void set_origin(int16_t dx, int16_t dy) noexcept { ox_ = dx; oy_ = dy; }
    void set_clip(const Rect& clip) noexcept;
    void reset_clip() noexcept { clip_ = Rect{0, 0, kWidth, kHeight}; }
    const Rect& clip() const noexcept { return clip_; }

    void clear(Gray g = 0) noexcept { fb_.fill(g); }
    void pixel(int x, int y, Gray g) noexcept;
    void hline(int x, int y, int w, Gray g) noexcept;
    void vline(int x, int y, int h, Gray g) noexcept;
    void fill(const Rect& r, Gray g) noexcept;
    void frame(const Rect& r, Gray g) noexcept;
    void line(int x0, int y0, int x1, int y1, Gray g) noexcept;
    /** @brief Dotted horizontal line (every `step` pixels) */
    void dotted_hline(int x, int y, int w, Gray g, int step = 2) noexcept;
    void bitmap(int x, int y, const Bitmap& bm, Gray g) noexcept;

    /** @brief Draw UTF-8 text with its cell top at y; returns the x after the last glyph */
    int text(int x, int y, const char* s, const Font& font, Gray g) noexcept;
    /** @brief Text aligned inside [x, x + w) */
    int text(const Rect& r, const char* s, const Font& font, Gray g, Align align) noexcept;
    /** @brief Draw at most max_chars glyphs; an ellipsis replaces the rest */
    int text_clipped(int x, int y, const char* s, const Font& font, Gray g, int max_chars) noexcept;
    static int text_width(const char* s, const Font& font) noexcept;

private:
    void glyph(int x, int y, const Font& font, uint32_t codepoint, Gray g) noexcept;
    bool visible(int x, int y) const noexcept {
        return x >= clip_.x && y >= clip_.y && x < clip_.right() && y < clip_.bottom();
    }

    Framebuffer& fb_;
    Rect clip_{0, 0, kWidth, kHeight};
    int16_t ox_ = 0;
    int16_t oy_ = 0;
};

/**
 * @brief Finds the rows that changed since the previous flush (per-row hash), so only
 *        those are sent to the panel.
 */
class RowTracker {
public:
    struct Run {
        uint8_t y0;
        uint8_t y1;   // inclusive
    };

    /** @brief Compare with the last flushed frame; returns the number of runs written */
    size_t changed_runs(const Framebuffer& fb, Run* runs, size_t max_runs) noexcept;
    /** @brief Forget the panel content (after init / power on): everything is sent next time */
    void invalidate() noexcept { valid_ = false; }

private:
    static uint32_t hash_row(const uint8_t* row) noexcept;

    std::array<uint32_t, kHeight> hashes_{};
    bool valid_ = false;
};

} // namespace ui

#endif
