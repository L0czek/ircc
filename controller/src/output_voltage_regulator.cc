#include "output_voltage_regulator.hpp"
#include "i2c_bus.hpp"
#include "log.hpp"
#include "protocol.pb.h"
#include "cmsis_os2.h"
#include "board.h"
#include <cstring>
#include <mutex>

OutputVoltageRegulator::OutputVoltageRegulator(I2C_HandleTypeDef* hi2c)
    : driver_(hi2c),
      bus_mutex_(i2c_internal_bus_mutex()),
      update_sem_(1, 0, nullptr)
{
    cached_status_ = OutputRegulatorStatus_init_default;
    cached_measurements_ = OutputRegulatorMeasurements_init_default;
    last_update_ms_ = 0;
}

OutputVoltageRegulator::~OutputVoltageRegulator() = default;

namespace {

// The protocol enum order (BUCK=0, BOOST=1) differs from the TPS55289 STATUS
// encoding (00 = boost, 01 = buck, 10 = buck-boost), so map explicitly.
OutputRegulatorOperatingMode to_protocol(tps55289::OperatingMode mode) {
    switch (mode) {
        case tps55289::OperatingMode::Buck:      return OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BUCK;
        case tps55289::OperatingMode::Boost:     return OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BOOST;
        case tps55289::OperatingMode::BuckBoost: return OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BUCK_BOOST;
        default:                                 return OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_FREQUENCY_LIMIT;
    }
}

} // namespace

ControllerError OutputVoltageRegulator::log_error(const char* what, const std::error_code& ec) noexcept {
    os::error("Output regulator: %s failed: %s (source: %s)\n", what, ec.message().c_str(),
              get_error_source(ec).c_str());
    return from_tps55289_error(ec);
}

std::expected<void, ControllerError> OutputVoltageRegulator::initialize() noexcept {
    std::lock_guard lock(bus_mutex_);
    // Read-only probe; the reset defaults (HICCUP=1, output off) are preserved.
    if (auto r = driver_.initialize(); !r) {
        return std::unexpected(log_error("initialize", r.error()));
    }
    return {};
}

// =============================================================================
// Control
// =============================================================================

std::expected<void, ControllerError> OutputVoltageRegulator::set_enabled(bool enable) noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = enable ? driver_.enable() : driver_.disable();
    if (!r) {
        return std::unexpected(log_error(enable ? "enable output" : "disable output", r.error()));
    }
    return {};
}

std::expected<bool, ControllerError> OutputVoltageRegulator::is_enabled() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.output_enabled();
    if (!r) {
        return std::unexpected(log_error("read output enable", r.error()));
    }
    return *r;
}

std::expected<float, ControllerError> OutputVoltageRegulator::set_output_voltage(float voltage) noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.set_output_voltage(voltage);
    if (!r) {
        return std::unexpected(log_error("set output voltage", r.error()));
    }
    return *r;
}

std::expected<float, ControllerError> OutputVoltageRegulator::get_output_voltage() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.get_output_voltage();
    if (!r) {
        return std::unexpected(log_error("read output voltage", r.error()));
    }
    return *r;
}

std::expected<void, ControllerError> OutputVoltageRegulator::set_output_current_limit(float current, float sense_resistor) noexcept {
    std::lock_guard lock(bus_mutex_);
    if (auto r = driver_.set_output_current_limit(current, sense_resistor); !r) {
        return std::unexpected(log_error("set output current limit", r.error()));
    }
    return {};
}

std::expected<float, ControllerError> OutputVoltageRegulator::get_output_current_limit() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.get_output_current_limit();
    if (!r) {
        return std::unexpected(log_error("read output current limit", r.error()));
    }
    return *r;
}

std::expected<void, ControllerError> OutputVoltageRegulator::set_output_voltage_limit(float max_voltage) noexcept {
    std::lock_guard lock(bus_mutex_);
    if (auto r = driver_.set_output_voltage_limit(max_voltage); !r) {
        return std::unexpected(log_error("set output voltage limit", r.error()));
    }
    return {};
}

std::expected<void, ControllerError> OutputVoltageRegulator::apply_config_locked(float voltage, float current_limit,
                                                                                 float sense_resistor_ohms) noexcept {
    // Validate everything before the first write so a bad request changes nothing.
    const float r_sense = sense_resistor_ohms > 0.0f ? sense_resistor_ohms : driver_.get_sense_resistor_ohms();
    if (!(voltage >= driver_.get_min_output_voltage() && voltage <= driver_.get_max_output_voltage()) ||
        !(current_limit >= 0.0f && current_limit * r_sense * 1000.0f <= 63.5f)) {
        os::error("Output regulator: rejected out-of-range configuration request\n");
        return std::unexpected(ControllerError::TPS55289_InvalidParameter);
    }

    if (sense_resistor_ohms > 0.0f) {
        if (auto r = driver_.set_sense_resistor_ohms(sense_resistor_ohms); !r) {
            return std::unexpected(log_error("set sense resistor", r.error()));
        }
    }
    if (auto r = driver_.set_output_voltage(voltage); !r) {
        return std::unexpected(log_error("set output voltage", r.error()));
    }
    if (auto r = driver_.set_output_current_limit(current_limit); !r) {
        return std::unexpected(log_error("set output current limit", r.error()));
    }
    return {};
}

std::expected<void, ControllerError> OutputVoltageRegulator::set_config(const OutputRegulatorConfig& config) noexcept {
    std::lock_guard lock(bus_mutex_);

    float voltage = 0.0f;
    float current = 0.0f;
    if (!config.has_output_voltage || !config.has_output_current_limit) {
        // Partial config: keep the fields that were not supplied.
        if (auto r = read_setpoints_locked(voltage, current); !r) {
            return r;
        }
    }
    if (config.has_output_voltage) voltage = config.output_voltage;
    if (config.has_output_current_limit) current = config.output_current_limit;
    const float r_sense = config.has_sense_resistor_ohms ? config.sense_resistor_ohms : -1.0f;
    return apply_config_locked(voltage, current, r_sense);
}

std::expected<void, ControllerError> OutputVoltageRegulator::set_config(const OutputRegulatorConfigRequest& config) noexcept {
    std::lock_guard lock(bus_mutex_);
    // sense_resistor_ohms <= 0 keeps the stored value instead of producing a 0 mV limit.
    return apply_config_locked(config.output_voltage, config.output_current_limit, config.sense_resistor_ohms);
}

// =============================================================================
// Read-back helpers (caller holds bus_mutex_)
// =============================================================================

std::expected<void, ControllerError> OutputVoltageRegulator::read_status_locked(OutputRegulatorStatus& status) const noexcept {
    auto enabled = driver_.output_enabled();
    if (!enabled) return std::unexpected(log_error("read output enable", enabled.error()));
    auto feedback = driver_.get_feedback_mode();
    if (!feedback) return std::unexpected(log_error("read feedback mode", feedback.error()));
    // STATUS is clear-on-read: read it exactly once and decode all fields from that value.
    auto st = driver_.read_status();
    if (!st) return std::unexpected(log_error("read status", st.error()));

    status.enabled = *enabled;
    status.fault_present = st->any_fault();
    status.scp_fault = st->scp;
    status.ocp_fault = st->ocp;
    status.ovp_fault = st->ovp;
    status.operating_mode = to_protocol(st->mode);
    status.feedback_mode = static_cast<OutputRegulatorFeedbackMode>(*feedback);
    status.timestamp_ms = last_update_ms_;
    return {};
}

std::expected<void, ControllerError> OutputVoltageRegulator::read_setpoints_locked(float& voltage, float& current_limit) const noexcept {
    auto v = driver_.get_output_voltage();
    if (!v) return std::unexpected(log_error("read output voltage", v.error()));
    auto i = driver_.get_output_current_limit();
    if (!i) return std::unexpected(log_error("read output current limit", i.error()));
    voltage = *v;
    current_limit = *i;
    return {};
}

// =============================================================================
// Read-back (public)
// =============================================================================

std::expected<void, ControllerError> OutputVoltageRegulator::get_status(OutputRegulatorStatus& status) const noexcept {
    std::lock_guard lock(bus_mutex_);
    return read_status_locked(status);
}

std::expected<void, ControllerError> OutputVoltageRegulator::get_measurements(OutputRegulatorMeasurements& measurements) const noexcept {
    std::lock_guard lock(bus_mutex_);
    return read_setpoints_locked(measurements.output_voltage, measurements.output_current);
}

std::expected<void, ControllerError> OutputVoltageRegulator::get_config(OutputRegulatorConfig& config) const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = read_setpoints_locked(config.output_voltage, config.output_current_limit);
    config.has_output_voltage = config.has_output_current_limit = r.has_value();
    config.has_sense_resistor_ohms = true;
    config.sense_resistor_ohms = driver_.get_sense_resistor_ohms();
    return r;
}

std::expected<void, ControllerError> OutputVoltageRegulator::get_status(SystemStatus& status) const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = read_status_locked(status.output_regulator);
    status.has_output_regulator = r.has_value();
    return r;
}

std::expected<void, ControllerError> OutputVoltageRegulator::get_measurements(SystemStatus& status) const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto& m = status.output_regulator_measurements;
    auto r = read_setpoints_locked(m.output_voltage, m.output_current);
    status.has_output_regulator_measurements = r.has_value();
    return r;
}

std::expected<void, ControllerError> OutputVoltageRegulator::get_config(SystemStatus& status) const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto& c = status.output_regulator_config;
    auto r = read_setpoints_locked(c.output_voltage, c.output_current_limit);
    c.has_output_voltage = c.has_output_current_limit = r.has_value();
    c.has_sense_resistor_ohms = true;
    c.sense_resistor_ohms = driver_.get_sense_resistor_ohms();
    status.has_output_regulator_config = r.has_value();
    return r;
}

// =============================================================================
// Misc
// =============================================================================

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

float OutputVoltageRegulator::get_sense_resistor_ohms() const noexcept {
    return driver_.get_sense_resistor_ohms();
}
