#ifndef __AUX_PWM_APPLET_HPP__
#define __AUX_PWM_APPLET_HPP__

#include "applet.hpp"
#include <argum/parser.h>
#include <cstdint>
#include <string_view>

using namespace Argum;
using namespace std;

/**
 * @brief Auxiliary PWM Applet
 *
 * Provides PWM generation on CH1 and CH2 of a selected timer.
 * Timer channels are obtained from g_board_aux_pwm_cha and g_board_aux_pwm_chb.
 *
 * CLI Commands:
 * - aux-pwm set_freq <frequency_hz>      Set PWM frequency (1 Hz - timer_cap MHz)
 * - aux-pwm set_duty <channel> <duty>    Set duty cycle (0-100%) for ch1 or ch2
 * - aux-pwm enable                       Start PWM generation
 * - aux-pwm disable                      Stop PWM generation
 * - aux-pwm status                       Show current configuration
 * - aux-pwm help                         Show this help text
 */
class AuxPwmApplet : public Applet {
    TIM_HandleTypeDef* htim_;
    float frequency_hz_;
    float duty_cycle_ch1_;
    float duty_cycle_ch2_;
    bool enabled_;
    
    // Command parsers - created once during construction
    // Note: These capture values from command handler methods via lambda references
    Parser set_freq_parser_;
    Parser set_duty_parser_;
    Parser enable_parser_;
    Parser disable_parser_;
    Parser status_parser_;
    Parser help_parser_;

    /**
     * @brief Calculate pulse value for PWM duty cycle
     * @param freq Frequency in Hz
     * @param duty Duty cycle percentage (0-100)
     * @return Pulse value for CCR register
     */
    uint32_t calculate_pulse(float freq, float duty) const noexcept;

public:
    /**
     * @brief Construct auxiliary PWM applet
     * @param htim Timer handle from g_board_aux_pwm
     */
    explicit AuxPwmApplet(TIM_HandleTypeDef* htim) noexcept;

    // Applet interface implementation
    std::expected<void, ControllerError> configure(int argc, char ** argv) noexcept override;
    const char* help() const noexcept override;
    const char* name() const noexcept override;
    void shutdown() noexcept override;
    uint32_t config_size() const noexcept override;
    void save_config(uint8_t data[]) const noexcept override;
    void load_config(const uint8_t data[]) noexcept override;
    std::expected<void, ControllerError> step() noexcept override;
    std::expected<void, ControllerError> on_message(int argc, char ** argv) noexcept override;

    // Configure parsers (called from constructor)
    void configure_parsers() noexcept;

    // Command handlers (called from on_message)
    std::expected<void, ControllerError> cmd_set_freq(float frequency) noexcept;
    std::expected<void, ControllerError> cmd_set_duty() noexcept;
    std::expected<void, ControllerError> cmd_enable() noexcept;
    std::expected<void, ControllerError> cmd_disable() noexcept;
    std::expected<void, ControllerError> cmd_status() noexcept;
};

/**
 * @brief Automatic registration entry for AuxPwmApplet
 *
 * Uses static instance pattern to automatically register applet on program startup.
 * The applet is constructed with g_board_aux_pwm from board.h.
 */
struct AuxPwmAppletRegistration {
    AuxPwmAppletRegistration() noexcept;
};

// Global aux-pwm applet instance
extern AuxPwmApplet aux_pwm_applet;

#endif
