#include "charger.hpp"
#include "i2c_bus.hpp"
#include "log.hpp"
#include "protocol.pb.h"
#include "cmsis_os2.h"
#include "board.h"

namespace {

// Limits enforced by the BQ25792 driver; checked up front so a request is
// either applied completely or not at all.
bool in_range(float v, float lo, float hi) { return v >= lo && v <= hi; }  // false for NaN

bool valid_input_source(ChargerInputSource s) {
    return s == ChargerInputSource_CHARGER_INPUT_AC1 ||
           s == ChargerInputSource_CHARGER_INPUT_AC2 ||
           s == ChargerInputSource_CHARGER_INPUT_AUTO;
}

} // namespace

Charger::Charger(I2C_HandleTypeDef* hi2c)
    : driver_(hi2c),
      update_sem_(1, 0, nullptr),
      last_update_ms_(0)
{
    (void)i2c_internal_bus_mutex();  // create the bus mutex before any thread uses it
}

Charger::~Charger() = default;

std::unexpected<ControllerError> Charger::fail(const char* what, const std::error_code& ec) const noexcept {
    const ControllerError mapped = from_bq25792_error(ec);
    if (log_limiter_.allow(what, mapped, os::timestamp_ms())) {
        os::error("Charger: %s failed: %s [%s] (%lu repeats suppressed)\n", what, to_string(mapped),
                  get_error_source(ec), static_cast<unsigned long>(log_limiter_.suppressed()));
    }
    return std::unexpected(mapped);
}

// =============================================================================
// Lifecycle / housekeeping
// =============================================================================

std::expected<void, ControllerError> Charger::initialize() noexcept {
    return i2c_internal_bus_transaction([&] { return check("initialize", driver_.initialize()); });
}

std::expected<void, ControllerError> Charger::service() noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        if (!driver_.is_initialized()) {
            // Charger was absent / failed at boot: keep retrying so it recovers.
            if (auto r = check("initialize", driver_.initialize()); !r) {
                return r;
            }
            settings_dirty_ = true;
        } else {
            // Check before kicking: any write restarts an expired watchdog.
            auto expired = check("read watchdog status", driver_.is_watchdog_expired());
            if (!expired) {
                return std::unexpected(expired.error());
            }
            if (auto r = check("kick watchdog", driver_.kick_watchdog()); !r) {
                return r;
            }
            if (*expired) {
                os::warn("Charger: BQ25792 watchdog expired, re-applying configuration\n");
                if (auto r = check("configure ADC", driver_.configure_adc()); !r) {
                    return r;
                }
                settings_dirty_ = true;
            }
        }

        if (settings_dirty_) {
            if (auto r = apply_settings_locked(settings_); !r) {
                return r;   // stays dirty, retried on the next service() call
            }
            settings_dirty_ = false;
        }
        return {};
    });
}

std::expected<void, ControllerError> Charger::apply_settings_locked(const Settings& s) noexcept {
    // Order: input path first, then limits, then enable.
    if (s.has_input_source) {
        if (auto r = check("set input source",
                           driver_.set_input_source(static_cast<bq25792::ChargerInputSource>(s.input_source))); !r) {
            return r;
        }
    }
    if (s.has_input_voltage_limit) {
        if (auto r = check("set input voltage limit", driver_.set_input_voltage_limit(s.input_voltage_limit)); !r) {
            return r;
        }
    }
    if (s.has_input_current_limit) {
        if (auto r = check("set input current limit", driver_.set_input_current_limit(s.input_current_limit)); !r) {
            return r;
        }
    }
    if (s.has_charge_voltage_limit) {
        if (auto r = check("set charge voltage limit", driver_.set_charge_voltage_limit(s.charge_voltage_limit)); !r) {
            return r;
        }
    }
    if (s.has_charge_current_limit) {
        if (auto r = check("set charge current limit", driver_.set_charge_current_limit(s.charge_current_limit)); !r) {
            return r;
        }
    }
    if (s.has_enabled) {
        if (auto r = check("set enabled", driver_.set_enabled(s.enabled)); !r) {
            return r;
        }
    }
    return {};
}

// =============================================================================
// Control
// =============================================================================

std::expected<void, ControllerError> Charger::set_enabled(bool enable) noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        settings_.has_enabled = true;
        settings_.enabled = enable;
        auto r = check("set enabled", driver_.set_enabled(enable));
        settings_dirty_ = settings_dirty_ || !r;
        return r;
    });
}

std::expected<bool, ControllerError> Charger::is_enabled() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read enabled state", driver_.is_enabled()); });
}

std::expected<void, ControllerError> Charger::set_input_source(ChargerInputSource source) noexcept {
    if (!valid_input_source(source)) {
        return std::unexpected(ControllerError::BQ25792_InvalidParameter);
    }
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        settings_.has_input_source = true;
        settings_.input_source = source;
        auto r = check("set input source",
                       driver_.set_input_source(static_cast<bq25792::ChargerInputSource>(source)));
        settings_dirty_ = settings_dirty_ || !r;
        return r;
    });
}

std::expected<ChargerInputSource, ControllerError> Charger::get_input_source() const noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<ChargerInputSource, ControllerError> {
        auto r = check("read input source", driver_.get_input_source());
        if (!r) {
            return std::unexpected(r.error());
        }
        return static_cast<ChargerInputSource>(*r);
    });
}

std::expected<void, ControllerError> Charger::set_config(const ChargerConfig& config) noexcept {
    ChargerConfigRequest req = ChargerConfigRequest_init_zero;
    req.has_charge_voltage_limit = true;
    req.charge_voltage_limit = config.charge_voltage_limit;
    req.has_charge_current_limit = true;
    req.charge_current_limit = config.charge_current_limit;
    req.has_input_voltage_limit = true;
    req.input_voltage_limit = config.input_voltage_limit;
    req.has_input_current_limit = true;
    req.input_current_limit = config.input_current_limit;
    return set_config(req);
}

std::expected<void, ControllerError> Charger::set_config(const ChargerConfigRequest& config) noexcept {
    // Validate every present field before touching the hardware, so a bad
    // request never leaves the charger half-configured.
    if ((config.has_charge_voltage_limit && !in_range(config.charge_voltage_limit, 3.0f, 18.8f)) ||
        (config.has_charge_current_limit && !in_range(config.charge_current_limit, 0.05f, 5.0f)) ||
        (config.has_input_voltage_limit && !in_range(config.input_voltage_limit, 3.6f, 22.0f)) ||
        (config.has_input_current_limit && !in_range(config.input_current_limit, 0.1f, 3.3f)) ||
        (config.has_input_source && !valid_input_source(config.input_source))) {
        os::error("Charger: rejected out-of-range configuration request\n");
        return std::unexpected(ControllerError::BQ25792_InvalidParameter);
    }

    Settings delta{};
    if (config.has_charge_voltage_limit) {
        delta.has_charge_voltage_limit = true;
        delta.charge_voltage_limit = config.charge_voltage_limit;
    }
    if (config.has_charge_current_limit) {
        delta.has_charge_current_limit = true;
        delta.charge_current_limit = config.charge_current_limit;
    }
    if (config.has_input_voltage_limit) {
        delta.has_input_voltage_limit = true;
        delta.input_voltage_limit = config.input_voltage_limit;
    }
    if (config.has_input_current_limit) {
        delta.has_input_current_limit = true;
        delta.input_current_limit = config.input_current_limit;
    }
    if (config.has_input_source) {
        delta.has_input_source = true;
        delta.input_source = config.input_source;
    }

    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        // Remember the request even if the write fails part-way: service() re-applies it.
        if (delta.has_charge_voltage_limit) {
            settings_.has_charge_voltage_limit = true;
            settings_.charge_voltage_limit = delta.charge_voltage_limit;
        }
        if (delta.has_charge_current_limit) {
            settings_.has_charge_current_limit = true;
            settings_.charge_current_limit = delta.charge_current_limit;
        }
        if (delta.has_input_voltage_limit) {
            settings_.has_input_voltage_limit = true;
            settings_.input_voltage_limit = delta.input_voltage_limit;
        }
        if (delta.has_input_current_limit) {
            settings_.has_input_current_limit = true;
            settings_.input_current_limit = delta.input_current_limit;
        }
        if (delta.has_input_source) {
            settings_.has_input_source = true;
            settings_.input_source = delta.input_source;
        }
        auto r = apply_settings_locked(delta);
        settings_dirty_ = settings_dirty_ || !r;
        return r;
    });
}

// =============================================================================
// Read-back helpers (caller holds the bus lock)
// =============================================================================

std::expected<void, ControllerError> Charger::read_status_locked(ChargerStatus& status) const noexcept {
    auto enabled = check("read enabled state", driver_.is_enabled());
    if (!enabled) return std::unexpected(enabled.error());
    auto plugged = check("read VBUS presence", driver_.is_plugged_in());
    if (!plugged) return std::unexpected(plugged.error());
    auto charge = check("read charge status", driver_.get_charge_status());
    if (!charge) return std::unexpected(charge.error());
    auto vbus = check("read VBUS status", driver_.get_vbus_status());
    if (!vbus) return std::unexpected(vbus.error());
    auto battery = check("read battery presence", driver_.is_battery_present());
    if (!battery) return std::unexpected(battery.error());
    auto fault = check("read fault status", driver_.is_fault_present());
    if (!fault) return std::unexpected(fault.error());
    auto source = check("read input source", driver_.get_input_source());
    if (!source) return std::unexpected(source.error());

    status.enabled = *enabled;
    status.plugged_in = *plugged;
    status.charge_status = static_cast<ChargeStatus>(*charge);
    status.vbus_status = static_cast<VBUSStatus>(*vbus);
    status.battery_present = *battery;
    status.fault_present = *fault;
    status.input_source = static_cast<ChargerInputSource>(*source);
    status.timestamp_ms = last_update_ms_;
    return {};
}

std::expected<void, ControllerError> Charger::read_measurements_locked(ChargerMeasurements& m) const noexcept {
    auto adc = check("read ADC", driver_.get_adc_measurements());
    if (!adc) {
        return std::unexpected(adc.error());
    }
    m.vbat_voltage = adc->vbat_v;
    m.ibus_current = adc->ibus_a;
    m.vbus_voltage = adc->vbus_v;
    m.vsys_voltage = adc->vsys_v;
    return {};
}

std::expected<void, ControllerError> Charger::read_config_locked(ChargerConfig& config) const noexcept {
    auto vlim = check("read charge voltage limit", driver_.get_charge_voltage_limit());
    if (!vlim) return std::unexpected(vlim.error());
    auto ilim = check("read charge current limit", driver_.get_charge_current_limit());
    if (!ilim) return std::unexpected(ilim.error());
    auto ivlim = check("read input voltage limit", driver_.get_input_voltage_limit());
    if (!ivlim) return std::unexpected(ivlim.error());
    auto iilim = check("read input current limit", driver_.get_input_current_limit());
    if (!iilim) return std::unexpected(iilim.error());

    config.charge_voltage_limit = *vlim;
    config.charge_current_limit = *ilim;
    config.input_voltage_limit = *ivlim;
    config.input_current_limit = *iilim;
    return {};
}

// =============================================================================
// Read-back (public)
// =============================================================================

std::expected<void, ControllerError> Charger::get_status(ChargerStatus& status) const noexcept {
    return i2c_internal_bus_transaction([&] { return read_status_locked(status); });
}

std::expected<void, ControllerError> Charger::get_measurements(ChargerMeasurements& measurements) const noexcept {
    return i2c_internal_bus_transaction([&] { return read_measurements_locked(measurements); });
}

std::expected<void, ControllerError> Charger::get_config(ChargerConfig& config) const noexcept {
    return i2c_internal_bus_transaction([&] { return read_config_locked(config); });
}

std::expected<void, ControllerError> Charger::get_status(SystemStatus& status) const noexcept {
    auto r = i2c_internal_bus_transaction([&] { return read_status_locked(status.charger); });
    status.has_charger = r.has_value();
    return r;
}

std::expected<void, ControllerError> Charger::get_measurements(SystemStatus& status) const noexcept {
    auto r = i2c_internal_bus_transaction([&] { return read_measurements_locked(status.charger_measurements); });
    status.has_charger_measurements = r.has_value();
    return r;
}

std::expected<void, ControllerError> Charger::get_config(SystemStatus& status) const noexcept {
    auto r = i2c_internal_bus_transaction([&] { return read_config_locked(status.charger_config); });
    status.has_charger_config = r.has_value();
    return r;
}

std::expected<void, ControllerError> Charger::get_full_status(SystemStatus& status) const noexcept {
    auto s = get_status(status);
    auto m = get_measurements(status);
    auto c = get_config(status);
    if (!s) return s;
    if (!m) return m;
    return c;
}

// =============================================================================
// Update notification
// =============================================================================

bool Charger::has_updates() const noexcept {
    return update_sem_.try_acquire();
}

bool Charger::wait_for_update(uint32_t timeout_ms) const noexcept {
    if (timeout_ms == 0) {
        update_sem_.acquire();
        return true;
    }
    return update_sem_.try_acquire_for(timeout_ms);
}

void Charger::trigger_update() noexcept {
    update_sem_.release();
}

// =============================================================================
// Single-value accessors
// =============================================================================

bool Charger::is_initialized() const noexcept {
    return driver_.is_initialized();
}

std::expected<bool, ControllerError> Charger::is_plugged_in() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read VBUS presence", driver_.is_plugged_in()); });
}

std::expected<bool, ControllerError> Charger::is_battery_present() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read battery presence", driver_.is_battery_present()); });
}

std::expected<bool, ControllerError> Charger::is_fault_present() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read fault status", driver_.is_fault_present()); });
}

std::expected<ChargeStatus, ControllerError> Charger::get_charge_status() const noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<ChargeStatus, ControllerError> {
        auto r = check("read charge status", driver_.get_charge_status());
        if (!r) return std::unexpected(r.error());
        return static_cast<ChargeStatus>(*r);
    });
}

std::expected<VBUSStatus, ControllerError> Charger::get_vbus_status() const noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<VBUSStatus, ControllerError> {
        auto r = check("read VBUS status", driver_.get_vbus_status());
        if (!r) return std::unexpected(r.error());
        return static_cast<VBUSStatus>(*r);
    });
}

std::expected<float, ControllerError> Charger::get_vbus_voltage() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read VBUS voltage", driver_.get_vbus()); });
}

std::expected<float, ControllerError> Charger::get_vsys_voltage() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read VSYS voltage", driver_.get_vsys()); });
}

std::expected<float, ControllerError> Charger::get_vbat_voltage() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read VBAT voltage", driver_.get_vbat()); });
}

std::expected<float, ControllerError> Charger::get_ibus_current() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read IBUS current", driver_.get_ibus()); });
}

std::expected<float, ControllerError> Charger::get_ibat_current() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read IBAT current", driver_.get_ibat()); });
}
