// Home dashboard pages, applet live view, input and display test screens
#include "ui/format.hpp"
#include "ui/screens.hpp"
#include "widgets.hpp"

#include <cmath>
#include <cstring>

namespace ui {

// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------

void draw_kv(Canvas& c, int y, const char* label, const char* value, Gray value_gray) noexcept {
    c.text(2, y, label, kFontBody, gray::kLabel);
    c.text(Rect{0, static_cast<int16_t>(y), kWidth - 2, 10}, value, kFontBody, value_gray, Align::Right);
}

void draw_dots(Canvas& c, const Rect& r, int count, int current) noexcept {
    const int w = count * 5 - 1;
    int x = r.x + (r.w - w) / 2;
    const int y = r.y + r.h / 2 - 1;
    for (int i = 0; i < count; ++i) {
        if (i == current) {
            c.fill(Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), 3, 3}, gray::kValue);
        } else {
            c.pixel(x + 1, y + 1, gray::kLabel);
        }
        x += 5;
    }
}

void draw_chart(Canvas& c, const Rect& r, const Series& s, const char* label, const char* unit, int decimals) noexcept {
    char buf[16];
    c.text(r.x + 1, r.y, label, kFontSmall, gray::kLabel);
    if (s.size() == 0) {
        c.text(Rect{r.x, r.y, static_cast<int16_t>(r.w - 1), 8}, "--", kFontSmall, gray::kDim, Align::Right);
        return;
    }
    format_value(buf, sizeof(buf), s.last(), decimals, unit);
    c.text(r.x + 1 + Canvas::text_width(label, kFontSmall) + 4, r.y, buf, kFontSmall, gray::kValue);

    float lo = s.at(0);
    float hi = lo;
    for (size_t i = 1; i < s.size(); ++i) {
        const float v = s.at(i);
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    // Minimum span so noise does not fill the chart
    const float min_span = decimals >= 2 ? 0.05f : (decimals == 1 ? 0.5f : 5.0f);
    if (hi - lo < min_span) {
        const float mid = (hi + lo) / 2.0f;
        lo = mid - min_span / 2.0f;
        hi = mid + min_span / 2.0f;
    }
    const Rect plot{r.x, static_cast<int16_t>(r.y + 9), static_cast<int16_t>(r.w - 26), static_cast<int16_t>(r.h - 9)};
    c.dotted_hline(plot.x, plot.y, plot.w, gray::kGrid, 3);
    c.dotted_hline(plot.x, plot.bottom() - 1, plot.w, gray::kGrid, 3);
    format_fixed(buf, sizeof(buf), hi, decimals);
    c.text(plot.right() + 2, plot.y, buf, kFontSmall, gray::kDim);
    format_fixed(buf, sizeof(buf), lo, decimals);
    c.text(plot.right() + 2, plot.bottom() - 7, buf, kFontSmall, gray::kDim);

    const size_t n = s.size();
    const int count = static_cast<int>(n < static_cast<size_t>(plot.w) ? n : static_cast<size_t>(plot.w));
    int px = -1;
    int py = 0;
    for (int i = 0; i < count; ++i) {
        const float v = s.at(n - static_cast<size_t>(count) + static_cast<size_t>(i));
        const int x = plot.right() - count + i;
        const int y = plot.bottom() - 1 - static_cast<int>(std::lround((v - lo) / (hi - lo) * static_cast<float>(plot.h - 1)));
        if (px >= 0) {
            c.line(px, py, x, y, gray::kValue);
        } else {
            c.pixel(x, y, gray::kValue);
        }
        px = x;
        py = y;
    }
}

const char* usb_state_text(UsbPdState s) noexcept {
    switch (s) {
        case UsbPdState_USB_PD_STATE_DETACHED: return "DETACHED";
        case UsbPdState_USB_PD_STATE_ATTACHED: return "ATTACHED";
        case UsbPdState_USB_PD_STATE_CONTRACT: return "CONTRACT";
        case UsbPdState_USB_PD_STATE_HARD_RESET: return "HARD RESET";
    }
    return "?";
}

const char* usb_role_text(UsbPdPowerRole r) noexcept {
    switch (r) {
        case UsbPdPowerRole_USB_PD_POWER_ROLE_NONE: return "-";
        case UsbPdPowerRole_USB_PD_POWER_ROLE_SINK: return "SINK";
        case UsbPdPowerRole_USB_PD_POWER_ROLE_SOURCE: return "SOURCE";
    }
    return "?";
}

const char* usb_source_text(UsbPdSourceState s) noexcept {
    switch (s) {
        case UsbPdSourceState_USB_PD_SOURCE_DISABLED: return "OFF";
        case UsbPdSourceState_USB_PD_SOURCE_READY: return "READY";
        case UsbPdSourceState_USB_PD_SOURCE_ACTIVE: return "ACTIVE";
        case UsbPdSourceState_USB_PD_SOURCE_BLOCKED_DOCK: return "BLK DOCK";
        case UsbPdSourceState_USB_PD_SOURCE_BLOCKED_BATTERY: return "BLK BATT";
        case UsbPdSourceState_USB_PD_SOURCE_BLOCKED_VOUT: return "BLK VOUT";
    }
    return "?";
}

const char* usb_path_text(UsbPdSourcePath p) noexcept {
    switch (p) {
        case UsbPdSourcePath_USB_PD_SOURCE_PATH_NONE: return "-";
        case UsbPdSourcePath_USB_PD_SOURCE_PATH_OTG: return "OTG";
        case UsbPdSourcePath_USB_PD_SOURCE_PATH_VOUT: return "VOUT";
    }
    return "?";
}

const char* input_text(const SystemStatus& s) noexcept {
    if (!s.has_charger || !s.charger.plugged_in) {
        return "BATTERY";
    }
    switch (s.charger.input_source) {
        case ChargerInputSource_CHARGER_INPUT_AC1: return "USB-C";
        case ChargerInputSource_CHARGER_INPUT_AC2: return "DOCK";
        default: return "VBUS";
    }
}

const char* regulator_mode_text(OutputRegulatorOperatingMode m) noexcept {
    switch (m) {
        case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BUCK: return "BUCK";
        case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BOOST: return "BOOST";
        case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BUCK_BOOST: return "BUCK-BOOST";
        case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_FREQUENCY_LIMIT: return "F-LIMIT";
    }
    return "?";
}

namespace {

constexpr int kLine = 10;

void two_values(char* buf, size_t n, float a, int da, const char* ua, float b, int db, const char* ub, bool plus_b = false) {
    char tmp[16];
    format_value(buf, n, a, da, ua);
    append(buf, n, " ");
    format_value(tmp, sizeof(tmp), b, db, ub, plus_b);
    append(buf, n, tmp);
}

// ---------------------------------------------------------------------------------------
// Dashboard
// ---------------------------------------------------------------------------------------

class Dashboard final : public Screen {
public:
    enum Page : int { Overview, Charger, Vout, Usb, Trends, Applet, PageCount };

    bool on_event(Ui& ui, const Event& e) noexcept override {
        switch (e.type) {
            case EventType::Turn:
                step(ui, e.delta > 0 ? 1 : -1);
                return true;
            case EventType::Press:
            case EventType::Repeat:
                if (e.key == Key::Left || e.key == Key::Right) {
                    step(ui, e.key == Key::Right ? 1 : -1);
                    return true;
                }
                if (page_ == Trends && (e.key == Key::Up || e.key == Key::Down)) {
                    trend_ = (trend_ + (e.key == Key::Down ? 1 : 2)) % 3;
                    return true;
                }
                return false;
            case EventType::Click:
                if (is_select(e.key)) {
                    const int running = ui.running_applet();
                    if (page_ == Applet && running >= 0) {
                        ui.push(applet_run_screen(static_cast<size_t>(running)));
                    } else {
                        ui.push(main_menu_screen());
                    }
                    return true;
                }
                return false;
            default:
                return false;
        }
    }

    void tick(Ui& ui) noexcept override {
        if (page_ == Applet && ui.running_applet() < 0) {
            page_ = Overview;
        }
    }

    void draw(Ui& ui, Canvas& c, const Rect& body) noexcept override {
        if (!ui.status_valid()) {
            c.text(body, "waiting for data", kFontBody, gray::kDim, Align::Center);
            return;
        }
        switch (page_) {
            case Overview: draw_overview(ui, c, body); break;
            case Charger: draw_charger(ui, c, body); break;
            case Vout: draw_vout(ui, c, body); break;
            case Usb: draw_usb(ui, c, body); break;
            case Trends: draw_trends(ui, c, body); break;
            case Applet: draw_applet(ui, c, body); break;
            default: break;
        }
    }

    void draw_footer(Ui& ui, Canvas& c, const Rect& footer) noexcept override {
        static const char* const kNames[] = {"Overview", "Charger", "VOUT", "USB-C", "Trends", "Applet"};
        c.text(footer.x + 1, footer.y + 1, kNames[page_], kFontSmall, gray::kLabel);
        const int pages = ui.running_applet() >= 0 ? PageCount : PageCount - 1;
        draw_dots(c, Rect{60, footer.y, 68, footer.h}, pages, page_);
    }

private:
    void step(Ui& ui, int delta) noexcept {
        const int pages = ui.running_applet() >= 0 ? PageCount : PageCount - 1;
        page_ = ((page_ + delta) % pages + pages) % pages;
    }

    static void draw_overview(Ui& ui, Canvas& c, const Rect& body) noexcept {
        const auto& s = ui.status();
        char buf[32];
        char tmp[16];
        int y = body.y;

        // Battery
        if (s.has_charger_measurements) {
            const auto& m = s.charger_measurements;
            two_values(buf, sizeof(buf), m.vbat_voltage, 2, "V", m.has_ibat_current ? m.ibat_current : NAN, 2, "A", true);
            draw_kv(c, y, "BAT", buf);
            y += kLine;
            if (s.has_charger && s.charger.battery_present) {
                format_uint(buf, sizeof(buf), static_cast<uint32_t>(battery_percent(s)));
                append(buf, sizeof(buf), "% ");
                append(buf, sizeof(buf), charge_state_text(s.charger.charge_status));
                if (s.has_charger_config && !s.charger.enabled) {
                    append(buf, sizeof(buf), " (off)");
                }
            } else {
                std::strcpy(buf, "no battery");
            }
            draw_kv(c, y, "", buf, gray::kText);
            y += kLine;
        }

        // Input
        if (s.has_charger_measurements) {
            const auto& m = s.charger_measurements;
            std::strcpy(buf, input_text(s));
            if (s.has_charger && s.charger.plugged_in) {
                append(buf, sizeof(buf), " ");
                format_value(tmp, sizeof(tmp), m.vbus_voltage, 1, "V");
                append(buf, sizeof(buf), tmp);
                append(buf, sizeof(buf), " ");
                format_value(tmp, sizeof(tmp), m.ibus_current, 2, "A");
                append(buf, sizeof(buf), tmp);
            }
            draw_kv(c, y, "IN", buf);
            y += kLine;
        }

        // VOUT
        if (s.has_output_regulator) {
            const bool on = s.output_regulator.enabled;
            if (on && s.has_output_regulator_measurements) {
                two_values(buf, sizeof(buf), s.output_regulator_measurements.output_voltage, 2, "V",
                           s.output_regulator_measurements.output_current, 2, "A");
            } else {
                std::strcpy(buf, "OFF");
                if (s.has_output_regulator_config && s.output_regulator_config.has_output_voltage) {
                    append(buf, sizeof(buf), " set ");
                    format_value(tmp, sizeof(tmp), s.output_regulator_config.output_voltage, 2, "V");
                    append(buf, sizeof(buf), tmp);
                }
            }
            draw_kv(c, y, "VOUT", buf, on ? gray::kValue : gray::kLabel);
            y += kLine;
        }

        // USB-C
        if (s.has_usb_pd) {
            const auto& u = s.usb_pd;
            if (u.state == UsbPdState_USB_PD_STATE_DETACHED) {
                std::strcpy(buf, "-");
            } else if (u.power_role == UsbPdPowerRole_USB_PD_POWER_ROLE_SOURCE) {
                std::strcpy(buf, "SRC 5V ");
                append(buf, sizeof(buf), u.has_source_path ? usb_path_text(u.source_path) : "");
            } else {
                std::strcpy(buf, "SINK ");
                format_value(tmp, sizeof(tmp), u.has_contract_voltage ? u.contract_voltage : 5.0f, 0, "V ");
                append(buf, sizeof(buf), tmp);
                format_value(tmp, sizeof(tmp), u.has_contract_current ? u.contract_current : 0.0f, 1, "A");
                append(buf, sizeof(buf), tmp);
            }
            draw_kv(c, y, "USB", buf, u.state == UsbPdState_USB_PD_STATE_DETACHED ? gray::kDim : gray::kValue);
            y += kLine;
        }

        // Applet
        c.dotted_hline(2, y + 2, kWidth - 4, gray::kGrid, 2);
        y += 5;
        const int running = ui.running_applet();
        if (running >= 0) {
            AppletControl* a = ui.backend().applet(static_cast<size_t>(running));
            std::strcpy(buf, a->name());
            const uint32_t started = ui.backend().applet_started_ms(static_cast<size_t>(running));
            if (started != 0) {
                append(buf, sizeof(buf), " ");
                format_duration(tmp, sizeof(tmp), (ui.now() - started) / 1000);
                append(buf, sizeof(buf), tmp);
            }
            c.text(2, y, "▶", kFontSmall, gray::kValue);
            c.text(10, y, buf, kFontBody, gray::kValue);
        } else {
            c.text(2, y, "no applet running", kFontBody, gray::kDim);
        }
    }

    static void draw_charger(Ui& ui, Canvas& c, const Rect& body) noexcept {
        const auto& s = ui.status();
        if (!s.has_charger_measurements) {
            c.text(body, "charger absent", kFontBody, gray::kDim, Align::Center);
            return;
        }
        const auto& m = s.charger_measurements;
        char buf[24];
        int y = body.y;
        two_values(buf, sizeof(buf), m.vbus_voltage, 2, "V", m.ibus_current, 2, "A");
        draw_kv(c, y, "VBUS", buf);
        y += kLine;
        two_values(buf, sizeof(buf), m.vbat_voltage, 2, "V", m.has_ibat_current ? m.ibat_current : NAN, 2, "A", true);
        draw_kv(c, y, "VBAT", buf);
        y += kLine;
        format_value(buf, sizeof(buf), m.vsys_voltage, 2, "V");
        draw_kv(c, y, "VSYS", buf);
        y += kLine;
        two_values(buf, sizeof(buf), m.has_vac1_voltage ? m.vac1_voltage : NAN, 1, "V",
                   m.has_vac2_voltage ? m.vac2_voltage : NAN, 1, "V");
        draw_kv(c, y, "AC1/2", buf, gray::kText);
        y += kLine;
        if (s.has_charger) {
            draw_kv(c, y, "State", charge_state_text(s.charger.charge_status));
            y += kLine;
            draw_kv(c, y, "Input", input_text(s));
            y += kLine;
        }
        if (s.has_charger_config) {
            const auto& cfg = s.charger_config;
            two_values(buf, sizeof(buf), cfg.input_current_limit, 2, "A", cfg.input_voltage_limit, 1, "V");
            const bool iindpm = s.has_charger && s.charger.has_status_flags && (s.charger.status_flags & (1u << 7));
            const bool vindpm = s.has_charger && s.charger.has_status_flags && (s.charger.status_flags & (1u << 6));
            draw_kv(c, y, iindpm || vindpm ? "Limit •" : "Limit", buf, iindpm || vindpm ? gray::kValue : gray::kText);
            y += kLine;
            two_values(buf, sizeof(buf), cfg.charge_voltage_limit, 2, "V", cfg.charge_current_limit, 2, "A");
            draw_kv(c, y, "Charge", buf, gray::kText);
            y += kLine;
        }
        if (s.has_charger && s.charger.fault_present) {
            draw_kv(c, y, "Fault", "YES", gray::kValue);
        }
    }

    static void draw_vout(Ui& ui, Canvas& c, const Rect& body) noexcept {
        const auto& s = ui.status();
        if (!s.has_output_regulator) {
            c.text(body, "regulator absent", kFontBody, gray::kDim, Align::Center);
            return;
        }
        const auto& st = s.output_regulator;
        char buf[24];
        char tmp[16];
        const float v = s.has_output_regulator_measurements ? s.output_regulator_measurements.output_voltage : 0.0f;
        const float i = s.has_output_regulator_measurements ? s.output_regulator_measurements.output_current : 0.0f;

        c.text(2, body.y, st.enabled ? "ON" : "OFF", kFontBody, st.enabled ? gray::kValue : gray::kLabel);
        c.text(Rect{0, body.y, kWidth - 2, 10}, regulator_mode_text(st.operating_mode), kFontSmall, gray::kLabel,
               Align::Right);
        format_value(buf, sizeof(buf), v, 2, "V");
        c.text(Rect{0, static_cast<int16_t>(body.y + 11), kWidth, 20}, buf, kFontBig,
               st.enabled ? gray::kValue : gray::kDim, Align::Center);
        int y = body.y + 33;
        format_value(buf, sizeof(buf), i, 2, "A  ");
        format_value(tmp, sizeof(tmp), v * i, 1, "W");
        append(buf, sizeof(buf), tmp);
        c.text(Rect{0, static_cast<int16_t>(y), kWidth, 10}, buf, kFontBody, gray::kText, Align::Center);
        y += 11;
        if (s.has_output_regulator_config) {
            const auto& cfg = s.output_regulator_config;
            std::strcpy(buf, "set ");
            format_value(tmp, sizeof(tmp), cfg.has_output_voltage ? cfg.output_voltage : NAN, 2, "V lim ");
            append(buf, sizeof(buf), tmp);
            format_value(tmp, sizeof(tmp), cfg.has_output_current_limit ? cfg.output_current_limit : NAN, 2, "A");
            append(buf, sizeof(buf), tmp);
            c.text(Rect{0, static_cast<int16_t>(y), kWidth, 10}, buf, kFontSmall, gray::kLabel, Align::Center);
        }
        y += 9;
        if (st.fault_present) {
            std::strcpy(buf, "FAULT");
            if (st.scp_fault) append(buf, sizeof(buf), " SCP");
            if (st.ocp_fault) append(buf, sizeof(buf), " OCP");
            if (st.ovp_fault) append(buf, sizeof(buf), " OVP");
            c.text(Rect{0, static_cast<int16_t>(y), kWidth, 10}, buf, kFontBody, gray::kValue, Align::Center);
        }
        draw_chart(c, Rect{2, static_cast<int16_t>(body.bottom() - 32), kWidth - 4, 32}, ui.history(Signal::Iout),
                   "IOUT", "A", 2);
    }

    static void draw_usb(Ui& ui, Canvas& c, const Rect& body) noexcept {
        const auto& s = ui.status();
        if (!s.has_usb_pd) {
            c.text(body, "no USB-C port", kFontBody, gray::kDim, Align::Center);
            return;
        }
        const auto& u = s.usb_pd;
        char buf[24];
        char tmp[16];
        int y = body.y;
        draw_kv(c, y, "State", usb_state_text(u.state));
        y += kLine;
        std::strcpy(buf, usb_role_text(u.power_role));
        if (u.has_data_role && u.data_role != UsbPdDataRole_USB_PD_DATA_ROLE_NONE) {
            append(buf, sizeof(buf), u.data_role == UsbPdDataRole_USB_PD_DATA_ROLE_DFP ? " DFP" : " UFP");
        }
        draw_kv(c, y, "Role", buf);
        y += kLine;
        if (u.has_contract_voltage) {
            two_values(buf, sizeof(buf), u.contract_voltage, 1, "V", u.has_contract_current ? u.contract_current : NAN, 2, "A");
        } else {
            std::strcpy(buf, "-");
        }
        draw_kv(c, y, "Contract", buf);
        y += kLine;
        if (u.has_vbus_voltage) {
            format_value(buf, sizeof(buf), u.vbus_voltage, 2, "V");
            draw_kv(c, y, "VBUS", buf);
            y += kLine;
        }
        if (u.has_input_current_limit) {
            format_value(buf, sizeof(buf), u.input_current_limit, 2, "A");
            draw_kv(c, y, "In limit", buf, gray::kText);
            y += kLine;
        }
        std::strcpy(buf, usb_source_text(u.source_state));
        if (u.has_source_path && u.source_path != UsbPdSourcePath_USB_PD_SOURCE_PATH_NONE) {
            append(buf, sizeof(buf), " ");
            append(buf, sizeof(buf), usb_path_text(u.source_path));
        }
        draw_kv(c, y, "Source", buf);
        y += kLine;
        if (u.has_cc_line && u.cc_line != 0) {
            std::strcpy(buf, u.cc_line == 1 ? "CC1" : "CC2");
            draw_kv(c, y, "CC", buf, gray::kText);
            y += kLine;
        }
        if (u.has_hard_resets) {
            format_uint(tmp, sizeof(tmp), u.hard_resets);
            draw_kv(c, y, "Hard rst", tmp, u.hard_resets ? gray::kValue : gray::kText);
        }
    }

    void draw_trends(Ui& ui, Canvas& c, const Rect& body) noexcept {
        static constexpr struct {
            Signal a, b;
            const char *la, *lb;
        } kPairs[] = {
            {Signal::Vbat, Signal::Ibat, "VBAT", "IBAT"},
            {Signal::Vbus, Signal::Ibus, "VBUS", "IBUS"},
            {Signal::Vout, Signal::Iout, "VOUT", "IOUT"},
        };
        const auto& p = kPairs[trend_];
        const int16_t h = static_cast<int16_t>((body.h - 3) / 2);
        draw_chart(c, Rect{2, body.y, kWidth - 4, h}, ui.history(p.a), p.la, "V", 2);
        draw_chart(c, Rect{2, static_cast<int16_t>(body.y + h + 3), kWidth - 4, h}, ui.history(p.b), p.lb, "A", 2);
    }

    static void draw_applet(Ui& ui, Canvas& c, const Rect& body) noexcept {
        const int running = ui.running_applet();
        if (running < 0) {
            return;
        }
        AppletControl* a = ui.backend().applet(static_cast<size_t>(running));
        char buf[24];
        int y = body.y;
        c.text(2, y, a->name(), kFontBody, gray::kValue);
        c.text(Rect{0, static_cast<int16_t>(y), kWidth - 2, 10}, applet_run_text(a->run_state()), kFontSmall,
               gray::kLabel, Align::Right);
        y += 12;
        const auto readouts = a->readouts();
        for (size_t i = 0; i < readouts.size() && y < body.bottom() - 10; ++i) {
            format_value(buf, sizeof(buf), a->readout(i), readouts[i].decimals, readouts[i].unit);
            draw_kv(c, y, readouts[i].label, buf);
            y += kLine;
        }
        const auto params = a->params();
        for (size_t i = 0; i < params.size() && y < body.bottom() - 10; ++i) {
            if (params[i].type != ParamType::Number) {
                continue;
            }
            format_value(buf, sizeof(buf), a->get_param(i), params[i].decimals, params[i].unit);
            draw_kv(c, y, params[i].label, buf, gray::kText);
            y += kLine;
        }
    }

    int page_ = Overview;
    int trend_ = 0;
};

// ---------------------------------------------------------------------------------------
// Applet live view
// ---------------------------------------------------------------------------------------

class AppletRunScreen final : public Screen {
public:
    void set_applet(size_t index) noexcept {
        applet_ = index;
        selected_ = 0;
    }

    void on_enter(Ui& ui) noexcept override {
        (void)ui;
        pending_ = false;
    }

    bool on_event(Ui& ui, const Event& e) noexcept override {
        AppletControl* a = ui.backend().applet(applet_);
        if (!a) {
            return false;
        }
        const size_t live = live_count(*a);
        switch (e.type) {
            case EventType::Turn:
                if (live > 0 && a->run_state() == AppletRun::Running) {
                    const size_t p = live_index(*a, selected_);
                    const ParamDesc& d = a->params()[p];
                    float v = (pending_ ? pending_value_ : a->get_param(p)) +
                              static_cast<float>(e.delta) * static_cast<float>(e.accel) * d.step;
                    v = v < d.min ? d.min : (v > d.max ? d.max : v);
                    pending_value_ = v;
                    pending_ = true;
                }
                return true;
            case EventType::Press:
            case EventType::Repeat:
                if ((e.key == Key::Up || e.key == Key::Down) && live > 0) {
                    commit(ui);
                    selected_ = (selected_ + (e.key == Key::Down ? 1 : live - 1)) % live;
                    return true;
                }
                return e.key != Key::Left;
            case EventType::Click:
                if (is_select(e.key)) {
                    if (a->run_state() == AppletRun::Running) {
                        commit(ui);
                        ui.stop_applet(applet_);
                        ui.toast("Stopped");
                    }
                    ui.pop();
                    return true;
                }
                return false;
            default:
                return false;
        }
    }

    void tick(Ui& ui) noexcept override {
        if (pending_ && ui.now() - applied_at_ >= 100) {
            commit(ui);
        }
    }

    void draw(Ui& ui, Canvas& c, const Rect& body) noexcept override {
        AppletControl* a = ui.backend().applet(applet_);
        if (!a) {
            return;
        }
        char buf[24];
        int y = body.y;
        c.text(2, y, a->name(), kFontBody, gray::kValue);
        const AppletRun state = a->run_state();
        std::strcpy(buf, applet_run_text(state));
        const uint32_t started = ui.backend().applet_started_ms(applet_);
        if (state == AppletRun::Running && started != 0) {
            char tmp[12];
            append(buf, sizeof(buf), " ");
            format_duration(tmp, sizeof(tmp), (ui.now() - started) / 1000);
            append(buf, sizeof(buf), tmp);
        }
        c.text(Rect{0, static_cast<int16_t>(y), kWidth - 2, 10}, buf, kFontSmall,
               state == AppletRun::Running ? gray::kValue : gray::kLabel, Align::Right);
        y += 12;

        const auto readouts = a->readouts();
        for (size_t i = 0; i < readouts.size() && y < body.bottom() - 10; ++i) {
            format_value(buf, sizeof(buf), a->readout(i), readouts[i].decimals, readouts[i].unit);
            draw_kv(c, y, readouts[i].label, buf);
            y += kLine;
        }
        if (!readouts.empty()) {
            c.dotted_hline(2, y + 1, kWidth - 4, gray::kGrid, 2);
            y += 4;
        }

        const size_t live = live_count(*a);
        for (size_t k = 0; k < live && y < body.bottom() - 10; ++k) {
            const size_t p = live_index(*a, k);
            const ParamDesc& d = a->params()[p];
            const float v = (k == selected_ && pending_) ? pending_value_ : a->get_param(p);
            format_value(buf, sizeof(buf), v, d.decimals, d.unit);
            if (k == selected_) {
                c.fill(Rect{0, static_cast<int16_t>(y - 1), kWidth, 11}, gray::kGrid);
                c.text(2, y, "▶", kFontSmall, gray::kValue);
            }
            c.text(9, y, d.label, kFontBody, k == selected_ ? gray::kValue : gray::kLabel);
            c.text(Rect{0, static_cast<int16_t>(y), kWidth - 2, 10}, buf, kFontBody, gray::kValue, Align::Right);
            y += 11;
        }
        if (state != AppletRun::Running) {
            c.text(Rect{0, static_cast<int16_t>(body.bottom() - 12), kWidth, 10}, "not running", kFontBody, gray::kDim,
                   Align::Center);
        }
    }

    const char* hints() const noexcept override { return "turn adjust ↑↓ sel •STOP"; }

private:
    static size_t live_count(const AppletControl& a) noexcept {
        size_t n = 0;
        for (const auto& d : a.params()) {
            if ((d.flags & kParamLive) && d.type == ParamType::Number) {
                ++n;
            }
        }
        return n;
    }

    static size_t live_index(const AppletControl& a, size_t k) noexcept {
        const auto params = a.params();
        for (size_t i = 0; i < params.size(); ++i) {
            if ((params[i].flags & kParamLive) && params[i].type == ParamType::Number) {
                if (k == 0) {
                    return i;
                }
                --k;
            }
        }
        return 0;
    }

    void commit(Ui& ui) noexcept {
        if (!pending_) {
            return;
        }
        pending_ = false;
        applied_at_ = ui.now();
        AppletControl* a = ui.backend().applet(applet_);
        if (!a) {
            return;
        }
        auto r = ui.backend().applet_set(applet_, live_index(*a, selected_), pending_value_);
        if (!r) {
            ui.toast(applet_error_text(r.error()));
        }
    }

    size_t applet_ = 0;
    size_t selected_ = 0;
    bool pending_ = false;
    float pending_value_ = 0.0f;
    uint32_t applied_at_ = 0;
};

// ---------------------------------------------------------------------------------------
// Input test
// ---------------------------------------------------------------------------------------

class InputTest final : public Screen {
public:
    bool on_event(Ui& ui, const Event& e) noexcept override {
        if (e.is(EventType::Long, Key::Push)) {
            ui.pop();
        }
        last_ = e;
        have_last_ = true;
        return true;
    }

    bool full_screen() const noexcept override { return true; }

    void draw(Ui& ui, Canvas& c, const Rect& body) noexcept override {
        (void)body;
        const uint8_t keys = ui.key_tracker().pressed();
        c.text(Rect{0, 0, kWidth, 10}, "Input test", kFontBody, gray::kLabel, Align::Center);
        auto box = [&](int x, int y, Key k, const char* label) {
            const bool on = keys & key_bit(k);
            const Rect r{static_cast<int16_t>(x), static_cast<int16_t>(y), 16, 14};
            if (on) {
                c.fill(r, gray::kValue);
            } else {
                c.frame(r, gray::kLabel);
            }
            c.text(r, label, kFontSmall, on ? gray::kOff : gray::kLabel, Align::Center);
        };
        // Joystick cross
        box(24, 16, Key::Up, "U");
        box(6, 32, Key::Left, "L");
        box(24, 32, Key::Center, "C");
        box(42, 32, Key::Right, "R");
        box(24, 48, Key::Down, "D");
        // Encoder and SHUTDOWN
        box(78, 16, Key::Push, "P");
        box(100, 16, Key::Shutdown, "S");
        char buf[24];
        std::strcpy(buf, "enc ");
        char tmp[12];
        const int32_t pos = ui.encoder_position();
        if (pos < 0) {
            append(buf, sizeof(buf), "-");
        }
        format_uint(tmp, sizeof(tmp), static_cast<uint32_t>(pos < 0 ? -pos : pos));
        append(buf, sizeof(buf), tmp);
        c.text(70, 36, buf, kFontBody, gray::kValue);
        std::strcpy(buf, "raw 0x");
        static const char kHex[] = "0123456789ABCDEF";
        const uint8_t raw = ui.raw_keys();
        const char hex[3] = {kHex[raw >> 4], kHex[raw & 15], 0};
        append(buf, sizeof(buf), hex);
        c.text(70, 48, buf, kFontSmall, gray::kLabel);

        if (have_last_) {
            static const char* const kTypes[] = {"press", "release", "click", "long", "repeat", "turn"};
            static const char* const kKeys[] = {"up", "down", "left", "right", "centre", "push", "shutdown", ""};
            std::strcpy(buf, kTypes[static_cast<int>(last_.type)]);
            append(buf, sizeof(buf), " ");
            if (last_.type == EventType::Turn) {
                format_fixed(tmp, sizeof(tmp), static_cast<float>(last_.delta), 0, true);
                append(buf, sizeof(buf), tmp);
                append(buf, sizeof(buf), " x");
                format_uint(tmp, sizeof(tmp), last_.accel);
                append(buf, sizeof(buf), tmp);
            } else {
                append(buf, sizeof(buf), kKeys[static_cast<int>(last_.key)]);
            }
            c.text(Rect{0, 74, kWidth, 10}, buf, kFontBody, gray::kValue, Align::Center);
        }
        c.text(Rect{0, 116, kWidth, 10}, "hold encoder to exit", kFontSmall, gray::kDim, Align::Center);
    }

private:
    Event last_{EventType::Press};
    bool have_last_ = false;
};

// ---------------------------------------------------------------------------------------
// Display test
// ---------------------------------------------------------------------------------------

class DisplayTest final : public Screen {
public:
    static constexpr int kPatterns = 5;

    bool on_event(Ui& ui, const Event& e) noexcept override {
        if (e.type == EventType::Turn) {
            pattern_ = ((pattern_ + (e.delta > 0 ? 1 : -1)) % kPatterns + kPatterns) % kPatterns;
            return true;
        }
        if ((e.type == EventType::Press || e.type == EventType::Repeat) && (e.key == Key::Up || e.key == Key::Down)) {
            pattern_ = (pattern_ + (e.key == Key::Down ? 1 : kPatterns - 1)) % kPatterns;
            return true;
        }
        if (e.select_click() || e.select_long() || e.is(EventType::Press, Key::Left)) {
            ui.pop();
            return true;
        }
        return true;
    }

    bool full_screen() const noexcept override { return true; }

    void draw(Ui& ui, Canvas& c, const Rect& body) noexcept override {
        (void)ui;
        (void)body;
        switch (pattern_) {
            case 0:   // 16 gray bars
                for (int g = 0; g < 16; ++g) {
                    c.fill(Rect{static_cast<int16_t>(g * 8), 0, 8, 112}, static_cast<Gray>(g));
                }
                c.text(Rect{0, 116, kWidth, 10}, "gray 0..15", kFontSmall, gray::kLabel, Align::Center);
                break;
            case 1:   // orientation: corners, arrow up, left/right pixel pair
                c.frame(Rect{0, 0, kWidth, kHeight}, gray::kValue);
                c.text(3, 3, "TL", kFontBody, gray::kValue);
                c.text(kWidth - 15, 3, "TR", kFontBody, gray::kValue);
                c.text(3, kHeight - 13, "BL", kFontBody, gray::kValue);
                c.text(kWidth - 15, kHeight - 13, "BR", kFontBody, gray::kValue);
                c.line(64, 30, 64, 98, gray::kValue);
                c.line(64, 30, 50, 44, gray::kValue);
                c.line(64, 30, 78, 44, gray::kValue);
                c.text(Rect{0, 100, kWidth, 10}, "UP  (tail at bottom)", kFontSmall, gray::kLabel, Align::Center);
                // A bright pixel left of a dark one in every pair: shows a nibble swap
                for (int x = 20; x < 108; x += 4) {
                    c.pixel(x, 60, gray::kValue);
                    c.pixel(x + 1, 60, gray::kOff);
                }
                break;
            case 2:   // 1 px checkerboard
                for (int y = 0; y < kHeight; ++y) {
                    for (int x = (y & 1); x < kWidth; x += 2) {
                        c.pixel(x, y, gray::kValue);
                    }
                }
                break;
            case 3:   // full white (current draw check)
                c.fill(Rect{0, 0, kWidth, kHeight}, gray::kValue);
                break;
            case 4: { // text sample
                int y = 2;
                c.text(2, y, "Small 5x7 0123456789", kFontSmall, gray::kValue);
                y += 9;
                c.text(2, y, "Body 6x10 ° µ ± Ω", kFontBody, gray::kValue);
                y += 12;
                c.text(2, y, "ABCDEFGHIJKLMNOPQRSTU", kFontBody, gray::kText);
                y += 11;
                c.text(2, y, "abcdefghijklmnopqrstu", kFontBody, gray::kLabel);
                y += 13;
                c.text(2, y, "12.34V", kFontBig, gray::kValue);
                break;
            }
            default:
                break;
        }
    }

private:
    int pattern_ = 0;
};

Dashboard g_dashboard;
AppletRunScreen g_run_screen;
InputTest g_input_test;
DisplayTest g_display_test;

} // namespace

Screen& home_screen() noexcept { return g_dashboard; }
Screen& input_test_screen() noexcept { return g_input_test; }
Screen& display_test_screen() noexcept { return g_display_test; }

Screen& applet_run_screen(size_t applet) noexcept {
    g_run_screen.set_applet(applet);
    return g_run_screen;
}

} // namespace ui
