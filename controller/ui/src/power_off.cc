// SHUTDOWN key: soft stop (click) and ship-mode power off (hold). docs/lcd_ui.md "Power off".
#include "ui/format.hpp"
#include "ui/icons.hpp"
#include "ui/screens.hpp"
#include "widgets.hpp"
#include <cstring>

namespace ui {

namespace {

// The firmware reports the power off in system_status.power_off once its thread has taken
// the request. Without it for this long, the power off is over (cancelled by the host).
constexpr uint32_t kGoneMs = 1500;

bool send_power_off(Ui& ui, PowerOffAction action) noexcept {
    Request& r = ui.new_request();
    r.which_command = Request_power_off_tag;
    r.command.power_off.action = action;
    return ui.execute();
}

/**
 * Shows which power sources are still plugged in, then the count-down to the ship FET
 * turning off. Cancel: ◀, select or SHUTDOWN click.
 */
class PowerOffScreen final : public Screen {
public:
    void on_enter(Ui& ui) noexcept override {
        entered_ms_ = ui.now();
        last_seen_ms_ = ui.now();
        ui.poll_soon();
    }

    bool handles_shutdown_key() const noexcept override { return true; }
    bool keep_awake() const noexcept override { return true; }
    bool full_screen() const noexcept override { return true; }

    bool on_event(Ui& ui, const Event& e) noexcept override {
        const bool cancel = e.is(EventType::Press, Key::Left) || e.select_click() ||
                            e.is(EventType::Click, Key::Shutdown);
        if (cancel) {
            send_power_off(ui, PowerOffAction_POWER_OFF_CANCEL);
            ui.pop();
            ui.toast("Power off cancelled");
        }
        return true;   // nothing else while powering off
    }

    void tick(Ui& ui) noexcept override {
        if (ui.status_valid() && ui.status().has_power_off) {
            last_seen_ms_ = ui.now();
        } else if (ui.now() - last_seen_ms_ > kGoneMs && ui.now() - entered_ms_ > kGoneMs) {
            ui.pop();
            ui.toast("Power off cancelled");
        }
    }

    void draw(Ui& ui, Canvas& c, const Rect& body) noexcept override {
        (void)body;
        c.text(Rect{0, 2, kWidth, 20}, "Power off", kFontBig, gray::kValue, Align::Center);
        c.hline(8, 24, kWidth - 16, gray::kGrid);

        const bool have = ui.status_valid() && ui.status().has_power_off;
        const PowerOffStatus& p = ui.status().power_off;
        const PowerOffState state = have ? p.state : PowerOffState_POWER_OFF_WAIT_SOURCES;

        switch (state) {
            case PowerOffState_POWER_OFF_WAIT_SOURCES:
            case PowerOffState_POWER_OFF_DRAIN_VBUS:
                c.text(Rect{0, 30, kWidth, 10}, "Unplug all power:", kFontBody, gray::kText, Align::Center);
                draw_source(c, 46, "USB-C", have && p.usb_c_present);
                draw_source(c, 60, "Dock", have && p.dock_present);
                if (state == PowerOffState_POWER_OFF_DRAIN_VBUS) {
                    c.text(Rect{0, 80, kWidth, 10}, "Discharging input…", kFontBody, gray::kLabel, Align::Center);
                } else if (have && !p.battery_present) {
                    c.text(Rect{0, 78, kWidth, 8}, "No battery: the board", kFontSmall, gray::kLabel, Align::Center);
                    c.text(Rect{0, 87, kWidth, 8}, "turns off when unplugged", kFontSmall, gray::kLabel, Align::Center);
                }
                break;
            case PowerOffState_POWER_OFF_PENDING: {
                char buf[24];
                std::strcpy(buf, "Off in ");
                char tmp[8];
                const uint32_t ms = p.has_off_in_ms ? p.off_in_ms : 0;
                format_uint(tmp, sizeof(tmp), (ms + 999) / 1000);
                append(buf, sizeof(buf), tmp);
                append(buf, sizeof(buf), " s");
                c.text(Rect{0, 38, kWidth, 20}, buf, kFontBig, gray::kValue, Align::Center);
                c.text(Rect{0, 66, kWidth, 10}, "Battery disconnects", kFontBody, gray::kText, Align::Center);
                c.text(Rect{0, 82, kWidth, 8}, "Press POWER (QON)", kFontSmall, gray::kLabel, Align::Center);
                c.text(Rect{0, 91, kWidth, 8}, "to turn on again", kFontSmall, gray::kLabel, Align::Center);
                break;
            }
            case PowerOffState_POWER_OFF_FAILED:
                c.text(Rect{0, 40, kWidth, 10}, "Charger error", kFontBody, gray::kValue, Align::Center);
                c.text(Rect{0, 56, kWidth, 8}, "Cannot enter ship mode", kFontSmall, gray::kLabel, Align::Center);
                break;
            default:
                break;
        }
        c.hline(0, kHeight - kFooterHeightFull - 1, kWidth, gray::kGrid);
        c.text(Rect{0, kHeight - kFooterHeightFull, kWidth, kFooterHeightFull}, "◀ or • cancel", kFontSmall,
               gray::kDim, Align::Center);
    }

private:
    static constexpr int kFooterHeightFull = 9;

    static void draw_source(Canvas& c, int y, const char* name, bool present) noexcept {
        const Rect box{20, static_cast<int16_t>(y), 9, 9};
        if (present) {
            c.fill(box, gray::kValue);
        } else {
            c.frame(box, gray::kDim);
        }
        c.text(34, y, name, kFontBody, present ? gray::kValue : gray::kDim);
        c.text(Rect{70, static_cast<int16_t>(y), 50, 10}, present ? "plugged" : "unplugged", kFontSmall,
               present ? gray::kValue : gray::kDim, Align::Right);
    }

    uint32_t entered_ms_ = 0;
    uint32_t last_seen_ms_ = 0;
};

PowerOffScreen g_power_off;

} // namespace

Screen& power_off_screen() noexcept { return g_power_off; }

void Ui::soft_stop() noexcept {
    if (send_power_off(*this, PowerOffAction_POWER_OFF_SOFT)) {
        toast("Outputs stopped", 2500);
    }
}

void Ui::power_off() noexcept {
    if (!send_power_off(*this, PowerOffAction_POWER_OFF_SHIP)) {
        return;
    }
    if (top() == &g_power_off) {
        return;
    }
    if (depth_ >= stack_.size()) {
        home();
    }
    push(g_power_off);
}

} // namespace ui
