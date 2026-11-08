// Front panel simulator: runs the panel UI (controller/ui) against a fake backend, sends
// every frame through the SSD1327 driver into its GDDRAM emulator, and saves screenshots
// as PNG (4x, yellow like the REX128128 panel).
//
//   ui_sim <out_dir>               built-in tour of every screen
//   ui_sim <out_dir> <script>      script lines (# comments):
//       wait <ms>                  advance time (25 ms ticks)
//       press <key> / release <key> / click <key> / hold <key> <ms>
//                                  keys: up down left right center push shutdown
//       turn <detents> [fast]      encoder (fast: within 20 ms, accelerated)
//       shot <name>                save <out_dir>/<name>.png
//       set <thing> <value>        fake backend: charger_fault 0/1, vout_fault 0/1, host 0/1,
//                                  usb sink|source|none, dock 0/1
#include "ssd1327.hpp"
#include "ssd1327_emulator.hpp"
#include "ui/gfx.hpp"
#include "ui/screens.hpp"
#include "ui/ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------
// PNG writer (stored deflate blocks, no compression library needed)
// ---------------------------------------------------------------------------------------

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(body.data(), body.size()));
}

bool write_png(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + y * w * 3, rgb.begin() + (y + 1) * w * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + n));
        pos += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(w));
    put32(ihdr, static_cast<uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(f);
}

// ---------------------------------------------------------------------------------------
// Fake applets
// ---------------------------------------------------------------------------------------

class FakePwm final : public AppletControl {
public:
    const char* name() const noexcept override { return "aux-pwm"; }
    const char* summary() const noexcept override { return "PWM on TIM2 CH1/CH2"; }
    std::span<const ParamDesc> params() const noexcept override { return kParams; }
    float get_param(size_t i) const noexcept override { return values_[i]; }
    AppletResult set_param(size_t i, float v) noexcept override {
        values_[i] = v;
        return {};
    }
    std::span<const ReadoutDesc> readouts() const noexcept override { return kReadouts; }
    float readout(size_t i) const noexcept override {
        return i == 0 ? 1e6f / values_[0] : values_[1] / 100.0f * 3.3f;
    }
    AppletRun run_state() const noexcept override { return running_ ? AppletRun::Running : AppletRun::Stopped; }
    AppletResult start() noexcept override {
        running_ = true;
        return {};
    }
    void stop() noexcept override { running_ = false; }
    bool needs_arming() const noexcept override { return false; }

private:
    static constexpr ParamDesc kParams[] = {
        {"freq", "Frequency", "Hz", ParamType::Number, 1.0f, 1000000.0f, 1.0f, 0, {}, kParamLive | kParamPersist},
        {"duty1", "Duty A", "%", ParamType::Number, 0.0f, 100.0f, 0.1f, 1, {}, kParamLive | kParamPersist},
        {"duty2", "Duty B", "%", ParamType::Number, 0.0f, 100.0f, 0.1f, 1, {}, kParamLive | kParamPersist},
    };
    static constexpr ReadoutDesc kReadouts[] = {{"Period", "us", 2}, {"Avg A", "V", 2}};
    std::array<float, 3> values_{125000.0f, 50.0f, 50.0f};
    bool running_ = false;
};

const char* const kBurstModes[] = {"Single", "Burst", "Continuous"};

class FakeQcw final : public AppletControl {
public:
    const char* name() const noexcept override { return "qcw-drive"; }
    std::span<const ParamDesc> params() const noexcept override { return kParams; }
    float get_param(size_t i) const noexcept override { return values_[i]; }
    AppletResult set_param(size_t i, float v) noexcept override {
        if (running_ && !(kParams[i].flags & kParamLive)) {
            return std::unexpected(AppletError::InvalidState);
        }
        values_[i] = v;
        return {};
    }
    std::span<const ReadoutDesc> readouts() const noexcept override { return kReadouts; }
    float readout(size_t i) const noexcept override { return i == 0 ? 342.5f : 12.4f; }
    AppletRun run_state() const noexcept override { return running_ ? AppletRun::Running : AppletRun::Stopped; }
    AppletResult start() noexcept override {
        running_ = true;
        return {};
    }
    void stop() noexcept override { running_ = false; }

private:
    static constexpr ParamDesc kParams[] = {
        {"mode", "Mode", "", ParamType::Choice, 0, 2, 1, 0, kBurstModes, kParamPersist},
        {"ontime", "On time", "ms", ParamType::Number, 0.1f, 20.0f, 0.1f, 1, {}, kParamLive | kParamPersist},
        {"bps", "Rate", "Hz", ParamType::Number, 1.0f, 100.0f, 1.0f, 0, {}, kParamLive | kParamPersist},
        {"ramp", "Ramp", "", ParamType::Bool, 0, 1, 1, 0, {}, kParamPersist},
    };
    static constexpr ReadoutDesc kReadouts[] = {{"Freq", "kHz", 1}, {"Peak I", "A", 1}};
    std::array<float, 4> values_{1.0f, 5.0f, 10.0f, 1.0f};
    bool running_ = false;
};

// ---------------------------------------------------------------------------------------
// Fake backend
// ---------------------------------------------------------------------------------------

class FakeBackend final : public ui::Backend {
public:
    uint32_t now = 0;
    bool charger_fault = false;
    bool vout_fault = false;
    bool host = true;
    bool dock = false;
    int usb = 1;   // 0 none, 1 sink, 2 source

    bool execute(const Request& req, Response& resp) noexcept override {
        switch (req.which_command) {
            case Request_set_output_regulator_config_tag: {
                const auto& c = req.command.set_output_regulator_config;
                if (c.has_output_voltage) {
                    if (c.output_voltage < 0.8f || c.output_voltage > 22.0f) return fail(resp);
                    vout_set = c.output_voltage;
                }
                if (c.has_output_current_limit) vout_ilim = c.output_current_limit;
                if (c.has_enabled) vout_on = c.enabled;
                break;
            }
            case Request_set_charger_config_tag: {
                const auto& c = req.command.set_charger_config;
                if (c.has_enabled) chg_on = c.enabled;
                if (c.has_charge_voltage_limit) vreg = c.charge_voltage_limit;
                if (c.has_charge_current_limit) ichg = c.charge_current_limit;
                if (c.has_input_source) input_mode = c.input_source;
                if (c.has_input_current_limit) in_cap = c.input_current_limit;
                if (c.has_input_voltage_limit) in_floor = c.input_voltage_limit;
                if (c.has_dock_input_current_limit) dock_lim = c.dock_input_current_limit;
                break;
            }
            case Request_set_usb_pd_config_tag: {
                const auto& c = req.command.set_usb_pd_config;
                if (c.has_source_enabled) src_en = c.source_enabled;
                if (c.has_max_sink_voltage) sink_v = c.max_sink_voltage;
                if (c.has_max_sink_current) sink_i = c.max_sink_current;
                break;
            }
            case Request_power_off_tag: {
                const PowerOffAction a = req.command.power_off.action;
                if (a != PowerOffAction_POWER_OFF_CANCEL) {
                    applet_stop(0);
                    applet_stop(1);
                    vout_on = false;
                }
                if (a == PowerOffAction_POWER_OFF_SHIP) {
                    off_state = PowerOffState_POWER_OFF_WAIT_SOURCES;
                    if (usb == 2) usb = 0;   // the USB-C source stops
                } else if (a == PowerOffAction_POWER_OFF_CANCEL) {
                    off_state = PowerOffState_POWER_OFF_IDLE;
                }
                break;
            }
            case Request_get_system_status_tag:
            case Request_usb_pd_renegotiate_tag:
                break;
            default:
                resp.has_result = true;
                resp.result.status = ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND;
                return true;
        }
        resp.which_payload = Response_system_status_tag;
        fill(resp.payload.system_status);
        resp.has_result = true;
        resp.result.status = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
        return true;
    }

    size_t applet_count() noexcept override { return 2; }
    AppletControl* applet(size_t i) noexcept override {
        return i == 0 ? static_cast<AppletControl*>(&pwm_) : (i == 1 ? &qcw_ : nullptr);
    }
    AppletResult applet_start(size_t i) noexcept override {
        for (size_t k = 0; k < 2; ++k) {
            if (k != i) applet(k)->stop();
        }
        started_[i] = now ? now : 1;
        return applet(i)->start();
    }
    void applet_stop(size_t i) noexcept override { applet(i)->stop(); }
    AppletResult applet_set(size_t i, size_t p, float v) noexcept override {
        auto checked = check_param(applet(i)->params()[p], v);
        if (!checked) return std::unexpected(checked.error());
        return applet(i)->set_param(p, *checked);
    }
    uint32_t applet_started_ms(size_t i) noexcept override { return started_[i]; }
    void system_info(ui::SystemInfo& info) noexcept override {
        info.firmware = "e1ef7ef-dirty";
        info.board = "resonant_converter_controller";
        info.uptime_s = now / 1000 + 3600;
        info.host_connected = host;
        info.free_heap = 11264;
        info.keypad_ok = true;
        info.keypad_address = 0x20;
    }

private:
    static bool fail(Response& resp) {
        resp.has_result = true;
        resp.result.status = ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS;
        return true;
    }

    // Same steps as PowerPath::step_power_off, with a 300 ms VBUS drain
    void step_power_off() {
        const bool sources = dock || usb == 1;
        switch (off_state) {
            case PowerOffState_POWER_OFF_WAIT_SOURCES:
                if (!sources) {
                    off_state = PowerOffState_POWER_OFF_DRAIN_VBUS;
                    off_t0 = now;
                }
                break;
            case PowerOffState_POWER_OFF_DRAIN_VBUS:
                if (sources) {
                    off_state = PowerOffState_POWER_OFF_WAIT_SOURCES;
                } else if (now - off_t0 >= 300) {
                    off_state = PowerOffState_POWER_OFF_PENDING;
                    off_t0 = now + 10000;
                }
                break;
            case PowerOffState_POWER_OFF_PENDING:
                if (sources) {
                    off_state = PowerOffState_POWER_OFF_WAIT_SOURCES;
                }
                break;
            default:
                break;
        }
    }

    void fill(SystemStatus& s) {
        const float t = static_cast<float>(now) / 1000.0f;
        step_power_off();
        if (off_state != PowerOffState_POWER_OFF_IDLE) {
            s.has_power_off = true;
            s.power_off.state = off_state;
            s.power_off.usb_c_present = usb == 1;
            s.power_off.dock_present = dock;
            s.power_off.vbus_present = off_state != PowerOffState_POWER_OFF_PENDING && (usb == 1 || dock);
            s.power_off.battery_present = true;
            if (off_state == PowerOffState_POWER_OFF_PENDING) {
                s.power_off.has_off_in_ms = true;
                s.power_off.off_in_ms = off_t0 > now ? off_t0 - now : 0;
            }
        }
        s.has_charger = true;
        s.charger.enabled = chg_on;
        s.charger.plugged_in = dock || usb == 1;
        s.charger.charge_status = chg_on && s.charger.plugged_in ? ChargeStatus_CHARGE_STATUS_FAST_CHARGE
                                                                 : ChargeStatus_CHARGE_STATUS_NOT_CHARGING;
        s.charger.battery_present = true;
        s.charger.fault_present = charger_fault;
        s.charger.has_fault_flags = true;
        s.charger.fault_flags = charger_fault ? (1u << 4) | (1u << 10) : 0;
        s.charger.input_source = dock ? ChargerInputSource_CHARGER_INPUT_AC2 : ChargerInputSource_CHARGER_INPUT_AC1;
        s.charger.has_status_flags = true;
        s.charger.status_flags = (1u << 7);

        s.has_charger_measurements = true;
        auto& m = s.charger_measurements;
        m.vbat_voltage = 15.62f + 0.05f * std::sin(t / 7.0f);
        m.has_ibat_current = true;
        m.ibat_current = s.charger.plugged_in && chg_on ? 1.20f + 0.15f * std::sin(t / 3.0f) : -0.42f;
        m.vbus_voltage = dock ? 19.8f : (usb == 1 ? 19.92f : 0.0f);
        m.ibus_current = s.charger.plugged_in ? 1.05f + 0.1f * std::sin(t / 2.0f) : 0.0f;
        m.vsys_voltage = 15.8f;
        m.has_vac1_voltage = true;
        m.vac1_voltage = usb == 1 ? 20.01f : (usb == 2 ? 5.02f : 0.0f);
        m.has_vac2_voltage = true;
        m.vac2_voltage = dock ? 19.8f : 0.0f;

        s.has_charger_config = true;
        auto& c = s.charger_config;
        c.charge_voltage_limit = vreg;
        c.charge_current_limit = ichg;
        c.input_voltage_limit = 18.4f;
        c.input_current_limit = dock ? dock_lim : 2.25f;
        c.has_dock_input_current_limit = true;
        c.dock_input_current_limit = dock_lim;
        c.has_input_current_cap = true;
        c.input_current_cap = in_cap;
        c.has_input_voltage_floor = true;
        c.input_voltage_floor = in_floor;
        c.has_input_source_mode = true;
        c.input_source_mode = input_mode;

        s.has_output_regulator = true;
        s.output_regulator.enabled = vout_on;
        s.output_regulator.fault_present = vout_fault;
        s.output_regulator.ocp_fault = vout_fault;
        s.output_regulator.operating_mode = OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BUCK;
        s.has_output_regulator_measurements = true;
        s.output_regulator_measurements.output_voltage = vout_on ? vout_set - 0.01f : 0.0f;
        s.output_regulator_measurements.output_current = vout_on ? 0.84f + 0.3f * std::sin(t / 4.0f) : 0.0f;
        s.has_output_regulator_config = true;
        s.output_regulator_config.has_output_voltage = true;
        s.output_regulator_config.output_voltage = vout_set;
        s.output_regulator_config.has_output_current_limit = true;
        s.output_regulator_config.output_current_limit = vout_ilim;

        s.has_usb_pd = true;
        auto& u = s.usb_pd;
        u.state = usb == 0 ? UsbPdState_USB_PD_STATE_DETACHED : UsbPdState_USB_PD_STATE_CONTRACT;
        u.power_role = usb == 1 ? UsbPdPowerRole_USB_PD_POWER_ROLE_SINK
                                : (usb == 2 ? UsbPdPowerRole_USB_PD_POWER_ROLE_SOURCE : UsbPdPowerRole_USB_PD_POWER_ROLE_NONE);
        u.has_data_role = true;
        u.data_role = usb == 1 ? UsbPdDataRole_USB_PD_DATA_ROLE_UFP : UsbPdDataRole_USB_PD_DATA_ROLE_DFP;
        u.has_cc_line = usb != 0;
        u.cc_line = 2;
        u.has_contract_voltage = usb != 0;
        u.contract_voltage = usb == 1 ? 20.0f : 5.0f;
        u.has_contract_current = usb != 0;
        u.contract_current = usb == 1 ? 2.25f : 3.0f;
        u.source_state = usb == 2 ? UsbPdSourceState_USB_PD_SOURCE_ACTIVE
                                  : (src_en ? UsbPdSourceState_USB_PD_SOURCE_READY : UsbPdSourceState_USB_PD_SOURCE_DISABLED);
        u.has_source_path = true;
        u.source_path = usb == 2 ? (dock ? UsbPdSourcePath_USB_PD_SOURCE_PATH_VOUT : UsbPdSourcePath_USB_PD_SOURCE_PATH_OTG)
                                 : UsbPdSourcePath_USB_PD_SOURCE_PATH_NONE;
        u.has_vbus_voltage = true;
        u.vbus_voltage = m.vac1_voltage;
        u.has_input_current_limit = usb == 1;
        u.input_current_limit = 2.25f;
        u.has_hard_resets = true;
        u.hard_resets = 0;
        s.has_usb_pd_config = true;
        s.usb_pd_config.source_enabled = src_en;
        s.usb_pd_config.max_sink_voltage = sink_v;
        s.usb_pd_config.max_sink_current = sink_i;
        s.usb_pd_config.has_source_min_battery_voltage = true;
        s.usb_pd_config.source_min_battery_voltage = 13.0f;
    }

    FakePwm pwm_;
    FakeQcw qcw_;
    std::array<uint32_t, 2> started_{};
    bool vout_on = true;
    float vout_set = 12.0f;
    float vout_ilim = 3.0f;
    bool chg_on = true;
    float vreg = 16.8f;
    float ichg = 2.0f;
    ChargerInputSource input_mode = ChargerInputSource_CHARGER_INPUT_AUTO;
    float in_cap = 3.3f;
    float in_floor = 3.6f;
    float dock_lim = 3.0f;
    bool src_en = true;
    float sink_v = 20.0f;
    float sink_i = 3.0f;
    PowerOffState off_state = PowerOffState_POWER_OFF_IDLE;
    uint32_t off_t0 = 0;
};

// ---------------------------------------------------------------------------------------
// Simulator
// ---------------------------------------------------------------------------------------

class Sim {
public:
    explicit Sim(std::string out_dir) : out_(std::move(out_dir)), ui_(backend_, settings_), canvas_(fb_), display_(emu_, cfg()) {
        (void)display_.init();
        (void)display_.display_on(true);
        ui_.start(ui::home_screen(), 0);
    }

    void wait(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 25) {
            step(25);
        }
    }

    void key(ui::Key k, bool down) {
        if (down) {
            keys_ |= ui::key_bit(k);
        } else {
            keys_ &= static_cast<uint8_t>(~ui::key_bit(k));
        }
        ui_.keys(keys_, now_);
        step(25);
    }

    void click(ui::Key k) {
        key(k, true);
        wait(50);
        key(k, false);
        wait(50);
    }

    void hold(ui::Key k, uint32_t ms) {
        key(k, true);
        wait(ms);
        key(k, false);
        wait(50);
    }

    void turn(int detents, bool fast) {
        const int dir = detents > 0 ? 1 : -1;
        for (int i = 0; i < std::abs(detents); ++i) {
            ui_.encoder(dir, now_);
            step(fast ? 2 : 60);
        }
        wait(50);
    }

    bool shot(const std::string& name) {
        flush();
        const int scale = 4;
        const int w = 128 * scale;
        std::vector<uint8_t> rgb(static_cast<size_t>(w * w * 3));
        for (int y = 0; y < 128; ++y) {
            for (int x = 0; x < 128; ++x) {
                // Brightness follows the contrast setting (0x7F = full scale here)
                const int g = emu_.pixel(x, y) * std::min(255, emu_.contrast() * 2 + 32) / 255;
                const uint8_t r = static_cast<uint8_t>(g * 255 / 15);
                const uint8_t gr = static_cast<uint8_t>(g * 215 / 15);
                const uint8_t b = static_cast<uint8_t>(g * 30 / 15);
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        const size_t i = static_cast<size_t>(((y * scale + dy) * w + x * scale + dx) * 3);
                        rgb[i] = r;
                        rgb[i + 1] = gr;
                        rgb[i + 2] = b;
                    }
                }
            }
        }
        const std::string path = out_ + "/" + name + ".png";
        std::printf("%s (flushed rows: %zu)\n", path.c_str(), flushed_rows_);
        flushed_rows_ = 0;
        return write_png(path, w, w, rgb);
    }

    FakeBackend& backend() { return backend_; }

private:
    static ssd1327::Config cfg() {
        ssd1327::Config c;
        return c;
    }

    void step(uint32_t ms) {
        now_ += ms;
        backend_.now = now_;
        ui_.keys(keys_, now_);
        ui_.encoder(0, now_);
        ui_.tick(now_);
        flush();
    }

    void flush() {
        ui_.render(canvas_);
        ui::RowTracker::Run runs[8];
        const size_t n = rows_.changed_runs(fb_, runs, 8);
        for (size_t i = 0; i < n; ++i) {
            (void)display_.write_rows(runs[i].y0, runs[i].y1, fb_.row(runs[i].y0));
            flushed_rows_ += static_cast<size_t>(runs[i].y1 - runs[i].y0 + 1);
        }
        const bool want_on = ui_.display_power() != ui::DisplayPower::Off;
        if (want_on != display_.is_on()) {
            (void)display_.display_on(want_on);
        }
        if (want_on && display_.contrast() != ui_.contrast()) {
            (void)display_.set_contrast(ui_.contrast());
        }
    }

    std::string out_;
    FakeBackend backend_;
    ui::Settings settings_;
    ui::Ui ui_;
    ui::Framebuffer fb_;
    ui::Canvas canvas_;
    ui::RowTracker rows_;
    ssd1327::Emulator emu_;
    ssd1327::Display display_;
    uint32_t now_ = 0;
    uint8_t keys_ = 0;
    size_t flushed_rows_ = 0;
};

bool parse_key(const std::string& s, ui::Key& k) {
    static const std::pair<const char*, ui::Key> kKeys[] = {
        {"up", ui::Key::Up}, {"down", ui::Key::Down}, {"left", ui::Key::Left}, {"right", ui::Key::Right},
        {"center", ui::Key::Center}, {"push", ui::Key::Push}, {"shutdown", ui::Key::Shutdown},
    };
    for (const auto& [name, key] : kKeys) {
        if (s == name) {
            k = key;
            return true;
        }
    }
    return false;
}

const char* kTour = R"(
wait 3000
shot 01_overview
click right
shot 02_charger
click right
shot 03_vout
click right
shot 04_usb
click right
wait 2000
shot 05_trends
click right
click center
shot 10_menu
click push
shot 11_applets
click push
shot 12_applet_page
click center
wait 400
shot 13_live_view
turn 3
wait 200
shot 14_live_adjust
click left
wait 300
click down
click down
click down
shot 15_applet_params
click push
turn 4
shot 16_edit_number
click left
turn 2
shot 17_edit_coarse
click push
wait 200
shot 18_after_edit
hold push 800
wait 100
click right
click right
click right
click right
click right
wait 200
shot 19_dashboard_applet
click left
click center
click down
click push
shot 20_outputs
click push
shot 21_vout_menu
click down
click push
turn 25 fast
shot 22_vout_edit_fast
hold center 700
shot 23_vout_cancel
click left
click down
click push
shot 24_charger_menu
click down
click down
click down
click push
turn 1
shot 25_choice_edit
click push
click left
click down
click push
shot 26_usb_menu
hold push 800
click center
click down
click down
click push
shot 27_measurements
click left
click down
click push
shot 28_system
click left
click down
click push
shot 29_settings
hold push 800
click center
turn -4
click push
click down
click push
wait 300
shot 30_qcw_page
hold center 500
shot 31_hold_progress
wait 50
hold center 1200
wait 300
shot 32_qcw_running
hold push 800
set charger_fault 1
wait 600
shot 40_alarm
click center
set charger_fault 0
wait 300
shot 41_after_alarm
set usb source
set dock 1
wait 600
shot 42_source_dock
wait 61000
shot 43_dimmed
wait 240000
shot 44_off
click down
wait 100
shot 45_woken
set usb sink
set dock 1
click shutdown
wait 300
shot 50_soft_stop
wait 3000
hold shutdown 800
wait 300
shot 51_power_off_wait
set usb none
wait 400
shot 52_power_off_dock
click left
wait 300
shot 53_power_off_cancelled
wait 2000
hold shutdown 800
set dock 0
wait 1500
shot 54_power_off_pending
wait 4000
shot 55_power_off_countdown
)";

void run_script(Sim& sim, std::istream& in) {
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string cmd;
        if (!(ls >> cmd) || cmd[0] == '#') {
            continue;
        }
        std::string a;
        std::string b;
        ls >> a >> b;
        ui::Key k{};
        if (cmd == "wait") {
            sim.wait(static_cast<uint32_t>(std::stoul(a)));
        } else if ((cmd == "press" || cmd == "release" || cmd == "click" || cmd == "hold") && parse_key(a, k)) {
            if (cmd == "press") sim.key(k, true);
            else if (cmd == "release") sim.key(k, false);
            else if (cmd == "click") sim.click(k);
            else sim.hold(k, static_cast<uint32_t>(std::stoul(b)));
        } else if (cmd == "turn") {
            sim.turn(std::stoi(a), b == "fast");
        } else if (cmd == "shot") {
            sim.shot(a);
        } else if (cmd == "set") {
            auto& be = sim.backend();
            if (a == "charger_fault") be.charger_fault = b == "1";
            else if (a == "vout_fault") be.vout_fault = b == "1";
            else if (a == "host") be.host = b == "1";
            else if (a == "dock") be.dock = b == "1";
            else if (a == "usb") be.usb = b == "sink" ? 1 : (b == "source" ? 2 : 0);
        } else {
            std::fprintf(stderr, "unknown line: %s\n", line.c_str());
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <out_dir> [script]\n", argv[0]);
        return 2;
    }
    Sim sim(argv[1]);
    if (argc >= 3) {
        std::ifstream f(argv[2]);
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", argv[2]);
            return 1;
        }
        run_script(sim, f);
    } else {
        std::istringstream tour(kTour);
        run_script(sim, tour);
    }
    return 0;
}
