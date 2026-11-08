#include "hardware_emulator.hpp"
#include "pb_encode.h"
#include "pb_decode.h"
#include "protocol.pb.h"
#include "error_host.hpp"
#include "scope_data.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <cstdarg>
#include <iomanip>
#include <iterator>
#include <iostream>
#include <sstream>
#include <random>

namespace {

// Partner Source_Capabilities: fixed 5 / 9 / 15 / 20 V at 3 A, raw PDOs as on the wire
constexpr uint32_t kPartnerPdos[] = {0x0001912C, 0x0002D12C, 0x0004B12C, 0x0006412C};
constexpr float kDockVoltage = 20.0f;   // emulated dock DC on VAC2

bool in_range(float v, float lo, float hi) {
    return v >= lo && v <= hi;
}

// Same mapping as status_for() in controller/src/message_handler.cc for the codes used here
ResponseStatusCode status_for(ControllerError e) {
    switch (e) {
        case ControllerError::BQ25792_InvalidParameter:
        case ControllerError::USBPD_InvalidParameter:
            return ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS;
        case ControllerError::USBPD_NotAttached:
        case ControllerError::USBPD_StackBusy:
            return ResponseStatusCode_RESPONSE_STATUS_INVALID_STATE;
        default:
            return ResponseStatusCode_RESPONSE_STATUS_FAILURE;
    }
}

float typec_current_a(TypeCCurrent c) {
    switch (c) {
        case TypeCCurrent_TYPEC_CURRENT_3A0: return 3.0f;
        case TypeCCurrent_TYPEC_CURRENT_1A5: return 1.5f;
        default:                             return 0.5f;   // USB default
    }
}

// Nominal VAC1 / VAC2: the USB-C contract (Type-C only: 5 V) and the dock
std::pair<float, float> input_voltages(const SystemStatus& ss) {
    float vac1 = 0.0f;
    const auto& p = ss.usb_pd;
    if (ss.has_usb_pd && p.power_role == UsbPdPowerRole_USB_PD_POWER_ROLE_SINK &&
        p.state != UsbPdState_USB_PD_STATE_DETACHED) {
        vac1 = p.state == UsbPdState_USB_PD_STATE_CONTRACT && p.has_contract_voltage ? p.contract_voltage : 5.0f;
    }
    const float vac2 = (ss.charger.status_flags & (1u << 2)) ? kDockVoltage : 0.0f;
    return {vac1, vac2};
}

} // namespace

HardwareEmulator::HardwareEmulator(LogCallback log_callback,
                                    PacketCallback packet_callback)
    : log_callback_(std::move(log_callback)),
      packet_callback_(std::move(packet_callback)) {
    // Initialize with reasonable default values using SystemStatus
    state_.last_ping = 0;

    // Initialize embedded status messages with default values
    // All messages are optional, only present when hardware is initialized
    state_.system_status.has_charger = true;
    state_.system_status.charger.enabled = false;
    state_.system_status.charger.plugged_in = false;
    state_.system_status.charger.charge_status = ChargeStatus_CHARGE_STATUS_NOT_CHARGING;
    state_.system_status.charger.vbus_status = VBUSStatus_VBUS_STATUS_NO_INPUT;
    state_.system_status.charger.battery_present = false;
    state_.system_status.charger.fault_present = false;
    state_.system_status.charger.input_source = ChargerInputSource_CHARGER_INPUT_AUTO;
    state_.system_status.charger.timestamp_ms = 0;
    state_.system_status.charger.has_status_flags = true;
    state_.system_status.charger.status_flags = (1u << 0) | (1u << 3) | (1u << 8);   // VBUS present, PG, VBAT present
    state_.system_status.charger.has_fault_flags = true;
    state_.system_status.charger.fault_flags = 0;

    state_.system_status.has_charger_measurements = true;
    state_.system_status.charger_measurements.vbat_voltage = 12.0f;
    state_.system_status.charger_measurements.ibus_current = 0.0f;
    state_.system_status.charger_measurements.vbus_voltage = 0.0f;
    state_.system_status.charger_measurements.vsys_voltage = 12.0f;
    state_.system_status.charger_measurements.has_ibat_current = true;
    state_.system_status.charger_measurements.ibat_current = 0.0f;
    state_.system_status.charger_measurements.has_vac1_voltage = true;
    state_.system_status.charger_measurements.vac1_voltage = 0.0f;
    state_.system_status.charger_measurements.has_vac2_voltage = true;
    state_.system_status.charger_measurements.vac2_voltage = 0.0f;

    state_.system_status.has_charger_config = true;
    state_.system_status.charger_config.charge_voltage_limit = 12.6f;
    state_.system_status.charger_config.charge_current_limit = 1.0f;
    state_.system_status.charger_config.input_voltage_limit = 4.4f;
    state_.system_status.charger_config.input_current_limit = 3.0f;
    state_.system_status.charger_config.has_dock_input_current_limit = true;
    state_.system_status.charger_config.dock_input_current_limit = 3.0f;
    state_.system_status.charger_config.has_input_current_cap = true;
    state_.system_status.charger_config.input_current_cap = 3.3f;     // none
    state_.system_status.charger_config.has_input_voltage_floor = true;
    state_.system_status.charger_config.input_voltage_floor = 3.6f;   // none

    state_.system_status.has_output_regulator = true;
    state_.system_status.output_regulator.enabled = false;
    state_.system_status.output_regulator.fault_present = false;
    state_.system_status.output_regulator.scp_fault = false;
    state_.system_status.output_regulator.ocp_fault = false;
    state_.system_status.output_regulator.ovp_fault = false;
    state_.system_status.output_regulator.operating_mode = OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BUCK;
    state_.system_status.output_regulator.feedback_mode = OutputRegulatorFeedbackMode_OUTPUT_REGULATOR_FEEDBACK_MODE_PWM;
    state_.system_status.output_regulator.timestamp_ms = 0;

    state_.system_status.has_output_regulator_measurements = true;
    state_.system_status.output_regulator_measurements.output_voltage = 12.0f;
    state_.system_status.output_regulator_measurements.output_current = 1.5f;

    state_.system_status.has_output_regulator_config = true;
    state_.system_status.output_regulator_config.has_output_voltage = true;
    state_.system_status.output_regulator_config.output_voltage = 12.0f;
    state_.system_status.output_regulator_config.has_output_current_limit = true;
    state_.system_status.output_regulator_config.output_current_limit = 3.0f;
    state_.system_status.output_regulator_config.has_sense_resistor_ohms = true;
    state_.system_status.output_regulator_config.sense_resistor_ohms = 0.01f;

    // The DAC controller was removed from the firmware: no DAC status.

    // USB-C: firmware defaults, a PD 3.0 charger attached on CC1, no dock
    state_.system_status.has_usb_pd_config = true;
    auto& pd_cfg = state_.system_status.usb_pd_config;
    pd_cfg.source_enabled = true;
    pd_cfg.max_sink_voltage = 20.0f;
    pd_cfg.max_sink_current = 3.0f;
    pd_cfg.has_source_min_battery_voltage = true;
    pd_cfg.source_min_battery_voltage = 13.2f;
    state_.system_status.has_usb_pd = true;
    usb_pd_set_partner_locked(true);
    update_power_path();

    mock_applets_.push_back(MockApplets::make_aux_pwm());

    log("HardwareEmulator initialized");
}

HardwareEmulator::~HardwareEmulator() {
    stop();
}

void HardwareEmulator::start() {
    if (running_.load()) {
        return;
    }

    running_.store(true);
    worker_thread_ = std::thread(&HardwareEmulator::update_hardware_state, this);
    log("HardwareEmulator started");
}

void HardwareEmulator::stop() {
    const bool was_running = running_.exchange(false);
    if (!was_running && !worker_thread_.joinable()) {
        return;
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    log("HardwareEmulator stopped");
}

bool HardwareEmulator::is_running() const noexcept {
    return running_.load();
}

HardwareEmulator::State HardwareEmulator::get_state() const {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    return state_;
}

void HardwareEmulator::log(const std::string& msg) {
    if (log_callback_) {
        log_callback_(msg);
    }
}

void HardwareEmulator::log_packet(const std::string& direction, const std::string& msg) {
    std::string timestamp = "[";
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    timestamp += std::ctime(&time_t_now);
    timestamp.pop_back(); // Remove newline
    timestamp += "]";

    std::string log_entry = timestamp + " " + direction + " " + msg;

    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        packet_log_.push_back(log_entry);
        if (packet_log_.size() > 100) {
            packet_log_.erase(packet_log_.begin());
        }
    }

    if (packet_callback_) {
        packet_callback_(log_entry);
    }
}

// Setters
void HardwareEmulator::set_charger_config(const ChargerConfig& config) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    // All fields are now required, directly assign
    state_.system_status.has_charger_config = true;
    state_.system_status.charger_config.charge_voltage_limit = config.charge_voltage_limit;
    state_.system_status.charger_config.charge_current_limit = config.charge_current_limit;
    state_.system_status.charger_config.input_voltage_limit = config.input_voltage_limit;
    state_.system_status.charger_config.input_current_limit = config.input_current_limit;
    log("Charger config updated");
}

void HardwareEmulator::set_backboost_config(const OutputRegulatorConfig& config) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    // All fields are now required, directly assign
    state_.system_status.output_regulator_config.output_voltage = config.output_voltage;
    state_.system_status.output_regulator_config.output_current_limit = config.output_current_limit;
    state_.system_status.output_regulator_config.sense_resistor_ohms = config.sense_resistor_ohms;
    log("Output Regulator config updated");
}

void HardwareEmulator::set_dac_state(const DACState& config) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    // With required fields, all fields are always present
    state_.system_status.dac_state.voltage = config.voltage;
    state_.system_status.dac_state.waveform_type = config.waveform_type;
    state_.system_status.dac_state.frequency = config.frequency;
    state_.system_status.dac_state.amplitude = config.amplitude;
    log("DAC state updated");
}

void HardwareEmulator::set_system_status(const SystemStatus& status) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    state_.system_status = status;
    log("System status updated");
}

void HardwareEmulator::send_request(const Request& req) {
    std::string req_str;

    switch (req.which_command) {
        case Request_ping_tag:
            req_str = "PING";
            break;
        case Request_get_system_status_tag:
            req_str = "GET_SYSTEM_STATUS";
            break;
        case Request_set_charger_config_tag:
            req_str = "SET_CHARGER_CONFIG";
            break;
        case Request_set_output_regulator_config_tag:
            req_str = "SET_OUTPUT_REGULATOR_CONFIG";
            break;
        case Request_set_dac_config_tag:
            req_str = "SET_DAC_CONFIG";
            break;
        case Request_applet_start_tag:
            req_str = "APPLET_START";
            break;
        case Request_applet_stop_tag:
            req_str = "APPLET_STOP";
            break;
        case Request_applet_command_tag:
            req_str = "APPLET_COMMAND";
            break;
        case Request_applet_list_tag:
            req_str = "APPLET_LIST";
            break;
        case Request_set_usb_pd_config_tag:
            req_str = "SET_USB_PD_CONFIG";
            break;
        case Request_usb_pd_renegotiate_tag:
            req_str = "USB_PD_RENEGOTIATE";
            break;
        case Request_power_off_tag:
            req_str = "POWER_OFF";
            break;
        default:
            req_str = "UNKNOWN_COMMAND";
            break;
    }

    log_packet("TX ->", req_str);

    // Process command immediately
    switch (req.which_command) {
        case Request_ping_tag:
            handle_ping(req);
            break;
        case Request_get_system_status_tag:
            handle_get_system_status(req);
            break;
        case Request_set_charger_config_tag:
        case Request_set_usb_pd_config_tag:
        case Request_usb_pd_renegotiate_tag:
            break;   // validated and applied in get_response_for_request (the reply carries the result)
        case Request_set_output_regulator_config_tag:
            handle_set_backboost_config(req);
            break;
        case Request_set_dac_config_tag:
            break;   // unsupported, see get_response_for_request
        case Request_applet_start_tag:
        case Request_applet_stop_tag:
        case Request_applet_command_tag:
        case Request_applet_list_tag:
        case Request_power_off_tag:
            // Handled in response generation
            break;
        default:
            log("Unknown command received");
            break;
    }
}

std::vector<std::byte> HardwareEmulator::receive_response() {
    // Simulate response delay (10-50ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(10 + (std::rand() % 41)));

    // In a real implementation, this would return from a response queue
    // For now, we'll just return empty and rely on callbacks
    return {};
}

std::vector<std::string> HardwareEmulator::get_packet_log() const {
    std::lock_guard<std::mutex> lock(log_mutex_);
    return packet_log_;
}

// =============================================================================
// Command Handlers
// =============================================================================

void HardwareEmulator::handle_ping(const Request&) {
    // Pong is sent back by the Connector class
    log("Emulator: Processing PING -> PONG");
}

void HardwareEmulator::handle_get_system_status(const Request&) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    update_power_path();   // the source gate follows VBAT

    // Simulate some state changes for realism
    if (state_.system_status.charger.enabled) {
        // Randomly toggle fault status
        if (std::rand() % 1000 == 0) {
            state_.system_status.charger.fault_present = !state_.system_status.charger.fault_present;
            state_.system_status.has_charger = true;
            log("Emulator: Fault status changed");
        }
    }

    // Simulate dynamic operating mode based on load
    if (state_.system_status.output_regulator.enabled) {
        if (state_.system_status.output_regulator_measurements.output_current > 5.0f) {
            state_.system_status.output_regulator.operating_mode =
                OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BOOST;
        } else {
            state_.system_status.output_regulator.operating_mode =
                OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BUCK;
        }
    }
}

std::expected<void, ControllerError> HardwareEmulator::handle_set_charger_config(const Request& req) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    const auto& c = req.command.set_charger_config;
    // Same ranges as Charger::set_config; everything is checked before anything changes.
    const bool valid = (!c.has_charge_voltage_limit || in_range(c.charge_voltage_limit, 3.0f, 18.8f)) &&
                       (!c.has_charge_current_limit || in_range(c.charge_current_limit, 0.05f, 5.0f)) &&
                       (!c.has_input_voltage_limit || in_range(c.input_voltage_limit, 3.6f, 22.0f)) &&
                       (!c.has_input_current_limit || in_range(c.input_current_limit, 0.1f, 3.3f)) &&
                       (!c.has_dock_input_current_limit || in_range(c.dock_input_current_limit, 0.1f, 3.3f)) &&
                       (!c.has_input_source || c.input_source == ChargerInputSource_CHARGER_INPUT_AC1 ||
                        c.input_source == ChargerInputSource_CHARGER_INPUT_AC2 ||
                        c.input_source == ChargerInputSource_CHARGER_INPUT_AUTO);
    if (!valid) {
        return std::unexpected(ControllerError::BQ25792_InvalidParameter);
    }
    auto& cfg = state_.system_status.charger_config;
    state_.system_status.has_charger_config = true;
    if (c.has_charge_voltage_limit) cfg.charge_voltage_limit = c.charge_voltage_limit;
    if (c.has_charge_current_limit) cfg.charge_current_limit = c.charge_current_limit;
    // The host's input settings are a floor / cap / mode for the input policy (docs/usb_pd.md)
    if (c.has_input_voltage_limit) {
        cfg.has_input_voltage_floor = true;
        cfg.input_voltage_floor = c.input_voltage_limit;
    }
    if (c.has_input_current_limit) {
        cfg.has_input_current_cap = true;
        cfg.input_current_cap = c.input_current_limit;
    }
    if (c.has_dock_input_current_limit) {
        cfg.has_dock_input_current_limit = true;
        cfg.dock_input_current_limit = c.dock_input_current_limit;
    }
    if (c.has_input_source) power_.input_mode = c.input_source;
    if (c.has_enabled) {
        state_.system_status.charger.enabled = c.enabled;
        // Charging from a weak adapter: input current regulation (IINDPM) active
        auto& flags = state_.system_status.charger.status_flags;
        flags = c.enabled ? (flags | (1u << 7)) : (flags & ~(1u << 7));
    }
    update_power_path();
    log("Emulator: charger config updated");
    return {};
}

std::expected<void, ControllerError> HardwareEmulator::handle_set_usb_pd_config(const Request& req) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    const auto& c = req.command.set_usb_pd_config;
    if ((c.has_max_sink_voltage && !in_range(c.max_sink_voltage, 5.0f, 20.0f)) ||
        (c.has_max_sink_current && !in_range(c.max_sink_current, 0.5f, 3.0f)) ||
        (c.has_source_min_battery_voltage && !in_range(c.source_min_battery_voltage, 9.0f, 17.0f))) {
        return std::unexpected(ControllerError::USBPD_InvalidParameter);
    }
    auto& cfg = state_.system_status.usb_pd_config;
    const bool sink_changed = (c.has_max_sink_voltage && c.max_sink_voltage != cfg.max_sink_voltage) ||
                              (c.has_max_sink_current && c.max_sink_current != cfg.max_sink_current);
    if (c.has_source_enabled) cfg.source_enabled = c.source_enabled;
    if (c.has_max_sink_voltage) cfg.max_sink_voltage = c.max_sink_voltage;
    if (c.has_max_sink_current) cfg.max_sink_current = c.max_sink_current;
    if (c.has_source_min_battery_voltage) {
        cfg.has_source_min_battery_voltage = true;
        cfg.source_min_battery_voltage = c.source_min_battery_voltage;
    }
    const auto& p = state_.system_status.usb_pd;
    if (sink_changed && p.power_role == UsbPdPowerRole_USB_PD_POWER_ROLE_SINK) {
        usb_pd_select_contract();   // a changed sink limit renegotiates
    }
    update_power_path();
    log("Emulator: USB-PD config updated");
    return {};
}

std::expected<void, ControllerError> HardwareEmulator::handle_usb_pd_renegotiate(const Request& req) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    auto& p = state_.system_status.usb_pd;
    if (p.state == UsbPdState_USB_PD_STATE_DETACHED || p.power_role != UsbPdPowerRole_USB_PD_POWER_ROLE_SINK) {
        return std::unexpected(ControllerError::USBPD_NotAttached);
    }
    if (req.command.usb_pd_renegotiate.has_hard_reset && req.command.usb_pd_renegotiate.hard_reset) {
        p.has_hard_resets = true;
        ++p.hard_resets;   // the source cycles VBUS and sends its capabilities again
    }
    usb_pd_select_contract();
    update_power_path();
    log("Emulator: USB-PD renegotiated");
    return {};
}

// =============================================================================
// Emulated USB-C port and input power path
// =============================================================================

void HardwareEmulator::set_dock_present(bool present) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    power_.dock_present = present;
    update_power_path();
}

void HardwareEmulator::set_usb_pd_partner(bool attached) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    usb_pd_set_partner_locked(attached);
    update_power_path();
}

void HardwareEmulator::usb_pd_set_partner_locked(bool attached) {
    auto& p = state_.system_status.usb_pd;
    const uint32_t hard_resets = p.hard_resets;
    p = UsbPdStatus_init_zero;
    p.has_hard_resets = true;
    p.hard_resets = hard_resets;
    p.has_dead_battery_rd = true;
    p.dead_battery_rd = false;   // firmware runs: PD0 high
    if (!attached) {
        p.state = UsbPdState_USB_PD_STATE_DETACHED;
        p.power_role = UsbPdPowerRole_USB_PD_POWER_ROLE_NONE;
        return;
    }
    p.state = UsbPdState_USB_PD_STATE_ATTACHED;
    p.power_role = UsbPdPowerRole_USB_PD_POWER_ROLE_SINK;
    p.has_data_role = true;
    p.data_role = UsbPdDataRole_USB_PD_DATA_ROLE_UFP;
    p.has_cc_line = true;
    p.cc_line = 1;
    p.has_typec_current = true;
    p.typec_current = TypeCCurrent_TYPEC_CURRENT_3A0;
    p.has_spec_revision = true;
    p.spec_revision = 3;
    p.has_capability_mismatch = true;
    p.capability_mismatch = false;
    p.source_pdos_count = std::size(kPartnerPdos);
    std::copy(std::begin(kPartnerPdos), std::end(kPartnerPdos), p.source_pdos);
    usb_pd_select_contract();
}

// Sink selection (docs/usb_pd.md): fixed PDOs up to max_sink_voltage; the highest
// V x min(Imax, max_sink_current) wins, ties go to the higher voltage. Integer mV / mA so
// equal powers compare equal.
void HardwareEmulator::usb_pd_select_contract() {
    auto& p = state_.system_status.usb_pd;
    const auto& cfg = state_.system_status.usb_pd_config;
    const auto vmax_mv = static_cast<uint32_t>(std::lround(cfg.max_sink_voltage * 1000.0f));
    const auto imax_ma = static_cast<uint32_t>(std::lround(cfg.max_sink_current * 1000.0f));
    uint32_t best = 0, best_mv = 0, best_ma = 0;
    uint64_t best_power = 0;
    for (pb_size_t i = 0; i < p.source_pdos_count; ++i) {
        const uint32_t raw = p.source_pdos[i];
        if ((raw >> 30) != 0) {
            continue;   // PPS / APDOs are ignored for now
        }
        const uint32_t mv = ((raw >> 10) & 0x3FFu) * 50u;
        const uint32_t ma = std::min((raw & 0x3FFu) * 10u, imax_ma);
        const uint64_t power = static_cast<uint64_t>(mv) * ma;
        if (mv > vmax_mv) {
            continue;
        }
        if (best == 0 || power > best_power || (power == best_power && mv > best_mv)) {
            best = i + 1;
            best_mv = mv;
            best_ma = ma;
            best_power = power;
        }
    }
    if (best == 0) {   // no usable PDO: Type-C current only
        p.state = UsbPdState_USB_PD_STATE_ATTACHED;
        p.has_contract_voltage = p.has_contract_current = p.has_selected_pdo = false;
        return;
    }
    p.state = UsbPdState_USB_PD_STATE_CONTRACT;
    p.has_contract_voltage = true;
    p.contract_voltage = best_mv / 1000.0f;
    p.has_contract_current = true;
    p.contract_current = best_ma / 1000.0f;
    p.has_selected_pdo = true;
    p.selected_pdo = best;
}

// What PowerPath does every 20 ms on the device: source gate, active input, IINDPM / VINDPM
// (with the host's cap and floor), and the REG1B input bits the host decodes.
void HardwareEmulator::update_power_path() {
    auto& ss = state_.system_status;
    auto& p = ss.usb_pd;
    auto& cfg = ss.charger_config;
    const bool usb_in = ss.has_usb_pd && p.state != UsbPdState_USB_PD_STATE_DETACHED &&
                        p.power_role == UsbPdPowerRole_USB_PD_POWER_ROLE_SINK;
    const bool dock = power_.dock_present;

    if (ss.has_usb_pd) {
        const auto& pd_cfg = ss.usb_pd_config;
        const float vbat_min = pd_cfg.has_source_min_battery_voltage ? pd_cfg.source_min_battery_voltage : 13.2f;
        // Same rules as PowerPath: dock -> VOUT bridge if VOUT is on at 5 V / >= 3 A, else OTG.
        const auto& vreg = ss.output_regulator;
        const auto& vcfg = ss.output_regulator_config;
        const bool vout_fits = vreg.enabled && !vreg.fault_present && vcfg.output_voltage >= 4.75f &&
                               vcfg.output_voltage <= 5.25f && vcfg.output_current_limit >= 2.95f;
        UsbPdSourcePath path = UsbPdSourcePath_USB_PD_SOURCE_PATH_NONE;
        if (!pd_cfg.source_enabled || power_.off_state != PowerOffState_POWER_OFF_IDLE) {
            p.source_state = UsbPdSourceState_USB_PD_SOURCE_DISABLED;   // power off: no USB-C source
        } else if (dock && power_.input_mode == ChargerInputSource_CHARGER_INPUT_AC1) {
            p.source_state = UsbPdSourceState_USB_PD_SOURCE_BLOCKED_DOCK;
        } else if (dock && !vout_fits) {
            p.source_state = UsbPdSourceState_USB_PD_SOURCE_BLOCKED_VOUT;
        } else if (!dock && (!ss.charger.battery_present || ss.charger_measurements.vbat_voltage < vbat_min)) {
            p.source_state = UsbPdSourceState_USB_PD_SOURCE_BLOCKED_BATTERY;
        } else {
            path = dock ? UsbPdSourcePath_USB_PD_SOURCE_PATH_VOUT : UsbPdSourcePath_USB_PD_SOURCE_PATH_OTG;
            p.source_state = p.power_role == UsbPdPowerRole_USB_PD_POWER_ROLE_SOURCE
                                 ? UsbPdSourceState_USB_PD_SOURCE_ACTIVE
                                 : UsbPdSourceState_USB_PD_SOURCE_READY;
        }
        p.has_source_path = true;
        p.source_path = path;
        // USB-C input limit: the contract, or the partner's Rp current without one
        p.has_input_current_limit = usb_in;
        p.input_current_limit = !usb_in ? 0.0f
                                : p.state == UsbPdState_USB_PD_STATE_CONTRACT && p.has_contract_current
                                    ? p.contract_current
                                    : typec_current_a(p.typec_current);
        p.has_vbus_voltage = true;
        p.vbus_voltage = input_voltages(ss).first;
    }

    // Active input: forced by the host, or AUTO = dock first. The emulator swaps to the dock
    // without the firmware's battery check (VBAT >= 12.5 V).
    ChargerInputSource active = power_.input_mode;
    if (active != ChargerInputSource_CHARGER_INPUT_AC1 && active != ChargerInputSource_CHARGER_INPUT_AC2) {
        active = dock ? ChargerInputSource_CHARGER_INPUT_AC2 : ChargerInputSource_CHARGER_INPUT_AC1;
    }
    float iindpm = 0.5f, vindpm = 3.6f;
    bool powered = false;
    if (active == ChargerInputSource_CHARGER_INPUT_AC2 && dock) {
        iindpm = cfg.dock_input_current_limit;
        vindpm = 0.85f * kDockVoltage;
        powered = true;
    } else if (active == ChargerInputSource_CHARGER_INPUT_AC1 && usb_in) {
        iindpm = p.input_current_limit;
        vindpm = p.state == UsbPdState_USB_PD_STATE_CONTRACT ? 0.85f * p.contract_voltage : 4.25f;
        powered = true;
    }
    const float cap = cfg.has_input_current_cap ? cfg.input_current_cap : 3.3f;
    const float floor = cfg.has_input_voltage_floor ? cfg.input_voltage_floor : 3.6f;
    cfg.input_current_limit = std::min(iindpm, cap);
    cfg.input_voltage_limit = std::clamp(std::max(vindpm, floor), 3.6f, 22.0f);

    // REG1B bits 0..3: VBUS_PRESENT, AC1_PRESENT, AC2_PRESENT, PG
    const bool ac1 = usb_in || (ss.has_usb_pd && p.source_state == UsbPdSourceState_USB_PD_SOURCE_ACTIVE);
    uint32_t flags = ss.charger.status_flags & ~0xFu;
    if (ac1 || dock) flags |= 1u << 0;
    if (ac1) flags |= 1u << 1;
    if (dock) flags |= 1u << 2;
    if (powered) flags |= 1u << 3;
    ss.charger.has_status_flags = true;
    ss.charger.status_flags = flags;
    ss.charger.input_source = active;
    ss.charger.plugged_in = usb_in || dock;
    cfg.has_input_source_mode = true;
    cfg.input_source_mode = power_.input_mode;
    step_power_off_locked();
}

// =============================================================================
// Power off (as PowerPath on the device: wait for USB-C and the dock to be unplugged,
// drain VBUS, then ship mode with the charger's 10 s delay)
// =============================================================================

namespace {
constexpr auto kOffDrain = std::chrono::milliseconds(300);
constexpr auto kOffShipDelay = std::chrono::seconds(10);
}

void HardwareEmulator::handle_power_off(const PowerOffRequest& req) {
    // Lock order as the applet requests: applets_mutex_, then state_mutex_
    std::lock_guard<std::mutex> applets(applets_mutex_);
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    auto& ss = state_.system_status;
    if (req.action != PowerOffAction_POWER_OFF_CANCEL) {
        // Soft stop: applets and VOUT
        for (auto& a : mock_applets_) {
            a->stop();
            state_.applets[a->name()].running = false;
        }
        ss.output_regulator.enabled = false;
        log("Emulator: soft stop (applets stopped, VOUT off)");
    }
    if (req.action == PowerOffAction_POWER_OFF_SHIP) {
        if (power_.off_state == PowerOffState_POWER_OFF_IDLE || power_.off_state == PowerOffState_POWER_OFF_FAILED) {
            power_.off_state = PowerOffState_POWER_OFF_WAIT_SOURCES;
            power_.off_logged = false;
            log("Emulator: power off, waiting for USB-C and the dock to be unplugged");
        }
    } else if (req.action == PowerOffAction_POWER_OFF_CANCEL) {
        power_.off_state = PowerOffState_POWER_OFF_IDLE;
        log("Emulator: power off cancelled");
    }
    update_power_path();
}

void HardwareEmulator::step_power_off_locked() {
    auto& ss = state_.system_status;
    const auto now = std::chrono::steady_clock::now();
    const bool usb_in = ss.has_usb_pd && ss.usb_pd.state != UsbPdState_USB_PD_STATE_DETACHED &&
                        ss.usb_pd.power_role == UsbPdPowerRole_USB_PD_POWER_ROLE_SINK;
    const bool dock = power_.dock_present;
    const bool sources = usb_in || dock;
    auto& st = power_.off_state;
    switch (st) {
        case PowerOffState_POWER_OFF_WAIT_SOURCES:
            if (!sources) {
                st = PowerOffState_POWER_OFF_DRAIN_VBUS;
                power_.off_since = now;
            }
            break;
        case PowerOffState_POWER_OFF_DRAIN_VBUS:
            if (sources) {
                st = PowerOffState_POWER_OFF_WAIT_SOURCES;
            } else if (now - power_.off_since >= kOffDrain) {
                st = PowerOffState_POWER_OFF_PENDING;
                power_.off_since = now + kOffShipDelay;
                log("Emulator: ship mode accepted, battery disconnects in 10 s");
            }
            break;
        case PowerOffState_POWER_OFF_PENDING:
            if (sources) {
                st = PowerOffState_POWER_OFF_WAIT_SOURCES;
                log("Emulator: power plugged in, the charger left ship mode");
            } else if (now >= power_.off_since && !power_.off_logged) {
                power_.off_logged = true;
                log("Emulator: battery disconnected (a real board is off now)");
            }
            break;
        default:
            break;
    }
    ss.has_power_off = st != PowerOffState_POWER_OFF_IDLE;
    if (ss.has_power_off) {
        auto& p = ss.power_off;
        p = PowerOffStatus_init_zero;
        p.state = st;
        p.usb_c_present = usb_in;
        p.dock_present = dock;
        p.vbus_present = sources;
        p.battery_present = ss.charger.battery_present;
        if (st == PowerOffState_POWER_OFF_PENDING) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(power_.off_since - now).count();
            p.has_off_in_ms = true;
            p.off_in_ms = left > 0 ? static_cast<uint32_t>(left) : 0;
        }
    }
}

void HardwareEmulator::handle_set_backboost_config(const Request& req) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    const auto& c = req.command.set_output_regulator_config;
    auto& cfg = state_.system_status.output_regulator_config;
    state_.system_status.has_output_regulator_config = true;
    if (c.has_output_voltage) {
        cfg.has_output_voltage = true;
        cfg.output_voltage = c.output_voltage;
        state_.system_status.output_regulator_measurements.output_voltage = c.output_voltage;
    }
    if (c.has_output_current_limit) {
        cfg.has_output_current_limit = true;
        cfg.output_current_limit = c.output_current_limit;
    }
    if (c.has_sense_resistor_ohms && c.sense_resistor_ohms > 0.0f) {
        cfg.has_sense_resistor_ohms = true;
        cfg.sense_resistor_ohms = c.sense_resistor_ohms;
    }
    if (c.has_enabled) state_.system_status.output_regulator.enabled = c.enabled;
    log("Emulator: output regulator config updated");
}

void HardwareEmulator::update_hardware_state() {
    while (running_.load()) {
        SystemStatus status;
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            status = state_.system_status;
        }

        // Simulate dynamic voltage behavior (without holding lock)
        static std::random_device rd;
        static std::mt19937 gen(42); // Fixed seed for reproducibility

        // Voltage noise distribution (small random variations)
        static std::uniform_real_distribution<> noise_dis(-0.02f, 0.02f);
        // Slow drift distribution
        static std::uniform_real_distribution<> drift_dis(-0.005f, 0.005f);

        // Pre-compute values outside the lock
        float new_vbat = status.charger_measurements.vbat_voltage;
        float new_vbus = status.charger_measurements.vbus_voltage;
        float new_vout = status.output_regulator_measurements.output_voltage;
        float new_ibus = status.charger_measurements.ibus_current;
        float new_iout = status.output_regulator_measurements.output_current;

        if (status.charger.enabled) {
            // VBAT: slowly drifts and has small noise
            static float vbat_drift = 0.0f;
            vbat_drift += drift_dis(gen);
            new_vbat += vbat_drift + noise_dis(gen);
            // Clamp to reasonable range (10-14V for a battery)
            if (new_vbat < 10.0f) {
                new_vbat = 10.0f;
                vbat_drift = 0.0f;
            }
            if (new_vbat > 14.0f) {
                new_vbat = 14.0f;
                vbat_drift = 0.0f;
            }

            // IBUS: follows load, adds some ripple
            static float ibus_ripple = 0.0f;
            ibus_ripple = 0.9f * ibus_ripple + 0.1f * noise_dis(gen) * 0.5f;
            new_ibus += ibus_ripple;
            // Clamp to reasonable range
            if (new_ibus < 0.0f) {
                new_ibus = 0.0f;
            }
            if (new_ibus > 10.0f) {
                new_ibus = 10.0f;
            }
        }

        if (status.output_regulator.enabled) {
            // VOUT: follows configuration with some ripple and load simulation
            static float vout_ripple = 0.0f;
            vout_ripple = 0.95f * vout_ripple + 0.05f * noise_dis(gen) * 0.1f;

            float target_voltage = status.output_regulator_config.output_voltage;
            new_vout = target_voltage + vout_ripple;

            // Output current follows load simulation
            static float load_change_timer = 0.0f;
            load_change_timer += 0.01f;
            if (load_change_timer > 5.0f) {
                // Occasionally change the load
                static std::uniform_real_distribution<> load_dis(0.5f, 3.0f);
                new_iout = load_dis(gen);
                load_change_timer = 0.0f;
            }
        }

        // Input voltages: small common ripple on the nominal VAC1 / VAC2
        static float vin_noise = 0.0f;
        vin_noise = 0.9f * vin_noise + 0.1f * noise_dis(gen);

        // Now acquire lock only to update the actual state
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Update at 20Hz
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            auto& ss = state_.system_status;
            // Nominal inputs from the current state, not the copy: a request may have changed them
            const auto [vac1, vac2] = input_voltages(ss);
            const float new_vac1 = vac1 > 0.0f ? vac1 + vin_noise : 0.0f;
            const float new_vac2 = vac2 > 0.0f ? vac2 + vin_noise : 0.0f;
            ss.charger_measurements.vac1_voltage = new_vac1;
            ss.charger_measurements.vac2_voltage = new_vac2;
            if (ss.has_usb_pd) {
                ss.usb_pd.vbus_voltage = new_vac1;
            }
            // VBUS is the input the BQ25792 has selected
            new_vbus = !(ss.charger.status_flags & (1u << 3))                          ? 0.0f
                       : ss.charger.input_source == ChargerInputSource_CHARGER_INPUT_AC2 ? new_vac2
                                                                                         : new_vac1;
            state_.system_status.charger_measurements.vbat_voltage = new_vbat;
            state_.system_status.charger_measurements.vbus_voltage = new_vbus;
            state_.system_status.output_regulator_measurements.output_voltage = new_vout;
            state_.system_status.charger_measurements.ibus_current = new_ibus;
            state_.system_status.output_regulator_measurements.output_current = new_iout;
            step_power_off_locked();
        }
    }
}

// =============================================================================
// Helper functions for message encoding/decoding
// =============================================================================

void HardwareEmulator::append_frame(std::vector<std::byte>& out, frame::Type type, uint8_t flags, uint16_t seq,
                                    const uint8_t* payload, size_t len) {
    std::vector<uint8_t> buffer(frame::kMaxFrame);
    const size_t n = frame::encode(buffer.data(), buffer.size(), type, flags, seq, payload, len);
    const auto* p = reinterpret_cast<const std::byte*>(buffer.data());
    out.insert(out.end(), p, p + n);
}

void HardwareEmulator::append_response(std::vector<std::byte>& out, frame::Type type, uint16_t seq,
                                       const Response& resp) {
    std::vector<uint8_t> payload(frame::kMaxPayload);
    pb_ostream_t stream = pb_ostream_from_buffer(payload.data(), payload.size());
    if (!pb_encode(&stream, Response_fields, &resp)) {
        log("Failed to encode response: " + std::string(PB_GET_ERROR(&stream)));
        return;
    }
    log_packet("EMULATOR -> TX", "Encoded response: " + std::to_string(stream.bytes_written) + " bytes");
    append_frame(out, type, 0, seq, payload.data(), stream.bytes_written);
}

std::vector<std::byte> HardwareEmulator::encode_response(const Response& resp, uint16_t seq) {
    std::vector<std::byte> out;
    append_response(out, frame::Type::Response, seq, resp);
    return out;
}

// =============================================================================
// Emulated oscilloscope
// =============================================================================

void HardwareEmulator::scope_drop_frames_once(std::vector<uint32_t> frame_indices) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    scope_.drop_once = std::move(frame_indices);
}

void HardwareEmulator::scope_capture(ScopeTriggerKind source) {
    // Phase-shifted sines at rate/200 so a capture shows a few periods; deterministic.
    const size_t total = scope_.pre + scope_.post;
    scope_.data.assign(scope_.channels.size(), std::vector<uint16_t>(total));
    for (size_t c = 0; c < scope_.channels.size(); ++c) {
        for (size_t i = 0; i < total; ++i) {
            const double t = (static_cast<double>(i) - scope_.pre) / 200.0;
            const double v = 2048.0 + 1500.0 * std::sin(2.0 * M_PI * t + c * M_PI / 2.0);
            scope_.data[c][i] = static_cast<uint16_t>(v);
        }
    }
    scope_.trigger_source = source;
    scope_.state = ScopeState_SCOPE_STATE_DONE;
}

std::vector<std::byte> HardwareEmulator::handle_scope(const Request& req, uint16_t seq) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    static Response resp;
    resp = Response_init_zero;
    resp.has_result = true;
    resp.result.status = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
    auto fail = [](ResponseStatusCode status, ControllerError error) {
        resp.which_payload = 0;
        resp.result.status = status;
        resp.result.has_error = true;
        resp.result.error = static_cast<ControllerErrorCode>(error);
    };
    auto fill_status = [this] {
        resp.which_payload = Response_scope_status_tag;
        ScopeStatus& st = resp.payload.scope_status;
        st.state = scope_.state;
        if (scope_.state == ScopeState_SCOPE_STATE_DONE) {
            st.has_num_channels = true;
            st.num_channels = static_cast<uint32_t>(scope_.channels.size());
            st.has_sample_rate_hz = true;
            st.sample_rate_hz = scope_.sample_rate_hz;
            st.has_trigger_source = true;
            st.trigger_source = scope_.trigger_source;
            st.has_first_sample = true;
            st.first_sample = -static_cast<int64_t>(scope_.pre);
            st.has_post_trigger_samples = true;
            st.post_trigger_samples = scope_.post;
        }
    };

    std::vector<std::byte> out;
    switch (req.which_command) {
        case Request_scope_configure_tag: {
            const auto& c = req.command.scope_configure;
            if (c.channels_count == 0 || c.sample_rate_hz == 0 || c.post_trigger_samples == 0) {
                fail(ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS, ControllerError::OscilloscopeInvalidChannel);
                break;
            }
            scope_.channels.assign(c.channels, c.channels + c.channels_count);
            scope_.sample_rate_hz = c.sample_rate_hz;
            scope_.pre = c.pre_trigger_samples;
            scope_.post = c.post_trigger_samples;
            scope_.software_trigger = !c.has_trigger || !c.trigger.has_software || c.trigger.software;   // proto default: true
            scope_.configured = true;
            scope_.state = ScopeState_SCOPE_STATE_IDLE;
            scope_.data.clear();
            fill_status();
            break;
        }
        case Request_scope_arm_tag:
            if (!scope_.configured) {
                fail(ResponseStatusCode_RESPONSE_STATUS_INVALID_STATE, ControllerError::OscilloscopeNotConfigured);
                break;
            }
            scope_.state = ScopeState_SCOPE_STATE_PRETRIGGER;
            if (!scope_.software_trigger) {
                scope_capture(ScopeTriggerKind_SCOPE_TRIGGER_ANALOG_WATCHDOG);   // hardware trigger fires at once
            }
            fill_status();
            break;
        case Request_scope_force_trigger_tag:
            if (scope_.state != ScopeState_SCOPE_STATE_PRETRIGGER) {
                fail(ResponseStatusCode_RESPONSE_STATUS_INVALID_STATE, ControllerError::OscilloscopeNotConfigured);
                break;
            }
            scope_capture(ScopeTriggerKind_SCOPE_TRIGGER_SOFTWARE);
            fill_status();
            break;
        case Request_scope_stop_tag:
            if (scope_.state != ScopeState_SCOPE_STATE_DONE) {
                scope_.state = ScopeState_SCOPE_STATE_IDLE;
            }
            fill_status();
            break;
        case Request_scope_get_status_tag:
            fill_status();
            break;
        case Request_scope_read_cancel_tag:
            break;
        case Request_data_link_configure_tag:
            if (req.command.data_link_configure.baudrate == 0) {
                fail(ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS, ControllerError::HAL_ERROR);
                break;
            }
            scope_.aux_baudrate = req.command.data_link_configure.baudrate;
            resp.which_payload = Response_data_link_status_tag;
            resp.payload.data_link_status.baudrate = scope_.aux_baudrate;
            break;
        case Request_scope_read_tag: {
            const auto& r = req.command.scope_read;
            if (scope_.state != ScopeState_SCOPE_STATE_DONE) {
                fail(ResponseStatusCode_RESPONSE_STATUS_INVALID_STATE, ControllerError::OscilloscopeNotConfigured);
                break;
            }
            const uint32_t all = (1u << scope_.channels.size()) - 1u;
            if (r.channel_mask == 0 || (r.channel_mask & ~all) != 0) {
                fail(ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS, ControllerError::OscilloscopeInvalidChannel);
                break;
            }
            const int64_t first = static_cast<int64_t>(scope_.pre) + r.start;
            if (r.count == 0 || first < 0 || first + r.count > static_cast<int64_t>(scope_.pre + scope_.post)) {
                fail(ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS, ControllerError::OscilloscopeOutOfRange);
                break;
            }
            const uint32_t blocks = (r.count + scope_data::kSamplesPerFrame - 1) / scope_data::kSamplesPerFrame;
            resp.which_payload = Response_scope_read_started_tag;
            auto& started = resp.payload.scope_read_started;
            started.transfer_id = r.transfer_id;
            started.frames = blocks * static_cast<uint32_t>(std::popcount(r.channel_mask));
            started.samples_per_frame = scope_data::kSamplesPerFrame;
            started.link = r.has_link ? r.link : DataLink_DATA_LINK_ORIGIN;   // emulator: one stream
            append_response(out, frame::Type::Response, seq, resp);

            // DATA frames, block by block, one frame per requested channel (firmware order)
            std::vector<uint8_t> payload(frame::kMaxPayload);
            uint32_t index = 0;
            uint32_t sent = 0;
            for (uint32_t b = 0; b < blocks; ++b) {
                const uint32_t offset = b * scope_data::kSamplesPerFrame;
                const uint32_t n = std::min<uint32_t>(scope_data::kSamplesPerFrame, r.count - offset);
                for (uint32_t ch = 0; ch < scope_.channels.size(); ++ch) {
                    if (!(r.channel_mask & (1u << ch))) {
                        continue;
                    }
                    const uint32_t this_index = index++;
                    if (std::find(scope_.drop_once.begin(), scope_.drop_once.end(), this_index) != scope_.drop_once.end()) {
                        continue;   // simulated loss
                    }
                    scope_data::write_header(payload.data(), {static_cast<uint16_t>(r.transfer_id),
                                                              static_cast<uint8_t>(ch), offset,
                                                              static_cast<uint16_t>(n)});
                    for (uint32_t i = 0; i < n; ++i) {
                        frame::put_u16(payload.data() + scope_data::kHeaderSize + 2 * i,
                                       scope_.data[ch][first + offset + i]);
                    }
                    const uint8_t flags = (b + 1 == blocks) ? scope_data::kFlagLastOfChannel : 0;
                    append_frame(out, frame::Type::Data, flags, static_cast<uint16_t>(this_index), payload.data(),
                                 scope_data::kHeaderSize + 2 * n);
                    ++sent;
                }
            }
            scope_.drop_once.clear();

            resp = Response_init_zero;
            resp.which_payload = Response_scope_read_complete_tag;
            resp.payload.scope_read_complete.transfer_id = r.transfer_id;
            resp.payload.scope_read_complete.frames_sent = sent;
            append_response(out, frame::Type::Event, 0, resp);
            return out;
        }
        default:
            break;
    }
    append_response(out, frame::Type::Response, seq, resp);
    return out;
}

// =============================================================================
// Mock Applet Implementation
// =============================================================================

namespace MockApplets {

namespace {

constexpr ParamDesc kAuxPwmParams[] = {   // as applet/src/aux_pwm_applet.cc
    {"freq", "Frequency", "Hz", ParamType::Number, 1.0f, 1000000.0f, 1.0f, 0, {}, kParamLive | kParamPersist},
    {"duty1", "Duty CH1", "%", ParamType::Number, 0.0f, 100.0f, 0.1f, 1, {}, kParamLive | kParamPersist},
    {"duty2", "Duty CH2", "%", ParamType::Number, 0.0f, 100.0f, 0.1f, 1, {}, kParamLive | kParamPersist},
};
constexpr ReadoutDesc kAuxPwmReadouts[] = {{"Period", "us", 2}};

bool parse_float(const char* text, float& out) {
    char* end = nullptr;
    out = std::strtof(text, &end);
    return end != text && *end == '\0';
}

class AuxPwmMock final : public MockApplet {
public:
    const char* name() const noexcept override { return "aux-pwm"; }
    const char* summary() const noexcept override { return "PWM on CH1/CH2 (emulated)"; }
    std::span<const ParamDesc> params() const noexcept override { return kAuxPwmParams; }
    float get_param(size_t i) const noexcept override { return i < values_.size() ? values_[i] : 0.0f; }
    AppletResult set_param(size_t i, float v) noexcept override {
        if (i >= values_.size()) return std::unexpected(AppletError::NotFound);
        values_[i] = v;
        return {};
    }
    std::span<const ReadoutDesc> readouts() const noexcept override { return kAuxPwmReadouts; }
    float readout(size_t) const noexcept override { return 1e6f / values_[0]; }
    AppletRun run_state() const noexcept override { return running_ ? AppletRun::Running : AppletRun::Stopped; }
    AppletResult start() noexcept override {
        running_ = true;
        return {};
    }
    void stop() noexcept override { running_ = false; }
    bool needs_arming() const noexcept override { return false; }

    // aux-pwm <channel 1|2> <freq_hz> <duty 0-100>
    bool configure(int argc, char** argv) noexcept override {
        float freq = 0.0f, duty = 0.0f;
        const std::string ch = argc > 1 ? argv[1] : "";
        if (argc != 4 || (ch != "1" && ch != "2") || !parse_float(argv[2], freq) || !parse_float(argv[3], duty)) {
            return false;
        }
        return set_checked(0, freq) && set_checked(ch == "1" ? 1 : 2, duty);
    }

    // set_freq <hz> | set_duty <ch> <duty> | enable | disable | status | help
    bool command(int argc, char** argv) noexcept override {
        const std::string cmd = argc > 1 ? argv[1] : "";
        float v = 0.0f;
        if (cmd == "set_freq" && argc == 3 && parse_float(argv[2], v)) return set_checked(0, v);
        if (cmd == "set_duty" && argc == 4 && parse_float(argv[3], v)) {
            const std::string ch = argv[2];
            return (ch == "1" || ch == "2") && set_checked(ch == "1" ? 1 : 2, v);
        }
        if (cmd == "enable") return start().has_value();
        if (cmd == "disable") {
            stop();
            return true;
        }
        return cmd == "status" || cmd == "help";
    }

private:
    bool set_checked(size_t i, float v) {
        const auto checked = check_param(kAuxPwmParams[i], v);
        if (!checked) return false;
        values_[i] = *checked;
        return true;
    }

    std::array<float, 3> values_{1000.0f, 50.0f, 50.0f};
    bool running_ = false;
};

} // namespace

std::unique_ptr<MockApplet> make_aux_pwm() { return std::make_unique<AuxPwmMock>(); }

// As run_applet_param_command() in the firmware: parse, apply a set (the only action), then
// write the reply. False when argv[1] is not a parameter command.
bool run_param_command(MockApplet& applet, int argc, char** argv, AppletStatus& st) {
    const auto cmd = AppletParamCommand::parse(applet, static_cast<size_t>(argc - 1), argv + 1);
    if (!cmd && cmd.error().kind == AppletParamError::Kind::NotParamCommand) {
        return false;
    }
    AppletParamText reply(st.text, sizeof(st.text));
    if (!cmd) {
        st.status_code = cmd.error().kind == AppletParamError::Kind::UnknownParam
                             ? ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND
                             : ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS;
        reply.write_parse_error(cmd.error());
        return true;
    }
    if (cmd->verb == AppletParamCommand::Verb::Set) {
        // AppletRunner::set_param: range check, then the applet
        const auto param = static_cast<size_t>(cmd->param);
        const auto checked = check_param(applet.params()[param], cmd->value);
        const auto set = checked ? applet.set_param(param, *checked) : AppletResult(std::unexpected(checked.error()));
        if (!set) {
            st.status_code = set.error() == AppletError::OutOfRange ? ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS
                                                                    : ResponseStatusCode_RESPONSE_STATUS_INVALID_STATE;
            reply.write_set_error(applet, *cmd, set.error());
            return true;
        }
    }
    st.status_code = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
    reply.write_reply(applet, *cmd);
    return true;
}

} // namespace MockApplets

// =============================================================================
// HardwareEmulator Implementation - Mock Applet Methods
// =============================================================================

void HardwareEmulator::register_mock_applet(std::unique_ptr<MockApplets::MockApplet> applet) {
    std::lock_guard<std::mutex> lock(applets_mutex_);
    mock_applets_.push_back(std::move(applet));
    log("Mock applet registered: " + std::string(mock_applets_.back()->name()));
}

std::vector<std::string> HardwareEmulator::get_registered_applets() const {
    std::lock_guard<std::mutex> lock(applets_mutex_);
    std::vector<std::string> names;
    for (const auto& applet : mock_applets_) {
        names.push_back(applet->name());
    }
    return names;
}

std::map<std::string, EmulatorState::AppletState> HardwareEmulator::get_applet_states() const {
    std::lock_guard<std::mutex> lock(applets_mutex_);
    std::lock_guard<std::recursive_mutex> state_lock(state_mutex_);

    // Return copy of current applet states without modification
    return state_.applets;
}

// =============================================================================
// get_response_for_request - Single implementation with Mock Applet Support
// =============================================================================

std::vector<std::byte> HardwareEmulator::get_response_for_request(const Request& req, uint16_t seq) {
    switch (req.which_command) {
        case Request_scope_configure_tag:
        case Request_scope_arm_tag:
        case Request_scope_force_trigger_tag:
        case Request_scope_stop_tag:
        case Request_scope_get_status_tag:
        case Request_scope_read_tag:
        case Request_scope_read_cancel_tag:
        case Request_data_link_configure_tag:
            return handle_scope(req, seq);
        default:
            break;
    }

    if (req.which_command == Request_log_subscribe_tag) {
        // Acknowledge, then replay a short emulated boot log as LogMessage EVENTs.
        std::vector<std::byte> out;
        Response ack = Response_init_zero;
        ack.has_result = true;
        ack.result.status = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
        append_response(out, frame::Type::Response, seq, ack);
        // The struct was not decoded, so apply the proto defaults by hand.
        const auto& sub = req.command.log_subscribe;
        const bool enabled = !sub.has_enabled || sub.enabled;
        const bool replay = !sub.has_replay || sub.replay;
        const DeviceLogLevel min_level = sub.has_min_level ? sub.min_level : DeviceLogLevel_DEVICE_LOG_INFO;
        if (enabled && replay) {
            static constexpr struct { uint32_t tick; DeviceLogLevel level; const char* text; } kBoot[] = {
                {2, DeviceLogLevel_DEVICE_LOG_INFO, "Booting up"},
                {35, DeviceLogLevel_DEVICE_LOG_INFO, "Charger: BQ25792 found, ADC enabled"},
                {41, DeviceLogLevel_DEVICE_LOG_WARN, "emulator: no real hardware attached"},
            };
            uint16_t event_seq = 0;
            for (const auto& b : kBoot) {
                if (b.level < min_level) {
                    continue;
                }
                Response ev = Response_init_zero;
                ev.which_payload = Response_log_message_tag;
                ev.payload.log_message.tick_ms = b.tick;
                ev.payload.log_message.level = b.level;
                std::snprintf(ev.payload.log_message.text, sizeof(ev.payload.log_message.text), "%s", b.text);
                append_response(out, frame::Type::Event, event_seq++, ev);
            }
        }
        return out;
    }

    static Response resp;   // large; callers are serialised on the io thread
    resp = Response_init_zero;

    // Determine response type based on request
    switch (req.which_command) {
        case Request_ping_tag:
            resp.which_payload = Response_pong_tag;
            break;

        case Request_get_device_info_tag: {
            resp.which_payload = Response_device_info_tag;
            auto& d = resp.payload.device_info;
            std::snprintf(d.board, sizeof(d.board), "emulator");
            std::snprintf(d.firmware, sizeof(d.firmware), "hostctl emulator");
            d.uptime_ms = static_cast<uint32_t>(std::clock() * 1000.0 / CLOCKS_PER_SEC);
            d.heap_free = 6144;
            d.heap_min_free = 5120;
            d.reset_flags = 1u << 26;   // pin reset
            d.has_psram_ok = true;
            d.psram_ok = true;
            d.has_psram_config = true;
            std::snprintf(d.psram_config, sizeof(d.psram_config), "2x APS6404L dual-flash (emulated)");
            static constexpr struct { const char* name; uint32_t stack; uint32_t prio; uint32_t state; } kThreads[] = {
                {"defaultTask", 1210, 24, 2}, {"message-handler", 1630, 24, 0}, {"link-rx-0", 610, 32, 2},
                {"scope-pump", 980, 24, 2},   {"log-forwarder", 590, 16, 2},    {"IDLE", 90, 0, 1},
            };
            for (const auto& t : kThreads) {
                auto& ti = d.threads[d.threads_count++];
                std::snprintf(ti.name, sizeof(ti.name), "%s", t.name);
                ti.stack_free_bytes = t.stack;
                ti.priority = t.prio;
                ti.state = t.state;
            }
            break;
        }

        case Request_get_system_status_tag: {
            resp.which_payload = Response_system_status_tag;
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            resp.payload.system_status = state_.system_status;
            break;
        }

        case Request_set_dac_config_tag:
            resp.which_payload = Response_system_status_tag;
            resp.has_result = true;
            resp.result.status = ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND;
            break;

        case Request_set_charger_config_tag:
        case Request_set_usb_pd_config_tag:
        case Request_usb_pd_renegotiate_tag: {
            // Like the firmware: the full system status, plus the result when it failed
            const auto r = req.which_command == Request_set_charger_config_tag  ? handle_set_charger_config(req)
                           : req.which_command == Request_set_usb_pd_config_tag ? handle_set_usb_pd_config(req)
                                                                                : handle_usb_pd_renegotiate(req);
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            resp.which_payload = Response_system_status_tag;
            resp.payload.system_status = state_.system_status;
            if (!r) {
                resp.has_result = true;
                resp.result.status = status_for(r.error());
                resp.result.has_error = true;
                resp.result.error = static_cast<ControllerErrorCode>(r.error());
                log("Emulator: request rejected: " + get_error_message(r.error()));
            }
            break;
        }

        case Request_power_off_tag: {
            handle_power_off(req.command.power_off);
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            resp.which_payload = Response_system_status_tag;
            resp.payload.system_status = state_.system_status;
            break;
        }

        case Request_set_output_regulator_config_tag: {
            // For set commands, return the system status
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            resp.which_payload = Response_system_status_tag;
            resp.payload.system_status = state_.system_status;
            break;
        }

        case Request_applet_start_tag:
        case Request_applet_stop_tag:
        case Request_applet_command_tag: {
            // Same replies as MessageHandler (firmware)
            std::lock_guard<std::mutex> lock(applets_mutex_);
            resp.which_payload = Response_applet_status_tag;
            auto& st = resp.payload.applet_status;
            const char* name = req.which_command == Request_applet_start_tag  ? req.command.applet_start.applet_name
                               : req.which_command == Request_applet_stop_tag ? req.command.applet_stop.applet_name
                                                                              : req.command.applet_command.applet_name;
            MockApplets::MockApplet* applet = nullptr;
            for (auto& a : mock_applets_) {
                if (std::strcmp(a->name(), name) == 0) {
                    applet = a.get();
                }
            }
            if (!applet) {
                st.status_code = ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND;
                std::snprintf(st.text, sizeof(st.text), "applet not found: %s", name);
                break;
            }
            st.exists = true;
            // argv[0] = applet name, then the arguments (like the firmware)
            const pb_size_t count = req.which_command == Request_applet_start_tag ? req.command.applet_start.args_count
                                    : req.which_command == Request_applet_command_tag ? req.command.applet_command.args_count
                                                                                       : 0;
            std::array<char*, 16> argv{};
            argv[0] = const_cast<char*>(name);
            for (pb_size_t i = 0; i < count && i < 15; ++i) {
                argv[i + 1] = const_cast<char*>(req.which_command == Request_applet_start_tag ? req.command.applet_start.args[i]
                                                                                            : req.command.applet_command.args[i]);
            }
            const int argc = static_cast<int>(std::min<pb_size_t>(count, 15)) + 1;

            if (req.which_command == Request_applet_start_tag) {
                if (count > 0 && !applet->configure(argc, argv.data())) {
                    st.status_code = ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS;
                    std::snprintf(st.text, sizeof(st.text), "%s configure failed", name);
                    break;
                }
                for (auto& other : mock_applets_) {   // one applet at a time (AppletRunner)
                    if (other.get() != applet) other->stop();
                }
                const auto started = applet->start();
                st.running = started.has_value();
                st.status_code = started ? ResponseStatusCode_RESPONSE_STATUS_SUCCESS : ResponseStatusCode_RESPONSE_STATUS_FAILURE;
                std::snprintf(st.text, sizeof(st.text), "%s %s", name, started ? "started" : applet_error_text(started.error()));
            } else if (req.which_command == Request_applet_stop_tag) {
                applet->stop();
                st.running = false;
                st.status_code = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
                std::snprintf(st.text, sizeof(st.text), "%s stopped", name);
            } else {
                st.running = applet->is_running();
                // Generic parameter commands first: params | get [key] | set <key> <value>
                if (MockApplets::run_param_command(*applet, argc, argv.data(), st)) {
                    st.running = applet->is_running();
                    break;
                }
                const bool ok = applet->command(argc, argv.data());
                st.running = applet->is_running();
                st.status_code = ok ? ResponseStatusCode_RESPONSE_STATUS_SUCCESS : ResponseStatusCode_RESPONSE_STATUS_FAILURE;
                std::snprintf(st.text, sizeof(st.text), "%s", ok ? "Success" : "Failure");
            }
            std::lock_guard<std::recursive_mutex> state_lock(state_mutex_);
            state_.applets[name].running = applet->is_running();
            break;
        }

        case Request_applet_list_tag: {
            std::lock_guard<std::mutex> lock(applets_mutex_);
            resp.which_payload = Response_applet_list_response_tag;
            resp.payload.applet_list_response.status_code = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
            
            // Populate applet list with registered mock applets
            resp.payload.applet_list_response.applet_names_count = mock_applets_.size();
            for (size_t i = 0; i < mock_applets_.size() && i < 16; ++i) {
                snprintf(resp.payload.applet_list_response.applet_names[i], sizeof(resp.payload.applet_list_response.applet_names[i]),
                        "%s", mock_applets_[i]->name());
            }
            
            snprintf(resp.payload.applet_list_response.text, sizeof(resp.payload.applet_list_response.text),
                    "Found %zu applets", mock_applets_.size());
            break;
        }

        default:
            log("Unknown command for response generation");
            resp.has_result = true;
            resp.result.status = ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND;
            break;
    }

    return encode_response(resp, seq);
}
