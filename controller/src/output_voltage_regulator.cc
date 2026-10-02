#include "output_voltage_regulator.hpp"
#include "log.hpp"
#include "protocol.pb.h"
#include "cmsis_os2.h"
#include "board.h"
#include <cstring>
#include <mutex>

// External TPS55289 driver instance
extern "C" {
#include "tps55289.hpp"
}

OutputVoltageRegulator::OutputVoltageRegulator(I2C_HandleTypeDef* hi2c)
    : driver_(hi2c),
      mutex_(nullptr),
      update_sem_(1, 0, nullptr)
{
    // Initialize cached values
    cached_status_ = BackBoostStatus_init_default;
    cached_measurements_ = BackBoostMeasurements_init_default;
    last_update_ms_ = 0;
}

OutputVoltageRegulator::~OutputVoltageRegulator() = default;

std::expected<void, ControllerError> OutputVoltageRegulator::initialize() noexcept {
    auto result = driver_.verify_device_present();
    if (!result) {
        os::error("Failed to initialize TPS55289: %s (source: %s)\n",
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_tps55289_error(result.error()));
    }
    return {};
}

std::expected<void, ControllerError> OutputVoltageRegulator::set_enabled(bool enable) noexcept {
    std::lock_guard lock(mutex_);

    if (enable) {
        auto result = driver_.enable();
        if (!result) {
            os::error("Failed to enable output: %s (source: %s)\n",
                result.error().message().c_str(),
                get_error_source(result.error()).c_str());
            return std::unexpected(from_tps55289_error(result.error()));
        }
    } else {
        auto result = driver_.disable();
        if (!result) {
            os::error("Failed to disable output: %s (source: %s)\n",
                result.error().message().c_str(),
                get_error_source(result.error()).c_str());
            return std::unexpected(from_tps55289_error(result.error()));
        }
    }

    return {};
}

bool OutputVoltageRegulator::is_enabled() const noexcept {
    std::lock_guard lock(mutex_);

    return driver_.output_enabled();
}

std::expected<void, ControllerError> OutputVoltageRegulator::set_output_voltage(float voltage) noexcept {
    std::lock_guard lock(mutex_);

    auto result = driver_.set_output_voltage(voltage);
    if (!result) {
        os::error("Failed to set output voltage: %s (source: %s)\n",
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_tps55289_error(result.error()));
    }

    return {};
}

std::expected<float, ControllerError> OutputVoltageRegulator::get_output_voltage() const noexcept {
    std::lock_guard lock(mutex_);

    auto result = driver_.get_output_voltage();
    if (!result) {
        os::error("Failed to get output voltage: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_tps55289_error(result.error()));
    }

    return result.value();
}

std::expected<void, ControllerError> OutputVoltageRegulator::set_output_current_limit(float current, float sense_resistor) noexcept {
    std::lock_guard lock(mutex_);

    auto result = driver_.set_output_current_limit(current, sense_resistor);
    if (!result) {
        os::error("Failed to set output current limit: %s (source: %s)\n",
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_tps55289_error(result.error()));
    }

    return {};
}

std::expected<float, ControllerError> OutputVoltageRegulator::get_output_current_limit(float sense_resistor) const noexcept {
    std::lock_guard lock(mutex_);

    auto result = driver_.get_output_current_limit(sense_resistor);
    if (!result) {
        os::error("Failed to get output current limit: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_tps55289_error(result.error()));
    }

    return result.value();
}

std::expected<void, ControllerError> OutputVoltageRegulator::get_status(BackBoostStatus& status) const noexcept {
    std::lock_guard lock(mutex_);

    // Fill in basic status
    status.enabled = is_enabled();
    status.fault_present = is_fault_present();
    status.scp_fault = has_scp_fault();
    status.ocp_fault = has_ocp_fault();
    status.ovp_fault = has_ovp_fault();
    status.operating_mode = static_cast<BackBoostOperatingMode>(get_operating_mode());
    status.feedback_mode = static_cast<BackBoostFeedbackMode>(get_feedback_mode());
    status.timestamp_ms = last_update_ms_;

    return {};
}

std::expected<void, ControllerError> OutputVoltageRegulator::get_measurements(BackBoostMeasurements& measurements) const noexcept {
    std::lock_guard lock(mutex_);

    auto voltage = get_output_voltage();
    auto current = get_output_current_limit(get_sense_resistor_ohms());

    if (!voltage || !current) {
        if (!voltage) return std::unexpected(voltage.error());
        if (!current) return std::unexpected(current.error());
    }

    measurements.output_voltage = voltage.value();
    measurements.output_current = current.value();

    return {};
}

std::expected<void, ControllerError> OutputVoltageRegulator::get_config(BackBoostConfig& config) const noexcept {
    std::lock_guard lock(mutex_);

    auto voltage = get_output_voltage();
    auto current = get_output_current_limit(get_sense_resistor_ohms());

    if (!voltage || !current) {
        if (!voltage) return std::unexpected(voltage.error());
        if (!current) return std::unexpected(current.error());
    }

    config.output_voltage = voltage.value();
    config.output_current_limit = current.value();
    config.sense_resistor_ohms = get_sense_resistor_ohms();

    return {};
}

std::expected<void, ControllerError> OutputVoltageRegulator::set_config(const BackBoostConfig& config) noexcept {
    std::lock_guard lock(mutex_);

    {
        auto result = driver_.set_output_voltage(config.output_voltage);
        if (!result) {
            os::error("Failed to set output voltage: %s (source: %s)\n",
                result.error().message().c_str(),
                get_error_source(result.error()).c_str());
            return std::unexpected(from_tps55289_error(result.error()));
        }
    }

    {
        auto result = driver_.set_output_current_limit(config.output_current_limit, config.sense_resistor_ohms);
        if (!result) {
            os::error("Failed to set output current limit: %s (source: %s)\n",
                result.error().message().c_str(),
                get_error_source(result.error()).c_str());
            return std::unexpected(from_tps55289_error(result.error()));
        }
    }

    return {};
}

std::expected<void, ControllerError> OutputVoltageRegulator::set_config(const BackBoostConfigRequest& config) noexcept {
    std::lock_guard lock(mutex_);

    if (config.has_output_voltage) {
        auto result = driver_.set_output_voltage(config.output_voltage);
        if (!result) {
            os::error("Failed to set output voltage: %s (source: %s)\n",
                result.error().message().c_str(),
                get_error_source(result.error()).c_str());
            return std::unexpected(from_tps55289_error(result.error()));
        }
    }

    if (config.has_output_current_limit) {
        auto result = driver_.set_output_current_limit(config.output_current_limit, get_sense_resistor_ohms());
        if (!result) {
            os::error("Failed to set output current limit: %s (source: %s)\n",
                result.error().message().c_str(),
                get_error_source(result.error()).c_str());
            return std::unexpected(from_tps55289_error(result.error()));
        }
    }

    return {};
}

bool OutputVoltageRegulator::has_updates() const noexcept {
    return update_sem_.try_acquire();
}

bool OutputVoltageRegulator::wait_for_update(uint32_t timeout_ms) const noexcept {
    if (timeout_ms == 0) {
        update_sem_.acquire();
        return true;
    }
    return update_sem_.try_acquire_for(timeout_ms);
}

void OutputVoltageRegulator::trigger_update() noexcept {
    update_sem_.release();
}

bool OutputVoltageRegulator::is_initialized() const noexcept {
    return driver_.is_initialized();
}

bool OutputVoltageRegulator::is_fault_present() const noexcept {
    return driver_.has_fault();
}

bool OutputVoltageRegulator::has_scp_fault() const noexcept {
    return driver_.has_scp();
}

bool OutputVoltageRegulator::has_ocp_fault() const noexcept {
    return driver_.has_ocp();
}

bool OutputVoltageRegulator::has_ovp_fault() const noexcept {
    return driver_.has_ovp();
}

BackBoostOperatingMode OutputVoltageRegulator::get_operating_mode() const noexcept {
    return static_cast<BackBoostOperatingMode>(driver_.get_operating_mode());
}

BackBoostFeedbackMode OutputVoltageRegulator::get_feedback_mode() const noexcept {
    return static_cast<BackBoostFeedbackMode>(driver_.get_feedback_mode());
}

float OutputVoltageRegulator::get_sense_resistor_ohms() const noexcept {
    return driver_.get_sense_resistor_ohms();
}
