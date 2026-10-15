#include "aux_pwm_applet.hpp"
#include "board.h"
#include "log.hpp"
#include "error.hpp"
#include "applet.hpp"

#include <argum/parser.h>
#include <argum/validators.h>
#include <argum/type-parsers.h>
#include <cstdio>
#include <cstring>
#include <string_view>

using namespace Argum;
using namespace std;

// =============================================================================
// AuxPwmApplet Implementation
// =============================================================================

AuxPwmApplet::AuxPwmApplet(TIM_HandleTypeDef* htim) noexcept
    : htim_(htim),
      frequency_hz_(1000.0f),           // Default 1kHz
      duty_cycle_ch1_(50.0f),           // Default 50%
      duty_cycle_ch2_(50.0f),           // Default 50%
      enabled_(false) {
    configure_parsers();
}

void AuxPwmApplet::configure_parsers() noexcept {
    // Configure set_freq parser: "set_freq <freq>"
    set_freq_parser_.add(
        Positional("set_freq").
        handler([&](const string_view &) -> Expected<void> { return {}; })
    );
    set_freq_parser_.add(
        Positional("freq").
        handler([&](const string_view & value) -> Expected<void> {
            auto result = Argum::parseFloatingPoint<float>(value);
            if (auto error = result.error()) return error;
            frequency_hz_ = *result;
            return {};
        })
    );

    // Configure set_duty parser: "set_duty <channel> <duty>"
    // Uses local variable to capture channel during parsing
    int set_duty_channel = 1;
    set_duty_parser_.add(
        Positional("set_duty").
        handler([&](const string_view &) -> Expected<void> { return {}; })
    );
    set_duty_parser_.add(
        Positional("channel").
        handler([&](const string_view & value) -> Expected<void> {
            auto result = Argum::parseIntegral<int>(value);
            if (auto error = result.error()) return error;
            if (*result != 1 && *result != 2) {
                return {Failure<Parser::ValidationError>, "channel must be 1 or 2"};
            }
            set_duty_channel = *result;
            return {};
        })
    );
    set_duty_parser_.add(
        Positional("duty").
        handler([&](const string_view & value) -> Expected<void> {
            auto result = Argum::parseFloatingPoint<float>(value);
            if (auto error = result.error()) return error;
            float duty_val = *result;
            if (duty_val < 0.0f || duty_val > 100.0f) {
                return {Failure<Parser::ValidationError>, "duty cycle must be 0-100"};
            }
            // Update the captured channel's duty cycle
            if (set_duty_channel == 1) {
                duty_cycle_ch1_ = duty_val;
            } else {
                duty_cycle_ch2_ = duty_val;
            }
            return {};
        })
    );

    // Configure enable parser
    enable_parser_.add(
        Positional("enable").
        handler([&](const string_view &) -> Expected<void> { return {}; })
    );

    // Configure disable parser
    disable_parser_.add(
        Positional("disable").
        handler([&](const string_view &) -> Expected<void> { return {}; })
    );

    // Configure status parser
    status_parser_.add(
        Positional("status").
        handler([&](const string_view &) -> Expected<void> { return {}; })
    );

    // Configure help parser
    help_parser_.add(
        Positional("help").
        handler([&](const string_view &) -> Expected<void> { return {}; })
    );
}

uint32_t AuxPwmApplet::calculate_pulse(float freq, float duty) const noexcept {
    if (!htim_) {
        return 0;
    }

    // Calculate period from timer clock and prescaler
    // Timer clock = 160 MHz (assumed)
    // Period = TimerClock / (Prescaler * Frequency) - 1
    [[maybe_unused]] uint32_t timer_clock = 160000000;  // 160 MHz
    [[maybe_unused]] uint32_t prescaler = htim_->Init.Prescaler;
    uint32_t period = htim_->Init.Period;

    // Calculate pulse based on duty cycle
    // Pulse = (Period + 1) * Duty / 100 - 1
    uint32_t pulse = static_cast<uint32_t>((static_cast<float>(period + 1) * duty) / 100.0f) - 1;

    // Clamp to valid range
    if (pulse > period) {
        pulse = period;
    }

    return pulse;
}

std::expected<void, ControllerError> AuxPwmApplet::configure(int argc, char** argv) noexcept {
    if (argc < 1) {
        os::error("aux-pwm: configure requires arguments\n");
        return std::unexpected(ControllerError::HAL_ERROR);
    }

    // Define variables to populate during parsing
    int channel = 1;
    float freq = 1000.0f;
    float duty = 50.0f;

    const char * progname = (argc ? argv[0] : "aux-pwm");

    // Configure argum to use expected values mode (no exceptions)
    Parser parser;
    parser.add(
        Positional("channel").
        help("Channel (1 or 2)").
        handler([&](const string_view & value) -> Expected<void> {
            auto result = Argum::parseIntegral<int>(value);
            if (auto error = result.error()) return error;
            if (*result != 1 && *result != 2) {
                return {Failure<Parser::ValidationError>, "channel must be 1 or 2"};
            }
            channel = *result;
            return {};
        }));
    parser.add(
        Positional("freq").
        help("Frequency in Hz").
        handler([&](const string_view & value) -> Expected<void> {
            auto result = Argum::parseFloatingPoint<float>(value);
            if (auto error = result.error()) return error;
            freq = *result;
            return {};
        }));
    parser.add(
        Positional("duty").
        help("Duty cycle (0-100)").
        handler([&](const string_view & value) -> Expected<void> {
            auto result = Argum::parseFloatingPoint<float>(value);
            if (auto error = result.error()) return error;
            duty = *result;
            return {};
        }));

    // Parse directly with char** - no vector copy needed
    auto result = parser.parse(argc, argv);
    if (auto error = result.error()) {
        os::error("%s\n", error->message().data());
        os::error("%s\n", parser.formatUsage(progname).data());
        return std::unexpected(ControllerError::HAL_ERROR);
    }

    // Update configuration
    frequency_hz_ = freq;
    if (channel == 1) {
        duty_cycle_ch1_ = duty;
    } else {
        duty_cycle_ch2_ = duty;
    }

    os::info("aux-pwm: configured channel=%d, freq=%.0f Hz, duty=%.1f%%\n",
             channel, freq, duty);

    return {};
}

const char* AuxPwmApplet::help() const noexcept {
    return "aux-pwm: PWM applet for TIMx CH1/CH2\n"
           "  aux-pwm set_freq <freq_hz>    - Set PWM frequency\n"
           "  aux-pwm set_duty <ch> <duty>  - Set duty cycle (0-100) for ch1/ch2\n"
           "  aux-pwm enable                - Start PWM\n"
           "  aux-pwm disable               - Stop PWM\n"
           "  aux-pwm status                - Show status";
}

const char* AuxPwmApplet::name() const noexcept {
    return "aux-pwm";
}

void AuxPwmApplet::shutdown() noexcept {
    if (enabled_) {
        cmd_disable();
    }
}

uint32_t AuxPwmApplet::config_size() const noexcept {
    return sizeof(float) * 3 + sizeof(bool);
}

void AuxPwmApplet::save_config(uint8_t data[]) const noexcept {
    std::memcpy(data, &frequency_hz_, sizeof(float));
    std::memcpy(data + sizeof(float), &duty_cycle_ch1_, sizeof(float));
    std::memcpy(data + 2 * sizeof(float), &duty_cycle_ch2_, sizeof(float));
    std::memcpy(data + 3 * sizeof(float), &enabled_, sizeof(bool));
}

void AuxPwmApplet::load_config(const uint8_t data[]) noexcept {
    std::memcpy(&frequency_hz_, data, sizeof(float));
    std::memcpy(&duty_cycle_ch1_, data + sizeof(float), sizeof(float));
    std::memcpy(&duty_cycle_ch2_, data + 2 * sizeof(float), sizeof(float));
    std::memcpy(&enabled_, data + 3 * sizeof(float), sizeof(bool));
}

std::expected<void, ControllerError> AuxPwmApplet::step() noexcept {
    // No periodic processing needed - updates are message-driven
    return {};
}

std::expected<void, ControllerError> AuxPwmApplet::on_message(int argc, char** argv) noexcept {
    if (argc < 1) {
        os::error("aux-pwm: no command specified\n");
        return std::unexpected(ControllerError::HAL_ERROR);
    }

    // Try set_freq: "set_freq <freq>"
    {
        auto result = set_freq_parser_.parse(argc, argv);
        if (result) {
            return cmd_set_freq(frequency_hz_);
        }
    }

    // Try set_duty: "set_duty <channel> <duty>"
    {
        auto result = set_duty_parser_.parse(argc, argv);
        if (result) {
            return cmd_set_duty();
        }
    }

    // Try enable
    {
        auto result = enable_parser_.parse(argc, argv);
        if (result) {
            return cmd_enable();
        }
    }

    // Try disable
    {
        auto result = disable_parser_.parse(argc, argv);
        if (result) {
            return cmd_disable();
        }
    }

    // Try status
    {
        auto result = status_parser_.parse(argc, argv);
        if (result) {
            return cmd_status();
        }
    }

    // Try help
    {
        auto result = help_parser_.parse(argc, argv);
        if (result) {
            os::info("%s", help());
            return {};
        }
    }

    // No parser matched - invalid command
    os::error("aux-pwm: unknown command '%s'\n", argv[0]);
    os::error("aux-pwm: use 'aux-pwm help' for usage\n");
    return std::unexpected(ControllerError::HAL_ERROR);
}

std::expected<void, ControllerError> AuxPwmApplet::cmd_set_freq(float frequency) noexcept {
    // The parser has already validated and updated frequency_hz_ in on_message()
    // Now just apply to hardware if PWM is running

    frequency_hz_ = frequency;

    if (enabled_ && htim_) {
        // Calculate new pulse values
        uint32_t period = htim_->Init.Period;
        uint32_t pulse1 = static_cast<uint32_t>((static_cast<float>(period + 1) * duty_cycle_ch1_) / 100.0f) - 1;
        uint32_t pulse2 = static_cast<uint32_t>((static_cast<float>(period + 1) * duty_cycle_ch2_) / 100.0f) - 1;

        // Update CCR registers
        htim_->Instance->CCR1 = pulse1;
        htim_->Instance->CCR2 = pulse2;

        os::info("aux-pwm: frequency set to %.0f Hz, CH1 pulse=%u, CH2 pulse=%u\n",
                 frequency_hz_, pulse1, pulse2);
    } else {
        os::info("aux-pwm: frequency set to %.0f Hz (PWM not running)\n", frequency_hz_);
    }

    return {};
}

std::expected<void, ControllerError> AuxPwmApplet::cmd_set_duty() noexcept {
    // The parser already captured duty cycle into duty_cycle_ch1_/duty_cycle_ch2_,
    // now apply to hardware if PWM is running

    if (enabled_ && htim_) {
        uint32_t period = htim_->Init.Period;
        uint32_t pulse1 = static_cast<uint32_t>((static_cast<float>(period + 1) * duty_cycle_ch1_) / 100.0f) - 1;
        uint32_t pulse2 = static_cast<uint32_t>((static_cast<float>(period + 1) * duty_cycle_ch2_) / 100.0f) - 1;
        
        htim_->Instance->CCR1 = pulse1;
        htim_->Instance->CCR2 = pulse2;
        
        os::info("aux-pwm: duty cycles set - CH1=%.1f%% (pulse=%u), CH2=%.1f%% (pulse=%u)\n",
                 duty_cycle_ch1_, pulse1, duty_cycle_ch2_, pulse2);
    } else {
        os::info("aux-pwm: duty cycles set - CH1=%.1f%%, CH2=%.1f%% (PWM not running)\n",
                 duty_cycle_ch1_, duty_cycle_ch2_);
    }

    return {};
}

std::expected<void, ControllerError> AuxPwmApplet::cmd_enable() noexcept {
    if (enabled_) {
        os::info("aux-pwm: already enabled\n");
        return {};
    }

    if (!htim_) {
        os::error("aux-pwm: timer handle not available\n");
        return std::unexpected(ControllerError::HAL_ERROR);
    }

    // Configure PWM channels
    TIM_OC_InitTypeDef sConfigOC = {0};
    sConfigOC.OCMode = TIM_OCMODE_PWM1;
    sConfigOC.Pulse = calculate_pulse(frequency_hz_, duty_cycle_ch1_);
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;

    HAL_StatusTypeDef status;

    // Configure CH1
    status = HAL_TIM_PWM_ConfigChannel(htim_, &sConfigOC, g_board_aux_pwm_cha);
    if (status != HAL_OK) {
        os::error("aux-pwm: failed to configure CH1: %d\n", status);
        return std::unexpected(from_hal_status(status));
    }

    // Configure CH2
    sConfigOC.Pulse = calculate_pulse(frequency_hz_, duty_cycle_ch2_);
    status = HAL_TIM_PWM_ConfigChannel(htim_, &sConfigOC, g_board_aux_pwm_chb);
    if (status != HAL_OK) {
        os::error("aux-pwm: failed to configure CH2: %d\n", status);
        return std::unexpected(from_hal_status(status));
    }

    // Start PWM
    status = HAL_TIM_PWM_Start(htim_, g_board_aux_pwm_cha);
    if (status != HAL_OK) {
        os::error("aux-pwm: failed to start CH1: %d\n", status);
        return std::unexpected(from_hal_status(status));
    }

    status = HAL_TIM_PWM_Start(htim_, g_board_aux_pwm_chb);
    if (status != HAL_OK) {
        os::error("aux-pwm: failed to start CH2: %d\n", status);
        HAL_TIM_PWM_Stop(htim_, g_board_aux_pwm_cha);
        return std::unexpected(from_hal_status(status));
    }

    enabled_ = true;
    os::info("aux-pwm: PWM enabled (freq=%.0f Hz, CH1=%.1f%%, CH2=%.1f%%)\n",
             frequency_hz_, duty_cycle_ch1_, duty_cycle_ch2_);

    return {};
}

std::expected<void, ControllerError> AuxPwmApplet::cmd_disable() noexcept {
    if (!enabled_) {
        os::info("aux-pwm: already disabled\n");
        return {};
    }

    if (!htim_) {
        os::error("aux-pwm: timer handle not available\n");
        return std::unexpected(ControllerError::HAL_ERROR);
    }

    HAL_StatusTypeDef status;

    status = HAL_TIM_PWM_Stop(htim_, g_board_aux_pwm_cha);
    if (status != HAL_OK) {
        os::error("aux-pwm: failed to stop CH1: %d\n", status);
    }

    status = HAL_TIM_PWM_Stop(htim_, g_board_aux_pwm_chb);
    if (status != HAL_OK) {
        os::error("aux-pwm: failed to stop CH2: %d\n", status);
    }

    enabled_ = false;
    os::info("aux-pwm: PWM disabled\n");

    return {};
}

std::expected<void, ControllerError> AuxPwmApplet::cmd_status() noexcept {
    os::info("aux-pwm status:\n");
    os::info("  Frequency: %.0f Hz\n", frequency_hz_);
    os::info("  Channel 1: %.1f%% duty cycle\n", duty_cycle_ch1_);
    os::info("  Channel 2: %.1f%% duty cycle\n", duty_cycle_ch2_);
    os::info("  Enabled: %s\n", enabled_ ? "yes" : "no");
    os::info("  Timer: %s\n", htim_ ? "available" : "not available");

    return {};
}

// =============================================================================
// Global static instance - automatically registered via AuxPwmAppletRegistration
// =============================================================================

AuxPwmApplet aux_pwm_applet(g_board_aux_pwm);

AuxPwmAppletRegistration::AuxPwmAppletRegistration() noexcept {
    // Register with applet registry on program startup
    AppletRegistry::load().register_applet(&aux_pwm_applet);
    os::info("aux-pwm: registered with timer %p (CH1/CH2)\n",
             static_cast<void*>(g_board_aux_pwm));
}
