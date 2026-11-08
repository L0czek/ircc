#ifndef UI_UI_HPP
#define UI_UI_HPP

#include "ui/backend.hpp"
#include "ui/gfx.hpp"
#include "ui/input.hpp"
#include <array>
#include <cstdint>

namespace ui {

/** @brief Panel settings (RAM; defaults at boot) */
struct Settings {
    uint8_t contrast = 0x7F;
    uint8_t dim_contrast = 0x10;
    uint16_t dim_after_s = 60;      // 0 = never
    uint16_t off_after_s = 300;     // 0 = never
    bool rotate180 = false;
    bool encoder_reverse = false;
    bool encoder_accel = true;
    bool pixel_shift = true;        // move the image by 1 px every minute (burn-in)
};

enum class DisplayPower : uint8_t { On, Dim, Off };

/** @brief History of a measurement: one sample per second, 128 samples (one per pixel) */
class Series {
public:
    static constexpr size_t kSize = 128;
    void push(float value) noexcept;
    size_t size() const noexcept { return count_; }
    /** @brief i = 0 is the oldest sample */
    float at(size_t i) const noexcept;
    float last() const noexcept { return count_ ? at(count_ - 1) : 0.0f; }

private:
    std::array<int16_t, kSize> milli_{};   // value * 1000, saturated to +-32.767
    size_t head_ = 0;
    size_t count_ = 0;
};

enum class Signal : uint8_t { Vbat, Ibat, Vbus, Ibus, Vout, Iout, Count };

class Ui;

/** @brief One page of the panel. Screens are static objects pushed on Ui's stack. */
class Screen {
public:
    /** @brief Handle an event; false = not consumed (global handling: back / home) */
    virtual bool on_event(Ui& ui, const Event& e) noexcept = 0;
    virtual void on_enter(Ui& ui) noexcept { (void)ui; }
    virtual void tick(Ui& ui) noexcept { (void)ui; }
    virtual void draw(Ui& ui, Canvas& c, const Rect& body) noexcept = 0;
    /** @brief Footer (hint bar); default: hints() text */
    virtual void draw_footer(Ui& ui, Canvas& c, const Rect& footer) noexcept;
    virtual const char* hints() const noexcept { return "◀back  •select"; }
    /** @brief Hide the status bar and footer (tests) */
    virtual bool full_screen() const noexcept { return false; }
    /** @brief SHUTDOWN key events go to the screen instead of soft stop / power off */
    virtual bool handles_shutdown_key() const noexcept { return false; }
    /** @brief No dimming or display off while this screen is on top */
    virtual bool keep_awake() const noexcept { return false; }

protected:
    ~Screen() = default;
};

/**
 * @brief Panel UI: input events, screen stack, machine-state snapshot, alarms/toasts,
 *        display power policy. Platform independent: the firmware (front_panel.cc) and the
 *        host simulator feed it keys, encoder detents and time, and flush the framebuffer.
 */
class Ui {
public:
    static constexpr int kStatusHeight = 9;
    static constexpr int kFooterHeight = 9;
    static constexpr uint32_t kPollMs = 250;
    static constexpr uint32_t kHistoryMs = 1000;

    Ui(Backend& backend, Settings& settings) noexcept;

    /** @brief Push the home screen */
    void start(Screen& home, uint32_t now_ms) noexcept;

    // ---- Inputs (call before tick) ----
    void keys(uint8_t raw_mask, uint32_t now_ms) noexcept;
    void encoder(int32_t detents, uint32_t now_ms) noexcept;

    /** @brief Timers, polling, event dispatch */
    void tick(uint32_t now_ms) noexcept;
    /** @brief Draw the whole frame */
    void render(Canvas& canvas) noexcept;

    // ---- Display policy ----
    DisplayPower display_power() const noexcept { return power_; }
    uint8_t contrast() const noexcept;
    int16_t shift_x() const noexcept { return shift_; }

    // ---- For screens ----
    Backend& backend() noexcept { return backend_; }
    Settings& settings() noexcept { return settings_; }
    uint32_t now() const noexcept { return now_; }
    const SystemStatus& status() const noexcept { return status_; }
    bool status_valid() const noexcept { return status_valid_; }
    const Series& history(Signal s) const noexcept { return history_[static_cast<size_t>(s)]; }
    const KeyTracker& key_tracker() const noexcept { return keys_; }
    uint8_t raw_keys() const noexcept { return raw_keys_; }
    int32_t encoder_position() const noexcept { return encoder_position_; }

    void push(Screen& screen) noexcept;
    void pop() noexcept;
    void home() noexcept;
    Screen* top() noexcept { return depth_ ? stack_[depth_ - 1] : nullptr; }

    void toast(const char* text, uint32_t duration_ms = 1500) noexcept;
    void alarm(const char* title, const char* text) noexcept;
    bool alarm_active() const noexcept { return alarm_active_; }

    /** @brief The shared request, zeroed: fill it, then execute() */
    Request& new_request() noexcept;
    /**
     * @brief Run new_request() through the backend; a returned system_status refreshes the
     *        snapshot, a failure shows a toast. Returns true on success.
     */
    bool execute() noexcept;
    const Response& response() const noexcept { return response_; }

    /** @brief Applet helpers (toast on failure) */
    bool start_applet(size_t index) noexcept;
    void stop_applet(size_t index) noexcept;
    /** @brief Index of the running applet, or -1 */
    int running_applet() noexcept;

    /**
     * @brief SHUTDOWN key. Click: soft stop (applets stopped, VOUT off). Hold: soft stop,
     *        then power off (ship mode) once all power is unplugged; opens the power off
     *        screen. Both are host requests (power_off), so the host can do the same.
     */
    void soft_stop() noexcept;
    void power_off() noexcept;

    /** @brief Force a status poll on the next tick */
    void poll_soon() noexcept { next_poll_ = now_; }

private:
    void dispatch(const Event& e) noexcept;
    void poll() noexcept;
    void sample_history() noexcept;
    void check_alarms() noexcept;
    void update_power() noexcept;
    void wake() noexcept;
    void draw_status_bar(Canvas& c) noexcept;
    void draw_toast(Canvas& c) noexcept;
    void draw_alarm(Canvas& c) noexcept;

    Backend& backend_;
    Settings& settings_;
    uint32_t now_ = 0;

    KeyTracker keys_;
    EncoderAccel accel_;
    EventQueue events_;
    uint8_t raw_keys_ = 0;
    int32_t encoder_position_ = 0;

    std::array<Screen*, 8> stack_{};
    size_t depth_ = 0;

    SystemStatus status_{};
    bool status_valid_ = false;
    uint32_t next_poll_ = 0;
    uint32_t next_history_ = 0;
    std::array<Series, static_cast<size_t>(Signal::Count)> history_{};

    Request request_{};
    Response response_{};

    // Alarms (rising edges)
    bool charger_fault_ = false;
    bool regulator_fault_ = false;
    std::array<AppletRun, 16> applet_state_{};

    char toast_[40] = {};
    uint32_t toast_until_ = 0;
    bool alarm_active_ = false;
    char alarm_title_[24] = {};
    char alarm_text_[64] = {};

    DisplayPower power_ = DisplayPower::On;
    uint32_t last_input_ = 0;
    uint8_t swallow_keys_ = 0;      // keys that woke the display: their events are dropped
    int16_t shift_ = 0;
    uint32_t next_shift_ = 0;
};

/** @brief Charge state as short text ("FAST", "DONE", ...) */
const char* charge_state_text(ChargeStatus s) noexcept;
/** @brief Battery cells from the charge voltage limit (VREG), at least 1 */
int battery_cells(const SystemStatus& s) noexcept;
/** @brief Rough battery level 0..100 from the cell voltage (resting curve, Li-ion) */
int battery_percent(const SystemStatus& s) noexcept;

} // namespace ui

#endif
