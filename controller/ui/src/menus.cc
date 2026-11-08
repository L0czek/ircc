// Menu models: main menu, outputs, measurements, system, settings, applets
#include "ui/format.hpp"
#include "ui/menu.hpp"
#include "ui/screens.hpp"
#include "widgets.hpp"

#include <cmath>
#include <cstring>

namespace ui {

namespace {

void copy(char* buf, size_t n, const char* s) noexcept {
    std::strncpy(buf, s, n - 1);
    buf[n - 1] = '\0';
}

void value_or_dash(char* buf, size_t n, bool have, float v, int decimals, const char* unit) noexcept {
    if (have) {
        format_value(buf, n, v, decimals, unit);
    } else {
        copy(buf, n, "-");
    }
}

// ---------------------------------------------------------------------------------------
// Generic table of static items
// ---------------------------------------------------------------------------------------

/** @brief A model whose items are a fixed table; value/set/activate per index */
class TableModel : public MenuModel {
public:
    TableModel(const char* title, std::span<const MenuItem> items) noexcept : title_(title), items_(items) {}
    const char* title(Ui&) noexcept override { return title_; }
    size_t count(Ui&) noexcept override { return items_.size(); }
    MenuItem item(Ui&, size_t i) noexcept override { return items_[i]; }

private:
    const char* title_;
    std::span<const MenuItem> items_;
};

MenuItem submenu(const char* label) noexcept {
    MenuItem m;
    m.kind = ItemKind::Submenu;
    m.label = label;
    return m;
}

MenuItem info(const char* label) noexcept {
    MenuItem m;
    m.kind = ItemKind::Info;
    m.label = label;
    return m;
}

MenuItem toggle(const char* label) noexcept {
    MenuItem m;
    m.kind = ItemKind::Toggle;
    m.label = label;
    return m;
}

MenuItem action(const char* label) noexcept {
    MenuItem m;
    m.kind = ItemKind::Action;
    m.label = label;
    return m;
}

MenuItem number(const char* label, float min, float max, float step, uint8_t decimals, const char* unit,
                bool live = false) noexcept {
    MenuItem m;
    m.kind = ItemKind::Number;
    m.label = label;
    m.spec.min = min;
    m.spec.max = max;
    m.spec.step = step;
    m.spec.decimals = decimals;
    m.spec.unit = unit;
    m.spec.live = live;
    return m;
}

MenuItem choice(const char* label, std::span<const char* const> choices) noexcept {
    MenuItem m;
    m.kind = ItemKind::Choice;
    m.label = label;
    m.spec.choices = choices;
    m.spec.max = static_cast<float>(choices.size() - 1);
    return m;
}

// Forward: screens of the models below
extern MenuScreen g_outputs_screen;
extern MenuScreen g_vout_screen;
extern MenuScreen g_charger_screen;
extern MenuScreen g_usb_screen;
extern MenuScreen g_measurements_screen;
extern MenuScreen g_system_screen;
extern MenuScreen g_settings_screen;
extern MenuScreen g_applets_screen;
extern MenuScreen g_applet_screen;

// ---------------------------------------------------------------------------------------
// Main menu
// ---------------------------------------------------------------------------------------

class MainMenuModel final : public MenuModel {
public:
    const char* title(Ui&) noexcept override { return "Menu"; }
    size_t count(Ui&) noexcept override { return 5; }
    MenuItem item(Ui&, size_t i) noexcept override {
        static const char* const kLabels[] = {"Applets", "Outputs", "Measurements", "System", "Settings"};
        return submenu(kLabels[i]);
    }
    void info_text(Ui& ui, size_t i, char* buf, size_t n) noexcept override {
        buf[0] = '\0';
        if (i == 0) {
            const int running = ui.running_applet();
            if (running >= 0) {
                copy(buf, n, ui.backend().applet(static_cast<size_t>(running))->name());
            } else {
                format_uint(buf, n, static_cast<uint32_t>(ui.backend().applet_count()));
            }
        }
    }
    void activate(Ui& ui, size_t i) noexcept override {
        MenuScreen* screens[] = {&g_applets_screen, &g_outputs_screen, &g_measurements_screen, &g_system_screen,
                                 &g_settings_screen};
        ui.push(*screens[i]);
    }
};

class OutputsModel final : public MenuModel {
public:
    const char* title(Ui&) noexcept override { return "Outputs"; }
    size_t count(Ui&) noexcept override { return 3; }
    MenuItem item(Ui&, size_t i) noexcept override {
        static const char* const kLabels[] = {"VOUT", "Charger", "USB-C"};
        return submenu(kLabels[i]);
    }
    void info_text(Ui& ui, size_t i, char* buf, size_t n) noexcept override {
        const auto& s = ui.status();
        buf[0] = '\0';
        if (i == 0 && s.has_output_regulator) {
            copy(buf, n, s.output_regulator.enabled ? "ON" : "OFF");
        } else if (i == 1 && s.has_charger) {
            copy(buf, n, charge_state_text(s.charger.charge_status));
        } else if (i == 2 && s.has_usb_pd) {
            copy(buf, n, usb_role_text(s.usb_pd.power_role));
        }
    }
    void activate(Ui& ui, size_t i) noexcept override {
        MenuScreen* screens[] = {&g_vout_screen, &g_charger_screen, &g_usb_screen};
        ui.push(*screens[i]);
    }
};

// ---------------------------------------------------------------------------------------
// VOUT (TPS55289)
// ---------------------------------------------------------------------------------------

class VoutModel final : public MenuModel {
public:
    enum : size_t { Enable, Voltage, Limit, Measured, Current, Mode, Count };

    const char* title(Ui&) noexcept override { return "VOUT"; }
    size_t count(Ui&) noexcept override { return Count; }
    MenuItem item(Ui& ui, size_t i) noexcept override {
        MenuItem m;
        switch (i) {
            case Enable: m = toggle("Output"); break;
            case Voltage: m = number("Voltage", 0.8f, 22.0f, 0.01f, 2, "V"); break;
            case Limit: m = number("Current lim", 0.0f, 6.35f, 0.05f, 2, "A"); break;
            case Measured: m = info("Measured"); break;
            case Current: m = info("Current"); break;
            default: m = info("Mode"); break;
        }
        m.enabled = ui.status().has_output_regulator || m.kind == ItemKind::Info;
        return m;
    }
    float value(Ui& ui, size_t i) noexcept override {
        const auto& s = ui.status();
        switch (i) {
            case Enable: return s.has_output_regulator && s.output_regulator.enabled ? 1.0f : 0.0f;
            case Voltage: return s.output_regulator_config.has_output_voltage ? s.output_regulator_config.output_voltage : 0.0f;
            case Limit:
                return s.output_regulator_config.has_output_current_limit ? s.output_regulator_config.output_current_limit
                                                                          : 0.0f;
            default: return 0.0f;
        }
    }
    bool set(Ui& ui, size_t i, float v) noexcept override {
        Request& r = ui.new_request();
        r.which_command = Request_set_output_regulator_config_tag;
        auto& cfg = r.command.set_output_regulator_config;
        switch (i) {
            case Enable: cfg.has_enabled = true; cfg.enabled = v != 0.0f; break;
            case Voltage: cfg.has_output_voltage = true; cfg.output_voltage = v; break;
            case Limit: cfg.has_output_current_limit = true; cfg.output_current_limit = v; break;
            default: return false;
        }
        return ui.execute();
    }
    void info_text(Ui& ui, size_t i, char* buf, size_t n) noexcept override {
        const auto& s = ui.status();
        const bool m = s.has_output_regulator_measurements;
        switch (i) {
            case Measured: value_or_dash(buf, n, m, s.output_regulator_measurements.output_voltage, 2, "V"); break;
            case Current: value_or_dash(buf, n, m, s.output_regulator_measurements.output_current, 2, "A"); break;
            case Mode:
                copy(buf, n, s.has_output_regulator ? regulator_mode_text(s.output_regulator.operating_mode) : "-");
                break;
            default: buf[0] = '\0'; break;
        }
    }
};

// ---------------------------------------------------------------------------------------
// Charger (BQ25792)
// ---------------------------------------------------------------------------------------

const char* const kInputChoices[] = {"Auto", "USB-C", "Dock"};

class ChargerModel final : public MenuModel {
public:
    enum : size_t { Charging, ChargeV, ChargeI, Input, InCap, InFloor, DockLimit, Iindpm, Vindpm, Count };

    const char* title(Ui&) noexcept override { return "Charger"; }
    size_t count(Ui&) noexcept override { return Count; }
    MenuItem item(Ui& ui, size_t i) noexcept override {
        MenuItem m;
        switch (i) {
            case Charging: m = toggle("Charging"); break;
            case ChargeV: m = number("Charge V", 3.0f, 18.8f, 0.01f, 2, "V"); break;
            case ChargeI: m = number("Charge I", 0.05f, 5.0f, 0.01f, 2, "A"); break;
            case Input: m = choice("Input", kInputChoices); break;
            case InCap: m = number("Input cap", 0.1f, 3.3f, 0.05f, 2, "A"); break;
            case InFloor: m = number("Input min V", 3.6f, 22.0f, 0.1f, 1, "V"); break;
            case DockLimit: m = number("Dock limit", 0.1f, 3.3f, 0.05f, 2, "A"); break;
            case Iindpm: m = info("IINDPM"); break;
            default: m = info("VINDPM"); break;
        }
        m.enabled = ui.status().has_charger || m.kind == ItemKind::Info;
        if (i == Input && !ui.status().charger_config.has_input_source_mode) {
            m.enabled = false;
        }
        return m;
    }
    float value(Ui& ui, size_t i) noexcept override {
        const auto& s = ui.status();
        const auto& c = s.charger_config;
        switch (i) {
            case Charging: return s.has_charger && s.charger.enabled ? 1.0f : 0.0f;
            case ChargeV: return c.charge_voltage_limit;
            case ChargeI: return c.charge_current_limit;
            case Input:
                switch (c.input_source_mode) {
                    case ChargerInputSource_CHARGER_INPUT_AC1: return 1.0f;
                    case ChargerInputSource_CHARGER_INPUT_AC2: return 2.0f;
                    default: return 0.0f;
                }
            case InCap: return c.has_input_current_cap ? c.input_current_cap : 3.3f;
            case InFloor: return c.has_input_voltage_floor ? c.input_voltage_floor : 3.6f;
            case DockLimit: return c.has_dock_input_current_limit ? c.dock_input_current_limit : 3.0f;
            default: return 0.0f;
        }
    }
    bool set(Ui& ui, size_t i, float v) noexcept override {
        Request& r = ui.new_request();
        r.which_command = Request_set_charger_config_tag;
        auto& cfg = r.command.set_charger_config;
        switch (i) {
            case Charging: cfg.has_enabled = true; cfg.enabled = v != 0.0f; break;
            case ChargeV: cfg.has_charge_voltage_limit = true; cfg.charge_voltage_limit = v; break;
            case ChargeI: cfg.has_charge_current_limit = true; cfg.charge_current_limit = v; break;
            case Input: {
                static constexpr ChargerInputSource kSources[] = {ChargerInputSource_CHARGER_INPUT_AUTO,
                                                                  ChargerInputSource_CHARGER_INPUT_AC1,
                                                                  ChargerInputSource_CHARGER_INPUT_AC2};
                const auto idx = static_cast<size_t>(std::lround(v));
                cfg.has_input_source = true;
                cfg.input_source = kSources[idx < 3 ? idx : 0];
                break;
            }
            case InCap: cfg.has_input_current_limit = true; cfg.input_current_limit = v; break;
            case InFloor: cfg.has_input_voltage_limit = true; cfg.input_voltage_limit = v; break;
            case DockLimit: cfg.has_dock_input_current_limit = true; cfg.dock_input_current_limit = v; break;
            default: return false;
        }
        return ui.execute();
    }
    void info_text(Ui& ui, size_t i, char* buf, size_t n) noexcept override {
        const auto& s = ui.status();
        switch (i) {
            case Iindpm: value_or_dash(buf, n, s.has_charger_config, s.charger_config.input_current_limit, 2, "A"); break;
            case Vindpm: value_or_dash(buf, n, s.has_charger_config, s.charger_config.input_voltage_limit, 1, "V"); break;
            default: buf[0] = '\0'; break;
        }
    }
};

// ---------------------------------------------------------------------------------------
// USB-C (UCPD)
// ---------------------------------------------------------------------------------------

class UsbModel final : public MenuModel {
public:
    enum : size_t { Source, SinkV, SinkI, MinBatt, Renegotiate, State, Contract, SourceState, Count };

    const char* title(Ui&) noexcept override { return "USB-C"; }
    size_t count(Ui&) noexcept override { return Count; }
    MenuItem item(Ui& ui, size_t i) noexcept override {
        MenuItem m;
        switch (i) {
            case Source: m = toggle("Source role"); break;
            case SinkV: m = number("Max sink V", 5.0f, 20.0f, 1.0f, 0, "V"); break;
            case SinkI: m = number("Max sink I", 0.5f, 3.0f, 0.1f, 1, "A"); break;
            case MinBatt: m = number("Src min bat", 9.0f, 17.0f, 0.1f, 1, "V"); break;
            case Renegotiate: m = action("Renegotiate"); break;
            case State: m = info("State"); break;
            case Contract: m = info("Contract"); break;
            default: m = info("Source"); break;
        }
        m.enabled = ui.status().has_usb_pd_config || m.kind == ItemKind::Info;
        return m;
    }
    float value(Ui& ui, size_t i) noexcept override {
        const auto& c = ui.status().usb_pd_config;
        switch (i) {
            case Source: return c.source_enabled ? 1.0f : 0.0f;
            case SinkV: return c.max_sink_voltage;
            case SinkI: return c.max_sink_current;
            case MinBatt: return c.has_source_min_battery_voltage ? c.source_min_battery_voltage : 0.0f;
            default: return 0.0f;
        }
    }
    bool set(Ui& ui, size_t i, float v) noexcept override {
        Request& r = ui.new_request();
        r.which_command = Request_set_usb_pd_config_tag;
        auto& cfg = r.command.set_usb_pd_config;
        switch (i) {
            case Source: cfg.has_source_enabled = true; cfg.source_enabled = v != 0.0f; break;
            case SinkV: cfg.has_max_sink_voltage = true; cfg.max_sink_voltage = v; break;
            case SinkI: cfg.has_max_sink_current = true; cfg.max_sink_current = v; break;
            case MinBatt: cfg.has_source_min_battery_voltage = true; cfg.source_min_battery_voltage = v; break;
            default: return false;
        }
        return ui.execute();
    }
    void activate(Ui& ui, size_t i) noexcept override {
        if (i != Renegotiate) {
            return;
        }
        Request& r = ui.new_request();
        r.which_command = Request_usb_pd_renegotiate_tag;
        if (ui.execute()) {
            ui.toast("Renegotiating");
        }
    }
    void info_text(Ui& ui, size_t i, char* buf, size_t n) noexcept override {
        const auto& s = ui.status();
        buf[0] = '\0';
        if (!s.has_usb_pd) {
            return;
        }
        const auto& u = s.usb_pd;
        switch (i) {
            case State: copy(buf, n, usb_state_text(u.state)); break;
            case Contract:
                if (u.has_contract_voltage) {
                    char tmp[12];
                    format_value(buf, n, u.contract_voltage, 0, "V ");
                    format_value(tmp, sizeof(tmp), u.has_contract_current ? u.contract_current : 0.0f, 2, "A");
                    append(buf, n, tmp);
                } else {
                    copy(buf, n, "-");
                }
                break;
            case SourceState: copy(buf, n, usb_source_text(u.source_state)); break;
            default: break;
        }
    }
};

// ---------------------------------------------------------------------------------------
// Measurements (read-only list)
// ---------------------------------------------------------------------------------------

class MeasurementsModel final : public MenuModel {
public:
    const char* title(Ui&) noexcept override { return "Measurements"; }
    size_t count(Ui&) noexcept override { return kCount; }
    MenuItem item(Ui&, size_t i) noexcept override { return info(kRows[i].label); }
    void info_text(Ui& ui, size_t i, char* buf, size_t n) noexcept override {
        const auto& s = ui.status();
        const auto& m = s.charger_measurements;
        const auto& o = s.output_regulator_measurements;
        const auto& u = s.usb_pd;
        const auto& c = s.charger_config;
        const bool ch = s.has_charger_measurements;
        switch (i) {
            case 0: value_or_dash(buf, n, ch, m.vbus_voltage, 3, "V"); break;
            case 1: value_or_dash(buf, n, ch, m.ibus_current, 3, "A"); break;
            case 2: value_or_dash(buf, n, ch, m.vbat_voltage, 3, "V"); break;
            case 3: value_or_dash(buf, n, ch && m.has_ibat_current, m.ibat_current, 3, "A"); break;
            case 4: value_or_dash(buf, n, ch, m.vsys_voltage, 3, "V"); break;
            case 5: value_or_dash(buf, n, ch && m.has_vac1_voltage, m.vac1_voltage, 3, "V"); break;
            case 6: value_or_dash(buf, n, ch && m.has_vac2_voltage, m.vac2_voltage, 3, "V"); break;
            case 7: value_or_dash(buf, n, s.has_output_regulator_measurements, o.output_voltage, 3, "V"); break;
            case 8: value_or_dash(buf, n, s.has_output_regulator_measurements, o.output_current, 3, "A"); break;
            case 9: value_or_dash(buf, n, s.has_usb_pd && u.has_vbus_voltage, u.vbus_voltage, 3, "V"); break;
            case 10: value_or_dash(buf, n, s.has_usb_pd && u.has_contract_voltage, u.contract_voltage, 2, "V"); break;
            case 11: value_or_dash(buf, n, s.has_usb_pd && u.has_contract_current, u.contract_current, 2, "A"); break;
            case 12: value_or_dash(buf, n, s.has_charger_config, c.input_current_limit, 2, "A"); break;
            case 13: value_or_dash(buf, n, s.has_charger_config, c.input_voltage_limit, 2, "V"); break;
            case 14: value_or_dash(buf, n, s.has_charger_config, c.charge_voltage_limit, 2, "V"); break;
            case 15: value_or_dash(buf, n, s.has_charger_config, c.charge_current_limit, 2, "A"); break;
            default: buf[0] = '\0'; break;
        }
    }

private:
    struct Row {
        const char* label;
    };
    static constexpr Row kRows[] = {
        {"VBUS"}, {"IBUS"}, {"VBAT"}, {"IBAT"}, {"VSYS"}, {"VAC1 USB-C"}, {"VAC2 dock"}, {"VOUT"},
        {"IOUT"}, {"USB VBUS"}, {"Contract V"}, {"Contract I"}, {"IINDPM"}, {"VINDPM"}, {"Charge V"},
        {"Charge I"},
    };
    static constexpr size_t kCount = sizeof(kRows) / sizeof(kRows[0]);
};

// ---------------------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------------------

class SystemModel final : public MenuModel {
public:
    const char* title(Ui&) noexcept override { return "System"; }
    size_t count(Ui&) noexcept override { return 8; }
    MenuItem item(Ui&, size_t i) noexcept override {
        static const char* const kLabels[] = {"Firmware", "Board", "Uptime", "Host link",
                                              "Free heap", "I2C recov", "Keypad", "Display err"};
        return info(kLabels[i]);
    }
    void info_text(Ui& ui, size_t i, char* buf, size_t n) noexcept override {
        SystemInfo s;
        ui.backend().system_info(s);
        switch (i) {
            case 0: copy(buf, n, s.firmware); break;
            case 1: copy(buf, n, s.board); break;
            case 2: format_duration(buf, n, s.uptime_s); break;
            case 3: copy(buf, n, s.host_connected ? "connected" : "-"); break;
            case 4: format_uint(buf, n, s.free_heap); append(buf, n, " B"); break;
            case 5: format_uint(buf, n, s.i2c_recoveries); break;
            case 6: {
                static const char kHex[] = "0123456789ABCDEF";
                copy(buf, n, s.keypad_ok ? "OK 0x" : "MISSING 0x");
                const char hex[3] = {kHex[s.keypad_address >> 4], kHex[s.keypad_address & 15], 0};
                append(buf, n, hex);
                break;
            }
            case 7: format_uint(buf, n, s.display_errors); break;
            default: buf[0] = '\0'; break;
        }
    }
};

// ---------------------------------------------------------------------------------------
// Settings (panel only, RAM)
// ---------------------------------------------------------------------------------------

class SettingsModel final : public MenuModel {
public:
    enum : size_t { Contrast, DimAfter, OffAfter, Rotate, EncReverse, EncAccel, PixelShift, InputTest, DisplayTest, Count };

    const char* title(Ui&) noexcept override { return "Settings"; }
    size_t count(Ui&) noexcept override { return Count; }
    MenuItem item(Ui&, size_t i) noexcept override {
        switch (i) {
            case Contrast: return number("Contrast", 1.0f, 255.0f, 1.0f, 0, "", true);
            case DimAfter: return number("Dim after", 0.0f, 600.0f, 10.0f, 0, "s");
            case OffAfter: return number("Off after", 0.0f, 60.0f, 1.0f, 0, "min");
            case Rotate: return toggle("Rotate 180");
            case EncReverse: return toggle("Enc reverse");
            case EncAccel: return toggle("Enc accel");
            case PixelShift: return toggle("Pixel shift");
            case InputTest: return submenu("Input test");
            default: return submenu("Display test");
        }
    }
    float value(Ui& ui, size_t i) noexcept override {
        const Settings& s = ui.settings();
        switch (i) {
            case Contrast: return s.contrast;
            case DimAfter: return s.dim_after_s;
            case OffAfter: return static_cast<float>(s.off_after_s / 60);
            case Rotate: return s.rotate180 ? 1.0f : 0.0f;
            case EncReverse: return s.encoder_reverse ? 1.0f : 0.0f;
            case EncAccel: return s.encoder_accel ? 1.0f : 0.0f;
            case PixelShift: return s.pixel_shift ? 1.0f : 0.0f;
            default: return 0.0f;
        }
    }
    bool set(Ui& ui, size_t i, float v) noexcept override {
        Settings& s = ui.settings();
        switch (i) {
            case Contrast: s.contrast = static_cast<uint8_t>(std::lround(v)); break;
            case DimAfter: s.dim_after_s = static_cast<uint16_t>(std::lround(v)); break;
            case OffAfter: s.off_after_s = static_cast<uint16_t>(std::lround(v) * 60); break;
            case Rotate: s.rotate180 = v != 0.0f; break;
            case EncReverse: s.encoder_reverse = v != 0.0f; break;
            case EncAccel: s.encoder_accel = v != 0.0f; break;
            case PixelShift: s.pixel_shift = v != 0.0f; break;
            default: return false;
        }
        return true;
    }
    void activate(Ui& ui, size_t i) noexcept override {
        if (i == InputTest) {
            ui.push(input_test_screen());
        } else if (i == DisplayTest) {
            ui.push(display_test_screen());
        }
    }
};

// ---------------------------------------------------------------------------------------
// Applets
// ---------------------------------------------------------------------------------------

size_t g_applet_index = 0;

class AppletsModel final : public MenuModel {
public:
    const char* title(Ui&) noexcept override { return "Applets"; }
    size_t count(Ui& ui) noexcept override { return ui.backend().applet_count(); }
    MenuItem item(Ui& ui, size_t i) noexcept override {
        AppletControl* a = ui.backend().applet(i);
        return submenu(a ? a->name() : "?");
    }
    void info_text(Ui& ui, size_t i, char* buf, size_t n) noexcept override {
        AppletControl* a = ui.backend().applet(i);
        copy(buf, n, a && a->run_state() != AppletRun::Stopped ? applet_run_text(a->run_state()) : "");
    }
    void activate(Ui& ui, size_t i) noexcept override {
        g_applet_index = i;
        g_applet_screen.select(1);   // start on Run/Stop
        ui.push(g_applet_screen);
    }
};

/**
 * Applet page: 0 state, 1 run/stop, 2 live view, 3.. parameters.
 * Parameters without kParamLive are locked while the applet runs.
 */
class AppletModel final : public MenuModel {
public:
    static constexpr size_t kFixed = 3;

    const char* title(Ui& ui) noexcept override {
        AppletControl* a = applet(ui);
        return a ? a->name() : "Applet";
    }
    size_t count(Ui& ui) noexcept override {
        AppletControl* a = applet(ui);
        return a ? kFixed + a->params().size() : 0;
    }
    MenuItem item(Ui& ui, size_t i) noexcept override {
        AppletControl* a = applet(ui);
        const bool running = a->run_state() == AppletRun::Running;
        if (i == 0) {
            return info("State");
        }
        if (i == 1) {
            if (running) {
                return action("Stop");
            }
            MenuItem m = action("Start");
            if (a->needs_arming()) {
                m.kind = ItemKind::HoldAction;
                m.label = "Start (hold)";
            }
            return m;
        }
        if (i == 2) {
            MenuItem m = submenu("Live view");
            m.enabled = running;
            return m;
        }
        const ParamDesc& d = a->params()[i - kFixed];
        MenuItem m;
        m.label = d.label;
        m.spec.min = d.min;
        m.spec.max = d.max;
        m.spec.step = d.step;
        m.spec.decimals = d.decimals;
        m.spec.unit = d.unit;
        m.spec.choices = d.choices;
        m.spec.live = (d.flags & kParamLive) && running;
        switch (d.type) {
            case ParamType::Number: m.kind = ItemKind::Number; break;
            case ParamType::Bool: m.kind = ItemKind::Toggle; break;
            case ParamType::Choice: m.kind = ItemKind::Choice; break;
        }
        if (d.flags & kParamReadOnly) {
            m.kind = ItemKind::Info;
        }
        m.enabled = !running || (d.flags & kParamLive);
        return m;
    }
    float value(Ui& ui, size_t i) noexcept override {
        AppletControl* a = applet(ui);
        return i >= kFixed ? a->get_param(i - kFixed) : 0.0f;
    }
    bool set(Ui& ui, size_t i, float v) noexcept override {
        if (i < kFixed) {
            return false;
        }
        auto r = ui.backend().applet_set(g_applet_index, i - kFixed, v);
        if (!r) {
            ui.toast(applet_error_text(r.error()));
            return false;
        }
        return true;
    }
    void activate(Ui& ui, size_t i) noexcept override {
        AppletControl* a = applet(ui);
        if (i == 1) {
            if (a->run_state() == AppletRun::Running) {
                ui.stop_applet(g_applet_index);
                ui.toast("Stopped");
            } else if (ui.start_applet(g_applet_index)) {
                ui.push(applet_run_screen(g_applet_index));
            }
        } else if (i == 2) {
            ui.push(applet_run_screen(g_applet_index));
        }
    }
    void info_text(Ui& ui, size_t i, char* buf, size_t n) noexcept override {
        AppletControl* a = applet(ui);
        buf[0] = '\0';
        if (i == 0) {
            copy(buf, n, applet_run_text(a->run_state()));
            const uint32_t started = ui.backend().applet_started_ms(g_applet_index);
            if (a->run_state() == AppletRun::Running && started != 0) {
                char tmp[12];
                append(buf, n, " ");
                format_duration(tmp, sizeof(tmp), (ui.now() - started) / 1000);
                append(buf, n, tmp);
            }
        } else if (i >= kFixed) {
            const ParamDesc& d = a->params()[i - kFixed];
            format_value(buf, n, a->get_param(i - kFixed), d.decimals, d.unit);
        }
    }

private:
    static AppletControl* applet(Ui& ui) noexcept { return ui.backend().applet(g_applet_index); }
};

// ---------------------------------------------------------------------------------------

MainMenuModel g_main_model;
OutputsModel g_outputs_model;
VoutModel g_vout_model;
ChargerModel g_charger_model;
UsbModel g_usb_model;
MeasurementsModel g_measurements_model;
SystemModel g_system_model;
SettingsModel g_settings_model;
AppletsModel g_applets_model;
AppletModel g_applet_model;

MenuScreen g_main_screen(g_main_model);
MenuScreen g_outputs_screen(g_outputs_model);
MenuScreen g_vout_screen(g_vout_model);
MenuScreen g_charger_screen(g_charger_model);
MenuScreen g_usb_screen(g_usb_model);
MenuScreen g_measurements_screen(g_measurements_model);
MenuScreen g_system_screen(g_system_model);
MenuScreen g_settings_screen(g_settings_model);
MenuScreen g_applets_screen(g_applets_model);
MenuScreen g_applet_screen(g_applet_model);

} // namespace

Screen& main_menu_screen() noexcept { return g_main_screen; }
Screen& applets_screen() noexcept { return g_applets_screen; }

} // namespace ui
