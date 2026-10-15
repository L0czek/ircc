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
    ChargeStatus chargeStatus = get_charge_status();
    VBUSStatus vbusStatus = get_vbus_status();
    auto battery = is_battery_present();
    auto fault = is_fault_present();
    ChargerInputSource inputSource = get_input_source();

    if (!enabled) return std::unexpected(enabled.error());
    if (!plugged) return std::unexpected(plugged.error());
    if (!battery) return std::unexpected(battery.error());
    if (!fault) return std::unexpected(fault.error());

    status.enabled = enabled.value();
    status.plugged_in = plugged.value();
    status.charge_status = chargeStatus;
    status.vbus_status = vbusStatus;
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

    if (!vbat) return std::unexpected(vbat.error());
    if (!ibus) return std::unexpected(ibus.error());
    if (!vbus) return std::unexpected(vbus.error());
    if (!vsys) return std::unexpected(vsys.error());

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

    if (!vlim) return std::unexpected(from_bq25792_error(vlim.error()));
    if (!ilim) return std::unexpected(from_bq25792_error(ilim.error()));
    if (!ivlim) return std::unexpected(from_bq25792_error(ivlim.error()));
    if (!iilim) return std::unexpected(from_bq25792_error(iilim.error()));

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

// ==================== SystemStatus Methods ====================

std::expected<void, ControllerError> Charger::get_status(SystemStatus& status) const noexcept {
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

    // Populate embedded charger status message
    status.has_charger = true;
    status.charger.enabled = enabled.value();
    status.charger.plugged_in = plugged.value();
    status.charger.charge_status = static_cast<ChargeStatus>(chargeStatus);
    status.charger.vbus_status = static_cast<VBUSStatus>(vbusStatus);
    status.charger.battery_present = battery.value();
    status.charger.fault_present = fault.value();
    status.charger.input_source = inputSource;
    status.charger.timestamp_ms = last_update_ms_;

    return {};
}

std::expected<void, ControllerError> Charger::get_measurements(SystemStatus& status) const noexcept {
    std::lock_guard lock(mutex_);

    auto vbat = get_vbat_voltage();
    auto ibus = get_ibus_current();
    auto vbus = get_vbus_voltage();
    auto vsys = get_vsys_voltage();

    if (!vbat) return std::unexpected(vbat.error());
    if (!ibus) return std::unexpected(ibus.error());
    if (!vbus) return std::unexpected(vbus.error());
    if (!vsys) return std::unexpected(vsys.error());

    // Populate embedded measurements message
    status.has_charger_measurements = true;
    status.charger_measurements.vbat_voltage = vbat.value();
    status.charger_measurements.ibus_current = ibus.value();
    status.charger_measurements.vbus_voltage = vbus.value();
    status.charger_measurements.vsys_voltage = vsys.value();

    return {};
}

std::expected<void, ControllerError> Charger::get_config(SystemStatus& status) const noexcept {
    std::lock_guard lock(mutex_);

    auto vlim = driver_.get_charge_voltage_limit();
    auto ilim = driver_.get_charge_current_limit();
    auto ivlim = driver_.get_input_voltage_limit();
    auto iilim = driver_.get_input_current_limit();

    if (!vlim) return std::unexpected(from_bq25792_error(vlim.error()));
    if (!ilim) return std::unexpected(from_bq25792_error(ilim.error()));
    if (!ivlim) return std::unexpected(from_bq25792_error(ivlim.error()));
    if (!iilim) return std::unexpected(from_bq25792_error(iilim.error()));

    // Populate embedded config message
    status.has_charger_config = true;
    status.charger_config.charge_voltage_limit = vlim.value();
    status.charger_config.charge_current_limit = ilim.value();
    status.charger_config.input_voltage_limit = ivlim.value();
    status.charger_config.input_current_limit = iilim.value();

    return {};
}

std::expected<void, ControllerError> Charger::get_full_status(SystemStatus& status) const noexcept {
    std::lock_guard lock(mutex_);

    // Initialize status with default values (zero/false for booleans, 0 for numbers)
    memset(&status, 0, sizeof(status));

    // Read all status registers in one burst (registers 0x1B-0x27, 13 bytes)
    // Layout: STATUS0, STATUS1, STATUS2, STATUS3, STATUS4, FAULT0, FAULT1,
    //         FLAG0, FLAG1, FLAG2, FLAG3, FAULT_FLAG0, FAULT_FLAG1
    uint8_t status_block[13];
    auto result = driver_.read_status_block(status_block);
    if (!result) {
        return std::unexpected(from_bq25792_error(result.error()));
    }

    // Extract status from status_block
    // STATUS0 (index 0): VBUS_PRESENT_STAT (bit 0)
    bool plugged = (status_block[0] & BQ25792_VBUS_PRESENT_STAT_MASK) != 0;

    // STATUS1 (index 1): CHG_STAT (bits 7-5), VBUS_STAT (bits 4-1)
    uint8_t charge_stat = (status_block[1] & BQ25792_CHG_STAT_MASK) >> BQ25792_CHG_STAT_SHIFT;
    uint8_t vbus_stat = (status_block[1] & BQ25792_VBUS_STAT_MASK) >> BQ25792_VBUS_STAT_SHIFT;

    // STATUS2 (index 2): BATTERY_PRESENT (bit 0)
    bool battery = (status_block[2] & BQ25792_BATTERY_PRESENT_MASK) != 0;

    // FAULT0/FAULT1 (index 5,6): Any fault bit set?
    bool fault = (status_block[5] | status_block[6]) != 0;

    // STATUS4 (index 4): EN_CHG (bit 0)
    bool enabled = (status_block[4] & BQ25792_EN_CHG_MASK) != 0;

    // Input source from CHARGER_CONTROL_5 (register 0x14, offset 20 in config block)
    // We need to read config for input source - use read_block for config
    uint8_t config_block[21];
    result = driver_.read_config_block(config_block);
    if (!result) {
        return std::unexpected(from_bq25792_error(result.error()));
    }

    uint8_t input_sel = (config_block[20] & BQ25792_INPUT_SOURCE_MASK) >> BQ25792_INPUT_SOURCE_SHIFT;
    ChargerInputSource inputSource = static_cast<ChargerInputSource>(input_sel);

    // Note: We don't fail on fault - it's just reported in the status
    // enabled, plugged, and battery presence are checked but don't fail the operation
    // Fault presence is reported but doesn't cause an error return

    // Populate embedded charger status message
    status.has_charger = true;
    status.charger.enabled = enabled;
    status.charger.plugged_in = plugged;
    status.charger.charge_status = static_cast<ChargeStatus>(charge_stat);
    status.charger.vbus_status = static_cast<VBUSStatus>(vbus_stat);
    status.charger.battery_present = battery;
    status.charger.fault_present = fault;
    status.charger.input_source = inputSource;
    status.charger.timestamp_ms = last_update_ms_;

    // Read all ADC values in one burst (registers 0x31-0x45, 22 bytes)
    uint8_t adc_block[22];
    result = driver_.read_adc_block(adc_block);
    if (!result) {
        return std::unexpected(from_bq25792_error(result.error()));
    }

    // Extract ADC values (all are 16-bit big-endian, 1mV/1mA steps)
    // VBAT (index 10-11): 0x3B-0x3C
    uint16_t vbat_raw = (static_cast<uint16_t>(adc_block[10]) << 8) | adc_block[11];
    float vbat_voltage = static_cast<float>(vbat_raw) / 1000.0f;

    // IBUS (index 0-1): 0x31-0x32
    uint16_t ibus_raw = (static_cast<uint16_t>(adc_block[0]) << 8) | adc_block[1];
    float ibus_current = static_cast<float>(ibus_raw) / 1000.0f;

    // VBUS (index 4-5): 0x35-0x36
    uint16_t vbus_raw = (static_cast<uint16_t>(adc_block[4]) << 8) | adc_block[5];
    float vbus_voltage = static_cast<float>(vbus_raw) / 1000.0f;

    // VSYS (index 8-9): 0x3D-0x3E
    uint16_t vsys_raw = (static_cast<uint16_t>(adc_block[8]) << 8) | adc_block[9];
    float vsys_voltage = static_cast<float>(vsys_raw) / 1000.0f;

    // Populate embedded measurements message
    status.has_charger_measurements = true;
    status.charger_measurements.vbat_voltage = vbat_voltage;
    status.charger_measurements.ibus_current = ibus_current;
    status.charger_measurements.vbus_voltage = vbus_voltage;
    status.charger_measurements.vsys_voltage = vsys_voltage;

    // Charger config fields (13-16) - already have from config_block
    // Charge voltage limit (index 1-2): 0x01-0x02
    uint16_t vlim_raw = (static_cast<uint16_t>(config_block[1]) << 8) | config_block[2];
    float charge_voltage_limit = static_cast<float>(vlim_raw) / 100.0f;  // 10mV steps

    // Charge current limit (index 3-4): 0x03-0x04
    uint16_t ilim_raw = (static_cast<uint16_t>(config_block[3]) << 8) | config_block[4];
    float charge_current_limit = static_cast<float>(ilim_raw) / 100.0f;  // 10mA steps

    // Input voltage limit (index 5): 0x05 (8-bit)
    float input_voltage_limit = static_cast<float>(config_block[5]) * 0.1f;  // 100mV steps

    // Input current limit (index 6-7): 0x06-0x07
    uint16_t iilim_raw = (static_cast<uint16_t>(config_block[6]) << 8) | config_block[7];
    float input_current_limit = static_cast<float>(iilim_raw) / 100.0f;  // 10mA steps

    // Populate embedded config message
    status.has_charger_config = true;
    status.charger_config.charge_voltage_limit = charge_voltage_limit;
    status.charger_config.charge_current_limit = charge_current_limit;
    status.charger_config.input_voltage_limit = input_voltage_limit;
    status.charger_config.input_current_limit = input_current_limit;

    return {};
}
