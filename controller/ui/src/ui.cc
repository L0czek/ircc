#include "ui/ui.hpp"

#include "ui/format.hpp"
#include "ui/icons.hpp"
#include <cmath>
#include <cstring>

namespace ui {

namespace {

void copy(char* dst, size_t n, const char* src) noexcept {
    if (n == 0) {
        return;
    }
    size_t i = 0;
    while (src && src[i] && i + 1 < n) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

const char* result_text(ResponseStatusCode code) noexcept {
    switch (code) {
        case ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS: return "Invalid value";
        case ResponseStatusCode_RESPONSE_STATUS_TIMEOUT: return "Timeout";
        case ResponseStatusCode_RESPONSE_STATUS_INVALID_STATE: return "Not possible now";
        case ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND: return "Not available";
        case ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND: return "Not found";
        case ResponseStatusCode_RESPONSE_STATUS_NO_MEMORY: return "No memory";
        default: return "Failed";
    }
}

void charger_fault_text(uint32_t flags, char* buf, size_t n) noexcept {
    static constexpr struct {
        uint32_t bit;
        const char* name;
    } kFaults[] = {
        {1u << 0, "VAC1 OVP"}, {1u << 1, "VAC2 OVP"}, {1u << 2, "CONV OCP"}, {1u << 3, "IBAT OCP"},
        {1u << 4, "IBUS OCP"}, {1u << 5, "VBAT OVP"}, {1u << 6, "VBUS OVP"}, {1u << 10, "TSHUT"},
        {1u << 12, "OTG UVP"}, {1u << 13, "OTG OVP"}, {1u << 14, "VSYS OVP"}, {1u << 15, "VSYS SHORT"},
    };
    buf[0] = '\0';
    for (const auto& f : kFaults) {
        if (flags & f.bit) {
            if (buf[0]) {
                append(buf, n, ", ");
            }
            append(buf, n, f.name);
        }
    }
    if (!buf[0]) {
        copy(buf, n, "see host for details");
    }
}

} // namespace

// ---------------------------------------------------------------------------------------
// Series
// ---------------------------------------------------------------------------------------

void Series::push(float value) noexcept {
    float m = value * 1000.0f;
    if (std::isnan(m)) m = 0.0f;
    if (m > 32767.0f) m = 32767.0f;
    if (m < -32767.0f) m = -32767.0f;
    milli_[head_] = static_cast<int16_t>(std::lround(m));
    head_ = (head_ + 1) % kSize;
    if (count_ < kSize) {
        ++count_;
    }
}

float Series::at(size_t i) const noexcept {
    const size_t idx = (head_ + kSize - count_ + i) % kSize;
    return static_cast<float>(milli_[idx]) / 1000.0f;
}

// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------

const char* charge_state_text(ChargeStatus s) noexcept {
    switch (s) {
        case ChargeStatus_CHARGE_STATUS_NOT_CHARGING: return "IDLE";
        case ChargeStatus_CHARGE_STATUS_TRICKLE_CHARGE: return "TRICKLE";
        case ChargeStatus_CHARGE_STATUS_PRECHARGE: return "PRE";
        case ChargeStatus_CHARGE_STATUS_FAST_CHARGE: return "FAST";
        case ChargeStatus_CHARGE_STATUS_TAPER_CHARGE: return "TAPER";
        case ChargeStatus_CHARGE_STATUS_TOP_OFF: return "TOP-OFF";
        case ChargeStatus_CHARGE_STATUS_CHARGE_DONE: return "DONE";
    }
    return "?";
}

int battery_cells(const SystemStatus& s) noexcept {
    if (!s.has_charger_config) {
        return 1;
    }
    const int cells = static_cast<int>(std::lround(s.charger_config.charge_voltage_limit / 4.2f));
    return cells < 1 ? 1 : (cells > 6 ? 6 : cells);
}

int battery_percent(const SystemStatus& s) noexcept {
    if (!s.has_charger_measurements) {
        return 0;
    }
    static constexpr float kCurve[][2] = {
        {3.30f, 0}, {3.50f, 5}, {3.60f, 10}, {3.70f, 25}, {3.80f, 45},
        {3.90f, 62}, {4.00f, 78}, {4.10f, 91}, {4.18f, 100},
    };
    const float v = s.charger_measurements.vbat_voltage / static_cast<float>(battery_cells(s));
    if (v <= kCurve[0][0]) {
        return 0;
    }
    for (size_t i = 1; i < sizeof(kCurve) / sizeof(kCurve[0]); ++i) {
        if (v < kCurve[i][0]) {
            const float t = (v - kCurve[i - 1][0]) / (kCurve[i][0] - kCurve[i - 1][0]);
            return static_cast<int>(kCurve[i - 1][1] + t * (kCurve[i][1] - kCurve[i - 1][1]));
        }
    }
    return 100;
}

void Screen::draw_footer(Ui& ui, Canvas& c, const Rect& footer) noexcept {
    (void)ui;
    c.text(footer.x + 1, footer.y + 1, hints(), kFontSmall, gray::kDim);
}

// ---------------------------------------------------------------------------------------
// Ui
// ---------------------------------------------------------------------------------------

Ui::Ui(Backend& backend, Settings& settings) noexcept : backend_(backend), settings_(settings) {}

void Ui::start(Screen& home, uint32_t now_ms) noexcept {
    now_ = now_ms;
    last_input_ = now_ms;
    next_poll_ = now_ms;
    next_history_ = now_ms;
    next_shift_ = now_ms + 60000;
    depth_ = 0;
    push(home);
}

void Ui::keys(uint8_t raw_mask, uint32_t now_ms) noexcept {
    raw_keys_ = raw_mask;
    keys_.update(raw_mask, now_ms, events_);
}

void Ui::encoder(int32_t detents, uint32_t now_ms) noexcept {
    encoder_position_ += detents;
    accel_.set_config(EncoderAccel::Config{settings_.encoder_reverse, settings_.encoder_accel, 5, 15});
    accel_.update(detents, now_ms, events_);
}

void Ui::push(Screen& screen) noexcept {
    if (depth_ < stack_.size()) {
        stack_[depth_++] = &screen;
        screen.on_enter(*this);
    }
}

void Ui::pop() noexcept {
    if (depth_ > 1) {
        --depth_;
        stack_[depth_ - 1]->on_enter(*this);
    }
}

void Ui::home() noexcept {
    if (depth_ > 1) {
        depth_ = 1;
        stack_[0]->on_enter(*this);
    }
}

void Ui::toast(const char* text, uint32_t duration_ms) noexcept {
    copy(toast_, sizeof(toast_), text);
    toast_until_ = now_ + duration_ms;
}

void Ui::alarm(const char* title, const char* text) noexcept {
    copy(alarm_title_, sizeof(alarm_title_), title);
    copy(alarm_text_, sizeof(alarm_text_), text);
    alarm_active_ = true;
    wake();
}

void Ui::wake() noexcept {
    last_input_ = now_;
    power_ = DisplayPower::On;
}

Request& Ui::new_request() noexcept {
    request_ = Request_init_zero;
    return request_;
}

bool Ui::execute() noexcept {
    response_ = Response_init_zero;
    if (!backend_.execute(request_, response_)) {
        toast("Not available");
        return false;
    }
    if (response_.which_payload == Response_system_status_tag) {
        status_ = response_.payload.system_status;
        status_valid_ = true;
    }
    if (response_.has_result && response_.result.status != ResponseStatusCode_RESPONSE_STATUS_SUCCESS) {
        // get_system_status reports a missing part as a failure too: stay quiet for polls
        if (request_.which_command != Request_get_system_status_tag) {
            toast(result_text(response_.result.status));
        }
        return false;
    }
    return true;
}

bool Ui::start_applet(size_t index) noexcept {
    auto r = backend_.applet_start(index);
    if (!r) {
        toast(applet_error_text(r.error()));
        return false;
    }
    return true;
}

void Ui::stop_applet(size_t index) noexcept {
    backend_.applet_stop(index);
}

int Ui::running_applet() noexcept {
    const size_t n = backend_.applet_count();
    for (size_t i = 0; i < n; ++i) {
        AppletControl* a = backend_.applet(i);
        if (a && a->run_state() == AppletRun::Running) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Ui::poll() noexcept {
    Request& r = new_request();
    r.which_command = Request_get_system_status_tag;
    (void)execute();
    check_alarms();
}

void Ui::sample_history() noexcept {
    if (!status_valid_) {
        return;
    }
    const auto& s = status_;
    const auto& cm = s.charger_measurements;
    const auto& om = s.output_regulator_measurements;
    const bool ch = s.has_charger_measurements;
    const bool om_ok = s.has_output_regulator_measurements;
    history_[static_cast<size_t>(Signal::Vbat)].push(ch ? cm.vbat_voltage : NAN);
    history_[static_cast<size_t>(Signal::Ibat)].push(ch && cm.has_ibat_current ? cm.ibat_current : NAN);
    history_[static_cast<size_t>(Signal::Vbus)].push(ch ? cm.vbus_voltage : NAN);
    history_[static_cast<size_t>(Signal::Ibus)].push(ch ? cm.ibus_current : NAN);
    history_[static_cast<size_t>(Signal::Vout)].push(om_ok ? om.output_voltage : NAN);
    history_[static_cast<size_t>(Signal::Iout)].push(om_ok ? om.output_current : NAN);
}

void Ui::check_alarms() noexcept {
    const bool cf = status_valid_ && status_.has_charger && status_.charger.fault_present;
    if (cf && !charger_fault_) {
        char text[64];
        charger_fault_text(status_.charger.has_fault_flags ? status_.charger.fault_flags : 0, text, sizeof(text));
        alarm("CHARGER FAULT", text);
    }
    charger_fault_ = cf;

    const bool rf = status_valid_ && status_.has_output_regulator && status_.output_regulator.fault_present;
    if (rf && !regulator_fault_) {
        const auto& o = status_.output_regulator;
        char text[48] = {};
        if (o.scp_fault) append(text, sizeof(text), "short circuit ");
        if (o.ocp_fault) append(text, sizeof(text), "over current ");
        if (o.ovp_fault) append(text, sizeof(text), "over voltage");
        alarm("VOUT FAULT", text[0] ? text : "fault");
    }
    regulator_fault_ = rf;

    const size_t n = backend_.applet_count();
    for (size_t i = 0; i < n && i < applet_state_.size(); ++i) {
        AppletControl* a = backend_.applet(i);
        if (!a) {
            continue;
        }
        const AppletRun state = a->run_state();
        if (state == AppletRun::Fault && applet_state_[i] != AppletRun::Fault) {
            char title[24] = {};
            copy(title, sizeof(title), a->name());
            append(title, sizeof(title), " FAULT");
            const char* why = a->fault_text();
            alarm(title, why ? why : "stopped");
        }
        applet_state_[i] = state;
    }
}

void Ui::update_power() noexcept {
    const Screen* screen = depth_ ? stack_[depth_ - 1] : nullptr;
    if (alarm_active_ || (screen && screen->keep_awake())) {
        power_ = DisplayPower::On;
        return;
    }
    const uint32_t idle = now_ - last_input_;
    if (settings_.off_after_s != 0 && idle >= settings_.off_after_s * 1000u) {
        power_ = DisplayPower::Off;
    } else if (settings_.dim_after_s != 0 && idle >= settings_.dim_after_s * 1000u) {
        power_ = DisplayPower::Dim;
    } else {
        power_ = DisplayPower::On;
    }
}

uint8_t Ui::contrast() const noexcept {
    switch (power_) {
        case DisplayPower::On: return settings_.contrast;
        case DisplayPower::Dim:
            return settings_.dim_contrast < settings_.contrast ? settings_.dim_contrast : settings_.contrast;
        case DisplayPower::Off: return 0;
    }
    return settings_.contrast;
}

void Ui::dispatch(const Event& e) noexcept {
    Screen* screen = top();
    const bool shutdown = e.key == Key::Shutdown && (e.type == EventType::Click || e.type == EventType::Long);
    if (shutdown && !(screen && screen->handles_shutdown_key())) {
        // Also with an alarm on screen: the alarm stays
        if (e.type == EventType::Click) {
            soft_stop();
        } else {
            power_off();
        }
        return;
    }
    if (alarm_active_) {
        if (e.select_click() || e.is(EventType::Press, Key::Left)) {
            alarm_active_ = false;
        }
        return;
    }
    if (screen && screen->on_event(*this, e)) {
        return;
    }
    if (e.select_long()) {
        home();
    } else if (e.is(EventType::Press, Key::Left)) {
        pop();
    }
}

void Ui::tick(uint32_t now_ms) noexcept {
    now_ = now_ms;

    Event e;
    while (events_.pop(e)) {
        const uint8_t bit = e.type == EventType::Turn ? 0 : key_bit(e.key);
        if (e.type == EventType::Press || e.type == EventType::Turn) {
            if (power_ != DisplayPower::On && e.key != Key::Shutdown) {
                // Wake only: the press (and its click/release) or the turn is not acted on.
                // SHUTDOWN always acts (stopping must never take two presses).
                wake();
                swallow_keys_ = static_cast<uint8_t>(swallow_keys_ | bit);
                continue;
            }
            if (power_ != DisplayPower::On) {
                wake();
            }
            last_input_ = now_ms;
        }
        if (bit && (swallow_keys_ & bit)) {
            if (e.type == EventType::Release) {
                swallow_keys_ = static_cast<uint8_t>(swallow_keys_ & ~bit);
            }
            continue;
        }
        if (e.type == EventType::Repeat || e.type == EventType::Long) {
            last_input_ = now_ms;
        }
        dispatch(e);
    }

    if (static_cast<int32_t>(now_ms - next_poll_) >= 0) {
        poll();
        next_poll_ = now_ms + kPollMs;
    }
    if (static_cast<int32_t>(now_ms - next_history_) >= 0) {
        sample_history();
        next_history_ += kHistoryMs;
        if (static_cast<int32_t>(now_ms - next_history_) >= 0) {
            next_history_ = now_ms + kHistoryMs;
        }
    }
    Screen* screen = top();
    if (screen) {
        screen->tick(*this);
    }
    if (settings_.pixel_shift) {
        if (static_cast<int32_t>(now_ms - next_shift_) >= 0) {
            shift_ = static_cast<int16_t>(shift_ ? 0 : 1);
            next_shift_ = now_ms + 60000;
        }
    } else {
        shift_ = 0;
    }
    update_power();
}

// ---------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------

void Ui::draw_status_bar(Canvas& c) noexcept {
    const auto& s = status_;
    char buf[16];
    int x = 0;

    // Battery: outline, level, voltage
    const bool have_bat = status_valid_ && s.has_charger_measurements;
    const bool bat_present = have_bat && s.has_charger && s.charger.battery_present;
    c.frame(Rect{0, 1, 11, 7}, gray::kText);
    c.vline(11, 3, 3, gray::kText);
    if (bat_present) {
        const int pct = battery_percent(s);
        const int w = (pct * 9 + 50) / 100;
        c.fill(Rect{1, 2, static_cast<int16_t>(w), 5}, pct < 15 ? gray::kValue : gray::kLabel);
    } else {
        c.line(2, 6, 9, 2, gray::kLabel);
    }
    x = 13;
    const bool charging = bat_present && s.has_charger &&
                          s.charger.charge_status != ChargeStatus_CHARGE_STATUS_NOT_CHARGING &&
                          s.charger.charge_status != ChargeStatus_CHARGE_STATUS_CHARGE_DONE;
    if (charging) {
        c.bitmap(x, 1, kIconBolt, gray::kValue);
    }
    x += 6;
    if (have_bat) {
        format_value(buf, sizeof(buf), s.charger_measurements.vbat_voltage, 1, "V");
        c.text(x, 1, buf, kFontSmall, gray::kText);
    } else {
        c.text(x, 1, "--", kFontSmall, gray::kDim);
    }

    // Input: dock / USB-C contract / battery only
    x = 47;
    const bool dock = status_valid_ && s.has_charger && s.charger.input_source == ChargerInputSource_CHARGER_INPUT_AC2 &&
                      s.charger.plugged_in;
    const bool usb_sink = status_valid_ && s.has_usb_pd && s.usb_pd.power_role == UsbPdPowerRole_USB_PD_POWER_ROLE_SINK &&
                          s.usb_pd.state != UsbPdState_USB_PD_STATE_DETACHED;
    const bool usb_source = status_valid_ && s.has_usb_pd && s.usb_pd.source_state == UsbPdSourceState_USB_PD_SOURCE_ACTIVE;
    if (dock) {
        c.bitmap(x, 1, kIconPlug, gray::kText);
        c.text(x + 7, 1, "DOCK", kFontSmall, gray::kText);
    } else if (usb_sink) {
        c.bitmap(x, 1, kIconPlug, gray::kText);
        const float v = s.usb_pd.has_contract_voltage ? s.usb_pd.contract_voltage : 5.0f;
        format_value(buf, sizeof(buf), v, 0, "V");
        c.text(x + 7, 1, buf, kFontSmall, gray::kText);
    } else if (status_valid_ && s.has_charger && s.charger.plugged_in) {
        c.bitmap(x, 1, kIconPlug, gray::kText);
    }

    // VOUT
    x = 75;
    if (status_valid_ && s.has_output_regulator && s.output_regulator.enabled) {
        c.fill(Rect{static_cast<int16_t>(x), 0, 7, 9}, gray::kLabel);
        c.text(x + 1, 1, "O", kFontSmall, gray::kOff);
        const float v = s.has_output_regulator_measurements ? s.output_regulator_measurements.output_voltage : 0.0f;
        format_value(buf, sizeof(buf), v, v < 9.95f ? 1 : 0, "V");
        c.text(x + 8, 1, buf, kFontSmall, gray::kText);
    }

    // Right: USB-C source, applet running, host link, fault
    x = kWidth - 7;
    const bool fault = status_valid_ && ((s.has_charger && s.charger.fault_present) ||
                                         (s.has_output_regulator && s.output_regulator.fault_present));
    if (fault) {
        c.bitmap(x, 1, kIconWarning, (now_ / 500) % 2 ? gray::kValue : gray::kLabel);
    }
    x -= 7;
    SystemInfo info;
    backend_.system_info(info);
    if (info.host_connected) {
        c.bitmap(x, 1, kIconLink, gray::kLabel);
    }
    x -= 6;
    if (running_applet() >= 0) {
        c.text(x, 1, "▶", kFontSmall, gray::kValue);   // play triangle
    }
    x -= 6;
    if (usb_source) {
        c.text(x, 1, "↑", kFontSmall, gray::kText);   // up arrow: sourcing USB-C
    }

    c.hline(0, kStatusHeight, kWidth, gray::kGrid);
}

void Ui::draw_toast(Canvas& c) noexcept {
    if (static_cast<int32_t>(toast_until_ - now_) <= 0 || toast_[0] == '\0') {
        return;
    }
    const int w = Canvas::text_width(toast_, kFontBody) + 8;
    const int16_t bw = static_cast<int16_t>(w > kWidth - 4 ? kWidth - 4 : w);
    const Rect box{static_cast<int16_t>((kWidth - bw) / 2), 92, bw, 14};
    c.fill(box, gray::kOff);
    c.frame(box, gray::kValue);
    c.text(box.inset(2, 1), toast_, kFontBody, gray::kValue, Align::Center);
}

void Ui::draw_alarm(Canvas& c) noexcept {
    if (!alarm_active_) {
        return;
    }
    const Rect box{4, 24, 120, 80};
    c.fill(box, gray::kOff);
    c.frame(box, gray::kValue);
    c.fill(Rect{4, 24, 120, 13}, gray::kValue);
    c.text(Rect{4, 25, 120, 12}, alarm_title_, kFontBody, gray::kOff, Align::Center);
    // Word wrap the text, 19 chars per line, 4 lines
    const char* p = alarm_text_;
    int y = 41;
    for (int line = 0; line < 4 && *p; ++line) {
        char row[20] = {};
        int len = 0;
        int last_space = -1;
        const char* start = p;
        while (start[len] && len < 19) {
            if (start[len] == ' ') {
                last_space = len;
            }
            ++len;
        }
        if (start[len] && last_space > 0) {
            len = last_space;
        }
        std::memcpy(row, start, static_cast<size_t>(len));
        c.text(8, y, row, kFontBody, gray::kText);
        p = start + len;
        while (*p == ' ') {
            ++p;
        }
        y += 11;
    }
    c.text(Rect{4, 92, 120, 10}, "• OK", kFontSmall, gray::kLabel, Align::Center);
}

void Ui::render(Canvas& c) noexcept {
    c.reset_clip();
    c.set_origin(0, 0);
    c.clear(gray::kOff);
    c.set_origin(shift_, 0);
    Screen* screen = top();
    if (screen && screen->full_screen()) {
        const Rect all{0, 0, kWidth, kHeight};
        c.set_clip(all);
        screen->draw(*this, c, all);
        c.reset_clip();
    } else {
        draw_status_bar(c);
        const Rect body{0, kStatusHeight + 2, kWidth, kHeight - kStatusHeight - 2 - kFooterHeight - 1};
        const Rect footer{0, kHeight - kFooterHeight, kWidth, kFooterHeight};
        if (screen) {
            c.set_clip(body);
            screen->draw(*this, c, body);
            c.reset_clip();
            c.hline(0, footer.y - 1, kWidth, gray::kGrid);
            c.set_clip(footer);
            screen->draw_footer(*this, c, footer);
            c.reset_clip();
        }
    }
    draw_toast(c);
    draw_alarm(c);
    c.set_origin(0, 0);
}

} // namespace ui
