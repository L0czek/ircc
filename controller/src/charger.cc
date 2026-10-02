#include "charger.hpp"
#include "log.hpp"
#include "protocol.pb.h"
#include "cmsis_os2.h"
#include "board.h"
#include <cstring>
#include <mutex>

// Include the BQ25792 driver
extern "C" {
#include "bq25792.hpp"
}

Charger::Charger(I2C_HandleTypeDef* hi2c)
    : driver_(hi2c),
      mutex_(nullptr),
      update_sem_(1, 0, nullptr)
{
    // Initialize cached values
    cached_status_ = ChargerStatus_init_default;
    cached_measurements_ = ChargerMeasurements_init_default;
    last_update_ms_ = 0;
}

Charger::~Charger() = default;

std::expected<void, ControllerError> Charger::initialize() noexcept {
    auto result = driver_.initialize();
    if (!result) {
        os::error("Failed to initialize BQ25792: %s (source: %s)\n",
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }
    return {};
}

std::expected<void, ControllerError> Charger::set_enabled(bool enable) noexcept {
    std::lock_guard lock(mutex_);

    auto result = driver_.set_enabled(enable);
    if (!result) {
        os::error("Failed to set charger enabled: %s (source: %s)\n",
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }
    return {};
}

std::expected<bool, ControllerError> Charger::is_enabled() const noexcept {
    std::lock_guard lock(mutex_);

    auto result = driver_.is_enabled();
    if (!result) {
        os::error("Failed to read charger enabled state: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }

    return result.value();
}

std::expected<void, ControllerError> Charger::set_input_source(ChargerInputSource source) noexcept {
    std::lock_guard lock(mutex_);

    auto result = driver_.set_input_source(static_cast<bq25792::ChargerInputSource>(source));
    if (!result) {
        os::error("Failed to set input source: %s (source: %s)\n",
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }
    return {};
}

ChargerInputSource Charger::get_input_source() const noexcept {
    std::lock_guard lock(mutex_);

    auto result = driver_.get_input_source();
    if (!result) {
        os::error("Failed to get input source: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return ChargerInputSource_CHARGER_INPUT_USB_DPDM;  // Default fallback
    }

    return static_cast<ChargerInputSource>(result.value());
}

std::expected<void, ControllerError> Charger::get_status(ChargerStatus& status) const noexcept {
    std::lock_guard lock(mutex_);

    // Fill in basic status
    auto enabled = is_enabled();
    auto plugged = is_plugged_in();
    auto chargeStatus = get_charge_status();
    auto vbusStatus = get_vbus_status();
    auto battery = is_battery_present();
    auto fault = is_fault_present();
    auto inputSource = get_input_source();

    if (!enabled || !plugged || !battery || !fault) {
        if (!enabled) return std::unexpected(enabled.error());
        if (!plugged) return std::unexpected(plugged.error());
        if (!battery) return std::unexpected(battery.error());
        if (!fault) return std::unexpected(fault.error());
    }

    status.enabled = enabled.value();
    status.plugged_in = plugged.value();
    status.charge_status = static_cast<ChargeStatus>(chargeStatus);
    status.vbus_status = static_cast<VBUSStatus>(vbusStatus);
    status.battery_present = battery.value();
    status.fault_present = fault.value();
    status.input_source = inputSource;
    status.timestamp_ms = last_update_ms_;

    return {};
}

std::expected<void, ControllerError> Charger::get_measurements(ChargerMeasurements& measurements) const noexcept {
    std::lock_guard lock(mutex_);

    auto vbat = get_vbat_voltage();
    auto ibus = get_ibus_current();
    auto vbus = get_vbus_voltage();
    auto vsys = get_vsys_voltage();

    if (!vbat || !ibus || !vbus || !vsys) {
        if (!vbat) return std::unexpected(vbat.error());
        if (!ibus) return std::unexpected(ibus.error());
        if (!vbus) return std::unexpected(vbus.error());
        if (!vsys) return std::unexpected(vsys.error());
    }

    measurements.vbat_voltage = vbat.value();
    measurements.ibus_current = ibus.value();
    measurements.vbus_voltage = vbus.value();
    measurements.vsys_voltage = vsys.value();

    return {};
}

std::expected<void, ControllerError> Charger::get_config(ChargerConfig& config) const noexcept {
    std::lock_guard lock(mutex_);

    auto vlim = driver_.get_charge_voltage_limit();
    auto ilim = driver_.get_charge_current_limit();
    auto ivlim = driver_.get_input_voltage_limit();
    auto iilim = driver_.get_input_current_limit();

    if (!vlim || !ilim || !ivlim || !iilim) {
        os::error("Failed to get charger config\n");
        if (!vlim) return std::unexpected(from_bq25792_error(vlim.error()));
        if (!ilim) return std::unexpected(from_bq25792_error(ilim.error()));
        if (!ivlim) return std::unexpected(from_bq25792_error(ivlim.error()));
        if (!iilim) return std::unexpected(from_bq25792_error(iilim.error()));
    }

    config.charge_voltage_limit = vlim.value();
    config.charge_current_limit = ilim.value();
    config.input_voltage_limit = ivlim.value();
    config.input_current_limit = iilim.value();

    return {};
}

std::expected<void, ControllerError> Charger::set_config(const ChargerConfig& config) noexcept {
    std::lock_guard lock(mutex_);

    auto result = driver_.set_charge_voltage_limit(config.charge_voltage_limit);
    if (!result) {
        os::error("Failed to set charge voltage limit: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }

    result = driver_.set_charge_current_limit(config.charge_current_limit);
    if (!result) {
        os::error("Failed to set charge current limit: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }

    result = driver_.set_input_voltage_limit(config.input_voltage_limit);
    if (!result) {
        os::error("Failed to set input voltage limit: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }

    result = driver_.set_input_current_limit(config.input_current_limit);
    if (!result) {
        os::error("Failed to set input current limit: %s (source: %s)\n",
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }
    return {};
}

std::expected<void, ControllerError> Charger::set_config(const ChargerConfigRequest& config) noexcept {
    std::lock_guard lock(mutex_);

    // Set input source if specified
    if (config.has_input_source) {
        auto result = driver_.set_input_source(static_cast<bq25792::ChargerInputSource>(config.input_source));
        if (!result) {
            os::error("Failed to set input source: %s (source: %s)\n", 
                result.error().message().c_str(),
                get_error_source(result.error()).c_str());
            return std::unexpected(from_bq25792_error(result.error()));
        }
    }

    auto result = driver_.set_charge_voltage_limit(config.charge_voltage_limit);
    if (!result) {
        os::error("Failed to set charge voltage limit: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }

    result = driver_.set_charge_current_limit(config.charge_current_limit);
    if (!result) {
        os::error("Failed to set charge current limit: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }

    result = driver_.set_input_voltage_limit(config.input_voltage_limit);
    if (!result) {
        os::error("Failed to set input voltage limit: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }

    result = driver_.set_input_current_limit(config.input_current_limit);
    if (!result) {
        os::error("Failed to set input current limit: %s (source: %s)\n",
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }
    return {};
}

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

std::expected<bool, ControllerError> Charger::is_initialized() const noexcept {
    // is_initialized() from driver is a simple getter that cannot fail
    return std::expected<bool, ControllerError>{driver_.is_initialized()};
}

std::expected<bool, ControllerError> Charger::is_plugged_in() const noexcept {
    auto result = driver_.is_plugged_in();
    if (!result) {
        os::error("Failed to check VBUS presence: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }
    return result.value();
}

std::expected<bool, ControllerError> Charger::is_battery_present() const noexcept {
    auto result = driver_.is_battery_present();
    if (!result) {
        os::error("Failed to check battery presence: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }
    return result.value();
}

std::expected<bool, ControllerError> Charger::is_fault_present() const noexcept {
    auto result = driver_.is_fault_present();
    if (!result) {
        os::error("Failed to check fault status: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }
    return result.value();
}

ChargeStatus Charger::get_charge_status() const noexcept {
    auto result = driver_.get_charge_status();
    if (!result) {
        os::error("Failed to get charge status: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return ChargeStatus_CHARGE_STATUS_NOT_CHARGING;
    }
    return static_cast<ChargeStatus>(result.value());
}

VBUSStatus Charger::get_vbus_status() const noexcept {
    auto result = driver_.get_vbus_status();
    if (!result) {
        os::error("Failed to get VBUS status: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return VBUSStatus_VBUS_STATUS_NO_INPUT;
    }
    return static_cast<VBUSStatus>(result.value());
}

std::expected<float, ControllerError> Charger::get_vbus_voltage() const noexcept {
    auto result = driver_.get_vbus();
    if (!result) {
        os::error("Failed to get VBUS voltage: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }
    return result.value();
}

std::expected<float, ControllerError> Charger::get_vsys_voltage() const noexcept {
    auto result = driver_.get_vsys();
    if (!result) {
        os::error("Failed to get VSYS voltage: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }
    return result.value();
}

std::expected<float, ControllerError> Charger::get_vbat_voltage() const noexcept {
    auto result = driver_.get_vbat();
    if (!result) {
        os::error("Failed to get VBAT voltage: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }
    return result.value();
}

std::expected<float, ControllerError> Charger::get_ibus_current() const noexcept {
    auto result = driver_.get_ibus();
    if (!result) {
        os::error("Failed to get IBUS current: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }
    return result.value();
}

std::expected<float, ControllerError> Charger::get_ibat_current() const noexcept {
    auto result = driver_.get_ibat();
    if (!result) {
        os::error("Failed to get IBAT current: %s (source: %s)\n", 
            result.error().message().c_str(),
            get_error_source(result.error()).c_str());
        return std::unexpected(from_bq25792_error(result.error()));
    }
    return result.value();
}
