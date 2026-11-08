#ifndef __HARDWARE_EMULATOR_HPP__
#define __HARDWARE_EMULATOR_HPP__

#include "frame.hpp"
#include "protocol.pb.h"
#include "error_host.hpp"
#include <atomic>
#include <boost/asio.hpp>
#include <cstdint>
#include <chrono>
#include <expected>
#include <memory>
#include "applet_control.hpp"
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Forward declaration for mock applet
namespace MockApplets {
    class MockApplet;
}

/**
 * State structure that mirrors Connector::State for easy state access
 */
struct EmulatorState {
    std::time_t last_ping{0};

    // Merged system status for all components
    SystemStatus system_status{};

    // Applet state tracking
    struct AppletState {
        bool running{false};
        std::string name;
        std::map<std::string, std::string> parameters;  // Store applet parameters for mocking
    };
    
    std::map<std::string, AppletState> applets;  // Applet name -> state mapping
};

/**
 * Emulated applets: the same AppletControl declarations as the firmware, so the generic
 * `applet_command <name> params|get|set` uses the firmware's parser and reply text
 * (AppletParamCommand / AppletParamText).
 */
namespace MockApplets {
    class MockApplet : public AppletControl {
    public:
        virtual ~MockApplet() = default;
        /** @brief applet_start arguments (argv[0] = applet name), like Applet::configure */
        virtual bool configure(int argc, char** argv) noexcept = 0;
        /** @brief Applet-specific applet_command (argv[0] = applet name), like Applet::on_message */
        virtual bool command(int argc, char** argv) noexcept = 0;
        bool is_running() const noexcept { return run_state() == AppletRun::Running; }
    };

    /** @brief aux-pwm: freq / duty1 / duty2 and its CLI commands */
    std::unique_ptr<MockApplet> make_aux_pwm();

    /** @brief Generic parameter command (argv[0] = applet name); false: not one */
    bool run_param_command(MockApplet& applet, int argc, char** argv, AppletStatus& st);
}

/**
 * HardwareEmulator simulates the STM32 resonance converter controller
 * for testing the TUI without physical hardware.
 *
 * Features:
 * - Protocol buffer message encoding/decoding
 * - Realistic hardware behavior emulation
 * - Packet exchange logging
 * - Configurable response timing
 */
class HardwareEmulator {
public:
    using LogCallback = std::function<void(const std::string&)>;
    using PacketCallback = std::function<void(const std::string&)>;
    using State = EmulatorState;

    HardwareEmulator(LogCallback log_callback = nullptr,
                     PacketCallback packet_callback = nullptr);
    ~HardwareEmulator();

    // Start/stop the emulator
    void start();
    void stop();

    // Check if running
    bool is_running() const noexcept;

    // Get current state (thread-safe)
    State get_state() const;

    // Set configuration (simulates host setting values)
    void set_charger_config(const ChargerConfig& config);
    void set_backboost_config(const OutputRegulatorConfig& config);
    void set_dac_state(const DACState& config);

    // Set full data (for emulator mode)
    void set_system_status(const SystemStatus& status);

    // Packet I/O - called by Connector
    void send_request(const Request& req);
    std::vector<std::byte> receive_response();

    // Frames (RESPONSE, and for scope reads DATA + EVENT) answering a request with frame seq
    std::vector<std::byte> get_response_for_request(const Request& req, uint16_t seq = 0);

    // Test hook: drop these DATA frame indices (ScopeReadStarted order) in the next scope read
    void scope_drop_frames_once(std::vector<uint32_t> frame_indices);

    // Test hooks for the input power path: dock DC on VAC2; USB-C partner (a PD 3.0 source
    // with 5/9/15/20 V 3 A) attached to our sink or not
    void set_dock_present(bool present);
    void set_usb_pd_partner(bool attached);

    // Get packet log
    std::vector<std::string> get_packet_log() const;

    // Mock applet registration (called by mock applets on startup)
    void register_mock_applet(std::unique_ptr<MockApplets::MockApplet> applet);

    // Get list of registered applets
    std::vector<std::string> get_registered_applets() const;

    // Get applet state
    std::map<std::string, EmulatorState::AppletState> get_applet_states() const;

private:
    mutable std::recursive_mutex state_mutex_;
    mutable std::mutex log_mutex_;
    mutable std::mutex applets_mutex_;  // Separate mutex for applet registry

    LogCallback log_callback_;
    PacketCallback packet_callback_;

    // Hardware state
    State state_;

    // Thread control
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    // Packet log
    std::vector<std::string> packet_log_;

    // Mock applet registry
    std::vector<std::unique_ptr<MockApplets::MockApplet>> mock_applets_;

    // Logging helpers
    void log(const std::string& msg);
    void log_packet(const std::string& direction, const std::string& msg);

    // Command handlers
    void handle_ping(const Request& req);
    void handle_get_system_status(const Request& req);
    void handle_set_backboost_config(const Request& req);
    // Validated like the firmware; on error nothing changes (called from get_response_for_request)
    std::expected<void, ControllerError> handle_set_charger_config(const Request& req);
    std::expected<void, ControllerError> handle_set_usb_pd_config(const Request& req);
    std::expected<void, ControllerError> handle_usb_pd_renegotiate(const Request& req);

    // Emulated USB-C port and input power path (docs/usb_pd.md); state_mutex_ held
    struct PowerEmulation {
        bool dock_present{false};
        ChargerInputSource input_mode{ChargerInputSource_CHARGER_INPUT_AUTO};   // host setting
        // Power off (PowerPath::step_power_off): state and when the step started / ends
        PowerOffState off_state{PowerOffState_POWER_OFF_IDLE};
        std::chrono::steady_clock::time_point off_since{};
        bool off_logged{false};   // "battery disconnected" logged once
    };
    PowerEmulation power_;
    void usb_pd_set_partner_locked(bool attached);
    void usb_pd_select_contract();
    void update_power_path();
    /** @brief power_off request: soft stop, ship (wait for unplug, drain, 10 s), cancel */
    void handle_power_off(const PowerOffRequest& req);
    void step_power_off_locked();

    // State update simulation
    void update_hardware_state();

    // Emulated oscilloscope (mirrors controller/src/scope_service.cc behaviour)
    struct ScopeEmulation {
        bool configured{false};
        std::vector<ScopeChannelConfig> channels;
        uint32_t sample_rate_hz{0};
        uint32_t pre{0};
        uint32_t post{0};
        bool software_trigger{true};
        ScopeState state{ScopeState_SCOPE_STATE_IDLE};
        ScopeTriggerKind trigger_source{ScopeTriggerKind_SCOPE_TRIGGER_SOFTWARE};
        std::vector<std::vector<uint16_t>> data;   // [channel][pre + post]
        uint32_t aux_baudrate{115200};
        std::vector<uint32_t> drop_once;
    };
    ScopeEmulation scope_;
    void scope_capture(ScopeTriggerKind source);
    std::vector<std::byte> handle_scope(const Request& req, uint16_t seq);

    // Frame encoding helpers
    static void append_frame(std::vector<std::byte>& out, frame::Type type, uint8_t flags, uint16_t seq,
                             const uint8_t* payload, size_t len);
    void append_response(std::vector<std::byte>& out, frame::Type type, uint16_t seq, const Response& resp);
    std::vector<std::byte> encode_response(const Response& resp, uint16_t seq);
};

#endif
