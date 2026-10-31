#include "output_voltage_regulator.hpp"
#include "i2c_bus.hpp"
#include "log.hpp"
#include "protocol.pb.h"
#include "cmsis_os2.h"
#include "board.h"

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

OutputVoltageRegulator::OutputVoltageRegulator(I2C_HandleTypeDef* hi2c)
    : driver_(hi2c),
      update_sem_(1, 0, nullptr),
      last_update_ms_(0)
{
    (void)i2c_internal_bus_mutex();  // create the bus mutex before any thread uses it
}

OutputVoltageRegulator::~OutputVoltageRegulator() = default;

std::unexpected<ControllerError> OutputVoltageRegulator::fail(const char* what, const std::error_code& ec) const noexcept {
    const ControllerError mapped = from_tps55289_error(ec);
    if (log_limiter_.allow(what, mapped, os::timestamp_ms())) {
        os::error("Output regulator: %s failed: %s [%s] (%lu repeats suppressed)\n", what, to_string(mapped),
                  get_error_source(ec), static_cast<unsigned long>(log_limiter_.suppressed()));
    }
    return std::unexpected(mapped);
}

// =============================================================================
// Lifecycle / housekeeping
// =============================================================================

std::expected<void, ControllerError> OutputVoltageRegulator::initialize() noexcept {
    // Read-only probe; the reset defaults (HICCUP=1, output off) are preserved.
    return i2c_internal_bus_transaction([&] { return check("initialize", driver_.initialize()); });
}

std::expected<void, ControllerError> OutputVoltageRegulator::service() noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        if (!driver_.is_initialized()) {
            return check("initialize", driver_.initialize());
        }
        auto st = check("read status", driver_.read_status());
        if (!st) {
            return std::unexpected(st.error());
        }
        if (st->any_fault()) {
            os::warn("Output regulator: fault%s%s%s\n", st->scp ? " SCP" : "", st->ocp ? " OCP" : "",
                     st->ovp ? " OVP" : "");
        }
        latched_scp_ = latched_scp_ || st->scp;
        latched_ocp_ = latched_ocp_ || st->ocp;
        latched_ovp_ = latched_ovp_ || st->ovp;
        return {};
    });
}

// =============================================================================
// Control
// =============================================================================

std::expected<void, ControllerError> OutputVoltageRegulator::set_enabled(bool enable) noexcept {
    return i2c_internal_bus_transaction([&] {
        return enable ? check("enable output", driver_.enable()) : check("disable output", driver_.disable());
    });
}

std::expected<bool, ControllerError> OutputVoltageRegulator::is_enabled() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read output enable", driver_.output_enabled()); });
}

std::expected<float, ControllerError> OutputVoltageRegulator::set_output_voltage(float voltage) noexcept {
    return i2c_internal_bus_transaction([&] { return check("set output voltage", driver_.set_output_voltage(voltage)); });
}

std::expected<float, ControllerError> OutputVoltageRegulator::get_output_voltage() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read output voltage", driver_.get_output_voltage()); });
}

std::expected<void, ControllerError> OutputVoltageRegulator::set_output_current_limit(float current, float sense_resistor) noexcept {
    return i2c_internal_bus_transaction([&] {
        return check("set output current limit", driver_.set_output_current_limit(current, sense_resistor));
    });
}

std::expected<float, ControllerError> OutputVoltageRegulator::get_output_current_limit() const noexcept {
    return i2c_internal_bus_transaction([&] {
        return check("read output current limit", driver_.get_output_current_limit());
    });
}

std::expected<void, ControllerError> OutputVoltageRegulator::set_output_voltage_limit(float max_voltage) noexcept {
    return i2c_internal_bus_transaction([&] {
        return check("set output voltage limit", driver_.set_output_voltage_limit(max_voltage));
    });
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
        if (auto r = check("set sense resistor", driver_.set_sense_resistor_ohms(sense_resistor_ohms)); !r) {
            return r;
        }
    }
    if (auto r = check("set output voltage", driver_.set_output_voltage(voltage)); !r) {
        return std::unexpected(r.error());
    }
    return check("set output current limit", driver_.set_output_current_limit(current_limit));
}

std::expected<void, ControllerError> OutputVoltageRegulator::set_config(const OutputRegulatorConfig& config) noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
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
    });
}

std::expected<void, ControllerError> OutputVoltageRegulator::set_config(const OutputRegulatorConfigRequest& config) noexcept {
    // sense_resistor_ohms <= 0 keeps the stored value instead of producing a 0 mV limit.
    return i2c_internal_bus_transaction([&] {
        return apply_config_locked(config.output_voltage, config.output_current_limit, config.sense_resistor_ohms);
    });
}

// =============================================================================
// Read-back helpers (caller holds the bus lock)
// =============================================================================

std::expected<void, ControllerError> OutputVoltageRegulator::read_status_locked(OutputRegulatorStatus& status) noexcept {
    auto enabled = check("read output enable", driver_.output_enabled());
    if (!enabled) return std::unexpected(enabled.error());
    auto feedback = check("read feedback mode", driver_.get_feedback_mode());
    if (!feedback) return std::unexpected(feedback.error());
    // STATUS is clear-on-read: read it exactly once and decode all fields from that value.
    auto st = check("read status", driver_.read_status());
    if (!st) return std::unexpected(st.error());

    // Report faults seen now or latched by service() since the last report, then clear the latch.
    status.scp_fault = st->scp || latched_scp_;
    status.ocp_fault = st->ocp || latched_ocp_;
    status.ovp_fault = st->ovp || latched_ovp_;
    latched_scp_ = latched_ocp_ = latched_ovp_ = false;

    status.enabled = *enabled;
    status.fault_present = status.scp_fault || status.ocp_fault || status.ovp_fault;
    status.operating_mode = to_protocol(st->mode);
    status.feedback_mode = static_cast<OutputRegulatorFeedbackMode>(*feedback);
    status.timestamp_ms = last_update_ms_;
    return {};
}

std::expected<void, ControllerError> OutputVoltageRegulator::read_setpoints_locked(float& voltage, float& current_limit) const noexcept {
    auto v = check("read output voltage", driver_.get_output_voltage());
    if (!v) return std::unexpected(v.error());
    auto i = check("read output current limit", driver_.get_output_current_limit());
    if (!i) return std::unexpected(i.error());
    voltage = *v;
    current_limit = *i;
    return {};
}

// =============================================================================
// Read-back (public)
// =============================================================================

std::expected<void, ControllerError> OutputVoltageRegulator::get_status(OutputRegulatorStatus& status) noexcept {
    return i2c_internal_bus_transaction([&] { return read_status_locked(status); });
}

std::expected<void, ControllerError> OutputVoltageRegulator::get_measurements(OutputRegulatorMeasurements& measurements) const noexcept {
    return i2c_internal_bus_transaction([&] {
        return read_setpoints_locked(measurements.output_voltage, measurements.output_current);
    });
}

std::expected<void, ControllerError> OutputVoltageRegulator::get_config(OutputRegulatorConfig& config) const noexcept {
    auto r = i2c_internal_bus_transaction([&] {
        return read_setpoints_locked(config.output_voltage, config.output_current_limit);
    });
    config.has_output_voltage = config.has_output_current_limit = r.has_value();
    config.has_sense_resistor_ohms = true;
    config.sense_resistor_ohms = driver_.get_sense_resistor_ohms();
    return r;
}

std::expected<void, ControllerError> OutputVoltageRegulator::get_status(SystemStatus& status) noexcept {
    auto r = i2c_internal_bus_transaction([&] { return read_status_locked(status.output_regulator); });
    status.has_output_regulator = r.has_value();
    return r;
}

std::expected<void, ControllerError> OutputVoltageRegulator::get_measurements(SystemStatus& status) const noexcept {
    auto& m = status.output_regulator_measurements;
    auto r = i2c_internal_bus_transaction([&] { return read_setpoints_locked(m.output_voltage, m.output_current); });
    status.has_output_regulator_measurements = r.has_value();
    return r;
}

std::expected<void, ControllerError> OutputVoltageRegulator::get_config(SystemStatus& status) const noexcept {
    auto& c = status.output_regulator_config;
    auto r = i2c_internal_bus_transaction([&] { return read_setpoints_locked(c.output_voltage, c.output_current_limit); });
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
