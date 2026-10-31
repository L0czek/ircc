#ifndef __OUTPUT_VOLTAGE_REGULATOR_HPP__
#define __OUTPUT_VOLTAGE_REGULATOR_HPP__

#include "sem.hpp"
#include "protocol.pb.h"
#include "error.hpp"
#include "tps55289.hpp"
#include <cstdint>
#include <expected>

/**
 * @brief Output voltage regulator (TPS55289) management
 *
 * Every public method runs as one transaction on the shared internal I2C bus
 * (i2c_internal_bus_transaction): bus lock held, bus recovered and the operation
 * retried once on a bus fault. *_locked helpers assume the lock is held.
 *
 * Note: the TPS55289 has no ADC. "Measurements" report the programmed output
 * voltage and the programmed current limit, not measured values.
 */
class OutputVoltageRegulator {
public:
    explicit OutputVoltageRegulator(I2C_HandleTypeDef* hi2c);
    ~OutputVoltageRegulator();

    /** @brief Probe the TPS55289 and cache its feedback configuration (no register writes) */
    std::expected<void, ControllerError> initialize() noexcept;

    /**
     * @brief Periodic housekeeping: retries initialisation if the regulator was
     *        not found at boot, and polls STATUS so that short SCP/OCP/OVP events
     *        (clear-on-read bits) are latched until the next get_status().
     */
    std::expected<void, ControllerError> service() noexcept;

    std::expected<void, ControllerError> set_enabled(bool enable) noexcept;
    std::expected<bool, ControllerError> is_enabled() const noexcept;

    /** @return programmed output voltage after quantisation */
    std::expected<float, ControllerError> set_output_voltage(float voltage) noexcept;
    std::expected<float, ControllerError> get_output_voltage() const noexcept;

    /** @param sense_resistor <= 0 uses the stored sense resistor */
    std::expected<void, ControllerError> set_output_current_limit(float current, float sense_resistor) noexcept;
    std::expected<float, ControllerError> get_output_current_limit() const noexcept;

    /** @brief Application upper bound for the output voltage */
    std::expected<void, ControllerError> set_output_voltage_limit(float max_voltage) noexcept;

    /** @brief Status; fault flags include faults latched by service() since the last call */
    std::expected<void, ControllerError> get_status(OutputRegulatorStatus& status) noexcept;
    std::expected<void, ControllerError> get_measurements(OutputRegulatorMeasurements& measurements) const noexcept;
    std::expected<void, ControllerError> get_config(OutputRegulatorConfig& config) const noexcept;

    std::expected<void, ControllerError> get_status(SystemStatus& status) noexcept;
    std::expected<void, ControllerError> get_measurements(SystemStatus& status) const noexcept;
    std::expected<void, ControllerError> get_config(SystemStatus& status) const noexcept;

    std::expected<void, ControllerError> set_config(const OutputRegulatorConfig& config) noexcept;
    std::expected<void, ControllerError> set_config(const OutputRegulatorConfigRequest& config) noexcept;

    bool has_updates() const noexcept;
    bool wait_for_update(uint32_t timeout_ms) const noexcept;
    void trigger_update() noexcept;
    uint32_t get_timestamp_ms() const noexcept { return last_update_ms_; }

    bool is_initialized() const noexcept;
    float get_sense_resistor_ohms() const noexcept;

private:
    tps55289::TPS55289 driver_;
    os::semaphore update_sem_;
    mutable uint32_t last_update_ms_;

    // SCP/OCP/OVP seen by service() and not yet reported by get_status()
    bool latched_scp_ = false;
    bool latched_ocp_ = false;
    bool latched_ovp_ = false;

    mutable ErrorLogLimiter log_limiter_{5000};  // same error from the same place: log at most every 5 s

    std::expected<void, ControllerError> read_status_locked(OutputRegulatorStatus& status) noexcept;
    std::expected<void, ControllerError> read_setpoints_locked(float& voltage, float& current_limit) const noexcept;
    std::expected<void, ControllerError> apply_config_locked(float voltage, float current_limit, float sense_resistor_ohms) noexcept;

    /** @brief Map a driver error, log it (rate limited) and return it as unexpected */
    std::unexpected<ControllerError> fail(const char* what, const std::error_code& ec) const noexcept;

    template <typename T>
    std::expected<T, ControllerError> check(const char* what, tps55289::expected<T> r) const noexcept {
        if (!r) {
            return fail(what, r.error());
        }
        if constexpr (std::is_void_v<T>) {
            return {};
        } else {
            return std::move(*r);
        }
    }
};

#endif
