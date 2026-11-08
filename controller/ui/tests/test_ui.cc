// Host unit tests for the front panel UI (controller/ui)
#include "applet_control.hpp"
#include "ui/format.hpp"
#include "ui/gfx.hpp"
#include "ui/input.hpp"
#include "ui/menu.hpp"
#include "ui/screens.hpp"
#include "ui/ui.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace ui;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                           \
    } while (0)

#define CHECK_STR(a, b)                                                             \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (std::strcmp((a), (b)) != 0) {                                           \
            ++g_failures;                                                           \
            std::printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        }                                                                           \
    } while (0)

static void test_format() {
    char b[32];
    format_fixed(b, sizeof(b), 12.345f, 2);
    CHECK_STR(b, "12.35");
    format_fixed(b, sizeof(b), -0.5f, 1);
    CHECK_STR(b, "-0.5");
    format_fixed(b, sizeof(b), -0.004f, 2);
    CHECK_STR(b, "0.00");
    format_fixed(b, sizeof(b), 1.2f, 2, true);
    CHECK_STR(b, "+1.20");
    format_fixed(b, sizeof(b), 7.0f, 0);
    CHECK_STR(b, "7");
    format_fixed(b, sizeof(b), NAN, 2);
    CHECK_STR(b, "--");
    format_value(b, sizeof(b), 15.62f, 1, "V");
    CHECK_STR(b, "15.6V");
    format_si(b, sizeof(b), 125000.0f, "Hz");
    CHECK_STR(b, "125kHz");
    format_si(b, sizeof(b), 0.0042f, "A");
    CHECK_STR(b, "4.20mA");
    format_si(b, sizeof(b), 1.5e6f, "Hz");
    CHECK_STR(b, "1.50MHz");
    format_si(b, sizeof(b), 0.0f, "V");
    CHECK_STR(b, "0.00V");
    format_duration(b, sizeof(b), 42);
    CHECK_STR(b, "0:42");
    format_duration(b, sizeof(b), 3723);
    CHECK_STR(b, "1:02:03");
    char small[4];
    format_fixed(small, sizeof(small), 12345.0f, 0);
    CHECK_STR(small, "123");
    std::strcpy(b, "ab");
    append(b, sizeof(b), "cd");
    CHECK_STR(b, "abcd");
}

static void test_utf8() {
    const char* s = "A°▶";
    CHECK(utf8_next(s) == 'A');
    CHECK(utf8_next(s) == 0xB0);
    CHECK(utf8_next(s) == 0x25B6);
    CHECK(utf8_next(s) == 0);
    CHECK(utf8_length("12.3µ" "A") == 6);
    CHECK(kFontBody.index('A') != kFontBody.fallback);
    CHECK(kFontBody.index(0x1234) == kFontBody.fallback);
    CHECK(kFontSmall.index(0x25B6) != kFontSmall.fallback);
}

static std::vector<Event> drain(EventQueue& q) {
    std::vector<Event> v;
    Event e;
    while (q.pop(e)) {
        v.push_back(e);
    }
    return v;
}

static void test_keys() {
    KeyTracker t;
    EventQueue q;
    const uint8_t center = key_bit(Key::Center);
    // Bounce shorter than the debounce time is ignored
    t.update(center, 0, q);
    t.update(0, 5, q);
    t.update(0, 20, q);
    CHECK(drain(q).empty());
    // Click
    t.update(center, 100, q);
    t.update(center, 112, q);
    auto ev = drain(q);
    CHECK(ev.size() == 1 && ev[0].is(EventType::Press, Key::Center));
    t.update(0, 200, q);
    t.update(0, 212, q);
    ev = drain(q);
    CHECK(ev.size() == 2 && ev[0].is(EventType::Click, Key::Center) && ev[1].is(EventType::Release, Key::Center));
    // Long press: Long once, no Click on release
    t.update(center, 1000, q);
    t.update(center, 1012, q);
    t.update(center, 1700, q);
    t.update(center, 1800, q);
    ev = drain(q);
    CHECK(ev.size() == 2 && ev[1].is(EventType::Long, Key::Center));
    t.update(0, 1900, q);
    t.update(0, 1912, q);
    ev = drain(q);
    CHECK(ev.size() == 1 && ev[0].is(EventType::Release, Key::Center));
    // Repeat on navigation keys
    const uint8_t down = key_bit(Key::Down);
    t.update(down, 3000, q);
    t.update(down, 3012, q);
    t.update(down, 3500, q);
    t.update(down, 3620, q);
    ev = drain(q);
    int repeats = 0;
    for (auto& e : ev) {
        repeats += e.is(EventType::Repeat, Key::Down);
    }
    CHECK(ev[0].is(EventType::Press, Key::Down));
    CHECK(repeats == 2);
    // PCF8574 mapping (active low)
    CHECK(keys_from_pcf8574(0xFF) == 0);
    CHECK(keys_from_pcf8574(static_cast<uint8_t>(~0x01)) == key_bit(Key::Push));
    CHECK(keys_from_pcf8574(static_cast<uint8_t>(~0x22)) == (key_bit(Key::Up) | key_bit(Key::Center)));
    CHECK(keys_from_pcf8574(static_cast<uint8_t>(~0x40)) == 0);   // P6 unused
    CHECK(keys_from_pcf8574(static_cast<uint8_t>(~0x80)) == key_bit(Key::Shutdown));
}

static void test_encoder() {
    EncoderAccel a;
    EventQueue q;
    a.update(1, 0, q);
    auto ev = drain(q);
    CHECK(ev.size() == 1 && ev[0].type == EventType::Turn && ev[0].delta == 1 && ev[0].accel == 1);
    // Fast spin: 6 detents in 30 ms
    for (uint32_t t = 1000; t < 1030; t += 5) {
        a.update(1, t, q);
    }
    ev = drain(q);
    CHECK(ev.back().accel == 10);
    for (uint32_t t = 2000; t < 2016; t += 1) {
        a.update(1, t, q);
    }
    ev = drain(q);
    CHECK(ev.back().accel == 100);
    // Slow again after the window
    a.update(-1, 5000, q);
    ev = drain(q);
    CHECK(ev.size() == 1 && ev[0].delta == -1 && ev[0].accel == 1);
    a.set_config(EncoderAccel::Config{true, false, 5, 15});
    a.update(2, 9000, q);
    ev = drain(q);
    CHECK(ev[0].delta == -2);
    a.update(0, 9001, q);
    CHECK(q.empty());
}

static void test_canvas() {
    static Framebuffer fb;
    Canvas c(fb);
    c.clear(0);
    c.pixel(0, 0, 15);
    c.pixel(1, 0, 3);
    CHECK(fb.data()[0] == 0xF3);
    CHECK(fb.get(1, 0) == 3);
    c.fill(Rect{10, 10, 4, 2}, 7);
    CHECK(fb.get(10, 10) == 7 && fb.get(13, 11) == 7 && fb.get(14, 11) == 0 && fb.get(10, 12) == 0);
    // Clipping
    c.set_clip(Rect{20, 20, 5, 5});
    c.fill(Rect{0, 0, 128, 128}, 9);
    CHECK(fb.get(19, 20) == 0 && fb.get(20, 20) == 9 && fb.get(24, 24) == 9 && fb.get(25, 24) == 0);
    c.reset_clip();
    // Origin offset
    c.clear(0);
    c.set_origin(1, 0);
    c.pixel(0, 5, 15);
    CHECK(fb.get(1, 5) == 15 && fb.get(0, 5) == 0);
    c.set_origin(0, 0);
    // Text
    c.clear(0);
    const int end = c.text(0, 0, "AB", kFontBody, 15);
    CHECK(end == 12);
    int lit = 0;
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 12; ++x) {
            lit += fb.get(x, y) == 15;
        }
    }
    CHECK(lit > 10);
    CHECK(Canvas::text_width("°" "C", kFontSmall) == 10);
    // Ellipsis
    c.clear(0);
    const int e2 = c.text_clipped(0, 0, "abcdefgh", kFontBody, 15, 4);
    CHECK(e2 == 24);
}

static void test_rows() {
    static Framebuffer fb;
    RowTracker rows;
    RowTracker::Run runs[4];
    fb.fill(0);
    size_t n = rows.changed_runs(fb, runs, 4);
    CHECK(n == 1 && runs[0].y0 == 0 && runs[0].y1 == 127);   // first flush: everything
    n = rows.changed_runs(fb, runs, 4);
    CHECK(n == 0);
    fb.set(5, 10, 3);
    fb.set(5, 11, 3);
    fb.set(5, 127, 3);
    n = rows.changed_runs(fb, runs, 4);
    CHECK(n == 2 && runs[0].y0 == 10 && runs[0].y1 == 11 && runs[1].y0 == 127 && runs[1].y1 == 127);
    // More runs than slots: merged into the last one
    for (int y = 0; y < 20; y += 2) {
        fb.set(0, y, 9);
    }
    n = rows.changed_runs(fb, runs, 4);
    CHECK(n == 4 && runs[3].y1 == 18);
    rows.invalidate();
    n = rows.changed_runs(fb, runs, 4);
    CHECK(n == 1 && runs[0].y0 == 0 && runs[0].y1 == 127);
}

static void test_check_param() {
    const char* const choices[] = {"a", "b", "c"};
    const ParamDesc num{"f", "F", "Hz", ParamType::Number, 1.0f, 100.0f, 0.5f, 1, {}, 0};
    const ParamDesc ch{"m", "M", "", ParamType::Choice, 0, 2, 1, 0, choices, 0};
    const ParamDesc bl{"b", "B", "", ParamType::Bool, 0, 1, 1, 0, {}, 0};
    CHECK(*check_param(num, 10.26f) == 10.5f);
    CHECK(!check_param(num, 0.5f));
    CHECK(!check_param(num, 100.6f));
    CHECK(*check_param(num, 100.0f) == 100.0f);
    CHECK(!check_param(num, NAN));
    CHECK(*check_param(ch, 2.2f) == 2.0f);
    CHECK(!check_param(ch, 3.0f));
    CHECK(*check_param(bl, 5.0f) == 1.0f);
}

// ---- Menu editing through a minimal backend ----

class NullBackend final : public Backend {
public:
    bool execute(const Request&, Response& r) noexcept override {
        r.has_result = true;
        r.result.status = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
        return true;
    }
    size_t applet_count() noexcept override { return 0; }
    AppletControl* applet(size_t) noexcept override { return nullptr; }
    AppletResult applet_start(size_t) noexcept override { return std::unexpected(AppletError::NotFound); }
    void applet_stop(size_t) noexcept override {}
    AppletResult applet_set(size_t, size_t, float) noexcept override { return {}; }
    uint32_t applet_started_ms(size_t) noexcept override { return 0; }
    void system_info(SystemInfo&) noexcept override {}
};

class TestModel final : public MenuModel {
public:
    float number = 5.0f;
    float live = 10.0f;
    int sets = 0;
    int activations = 0;
    const char* title(Ui&) noexcept override { return "Test"; }
    size_t count(Ui&) noexcept override { return 3; }
    MenuItem item(Ui&, size_t i) noexcept override {
        MenuItem m;
        if (i == 0) {
            m.kind = ItemKind::Number;
            m.label = "Num";
            m.spec.min = 0.0f;
            m.spec.max = 20.0f;
            m.spec.step = 0.1f;
            m.spec.decimals = 1;
        } else if (i == 1) {
            m.kind = ItemKind::Number;
            m.label = "Live";
            m.spec.min = 0.0f;
            m.spec.max = 100.0f;
            m.spec.step = 1.0f;
            m.spec.live = true;
        } else {
            m.kind = ItemKind::HoldAction;
            m.label = "Start";
        }
        return m;
    }
    float value(Ui&, size_t i) noexcept override { return i == 0 ? number : live; }
    bool set(Ui&, size_t i, float v) noexcept override {
        ++sets;
        (i == 0 ? number : live) = v;
        return true;
    }
    void activate(Ui&, size_t) noexcept override { ++activations; }
};

static void test_menu() {
    NullBackend backend;
    Settings settings;
    Ui ui(backend, settings);
    TestModel model;
    MenuScreen menu(model);
    ui.start(menu, 0);
    uint32_t t = 0;
    auto tick = [&](uint32_t ms) {
        for (uint32_t i = 0; i < ms; i += 10) {
            t += 10;
            ui.tick(t);
        }
    };
    auto press = [&](Key k, uint32_t hold_ms = 40) {
        ui.keys(key_bit(k), t);
        tick(20);
        ui.keys(key_bit(k), t);
        tick(hold_ms);
        ui.keys(0, t);
        tick(20);
        ui.keys(0, t);
        tick(20);
    };
    auto turn = [&](int d) {
        ui.encoder(d, t);
        tick(200);
    };

    // Edit number: +3 detents x 0.1, coarser cursor (x1), +1, commit
    press(Key::Push);
    turn(3);
    press(Key::Left);
    press(Key::Up);
    CHECK(model.sets == 0);       // not live: nothing applied yet
    press(Key::Push);
    CHECK(model.sets == 1);
    CHECK(std::fabs(model.number - 6.3f) < 1e-4f);

    // Cancel with a long press restores (nothing applied)
    press(Key::Push);
    turn(5);
    press(Key::Push, 800);
    CHECK(model.sets == 1);
    CHECK(std::fabs(model.number - 6.3f) < 1e-4f);

    // Live value: applied while turning, restored on cancel
    turn(1);   // select item 1
    CHECK(menu.selected() == 1);
    press(Key::Center);
    turn(4);
    CHECK(model.live == 14.0f);
    press(Key::Center, 800);
    CHECK(model.live == 10.0f);

    // Hold action: short press does nothing, 1 s hold activates once
    turn(1);
    CHECK(menu.selected() == 2);
    press(Key::Center, 300);
    CHECK(model.activations == 0);
    press(Key::Center, 1200);
    CHECK(model.activations == 1);

    // Clamping at the range limit
    turn(-2);
    CHECK(menu.selected() == 0);
    press(Key::Push);
    ui.encoder(1, t);
    for (int i = 0; i < 20; ++i) {
        ui.encoder(1, t + static_cast<uint32_t>(i));
    }
    tick(200);
    press(Key::Push);
    CHECK(model.number <= 20.0f && model.number > 19.0f);

    // Render does not crash and draws something
    static Framebuffer fb;
    Canvas c(fb);
    ui.render(c);
    int lit = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            lit += fb.get(x, y) != 0;
        }
    }
    CHECK(lit > 100);
}

static void test_display_power() {
    NullBackend backend;
    Settings settings;
    settings.dim_after_s = 10;
    settings.off_after_s = 60;
    Ui ui(backend, settings);
    TestModel model;
    MenuScreen menu(model);
    ui.start(menu, 0);
    ui.tick(5000);
    CHECK(ui.display_power() == DisplayPower::On);
    ui.tick(11000);
    CHECK(ui.display_power() == DisplayPower::Dim);
    CHECK(ui.contrast() == settings.dim_contrast);
    ui.tick(61000);
    CHECK(ui.display_power() == DisplayPower::Off);
    CHECK(ui.contrast() == 0);
    // A press wakes the display and is not acted on
    ui.keys(key_bit(Key::Down), 61000);
    ui.keys(key_bit(Key::Down), 61020);
    ui.tick(61020);
    CHECK(ui.display_power() == DisplayPower::On);
    CHECK(menu.selected() == 0);
    ui.keys(0, 61100);
    ui.keys(0, 61120);
    ui.tick(61120);
    ui.keys(key_bit(Key::Down), 61200);
    ui.keys(key_bit(Key::Down), 61220);
    ui.tick(61220);
    CHECK(menu.selected() == 1);
}

// Records power_off requests and reports a ship-mode power off like the firmware
class PowerOffBackend final : public Backend {
public:
    std::vector<PowerOffAction> actions;
    PowerOffState state = PowerOffState_POWER_OFF_IDLE;
    bool usb_c = true;

    bool execute(const Request& req, Response& r) noexcept override {
        if (req.which_command == Request_power_off_tag) {
            actions.push_back(req.command.power_off.action);
            if (req.command.power_off.action == PowerOffAction_POWER_OFF_SHIP) {
                state = PowerOffState_POWER_OFF_WAIT_SOURCES;
            } else if (req.command.power_off.action == PowerOffAction_POWER_OFF_CANCEL) {
                state = PowerOffState_POWER_OFF_IDLE;
            }
        }
        r.which_payload = Response_system_status_tag;
        r.payload.system_status.has_power_off = state != PowerOffState_POWER_OFF_IDLE;
        r.payload.system_status.power_off.state = state;
        r.payload.system_status.power_off.usb_c_present = usb_c;
        r.has_result = true;
        r.result.status = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
        return true;
    }
    size_t applet_count() noexcept override { return 0; }
    AppletControl* applet(size_t) noexcept override { return nullptr; }
    AppletResult applet_start(size_t) noexcept override { return std::unexpected(AppletError::NotFound); }
    void applet_stop(size_t) noexcept override {}
    AppletResult applet_set(size_t, size_t, float) noexcept override { return {}; }
    uint32_t applet_started_ms(size_t) noexcept override { return 0; }
    void system_info(SystemInfo&) noexcept override {}
};

static void press_key(Ui& ui, Key k, uint32_t& t, uint32_t hold_ms) {
    for (uint32_t end = t + hold_ms; t <= end; t += 20) {
        ui.keys(key_bit(k), t);
        ui.tick(t);
    }
    for (int i = 0; i < 2; ++i, t += 20) {
        ui.keys(0, t);
        ui.tick(t);
    }
}

static void test_shutdown_key() {
    PowerOffBackend backend;
    Settings settings;
    Ui ui(backend, settings);
    TestModel model;
    MenuScreen menu(model);
    ui.start(menu, 0);
    uint32_t t = 1000;

    // Click: soft stop, the screen stays
    press_key(ui, Key::Shutdown, t, 100);
    CHECK(backend.actions.size() == 1 && backend.actions[0] == PowerOffAction_POWER_OFF_SOFT);
    CHECK(ui.top() == &menu);

    // Hold: ship-mode power off and its screen; navigation keys do nothing there
    press_key(ui, Key::Shutdown, t, 800);
    CHECK(backend.actions.size() == 2 && backend.actions[1] == PowerOffAction_POWER_OFF_SHIP);
    CHECK(ui.top() == &power_off_screen());
    CHECK(backend.actions.size() == 2);
    press_key(ui, Key::Down, t, 40);
    CHECK(ui.top() == &power_off_screen());

    // It keeps the display on
    ui.tick(t + 400000);
    t += 400000;
    CHECK(ui.display_power() == DisplayPower::On);

    // SHUTDOWN click on that screen cancels
    press_key(ui, Key::Shutdown, t, 100);
    CHECK(backend.actions.size() == 3 && backend.actions[2] == PowerOffAction_POWER_OFF_CANCEL);
    CHECK(ui.top() == &menu);

    // Cancelled elsewhere (host): the screen closes by itself
    press_key(ui, Key::Shutdown, t, 800);
    CHECK(ui.top() == &power_off_screen());
    backend.state = PowerOffState_POWER_OFF_IDLE;
    for (uint32_t end = t + 3000; t < end; t += 100) {
        ui.tick(t);
    }
    CHECK(ui.top() == &menu);

    // SHUTDOWN acts even while the display is off
    settings.off_after_s = 10;
    ui.tick(t + 20000);
    t += 20000;
    CHECK(ui.display_power() == DisplayPower::Off);
    press_key(ui, Key::Shutdown, t, 100);
    CHECK(backend.actions.back() == PowerOffAction_POWER_OFF_SOFT);
    CHECK(ui.display_power() == DisplayPower::On);
}

int main() {
    test_format();
    test_utf8();
    test_keys();
    test_encoder();
    test_canvas();
    test_rows();
    test_check_param();
    test_menu();
    test_display_power();
    test_shutdown_key();
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
