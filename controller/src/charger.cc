#include "charger.hpp"
#include "i2c_bus.hpp"
#include "log.hpp"
#include "protocol.pb.h"
#include "cmsis_os2.h"
#include "board.h"
#include <cstring>
#include <mutex>

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
      bus_mutex_(i2c_internal_bus_mutex()),
      update_sem_(1, 0, nullptr)
{
    cached_status_ = ChargerStatus_init_default;
    cached_measurements_ = ChargerMeasurements_init_default;
    last_update_ms_ = 0;
}

Charger::~Charger() = default;

ControllerError Charger::log_error(const char* what, const std::error_code& ec) noexcept {
    os::error("Charger: %s failed: %s (source: %s)\n", what, ec.message().c_str(),
              get_error_source(ec).c_str());
    return from_bq25792_error(ec);
}

// =============================================================================
// Lifecycle / housekeeping
// =============================================================================

std::expected<void, ControllerError> Charger::initialize() noexcept {
    std::lock_guard lock(bus_mutex_);
    if (auto r = driver_.initialize(); !r) {
        return std::unexpected(log_error("initialize", r.error()));
    }
    return {};
}

std::expected<void, ControllerError> Charger::service() noexcept {
    std::lock_guard lock(bus_mutex_);

    if (!driver_.is_initialized()) {
        // Charger was absent / failed at boot: keep retrying so it recovers.
        if (auto r = driver_.initialize(); !r) {
            return std::unexpected(from_bq25792_error(r.error()));
        }
        return apply_settings_locked(settings_);
    }

    // Check before kicking: any write restarts an expired watchdog.
    auto expired = driver_.is_watchdog_expired();
    if (!expired) {
        return std::unexpected(log_error("read watchdog status", expired.error()));
    }
    if (auto r = driver_.kick_watchdog(); !r) {
        return std::unexpected(log_error("kick watchdog", r.error()));
    }
    if (*expired) {
        os::warn("Charger: BQ25792 watchdog expired, re-applying configuration\n");
        if (auto r = driver_.configure_adc(); !r) {
            return std::unexpected(log_error("configure ADC", r.error()));
        }
        return apply_settings_locked(settings_);
    }
    return {};
}

void Charger::run_service(uint32_t period_ms) noexcept {
    for (;;) {
        (void)service();  // errors are logged inside
        osDelay(period_ms);
    }
}

std::expected<void, ControllerError> Charger::apply_settings_locked(const Settings& s) noexcept {
    // Order: input path first, then limits, then enable.
    if (s.has_input_source) {
        if (auto r = driver_.set_input_source(static_cast<bq25792::ChargerInputSource>(s.input_source)); !r) {
            return std::unexpected(log_error("set input source", r.error()));
        }
    }
    if (s.has_input_voltage_limit) {
        if (auto r = driver_.set_input_voltage_limit(s.input_voltage_limit); !r) {
            return std::unexpected(log_error("set input voltage limit", r.error()));
        }
    }
    if (s.has_input_current_limit) {
        if (auto r = driver_.set_input_current_limit(s.input_current_limit); !r) {
            return std::unexpected(log_error("set input current limit", r.error()));
        }
    }
    if (s.has_charge_voltage_limit) {
        if (auto r = driver_.set_charge_voltage_limit(s.charge_voltage_limit); !r) {
            return std::unexpected(log_error("set charge voltage limit", r.error()));
        }
    }
    if (s.has_charge_current_limit) {
        if (auto r = driver_.set_charge_current_limit(s.charge_current_limit); !r) {
            return std::unexpected(log_error("set charge current limit", r.error()));
        }
    }
    if (s.has_enabled) {
        if (auto r = driver_.set_enabled(s.enabled); !r) {
            return std::unexpected(log_error("set enabled", r.error()));
        }
    }
    return {};
}

// =============================================================================
// Control
// =============================================================================

std::expected<void, ControllerError> Charger::set_enabled(bool enable) noexcept {
    std::lock_guard lock(bus_mutex_);
    if (auto r = driver_.set_enabled(enable); !r) {
        return std::unexpected(log_error("set enabled", r.error()));
    }
    settings_.has_enabled = true;
    settings_.enabled = enable;
    return {};
}

std::expected<bool, ControllerError> Charger::is_enabled() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.is_enabled();
    if (!r) {
        return std::unexpected(log_error("read enabled state", r.error()));
    }
    return *r;
}

std::expected<void, ControllerError> Charger::set_input_source(ChargerInputSource source) noexcept {
    if (!valid_input_source(source)) {
        return std::unexpected(ControllerError::BQ25792_InvalidParameter);
    }
    std::lock_guard lock(bus_mutex_);
    if (auto r = driver_.set_input_source(static_cast<bq25792::ChargerInputSource>(source)); !r) {
        return std::unexpected(log_error("set input source", r.error()));
    }
    settings_.has_input_source = true;
    settings_.input_source = source;
    return {};
}

ChargerInputSource Charger::get_input_source() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.get_input_source();
    if (!r) {
        log_error("get input source", r.error());
        return ChargerInputSource_CHARGER_INPUT_AUTO;
    }
    return static_cast<ChargerInputSource>(*r);
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

    Settings next = settings_;
    Settings delta{};
    if (config.has_charge_voltage_limit) {
        next.has_charge_voltage_limit = delta.has_charge_voltage_limit = true;
        next.charge_voltage_limit = delta.charge_voltage_limit = config.charge_voltage_limit;
    }
    if (config.has_charge_current_limit) {
        next.has_charge_current_limit = delta.has_charge_current_limit = true;
        next.charge_current_limit = delta.charge_current_limit = config.charge_current_limit;
    }
    if (config.has_input_voltage_limit) {
        next.has_input_voltage_limit = delta.has_input_voltage_limit = true;
        next.input_voltage_limit = delta.input_voltage_limit = config.input_voltage_limit;
    }
    if (config.has_input_current_limit) {
        next.has_input_current_limit = delta.has_input_current_limit = true;
        next.input_current_limit = delta.input_current_limit = config.input_current_limit;
    }
    if (config.has_input_source) {
        next.has_input_source = delta.has_input_source = true;
        next.input_source = delta.input_source = config.input_source;
    }

    std::lock_guard lock(bus_mutex_);
    // Remember the request even if the write fails part-way: service() re-applies it.
    settings_ = next;
    return apply_settings_locked(delta);
}

// =============================================================================
// Read-back helpers (caller holds bus_mutex_)
// =============================================================================

std::expected<void, ControllerError> Charger::read_status_locked(ChargerStatus& status) const noexcept {
    auto enabled = driver_.is_enabled();
    if (!enabled) return std::unexpected(log_error("read enabled state", enabled.error()));
    auto plugged = driver_.is_plugged_in();
    if (!plugged) return std::unexpected(log_error("read VBUS presence", plugged.error()));
    auto charge = driver_.get_charge_status();
    if (!charge) return std::unexpected(log_error("read charge status", charge.error()));
    auto vbus = driver_.get_vbus_status();
    if (!vbus) return std::unexpected(log_error("read VBUS status", vbus.error()));
    auto battery = driver_.is_battery_present();
    if (!battery) return std::unexpected(log_error("read battery presence", battery.error()));
    auto fault = driver_.is_fault_present();
    if (!fault) return std::unexpected(log_error("read fault status", fault.error()));
    auto source = driver_.get_input_source();
    if (!source) return std::unexpected(log_error("read input source", source.error()));

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
    auto adc = driver_.get_adc_measurements();
    if (!adc) {
        return std::unexpected(log_error("read ADC", adc.error()));
    }
    m.vbat_voltage = adc->vbat_v;
    m.ibus_current = adc->ibus_a;
    m.vbus_voltage = adc->vbus_v;
    m.vsys_voltage = adc->vsys_v;
    return {};
}

std::expected<void, ControllerError> Charger::read_config_locked(ChargerConfig& config) const noexcept {
    auto vlim = driver_.get_charge_voltage_limit();
    if (!vlim) return std::unexpected(log_error("read charge voltage limit", vlim.error()));
    auto ilim = driver_.get_charge_current_limit();
    if (!ilim) return std::unexpected(log_error("read charge current limit", ilim.error()));
    auto ivlim = driver_.get_input_voltage_limit();
    if (!ivlim) return std::unexpected(log_error("read input voltage limit", ivlim.error()));
    auto iilim = driver_.get_input_current_limit();
    if (!iilim) return std::unexpected(log_error("read input current limit", iilim.error()));

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
    std::lock_guard lock(bus_mutex_);
    return read_status_locked(status);
}

std::expected<void, ControllerError> Charger::get_measurements(ChargerMeasurements& measurements) const noexcept {
    std::lock_guard lock(bus_mutex_);
    return read_measurements_locked(measurements);
}

std::expected<void, ControllerError> Charger::get_config(ChargerConfig& config) const noexcept {
    std::lock_guard lock(bus_mutex_);
    return read_config_locked(config);
}

std::expected<void, ControllerError> Charger::get_status(SystemStatus& status) const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = read_status_locked(status.charger);
    status.has_charger = r.has_value();
    return r;
}

std::expected<void, ControllerError> Charger::get_measurements(SystemStatus& status) const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = read_measurements_locked(status.charger_measurements);
    status.has_charger_measurements = r.has_value();
    return r;
}

std::expected<void, ControllerError> Charger::get_config(SystemStatus& status) const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = read_config_locked(status.charger_config);
    status.has_charger_config = r.has_value();
    return r;
}

std::expected<void, ControllerError> Charger::get_full_status(SystemStatus& status) const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto s = read_status_locked(status.charger);
    status.has_charger = s.has_value();
    auto m = read_measurements_locked(status.charger_measurements);
    status.has_charger_measurements = m.has_value();
    auto c = read_config_locked(status.charger_config);
    status.has_charger_config = c.has_value();
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
// Single-value accessors (each takes the bus lock)
// =============================================================================

std::expected<bool, ControllerError> Charger::is_initialized() const noexcept {
    return driver_.is_initialized();
}

std::expected<bool, ControllerError> Charger::is_plugged_in() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.is_plugged_in();
    if (!r) return std::unexpected(log_error("read VBUS presence", r.error()));
    return *r;
}

std::expected<bool, ControllerError> Charger::is_battery_present() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.is_battery_present();
    if (!r) return std::unexpected(log_error("read battery presence", r.error()));
    return *r;
}

std::expected<bool, ControllerError> Charger::is_fault_present() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.is_fault_present();
    if (!r) return std::unexpected(log_error("read fault status", r.error()));
    return *r;
}

ChargeStatus Charger::get_charge_status() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.get_charge_status();
    if (!r) {
        log_error("read charge status", r.error());
        return ChargeStatus_CHARGE_STATUS_NOT_CHARGING;
    }
    return static_cast<ChargeStatus>(*r);
}

VBUSStatus Charger::get_vbus_status() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.get_vbus_status();
    if (!r) {
        log_error("read VBUS status", r.error());
        return VBUSStatus_VBUS_STATUS_NO_INPUT;
    }
    return static_cast<VBUSStatus>(*r);
}

std::expected<float, ControllerError> Charger::get_vbus_voltage() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.get_vbus();
    if (!r) return std::unexpected(log_error("read VBUS voltage", r.error()));
    return *r;
}

std::expected<float, ControllerError> Charger::get_vsys_voltage() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.get_vsys();
    if (!r) return std::unexpected(log_error("read VSYS voltage", r.error()));
    return *r;
}

std::expected<float, ControllerError> Charger::get_vbat_voltage() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.get_vbat();
    if (!r) return std::unexpected(log_error("read VBAT voltage", r.error()));
    return *r;
}

std::expected<float, ControllerError> Charger::get_ibus_current() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.get_ibus();
    if (!r) return std::unexpected(log_error("read IBUS current", r.error()));
    return *r;
}

std::expected<float, ControllerError> Charger::get_ibat_current() const noexcept {
    std::lock_guard lock(bus_mutex_);
    auto r = driver_.get_ibat();
    if (!r) return std::unexpected(log_error("read IBAT current", r.error()));
    return *r;
}
