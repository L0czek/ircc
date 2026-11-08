#ifndef __AUX_PWM_APPLET_HPP__
#define __AUX_PWM_APPLET_HPP__

#include "applet.hpp"
#include "stm32g4xx_hal.h"
#include <cstdint>

/**
 * @brief Auxiliary PWM applet: PWM on CH1 and CH2 of g_board_aux_pwm
 *        (g_board_aux_pwm_cha / g_board_aux_pwm_chb).
 *
 * Parameters (front panel, `applet_command aux-pwm params|get|set`):
 *   freq 1 Hz .. 1 MHz, duty1 / duty2 0..100 %, all changeable while running.
 *
 * CLI commands (applet_command aux-pwm ...):
 * - set_freq <frequency_hz>      Set PWM frequency (1 Hz - 1 MHz)
 * - set_duty <channel> <duty>    Set duty cycle (0-100 %) for channel 1 or 2
 * - enable / disable             Start / stop PWM generation
 * - status                       Log the current configuration
 * - help                         Log the help text
 * applet_start aux-pwm [<channel> <freq> <duty>] configures, then starts.
 */
class AuxPwmApplet : public Applet {
public:
    explicit AuxPwmApplet(TIM_HandleTypeDef* htim) noexcept;

    // Applet
    std::expected<void, ControllerError> configure(int argc, char** argv) noexcept override;
    const char* help() const noexcept override;
    const char* name() const noexcept override { return "aux-pwm"; }
    uint32_t config_size() const noexcept override;
    void save_config(uint8_t data[]) const noexcept override;
    void load_config(const uint8_t data[]) noexcept override;
    std::expected<void, ControllerError> step() noexcept override { return {}; }
    std::expected<void, ControllerError> on_message(int argc, char** argv) noexcept override;

    // AppletControl
    const char* summary() const noexcept override { return "PWM on CH1/CH2"; }
    std::span<const ParamDesc> params() const noexcept override;
    float get_param(size_t index) const noexcept override;
    AppletResult set_param(size_t index, float value) noexcept override;
    std::span<const ReadoutDesc> readouts() const noexcept override;
    float readout(size_t index) const noexcept override;
    AppletRun run_state() const noexcept override { return enabled_ ? AppletRun::Running : AppletRun::Stopped; }
    AppletResult start() noexcept override;
    void stop() noexcept override;
    bool needs_arming() const noexcept override { return false; }   // signal level only

    // Command handlers
    std::expected<void, ControllerError> cmd_set_freq(float frequency) noexcept;
    std::expected<void, ControllerError> cmd_set_duty(int channel, float duty) noexcept;
    std::expected<void, ControllerError> cmd_enable() noexcept;
    std::expected<void, ControllerError> cmd_disable() noexcept;
    std::expected<void, ControllerError> cmd_status() noexcept;

private:
    /** @brief CCR for a duty cycle at the current period (PWM mode 1) */
    uint32_t calculate_pulse(float duty) const noexcept;
    /** @brief Program prescaler / period for frequency_hz_ and both compare values */
    void apply_timing() noexcept;

    TIM_HandleTypeDef* htim_;
    float frequency_hz_ = 1000.0f;
    float duty_cycle_ch1_ = 50.0f;
    float duty_cycle_ch2_ = 50.0f;
    bool enabled_ = false;
};

/** @brief Registers aux_pwm_applet with the AppletRegistry (instantiate once at boot) */
struct AuxPwmAppletRegistration {
    AuxPwmAppletRegistration() noexcept;
};

extern AuxPwmApplet aux_pwm_applet;

#endif
