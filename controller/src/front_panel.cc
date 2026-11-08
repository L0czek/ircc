#include "front_panel.hpp"

#if defined(resonant_converter_controller)

#include "FreeRTOS.h"
#include "applet_runner.hpp"
#include "board.h"
#include "board_io.hpp"
#include "cmsis_os2.h"
#include "diagnostics.hpp"
#include "error.hpp"
#include "i2c_bus.hpp"
#include "log.hpp"
#include "message_handler.hpp"
#include "ssd1327.hpp"
#include "ssd1327_hal.hpp"
#include "ui/screens.hpp"
#include "ui/ui.hpp"

namespace front_panel {

namespace {

constexpr uint32_t kFrameMs = 40;            // 25 Hz
constexpr uint32_t kRetryMs = 3000;          // missing panel / keypad
constexpr uint32_t kDmaTimeoutMs = 50;
constexpr uint32_t kKeyFlag = 0x01;
constexpr uint32_t kDmaFlag = 0x02;
constexpr uint32_t kI2cTimeoutMs = 5;

constexpr size_t kStackBytes = 3072;
alignas(8) uint8_t g_stack[kStackBytes];
StaticTask_t g_tcb;
osThreadId_t g_thread = nullptr;

void rtos_delay(uint32_t ms) {
    osDelay(ms);
}

ssd1327::HalTransport::Pin pin(const GPIO_pin_t& p) noexcept {
    return ssd1327::HalTransport::Pin{p.GPIOx, static_cast<uint16_t>(p.pin)};
}

// ---------------------------------------------------------------------------------------
// Backend: the panel's view of the controller
// ---------------------------------------------------------------------------------------

class FirmwareBackend final : public ui::Backend {
public:
    void set_handler(MessageHandler* handler) noexcept { handler_ = handler; }
    void set_keypad(bool ok) noexcept { keypad_ok_ = ok; }
    void count_display_error() noexcept { ++display_errors_; }

    bool execute(const Request& request, Response& response) noexcept override {
        return handler_ != nullptr && handler_->handle_local(request, response);
    }

    size_t applet_count() noexcept override { return AppletRunner::instance().count(); }
    AppletControl* applet(size_t index) noexcept override { return AppletRunner::instance().applet(index); }
    AppletResult applet_start(size_t index) noexcept override { return AppletRunner::instance().start(index); }
    void applet_stop(size_t index) noexcept override { AppletRunner::instance().stop(index); }
    AppletResult applet_set(size_t index, size_t param, float value) noexcept override {
        return AppletRunner::instance().set_param(index, param, value);
    }
    uint32_t applet_started_ms(size_t index) noexcept override { return AppletRunner::instance().started_ms(index); }

    void system_info(ui::SystemInfo& info) noexcept override {
        info.firmware = Diagnostics::firmware_version();
        info.board = Diagnostics::board_name();
        info.uptime_s = osKernelGetTickCount() / 1000;
        info.host_connected = MessageHandler::host_active();
        info.free_heap = static_cast<uint32_t>(xPortGetFreeHeapSize());
        info.i2c_recoveries = i2c_internal_bus_recovery_count();
        info.keypad_ok = keypad_ok_;
        info.keypad_address = kKeypadI2cAddress;
        info.display_errors = display_errors_;
    }

private:
    MessageHandler* handler_ = nullptr;
    bool keypad_ok_ = false;
    uint32_t display_errors_ = 0;
};

// ---------------------------------------------------------------------------------------
// Keypad (PCF8574 on the internal I2C bus)
// ---------------------------------------------------------------------------------------

std::expected<void, ControllerError> keypad_init() noexcept {
    return i2c_internal_bus_transaction([]() -> std::expected<void, ControllerError> {
        uint8_t all_inputs = 0xFF;   // quasi-bidirectional: 1 = weak pull-up, readable input
        const HAL_StatusTypeDef st = HAL_I2C_Master_Transmit(g_board_i2c_internal, static_cast<uint16_t>(kKeypadI2cAddress << 1),
                                                             &all_inputs, 1, kI2cTimeoutMs);
        if (st != HAL_OK) {
            return std::unexpected(from_hal_status(st));
        }
        return {};
    });
}

std::expected<uint8_t, ControllerError> keypad_read() noexcept {
    return i2c_internal_bus_transaction([]() -> std::expected<uint8_t, ControllerError> {
        uint8_t port = 0xFF;
        const HAL_StatusTypeDef st = HAL_I2C_Master_Receive(g_board_i2c_internal, static_cast<uint16_t>(kKeypadI2cAddress << 1),
                                                            &port, 1, kI2cTimeoutMs);
        if (st != HAL_OK) {
            return std::unexpected(from_hal_status(st));
        }
        return port;
    });
}

// ---------------------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------------------

class Panel {
public:
    Panel() noexcept
        : transport_(g_board_lcd_spi, pin(g_board_lcd_cs), pin(g_board_lcd_dc), pin(g_board_lcd_rst), &rtos_delay),
          display_(transport_, display_config()),
          ui_(backend_, settings_),
          canvas_(frame_) {}

    void set_handler(MessageHandler& handler) noexcept { backend_.set_handler(&handler); }
    ssd1327::HalTransport& transport() noexcept { return transport_; }

    void run() noexcept {
        transport_.set_done_callback(&Panel::dma_done, nullptr);
        ui_.start(ui::home_screen(), osKernelGetTickCount());
        uint32_t next_frame = osKernelGetTickCount();
        for (;;) {
            const uint32_t now = osKernelGetTickCount();
            const int32_t wait = static_cast<int32_t>(next_frame - now);
            const uint32_t flags = osThreadFlagsWait(kKeyFlag, osFlagsWaitAny, wait > 0 ? static_cast<uint32_t>(wait) : 0);
            const bool key_irq = (flags & 0x80000000u) == 0 && (flags & kKeyFlag);
            const uint32_t t = osKernelGetTickCount();

            read_inputs(t, key_irq);
            if (static_cast<int32_t>(t - next_frame) < 0) {
                ui_.tick(t);   // key interrupt between frames: react at once, draw on the frame tick
                continue;
            }
            next_frame += kFrameMs;
            if (static_cast<int32_t>(t - next_frame) > 0) {
                next_frame = t + kFrameMs;   // fell behind (bus busy): do not burst
            }
            ui_.tick(t);
            update_display(t);
        }
    }

private:
    static ssd1327::Config display_config() noexcept {
        ssd1327::Config c;
        c.contrast = 0x7F;
        c.orientation = ssd1327::Orientation::Normal;   // FPC tail at the bottom of the IO board
        c.internal_vdd = false;                         // VDD from the LP5907 2.5 V LDO
        return c;
    }

    static void dma_done(void*) {
        if (g_thread != nullptr) {
            (void)osThreadFlagsSet(g_thread, kDmaFlag);
        }
    }

    void read_inputs(uint32_t now, bool key_irq) noexcept {
        // Keypad: every frame (cheap), at once on KEY_INT; a missing keypad is retried slowly
        if (keypad_ok_ || static_cast<int32_t>(now - keypad_retry_at_) >= 0) {
            if (!keypad_ok_) {
                keypad_ok_ = keypad_init().has_value();
                backend_.set_keypad(keypad_ok_);
                keypad_retry_at_ = now + kRetryMs;
                if (!keypad_ok_ && !keypad_warned_) {
                    os::warn("panel: keypad (PCF8574 0x%02x) not answering\n", kKeypadI2cAddress);
                    keypad_warned_ = true;
                }
            }
            if (keypad_ok_ || key_irq) {
                auto port = keypad_read();
                if (port) {
                    keys_ = ui::keys_from_pcf8574(*port);
                } else {
                    keypad_ok_ = false;
                    backend_.set_keypad(false);
                    keypad_retry_at_ = now + kRetryMs;
                    keys_ = 0;
                }
            }
        }
        ui_.keys(keys_, now);

        Encoder* enc = board::encoder();
        ui_.encoder(enc ? enc->take_delta() : 0, now);
    }

    bool ensure_display(uint32_t now) noexcept {
        if (display_.powered()) {
            return true;
        }
        if (static_cast<int32_t>(now - display_retry_at_) < 0) {
            return false;
        }
        display_retry_at_ = now + kRetryMs;
        auto r = display_.init();
        if (!r) {
            backend_.count_display_error();
            if (!display_warned_) {
                os::warn("panel: display init failed (%s)\n", ssd1327::error_text(r.error()));
                display_warned_ = true;
            }
            return false;
        }
        rows_.invalidate();
        orientation_applied_ = false;
        contrast_applied_ = 0;
        return true;
    }

    void update_display(uint32_t now) noexcept {
        if (ui_.display_power() == ui::DisplayPower::Off) {
            if (display_.powered()) {
                (void)display_.power_off();   // RES# low: panel supply off
            }
            return;
        }
        if (!ensure_display(now)) {
            return;
        }

        const auto orientation = settings_.rotate180 ? ssd1327::Orientation::Rotated180 : ssd1327::Orientation::Normal;
        if (!orientation_applied_ || display_.orientation() != orientation) {
            if (display_.set_orientation(orientation)) {
                orientation_applied_ = true;
            }
        }

        ui_.render(canvas_);
        ui::RowTracker::Run runs[8];
        const size_t n = rows_.changed_runs(frame_, runs, 8);
        for (size_t i = 0; i < n; ++i) {
            (void)osThreadFlagsClear(kDmaFlag);
            auto r = display_.start_write_rows(runs[i].y0, runs[i].y1, frame_.row(runs[i].y0));
            if (!r) {
                fail_flush(r.error());
                return;
            }
            if (transport_.busy()) {
                const uint32_t f = osThreadFlagsWait(kDmaFlag, osFlagsWaitAny, kDmaTimeoutMs);
                if ((f & 0x80000000u) != 0 || transport_.last_async_failed()) {
                    (void)HAL_SPI_Abort(g_board_lcd_spi);
                    transport_.on_tx_complete(false);
                    fail_flush(ssd1327::Error::Timeout);
                    return;
                }
            }
        }

        const uint8_t contrast = ui_.contrast();
        if (contrast != contrast_applied_ && display_.set_contrast(contrast)) {
            contrast_applied_ = contrast;
        }
        if (!display_.is_on()) {
            (void)display_.display_on(true);
        }
    }

    void fail_flush(ssd1327::Error e) noexcept {
        backend_.count_display_error();
        rows_.invalidate();
        if (!display_warned_) {
            os::warn("panel: display write failed (%s)\n", ssd1327::error_text(e));
            display_warned_ = true;
        }
    }

    ssd1327::HalTransport transport_;
    ssd1327::Display display_;
    FirmwareBackend backend_;
    ui::Settings settings_;
    ui::Ui ui_;
    ui::Framebuffer frame_;
    ui::Canvas canvas_;
    ui::RowTracker rows_;

    uint8_t keys_ = 0;
    bool keypad_ok_ = false;
    bool keypad_warned_ = false;
    uint32_t keypad_retry_at_ = 0;
    bool display_warned_ = false;
    uint32_t display_retry_at_ = 0;
    bool orientation_applied_ = false;
    uint8_t contrast_applied_ = 0;
};

Panel g_panel;

void thread_main(void*) {
    g_panel.run();
}

} // namespace

void start(MessageHandler& handler) noexcept {
    if (g_thread != nullptr) {
        return;
    }
    g_panel.set_handler(handler);
    osThreadAttr_t attr{};
    attr.name = "panel";
    attr.attr_bits = osThreadDetached;
    attr.cb_mem = &g_tcb;
    attr.cb_size = sizeof(g_tcb);
    attr.stack_mem = g_stack;
    attr.stack_size = kStackBytes;
    attr.priority = osPriorityBelowNormal;
    g_thread = osThreadNew(&thread_main, nullptr, &attr);
    if (g_thread == nullptr) {
        os::error("panel: thread creation failed\n");
    }
}

void on_key_interrupt() noexcept {
    if (g_thread != nullptr) {
        (void)osThreadFlagsSet(g_thread, kKeyFlag);
    }
}

void on_spi_done(SPI_HandleTypeDef* hspi, bool ok) noexcept {
    if (hspi == g_board_lcd_spi) {
        g_panel.transport().on_tx_complete(ok);
    }
}

} // namespace front_panel

#else   // boards without the front panel

namespace front_panel {
void start(MessageHandler&) noexcept {}
void on_key_interrupt() noexcept {}
void on_spi_done(SPI_HandleTypeDef*, bool) noexcept {}
} // namespace front_panel

#endif
