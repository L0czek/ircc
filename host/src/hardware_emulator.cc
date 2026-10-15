#include "hardware_emulator.hpp"
#include "pb_encode.h"
#include "pb_decode.h"
#include "protocol.pb.h"
#include "error_host.hpp"
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <random>

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

    state_.system_status.has_charger_measurements = true;
    state_.system_status.charger_measurements.vbat_voltage = 12.0f;
    state_.system_status.charger_measurements.ibus_current = 0.0f;
    state_.system_status.charger_measurements.vbus_voltage = 0.0f;
    state_.system_status.charger_measurements.vsys_voltage = 12.0f;

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

    state_.system_status.has_dac = true;
    state_.system_status.dac.enabled = false;
    state_.system_status.dac.mode = DACMode_DAC_MODE_VOLTAGE;
    state_.system_status.dac.waveform_type = WaveformType_WAVEFORM_SINE;
    state_.system_status.dac.frequency = 1000.0f;
    state_.system_status.dac.amplitude = 1.65f;
    state_.system_status.dac.error = false;
    state_.system_status.dac.waveform_error = false;
    state_.system_status.dac.timestamp_ms = 0;

    state_.system_status.has_dac_measurements = true;
    state_.system_status.dac_measurements.voltage_output = 1.65f;

    state_.system_status.has_dac_state = true;
    state_.system_status.dac_state.voltage = 1.65f;
    state_.system_status.dac_state.waveform_type = WaveformType_WAVEFORM_SINE;
    state_.system_status.dac_state.frequency = 1000.0f;
    state_.system_status.dac_state.amplitude = 1.65f;

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
            handle_set_charger_config(req);
            break;
        case Request_set_output_regulator_config_tag:
            handle_set_backboost_config(req);
            break;
        case Request_set_dac_config_tag:
            handle_set_dac_config(req);
            break;
        case Request_applet_start_tag:
        case Request_applet_stop_tag:
        case Request_applet_command_tag:
        case Request_applet_list_tag:
            // Applet commands - handled in response generation
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

void HardwareEmulator::handle_set_charger_config(const Request& req) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);

    // With required fields in ChargerConfigRequest, all fields are always present
    state_.system_status.has_charger_config = true;
    state_.system_status.charger_config.charge_voltage_limit = req.command.set_charger_config.charge_voltage_limit;
    log("Emulator: Charge voltage set to " +
        std::to_string(state_.system_status.charger_config.charge_voltage_limit));

    state_.system_status.charger_config.charge_current_limit = req.command.set_charger_config.charge_current_limit;
    log("Emulator: Charge current set to " +
        std::to_string(state_.system_status.charger_config.charge_current_limit));

    state_.system_status.charger_config.input_voltage_limit = req.command.set_charger_config.input_voltage_limit;
    log("Emulator: Input voltage set to " +
        std::to_string(state_.system_status.charger_config.input_voltage_limit));

    state_.system_status.charger_config.input_current_limit = req.command.set_charger_config.input_current_limit;
    log("Emulator: Input current set to " +
        std::to_string(state_.system_status.charger_config.input_current_limit));
}

void HardwareEmulator::handle_set_backboost_config(const Request& req) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);

    // With required fields in OutputRegulatorConfigRequest, all fields are always present
    state_.system_status.has_output_regulator_config = true;
    state_.system_status.output_regulator_config.output_voltage =
        req.command.set_output_regulator_config.output_voltage;
    log("Emulator: Output Regulator voltage set to " +
        std::to_string(state_.system_status.output_regulator_config.output_voltage));

    state_.system_status.output_regulator_config.output_current_limit =
        req.command.set_output_regulator_config.output_current_limit;
    log("Emulator: Output Regulator current limit set to " +
        std::to_string(state_.system_status.output_regulator_config.output_current_limit));

    state_.system_status.output_regulator_config.sense_resistor_ohms =
        req.command.set_output_regulator_config.sense_resistor_ohms;
    log("Emulator: Output Regulator sense resistor set to " +
        std::to_string(state_.system_status.output_regulator_config.sense_resistor_ohms));
}

void HardwareEmulator::handle_set_dac_config(const Request& req) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);

    // With required fields in DACConfigRequest, all fields are always present
    state_.system_status.dac.mode = req.command.set_dac_config.mode;
    log("Emulator: DAC mode set to " + std::to_string(state_.system_status.dac.mode));

    state_.system_status.dac_state.voltage = req.command.set_dac_config.voltage;
    log("Emulator: DAC voltage set to " + std::to_string(state_.system_status.dac_state.voltage));

    state_.system_status.dac_state.waveform_type = req.command.set_dac_config.waveform_type;
    log("Emulator: DAC waveform type set");

    state_.system_status.dac_state.frequency = req.command.set_dac_config.frequency;
    log("Emulator: DAC frequency set to " + std::to_string(state_.system_status.dac_state.frequency));

    state_.system_status.dac_state.amplitude = req.command.set_dac_config.amplitude;
    log("Emulator: DAC amplitude set to " + std::to_string(state_.system_status.dac_state.amplitude));
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

        // VBUS: varies based on charger state
        if (status.charger.plugged_in && status.charger.enabled) {
            // VBUS is present when charger is plugged in and enabled
            static float vbus_noise = 0.0f;
            vbus_noise = 0.9f * vbus_noise + 0.1f * noise_dis(gen) * 0.1f;
            new_vbus = 12.0f + vbus_noise; // Simulated 12V input
        } else {
            new_vbus = 0.0f;
        }

        // Now acquire lock only to update the actual state
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Update at 20Hz
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            state_.system_status.charger_measurements.vbat_voltage = new_vbat;
            state_.system_status.charger_measurements.vbus_voltage = new_vbus;
            state_.system_status.output_regulator_measurements.output_voltage = new_vout;
            state_.system_status.charger_measurements.ibus_current = new_ibus;
            state_.system_status.output_regulator_measurements.output_current = new_iout;
        }
    }
}

// =============================================================================
// Helper functions for message encoding/decoding
// =============================================================================

std::vector<std::byte> HardwareEmulator::encode_response(const Response& resp) {
    log("Emulator: encode_response called");
    // Create a buffer large enough for the encoded response
    std::vector<std::byte> result(MESSAGE_SIZE);

    // Encode the response
    pb_ostream_t stream = pb_ostream_from_buffer(
        (pb_byte_t*)result.data(),
        result.size()
    );

    bool status = pb_encode(&stream, Response_fields, &resp);

    if (!status) {
        log("Failed to encode response: " + std::string(PB_GET_ERROR(&stream)));
        return {};
    }

    log_packet("EMULATOR -> TX", "Encoded response: " + std::to_string(stream.bytes_written) + " bytes");

    // Build full message with header
    std::vector<std::byte> full_msg(MESSAGE_SIZE);
    size_t header_size = MESSAGE_HEADER_SIZE;

    // Copy header ID
    std::memcpy(full_msg.data(), MESSAGE_HEADER_ID, 4);

    // Copy encoded data
    std::memcpy(full_msg.data() + header_size, result.data(), stream.bytes_written);
    full_msg[MESSAGE_HEADER_SIZE - 1 - 3] = static_cast<std::byte>(stream.bytes_written); // Set size in header (size is at offset 8, which is header_size - 4)

    return full_msg;
}

// =============================================================================
// Mock Applet Implementation
// =============================================================================

// Mock implementation of aux-pwm applet
namespace MockApplets {

static void emulator_info(const char* fmt, ...) {
    std::cout << "[Emulator][INFO] ";
    va_list list;
    va_start(list, fmt);
    std::vprintf(fmt, list);
    va_end(list);
}

static void emulator_error(const char* fmt, ...) {
    std::cerr << "[Emulator][ERROR] ";
    va_list list;
    va_start(list, fmt);
    std::vprintf(fmt, list);
    va_end(list);
}

class AuxPwmMock : public MockApplet {
public:
    AuxPwmMock() noexcept {
        // Initialize with default values
        frequency_hz_ = 1000.0f;
        duty_cycle_ch1_ = 50.0f;
        duty_cycle_ch2_ = 50.0f;
    }

    const char* name() const noexcept override {
        return "aux-pwm";
    }

    const char* help() const noexcept override {
        return "aux-pwm: PWM applet for TIMx CH1/CH2 (EMULATOR MODE)\n"
               "  set_freq <freq_hz>    - Set PWM frequency\n"
               "  set_duty <ch> <duty>  - Set duty cycle (0-100) for ch1/ch2\n"
               "  enable                - Start PWM\n"
               "  disable               - Stop PWM\n"
               "  status                - Show status";
    }

    std::expected<void, ControllerError> start() noexcept override {
        if (running_) {
            emulator_info("aux-pwm: already enabled in emulator\\n");
            return {};
        }

        running_ = true;
        emulator_info("aux-pwm: PWM enabled in emulator (freq=%.0f Hz, CH1=%.1f%%, CH2=%.1f%%)\\n",
                 frequency_hz_, duty_cycle_ch1_, duty_cycle_ch2_);

        return {};
    }

    std::expected<void, ControllerError> stop() noexcept override {
        if (!running_) {
            emulator_info("aux-pwm: already disabled in emulator\\n");
            return {};
        }

        running_ = false;
        emulator_info("aux-pwm: PWM disabled in emulator\\n");

        return {};
    }

    std::expected<void, ControllerError> command(int argc, char** argv) noexcept override {
        if (argc < 1) {
            emulator_error("aux-pwm: no command specified\\n");
            return std::unexpected(ControllerError::HAL_ERROR);
        }

        std::string cmd = argv[0];

        if (cmd == "set_freq") {
            if (argc < 2) {
                emulator_error("aux-pwm: set_freq requires frequency argument\\n");
                return std::unexpected(ControllerError::HAL_ERROR);
            }
            float freq = std::stof(argv[1]);
            frequency_hz_ = freq;
            emulator_info("aux-pwm: frequency set to %.0f Hz (emulator)\\n", frequency_hz_);
            return {};
        }
        else if (cmd == "set_duty") {
            if (argc < 3) {
                emulator_error("aux-pwm: set_duty requires channel and duty arguments\\n");
                return std::unexpected(ControllerError::HAL_ERROR);
            }
            int channel = std::stoi(argv[1]);
            float duty = std::stof(argv[2]);

            if (channel == 1) {
                duty_cycle_ch1_ = duty;
            } else if (channel == 2) {
                duty_cycle_ch2_ = duty;
            } else {
                emulator_error("aux-pwm: channel must be 1 or 2\\n");
                return std::unexpected(ControllerError::HAL_ERROR);
            }

            emulator_info("aux-pwm: duty cycle set - CH1=%.1f%%, CH2=%.1f%% (emulator)\\n",
                     duty_cycle_ch1_, duty_cycle_ch2_);
            return {};
        }
        else if (cmd == "enable") {
            return start();
        }
        else if (cmd == "disable") {
            return stop();
        }
        else if (cmd == "status") {
            emulator_info("aux-pwm status (EMULATOR MODE):\\n");
            emulator_info("  Frequency: %.0f Hz\\n", frequency_hz_);
            emulator_info("  Channel 1: %.1f%% duty cycle\\n", duty_cycle_ch1_);
            emulator_info("  Channel 2: %.1f%% duty cycle\\n", duty_cycle_ch2_);
            emulator_info("  Enabled: %s\\n", running_ ? "yes" : "no");
            return {};
        }
        else if (cmd == "help") {
            emulator_info("%s", help());
            return {};
        }
        else {
            emulator_error("aux-pwm: unknown command '%s'\\n", cmd.c_str());
            emulator_error("aux-pwm: use 'aux-pwm help' for usage\\n");
            return std::unexpected(ControllerError::HAL_ERROR);
        }
    }

private:
    float frequency_hz_;
    float duty_cycle_ch1_;
    float duty_cycle_ch2_;
};

// Auto-registration of aux-pwm mock applet
static AuxPwmMock aux_pwm_mock;

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

std::vector<std::byte> HardwareEmulator::get_response_for_request(const Request& req) {
    Response resp = Response_init_zero;

    // Determine response type based on request
    switch (req.which_command) {
        case Request_ping_tag:
            resp.which_payload = Response_pong_tag;
            break;

        case Request_get_system_status_tag: {
            resp.which_payload = Response_system_status_tag;
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            resp.payload.system_status = state_.system_status;
            break;
        }

        case Request_set_charger_config_tag:
        case Request_set_output_regulator_config_tag:
        case Request_set_dac_config_tag: {
            // For set commands, return the system status
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            resp.which_payload = Response_system_status_tag;
            resp.payload.system_status = state_.system_status;
            break;
        }

        case Request_applet_start_tag: {
            std::lock_guard<std::mutex> lock(applets_mutex_);
            resp.which_payload = Response_applet_status_tag;
            resp.payload.applet_status.exists = true;
            resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND;

            // Find and start the mock applet
            for (auto& applet : mock_applets_) {
                if (std::strcmp(applet->name(), req.command.applet_start.applet_name) == 0) {
                    auto result = applet->start();
                    if (result) {
                        resp.payload.applet_status.running = true;
                        resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
                        snprintf(resp.payload.applet_status.text, sizeof(resp.payload.applet_status.text),
                                "Applet %s started successfully", req.command.applet_start.applet_name);

                        // Update state
                        std::lock_guard<std::recursive_mutex> state_lock(state_mutex_);
                        state_.applets[req.command.applet_start.applet_name].running = true;
                    } else {
                        resp.payload.applet_status.running = false;
                        resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_FAILURE;
                        snprintf(resp.payload.applet_status.text, sizeof(resp.payload.applet_status.text),
                                "Failed to start applet %s", req.command.applet_start.applet_name);
                    }
                    break;
                }
            }
            break;
        }

        case Request_applet_stop_tag: {
            std::lock_guard<std::mutex> lock(applets_mutex_);
            resp.which_payload = Response_applet_status_tag;
            resp.payload.applet_status.exists = true;
            resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND;

            // Find and stop the mock applet
            for (auto& applet : mock_applets_) {
                if (std::strcmp(applet->name(), req.command.applet_stop.applet_name) == 0) {
                    auto result = applet->stop();
                    if (result) {
                        resp.payload.applet_status.running = false;
                        resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
                        snprintf(resp.payload.applet_status.text, sizeof(resp.payload.applet_status.text),
                                "Applet %s stopped successfully", req.command.applet_stop.applet_name);

                        // Update state
                        std::lock_guard<std::recursive_mutex> state_lock(state_mutex_);
                        state_.applets[req.command.applet_stop.applet_name].running = false;
                    } else {
                        resp.payload.applet_status.running = true;  // Still running
                        resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_FAILURE;
                        snprintf(resp.payload.applet_status.text, sizeof(resp.payload.applet_status.text),
                                "Failed to stop applet %s", req.command.applet_stop.applet_name);
                    }
                    break;
                }
            }
            break;
        }

        case Request_applet_command_tag: {
            std::lock_guard<std::mutex> lock(applets_mutex_);
            resp.which_payload = Response_applet_status_tag;
            resp.payload.applet_status.exists = true;
            resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND;

            // Find and execute command on the mock applet
            for (auto& applet : mock_applets_) {
                if (std::strcmp(applet->name(), req.command.applet_command.applet_name) == 0) {
                    // Convert nanopb repeated string args to argc/argv for command()
                    // Protocol uses: repeated string args (max 16 strings of 64 chars each)
                    // nanopb generates: pb_size_t args_count; char args[16][64]
                    int argc = req.command.applet_command.args_count + 1;  // +1 for applet name
                    std::vector<std::string> arg_strings;
                    arg_strings.reserve(req.command.applet_command.args_count);
                    for (pb_size_t i = 0; i < req.command.applet_command.args_count && i < 16; ++i) {
                        arg_strings.push_back(req.command.applet_command.args[i]);
                    }
                    
                    // Build argv array
                    std::vector<std::unique_ptr<char[]>> argv_storage;
                    argv_storage.reserve(argc);
                    std::vector<char*> argv_ptr;
                    argv_ptr.reserve(argc);
                    
                    // First arg is the applet name
                    argv_storage.push_back(std::make_unique<char[]>(32));
                    std::strncpy(argv_storage.back().get(), req.command.applet_command.applet_name, 31);
                    argv_storage.back()[31] = '\0';
                    argv_ptr.push_back(argv_storage.back().get());
                    
                    // Then the actual arguments
                    for (const auto& s : arg_strings) {
                        argv_storage.push_back(std::make_unique<char[]>(s.size() + 1));
                        std::strcpy(argv_storage.back().get(), s.c_str());
                        argv_ptr.push_back(argv_storage.back().get());
                    }

                    auto result = applet->command(argc, argv_ptr.data());

                    if (result) {
                        resp.payload.applet_status.running = applet->is_running();
                        resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
                        snprintf(resp.payload.applet_status.text, sizeof(resp.payload.applet_status.text),
                                "Command executed successfully on %s", req.command.applet_command.applet_name);

                        // Update state
                        std::lock_guard<std::recursive_mutex> state_lock(state_mutex_);
                        state_.applets[req.command.applet_command.applet_name].running = applet->is_running();
                    } else {
                        resp.payload.applet_status.running = applet->is_running();
                        resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_FAILURE;
                        snprintf(resp.payload.applet_status.text, sizeof(resp.payload.applet_status.text),
                                "Command failed on %s", req.command.applet_command.applet_name);
                    }
                    break;
                }
            }
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
            return {};
    }

    return encode_response(resp);
}
