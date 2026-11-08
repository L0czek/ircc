#include "ui/menu.hpp"

#include "ui/format.hpp"
#include <cmath>
#include <cstring>

namespace ui {

namespace {

void draw_chevron(Canvas& c, int x, int y, Gray g) noexcept {
    // 3x5 ">"
    c.line(x, y, x + 2, y + 2, g);
    c.line(x + 2, y + 2, x, y + 4, g);
}

float clampf(float v, float lo, float hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

const char* MenuScreen::hints() const noexcept {
    if (editing_) {
        return "turn/↑↓ ←→digit •ok";
    }
    if (holding_) {
        return "keep holding...";
    }
    return "◀back  •select";
}

void MenuScreen::on_enter(Ui& ui) noexcept {
    model_.on_enter(ui);
    const size_t n = model_.count(ui);
    if (selected_ >= n) {
        selected_ = n ? n - 1 : 0;
    }
    editing_ = false;
    holding_ = false;
    hold_fired_ = false;
}

void MenuScreen::move(Ui& ui, int delta) noexcept {
    const size_t n = model_.count(ui);
    if (n == 0) {
        return;
    }
    int s = static_cast<int>(selected_) + delta;
    if (s < 0) s = 0;
    if (s >= static_cast<int>(n)) s = static_cast<int>(n) - 1;
    selected_ = static_cast<size_t>(s);
}

void MenuScreen::begin_edit(Ui& ui) noexcept {
    const MenuItem item = model_.item(ui, selected_);
    editing_ = true;
    edit_value_ = original_ = model_.value(ui, selected_);
    model_at_edit_ = original_;
    cursor_ = item.kind == ItemKind::Number && item.spec.step > 0.0f ? item.spec.step : 1.0f;
    live_pending_ = false;
    live_applied_at_ = ui.now();
}

void MenuScreen::end_edit(Ui& ui, bool commit) noexcept {
    if (!editing_) {
        return;
    }
    editing_ = false;
    const MenuItem item = model_.item(ui, selected_);
    if (commit) {
        if (!item.spec.live || live_pending_ || edit_value_ != original_) {
            (void)model_.set(ui, selected_, edit_value_);
        }
    } else if (item.spec.live && edit_value_ != original_) {
        (void)model_.set(ui, selected_, original_);
    }
    live_pending_ = false;
}

void MenuScreen::edit_by(Ui& ui, float steps) noexcept {
    const MenuItem item = model_.item(ui, selected_);
    if (item.kind == ItemKind::Choice) {
        const int n = static_cast<int>(item.spec.choices.size());
        if (n == 0) {
            return;
        }
        int idx = static_cast<int>(std::lround(edit_value_)) + static_cast<int>(steps);
        idx = ((idx % n) + n) % n;
        edit_value_ = static_cast<float>(idx);
    } else {
        float v = edit_value_ + steps * cursor_;
        v = clampf(v, item.spec.min, item.spec.max);
        if (item.spec.step > 0.0f) {
            v = std::round(v / item.spec.step) * item.spec.step;
            v = clampf(v, item.spec.min, item.spec.max);
        }
        edit_value_ = v;
    }
    if (item.spec.live) {
        live_pending_ = true;
    }
}

bool MenuScreen::on_edit_event(Ui& ui, const Event& e) noexcept {
    const MenuItem item = model_.item(ui, selected_);
    switch (e.type) {
        case EventType::Turn:
            edit_by(ui, static_cast<float>(e.delta) * (item.kind == ItemKind::Number ? e.accel : 1));
            return true;
        case EventType::Press:
        case EventType::Repeat:
            if (e.key == Key::Up) {
                edit_by(ui, 1.0f);
            } else if (e.key == Key::Down) {
                edit_by(ui, -1.0f);
            } else if (e.key == Key::Left && item.kind == ItemKind::Number) {
                if (cursor_ * 10.0f <= (item.spec.max - item.spec.min) * 1.0001f) {
                    cursor_ *= 10.0f;
                }
            } else if (e.key == Key::Right && item.kind == ItemKind::Number) {
                if (cursor_ / 10.0f >= item.spec.step * 0.999f) {
                    cursor_ /= 10.0f;
                }
            }
            return true;
        case EventType::Click:
            if (is_select(e.key)) {
                end_edit(ui, true);
            }
            return true;
        case EventType::Long:
            if (is_select(e.key)) {
                end_edit(ui, false);
                ui.toast("Cancelled", 800);
            }
            return true;
        default:
            return true;
    }
}

bool MenuScreen::on_event(Ui& ui, const Event& e) noexcept {
    if (hold_fired_) {
        if (e.type == EventType::Release && is_select(e.key)) {
            hold_fired_ = false;
        }
        return e.type != EventType::Turn ? is_select(e.key) : false;
    }
    if (holding_) {
        if (e.type == EventType::Release && is_select(e.key)) {
            holding_ = false;
            ui.toast("Hold to start", 1000);
        }
        return true;
    }
    if (editing_) {
        return on_edit_event(ui, e);
    }

    const size_t n = model_.count(ui);
    switch (e.type) {
        case EventType::Turn:
            move(ui, e.delta);
            return true;
        case EventType::Press:
        case EventType::Repeat:
            if (e.key == Key::Up) {
                move(ui, -1);
                return true;
            }
            if (e.key == Key::Down) {
                move(ui, 1);
                return true;
            }
            if (e.type == EventType::Press && is_select(e.key) && selected_ < n) {
                const MenuItem item = model_.item(ui, selected_);
                if (item.kind == ItemKind::HoldAction && item.enabled) {
                    holding_ = true;
                    hold_since_ = ui.now();
                    return true;
                }
            }
            if (e.type == EventType::Press && e.key == Key::Right && selected_ < n) {
                const MenuItem item = model_.item(ui, selected_);
                if (item.kind == ItemKind::Submenu && item.enabled) {
                    model_.activate(ui, selected_);
                    return true;
                }
            }
            return e.key != Key::Left;
        case EventType::Click: {
            if (!is_select(e.key) || selected_ >= n) {
                return false;
            }
            const MenuItem item = model_.item(ui, selected_);
            if (!item.enabled) {
                ui.toast("Not available", 1000);
                return true;
            }
            switch (item.kind) {
                case ItemKind::Submenu:
                case ItemKind::Action:
                    model_.activate(ui, selected_);
                    break;
                case ItemKind::Toggle:
                    (void)model_.set(ui, selected_, model_.value(ui, selected_) != 0.0f ? 0.0f : 1.0f);
                    break;
                case ItemKind::Number:
                case ItemKind::Choice:
                    begin_edit(ui);
                    break;
                case ItemKind::HoldAction:
                case ItemKind::Info:
                    break;
            }
            return true;
        }
        default:
            return false;
    }
}

void MenuScreen::tick(Ui& ui) noexcept {
    if (holding_ && ui.now() - hold_since_ >= kHoldMs) {
        holding_ = false;
        hold_fired_ = true;
        model_.activate(ui, selected_);
    }
    if (!editing_) {
        return;
    }
    const MenuItem item = model_.item(ui, selected_);
    if (item.spec.live) {
        if (live_pending_ && ui.now() - live_applied_at_ >= kLiveApplyMs) {
            (void)model_.set(ui, selected_, edit_value_);
            live_pending_ = false;
            live_applied_at_ = ui.now();
        }
    } else {
        const float now_value = model_.value(ui, selected_);
        const float tolerance = (item.spec.step > 0.0f ? item.spec.step : 1.0f) * 0.5f;
        if (std::fabs(now_value - model_at_edit_) > tolerance) {
            editing_ = false;
            ui.toast("Changed by host");
        }
    }
}

void MenuScreen::format_value(Ui& ui, size_t index, const MenuItem& item, float value, char* buf, size_t n) noexcept {
    buf[0] = '\0';
    switch (item.kind) {
        case ItemKind::Number:
            ui::format_value(buf, n, value, item.spec.decimals, item.spec.unit);
            break;
        case ItemKind::Toggle:
            std::strncpy(buf, value != 0.0f ? "ON" : "OFF", n - 1);
            buf[n - 1] = '\0';
            break;
        case ItemKind::Choice: {
            const size_t idx = static_cast<size_t>(std::lround(value));
            const char* text = idx < item.spec.choices.size() ? item.spec.choices[idx] : "?";
            std::strncpy(buf, text, n - 1);
            buf[n - 1] = '\0';
            break;
        }
        case ItemKind::Info:
        case ItemKind::Submenu:
        case ItemKind::Action:
        case ItemKind::HoldAction:
            model_.info_text(ui, index, buf, n);
            break;
    }
}

void MenuScreen::draw(Ui& ui, Canvas& c, const Rect& body) noexcept {
    const size_t n = model_.count(ui);

    // Title
    c.text(2, body.y, model_.title(ui), kFontBody, gray::kLabel);
    if (n > 0) {
        char pos[12];
        format_uint(pos, sizeof(pos), static_cast<uint32_t>(selected_ + 1));
        append(pos, sizeof(pos), "/");
        char total[6];
        format_uint(total, sizeof(total), static_cast<uint32_t>(n));
        append(pos, sizeof(pos), total);
        c.text(Rect{0, body.y, static_cast<int16_t>(kWidth - 2), 10}, pos, kFontSmall, gray::kDim, Align::Right);
    }
    const int top = body.y + 12;
    c.hline(0, top - 2, kWidth, gray::kGrid);

    const int visible = (body.bottom() - top) / kRowHeight;
    if (visible <= 0) {
        return;
    }
    if (selected_ < scroll_) {
        scroll_ = selected_;
    }
    if (selected_ >= scroll_ + static_cast<size_t>(visible)) {
        scroll_ = selected_ - static_cast<size_t>(visible) + 1;
    }
    if (scroll_ + static_cast<size_t>(visible) > n) {
        scroll_ = n > static_cast<size_t>(visible) ? n - static_cast<size_t>(visible) : 0;
    }

    const int text_right = n > static_cast<size_t>(visible) ? kWidth - 4 : kWidth - 2;
    for (int row = 0; row < visible; ++row) {
        const size_t i = scroll_ + static_cast<size_t>(row);
        if (i >= n) {
            break;
        }
        const MenuItem item = model_.item(ui, i);
        const int y = top + row * kRowHeight;
        const Rect line{0, static_cast<int16_t>(y - 1), static_cast<int16_t>(text_right + 1), kRowHeight};
        const bool sel = i == selected_;
        const bool edit = sel && editing_;

        Gray label_gray = item.enabled ? gray::kText : gray::kDim;
        Gray value_gray = item.enabled ? gray::kValue : gray::kDim;
        if (sel && !edit) {
            c.fill(line, holding_ ? gray::kDim : gray::kValue);
            if (holding_) {
                const uint32_t held = ui.now() - hold_since_;
                const int w = static_cast<int>((held > kHoldMs ? kHoldMs : held) * static_cast<uint32_t>(line.w) / kHoldMs);
                c.fill(Rect{line.x, line.y, static_cast<int16_t>(w), line.h}, gray::kValue);
            }
            label_gray = gray::kOff;
            value_gray = gray::kOff;
        } else if (edit) {
            c.fill(line, gray::kGrid);
        }

        char value[24];
        const float v = edit ? edit_value_ : model_.value(ui, i);
        format_value(ui, i, item, v, value, sizeof(value));
        const int value_w = Canvas::text_width(value, kFontBody);
        int label_chars = (text_right - 3 - value_w - (value[0] ? 4 : 0)) / kFontBody.width;
        if (item.kind == ItemKind::Submenu) {
            label_chars -= 1;
        }
        c.text_clipped(3, y, item.label, kFontBody, label_gray, label_chars);

        int vx = text_right - value_w;
        if (item.kind == ItemKind::Submenu) {
            vx -= 6;
            draw_chevron(c, text_right - 3, y + 2, sel && !edit ? gray::kOff : gray::kLabel);
        }
        if (edit) {
            c.fill(Rect{static_cast<int16_t>(vx - 2), static_cast<int16_t>(y - 1), static_cast<int16_t>(value_w + 3),
                        kRowHeight},
                   (ui.now() / 400) % 2 ? gray::kValue : gray::kText);
            c.text(vx, y, value, kFontBody, gray::kOff);
            if (item.kind == ItemKind::Number) {
                // Underline the digit the cursor step changes
                char digits[20];
                format_fixed(digits, sizeof(digits), v, item.spec.decimals);
                const char* dot = std::strchr(digits, '.');
                const int dp = dot ? static_cast<int>(dot - digits) : static_cast<int>(std::strlen(digits));
                const int p = static_cast<int>(std::lround(std::log10(cursor_)));
                const int idx = p >= 0 ? dp - 1 - p : dp - p;
                if (idx >= 0 && idx < static_cast<int>(std::strlen(digits)) && digits[idx] != '-') {
                    // The digit the cursor step changes: inverted inside the value box
                    const int cx = vx + idx * kFontBody.width;
                    c.fill(Rect{static_cast<int16_t>(cx), static_cast<int16_t>(y - 1),
                                static_cast<int16_t>(kFontBody.width), kRowHeight},
                           gray::kOff);
                    const char digit[2] = {digits[idx], '\0'};
                    c.text(cx, y, digit, kFontBody, gray::kValue);
                }
            }
        } else {
            c.text(vx, y, value, kFontBody, value_gray);
        }
    }

    // Scroll bar
    if (n > static_cast<size_t>(visible)) {
        const int track_y = top - 1;
        const int track_h = visible * kRowHeight;
        c.vline(kWidth - 2, track_y, track_h, gray::kGrid);
        const int thumb_h = track_h * visible / static_cast<int>(n) < 4 ? 4 : track_h * visible / static_cast<int>(n);
        const int thumb_y = track_y + (track_h - thumb_h) * static_cast<int>(scroll_) /
                                          static_cast<int>(n - static_cast<size_t>(visible));
        c.vline(kWidth - 2, thumb_y, thumb_h, gray::kLabel);
        c.vline(kWidth - 1, thumb_y, thumb_h, gray::kLabel);
    }
}

} // namespace ui
