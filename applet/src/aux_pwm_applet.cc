#include "aux_pwm_applet.hpp"

#include "board.h"
#include "log.hpp"

#include <cstdlib>
#include <cstring>

namespace {

constexpr float kMinFrequency = 1.0f;
constexpr float kMaxFrequency = 1000000.0f;

constexpr ParamDesc kAuxPwmParams[] = {
    {"freq", "Frequency", "Hz", ParamType::Number, kMinFrequency, kMaxFrequency, 1.0f, 0, {}, kParamLive | kParamPersist},
    {"duty1", "Duty CH1", "%", ParamType::Number, 0.0f, 100.0f, 0.1f, 1, {}, kParamLive | kParamPersist},
    {"duty2", "Duty CH2", "%", ParamType::Number, 0.0f, 100.0f, 0.1f, 1, {}, kParamLive | kParamPersist},
};

constexpr ReadoutDesc kAuxPwmReadouts[] = {
    {"Period", "us", 2},
};

bool parse_float(const char* text, float& out) noexcept {
    if (text == nullptr) {
        return false;
    }
    char* end = nullptr;
    out = std::strtof(text, &end);
    return end != text && *end == '\0';
}

bool parse_channel(const char* text, int& out) noexcept {
    if (text == nullptr) {
        return false;
    }
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || (v != 1 && v != 2)) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

} // namespace

AuxPwmApplet::AuxPwmApplet(TIM_HandleTypeDef* htim) noexcept : htim_(htim) {}

// ---------------------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------------------

uint32_t AuxPwmApplet::calculate_pulse(float duty) const noexcept {
    if (!htim_) {
        return 0;
    }
    // PWM mode 1: output high while CNT < CCR, so CCR = (Period + 1) * Duty / 100
    // (CCR = Period + 1 keeps the output high for 100 %)
    const uint32_t period = htim_->Init.Period;
    uint32_t pulse = static_cast<uint32_t>((static_cast<float>(period) + 1.0f) * duty / 100.0f + 0.5f);
    if (pulse > period + 1) {
        pulse = period + 1;
    }
    return pulse;
}

void AuxPwmApplet::apply_timing() noexcept {
    if (!htim_ || frequency_hz_ <= 0.0f) {
        return;
    }
    // Timer kernel clock: PCLK1, doubled when the APB1 prescaler is not 1
    uint32_t clock = HAL_RCC_GetPCLK1Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_HCLK_DIV1) {
        clock *= 2;
    }
    const uint64_t ticks = static_cast<uint64_t>(static_cast<double>(clock) / frequency_hz_ + 0.5);
    const uint64_t max_arr = IS_TIM_32B_COUNTER_INSTANCE(htim_->Instance) ? 0xFFFFFFFFull : 0xFFFFull;
    uint64_t psc = (ticks + max_arr) / (max_arr + 1);   // smallest prescaler that fits
    if (psc == 0) {
        psc = 1;
    }
    if (psc > 0x10000) {
        psc = 0x10000;
    }
    uint64_t arr = ticks / psc;
    if (arr < 2) {
        arr = 2;
    }
    if (arr - 1 > max_arr) {
        arr = max_arr + 1;
    }
    htim_->Init.Prescaler = static_cast<uint32_t>(psc - 1);
    htim_->Init.Period = static_cast<uint32_t>(arr - 1);
    __HAL_TIM_SET_PRESCALER(htim_, htim_->Init.Prescaler);
    __HAL_TIM_SET_AUTORELOAD(htim_, htim_->Init.Period);
    __HAL_TIM_SET_COMPARE(htim_, g_board_aux_pwm_cha, calculate_pulse(duty_cycle_ch1_));
    __HAL_TIM_SET_COMPARE(htim_, g_board_aux_pwm_chb, calculate_pulse(duty_cycle_ch2_));
    if (__HAL_TIM_GET_COUNTER(htim_) > htim_->Init.Period) {
        __HAL_TIM_SET_COUNTER(htim_, 0);
    }
}

// ---------------------------------------------------------------------------------------
// Applet
// ---------------------------------------------------------------------------------------

std::expected<void, ControllerError> AuxPwmApplet::configure(int argc, char** argv) noexcept {
    // aux-pwm <channel> <freq> <duty>
    int channel = 1;
    float freq = 0.0f;
    float duty = 0.0f;
    if (argc != 4 || !parse_channel(argv[1], channel) || !parse_float(argv[2], freq) || !parse_float(argv[3], duty)) {
        os::error("aux-pwm: usage: aux-pwm <channel 1|2> <freq_hz> <duty 0-100>\n");
        return std::unexpected(ControllerError::Peripheral_InvalidParameter);
    }
    auto r = cmd_set_freq(freq);
    if (!r) {
        return r;
    }
    r = cmd_set_duty(channel, duty);
    if (!r) {
        return r;
    }
    os::info("aux-pwm: configured channel=%d, freq=%.0f Hz, duty=%.1f%%\n", channel, freq, duty);
    return {};
}

std::expected<void, ControllerError> AuxPwmApplet::on_message(int argc, char** argv) noexcept {
    // argv[0] is the applet name
    if (argc < 2) {
        os::error("aux-pwm: no command (aux-pwm help)\n");
        return std::unexpected(ControllerError::Peripheral_InvalidParameter);
    }
    const char* cmd = argv[1];
    if (std::strcmp(cmd, "set_freq") == 0) {
        float freq = 0.0f;
        if (argc != 3 || !parse_float(argv[2], freq)) {
            os::error("aux-pwm: usage: set_freq <freq_hz>\n");
            return std::unexpected(ControllerError::Peripheral_InvalidParameter);
        }
        return cmd_set_freq(freq);
    }
    if (std::strcmp(cmd, "set_duty") == 0) {
        int channel = 1;
        float duty = 0.0f;
        if (argc != 4 || !parse_channel(argv[2], channel) || !parse_float(argv[3], duty)) {
            os::error("aux-pwm: usage: set_duty <channel 1|2> <duty 0-100>\n");
            return std::unexpected(ControllerError::Peripheral_InvalidParameter);
        }
        return cmd_set_duty(channel, duty);
    }
    if (std::strcmp(cmd, "enable") == 0) {
        return cmd_enable();
    }
    if (std::strcmp(cmd, "disable") == 0) {
        return cmd_disable();
    }
    if (std::strcmp(cmd, "status") == 0) {
        return cmd_status();
    }
    if (std::strcmp(cmd, "help") == 0) {
        os::info("%s\n", help());
        return {};
    }
    os::error("aux-pwm: unknown command '%s' (aux-pwm help)\n", cmd);
    return std::unexpected(ControllerError::Peripheral_InvalidParameter);
}

const char* AuxPwmApplet::help() const noexcept {
    return "aux-pwm: PWM applet for TIMx CH1/CH2\n"
           "  aux-pwm set_freq <freq_hz>    - Set PWM frequency (1 Hz - 1 MHz)\n"
           "  aux-pwm set_duty <ch> <duty>  - Set duty cycle (0-100) for ch1/ch2\n"
           "  aux-pwm enable                - Start PWM\n"
           "  aux-pwm disable               - Stop PWM\n"
           "  aux-pwm status                - Show status\n"
           "  aux-pwm params | get | set <key> <value>";
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

// ---------------------------------------------------------------------------------------
// AppletControl
// ---------------------------------------------------------------------------------------

std::span<const ParamDesc> AuxPwmApplet::params() const noexcept { return kAuxPwmParams; }
std::span<const ReadoutDesc> AuxPwmApplet::readouts() const noexcept { return kAuxPwmReadouts; }

float AuxPwmApplet::get_param(size_t index) const noexcept {
    switch (index) {
        case 0: return frequency_hz_;
        case 1: return duty_cycle_ch1_;
        case 2: return duty_cycle_ch2_;
        default: return 0.0f;
    }
}

AppletResult AuxPwmApplet::set_param(size_t index, float value) noexcept {
    switch (index) {
        case 0: frequency_hz_ = value; break;
        case 1: duty_cycle_ch1_ = value; break;
        case 2: duty_cycle_ch2_ = value; break;
        default: return std::unexpected(AppletError::NotFound);
    }
    if (enabled_) {
        apply_timing();
    }
    return {};
}

float AuxPwmApplet::readout(size_t index) const noexcept {
    return index == 0 && frequency_hz_ > 0.0f ? 1e6f / frequency_hz_ : 0.0f;
}

AppletResult AuxPwmApplet::start() noexcept {
    auto r = cmd_enable();
    if (!r) {
        return std::unexpected(AppletError::Hardware);
    }
    return {};
}

void AuxPwmApplet::stop() noexcept {
    if (enabled_) {
        (void)cmd_disable();
    }
}

// ---------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------

std::expected<void, ControllerError> AuxPwmApplet::cmd_set_freq(float frequency) noexcept {
    if (!(frequency >= kMinFrequency && frequency <= kMaxFrequency)) {
        os::error("aux-pwm: frequency %.0f Hz out of range (1 Hz - 1 MHz)\n", frequency);
        return std::unexpected(ControllerError::Peripheral_InvalidParameter);
    }
    frequency_hz_ = frequency;
    if (enabled_) {
        apply_timing();
    }
    os::info("aux-pwm: frequency %.0f Hz%s\n", frequency_hz_, enabled_ ? "" : " (PWM not running)");
    return {};
}

std::expected<void, ControllerError> AuxPwmApplet::cmd_set_duty(int channel, float duty) noexcept {
    if (!(duty >= 0.0f && duty <= 100.0f) || (channel != 1 && channel != 2)) {
        os::error("aux-pwm: duty must be 0-100 %% on channel 1 or 2\n");
        return std::unexpected(ControllerError::Peripheral_InvalidParameter);
    }
    (channel == 1 ? duty_cycle_ch1_ : duty_cycle_ch2_) = duty;
    if (enabled_) {
        apply_timing();
    }
    os::info("aux-pwm: duty CH1=%.1f%%, CH2=%.1f%%%s\n", duty_cycle_ch1_, duty_cycle_ch2_,
             enabled_ ? "" : " (PWM not running)");
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

    apply_timing();

    TIM_OC_InitTypeDef oc{};
    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = calculate_pulse(duty_cycle_ch1_);
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;

    HAL_StatusTypeDef status = HAL_TIM_PWM_ConfigChannel(htim_, &oc, g_board_aux_pwm_cha);
    if (status != HAL_OK) {
        os::error("aux-pwm: failed to configure CH1: %d\n", status);
        return std::unexpected(from_hal_status(status));
    }
    oc.Pulse = calculate_pulse(duty_cycle_ch2_);
    status = HAL_TIM_PWM_ConfigChannel(htim_, &oc, g_board_aux_pwm_chb);
    if (status != HAL_OK) {
        os::error("aux-pwm: failed to configure CH2: %d\n", status);
        return std::unexpected(from_hal_status(status));
    }
    status = HAL_TIM_PWM_Start(htim_, g_board_aux_pwm_cha);
    if (status != HAL_OK) {
        os::error("aux-pwm: failed to start CH1: %d\n", status);
        return std::unexpected(from_hal_status(status));
    }
    status = HAL_TIM_PWM_Start(htim_, g_board_aux_pwm_chb);
    if (status != HAL_OK) {
        os::error("aux-pwm: failed to start CH2: %d\n", status);
        (void)HAL_TIM_PWM_Stop(htim_, g_board_aux_pwm_cha);
        return std::unexpected(from_hal_status(status));
    }

    enabled_ = true;
    os::info("aux-pwm: PWM enabled (freq=%.0f Hz, CH1=%.1f%%, CH2=%.1f%%)\n", frequency_hz_, duty_cycle_ch1_,
             duty_cycle_ch2_);
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
    HAL_StatusTypeDef status = HAL_TIM_PWM_Stop(htim_, g_board_aux_pwm_cha);
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

// ---------------------------------------------------------------------------------------

AuxPwmApplet aux_pwm_applet(g_board_aux_pwm);

AuxPwmAppletRegistration::AuxPwmAppletRegistration() noexcept {
    AppletRegistry::load().register_applet(&aux_pwm_applet);
    os::info("aux-pwm: registered with timer %p (CH1/CH2)\n", static_cast<void*>(g_board_aux_pwm));
}
