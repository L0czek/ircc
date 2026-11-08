#include "ui/gfx.hpp"

#include <algorithm>
#include <cstdlib>

namespace ui {

// ---------------------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------------------

uint32_t utf8_next(const char*& s) noexcept {
    const auto* p = reinterpret_cast<const uint8_t*>(s);
    const uint8_t c = p[0];
    if (c == 0) {
        return 0;
    }
    int extra = 0;
    uint32_t cp = 0;
    if (c < 0x80) {
        s += 1;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        extra = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        cp = c & 0x07;
    } else {
        s += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; ++i) {
        if ((p[i] & 0xC0) != 0x80) {
            s += i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    s += 1 + extra;
    return cp;
}

int utf8_length(const char* s) noexcept {
    int n = 0;
    while (utf8_next(s) != 0) {
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------------------
// Rect / Framebuffer
// ---------------------------------------------------------------------------------------

Rect Rect::intersect(const Rect& o) const noexcept {
    const int x0 = std::max<int>(x, o.x);
    const int y0 = std::max<int>(y, o.y);
    const int x1 = std::min<int>(right(), o.right());
    const int y1 = std::min<int>(bottom(), o.bottom());
    if (x1 <= x0 || y1 <= y0) {
        return Rect{static_cast<int16_t>(x0), static_cast<int16_t>(y0), 0, 0};
    }
    return Rect{static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0),
                static_cast<int16_t>(y1 - y0)};
}

void Framebuffer::fill(Gray g) noexcept {
    const uint8_t v = static_cast<uint8_t>(((g & 0x0F) << 4) | (g & 0x0F));
    pixels_.fill(v);
}

// ---------------------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------------------

void Canvas::set_clip(const Rect& clip) noexcept {
    clip_ = clip.intersect(Rect{0, 0, kWidth, kHeight});
}

void Canvas::pixel(int x, int y, Gray g) noexcept {
    x += ox_;
    y += oy_;
    if (visible(x, y)) {
        fb_.set(x, y, g);
    }
}

void Canvas::hline(int x, int y, int w, Gray g) noexcept {
    for (int i = 0; i < w; ++i) {
        pixel(x + i, y, g);
    }
}

void Canvas::vline(int x, int y, int h, Gray g) noexcept {
    for (int i = 0; i < h; ++i) {
        pixel(x, y + i, g);
    }
}

void Canvas::dotted_hline(int x, int y, int w, Gray g, int step) noexcept {
    for (int i = 0; i < w; i += step) {
        pixel(x + i, y, g);
    }
}

void Canvas::fill(const Rect& r, Gray g) noexcept {
    const Rect shifted{static_cast<int16_t>(r.x + ox_), static_cast<int16_t>(r.y + oy_), r.w, r.h};
    const Rect c = shifted.intersect(clip_);
    for (int y = c.y; y < c.bottom(); ++y) {
        for (int x = c.x; x < c.right(); ++x) {
            fb_.set(x, y, g);
        }
    }
}

void Canvas::frame(const Rect& r, Gray g) noexcept {
    if (r.empty()) {
        return;
    }
    hline(r.x, r.y, r.w, g);
    hline(r.x, r.bottom() - 1, r.w, g);
    vline(r.x, r.y, r.h, g);
    vline(r.right() - 1, r.y, r.h, g);
}

void Canvas::line(int x0, int y0, int x1, int y1, Gray g) noexcept {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        pixel(x0, y0, g);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void Canvas::bitmap(int x, int y, const Bitmap& bm, Gray g) noexcept {
    const int bpr = (bm.width + 7) / 8;
    for (int r = 0; r < bm.height; ++r) {
        for (int c = 0; c < bm.width; ++c) {
            if (bm.bits[r * bpr + c / 8] & (0x80 >> (c % 8))) {
                pixel(x + c, y + r, g);
            }
        }
    }
}

void Canvas::glyph(int x, int y, const Font& font, uint32_t codepoint, Gray g) noexcept {
    const uint8_t* bits = font.glyph(font.index(codepoint));
    for (int r = 0; r < font.height; ++r) {
        const uint8_t* row = bits + r * font.bytes_per_row;
        for (int c = 0; c < font.width; ++c) {
            if (row[c / 8] & (0x80 >> (c % 8))) {
                pixel(x + c, y + r, g);
            }
        }
    }
}

int Canvas::text(int x, int y, const char* s, const Font& font, Gray g) noexcept {
    if (s == nullptr) {
        return x;
    }
    for (uint32_t cp = utf8_next(s); cp != 0; cp = utf8_next(s)) {
        if (cp != ' ') {
            glyph(x, y, font, cp, g);
        }
        x += font.width;
    }
    return x;
}

int Canvas::text(const Rect& r, const char* s, const Font& font, Gray g, Align align) noexcept {
    const int w = text_width(s, font);
    int x = r.x;
    if (align == Align::Center) {
        x = r.x + (r.w - w) / 2;
    } else if (align == Align::Right) {
        x = r.right() - w;
    }
    const int y = r.y + (r.h - font.height) / 2;
    return text(x, y, s, font, g);
}

int Canvas::text_clipped(int x, int y, const char* s, const Font& font, Gray g, int max_chars) noexcept {
    if (s == nullptr || max_chars <= 0) {
        return x;
    }
    const int n = utf8_length(s);
    if (n <= max_chars) {
        return text(x, y, s, font, g);
    }
    for (int i = 0; i < max_chars - 1; ++i) {
        const uint32_t cp = utf8_next(s);
        if (cp != ' ') {
            glyph(x, y, font, cp, g);
        }
        x += font.width;
    }
    glyph(x, y, font, 0x2026, g);   // ellipsis
    return x + font.width;
}

int Canvas::text_width(const char* s, const Font& font) noexcept {
    return s ? utf8_length(s) * font.width : 0;
}

// ---------------------------------------------------------------------------------------
// RowTracker
// ---------------------------------------------------------------------------------------

uint32_t RowTracker::hash_row(const uint8_t* row) noexcept {
    // FNV-1a over the 64 bytes of a row
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < kRowBytes; ++i) {
        h = (h ^ row[i]) * 16777619u;
    }
    return h;
}

size_t RowTracker::changed_runs(const Framebuffer& fb, Run* runs, size_t max_runs) noexcept {
    size_t n = 0;
    int start = -1;
    for (int y = 0; y < kHeight; ++y) {
        const uint32_t h = hash_row(fb.row(y));
        const bool changed = !valid_ || h != hashes_[static_cast<size_t>(y)];
        hashes_[static_cast<size_t>(y)] = h;
        if (changed && start < 0) {
            start = y;
        }
        if ((!changed || y == kHeight - 1) && start >= 0) {
            const int end = changed ? y : y - 1;
            if (n < max_runs) {
                runs[n++] = Run{static_cast<uint8_t>(start), static_cast<uint8_t>(end)};
            } else if (n > 0) {
                runs[n - 1].y1 = static_cast<uint8_t>(end);   // merge into the last run
            }
            start = -1;
        }
    }
    valid_ = true;
    return n;
}

} // namespace ui
