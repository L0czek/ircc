#ifndef __HARDWARE_EMULATOR_HPP__
#define __HARDWARE_EMULATOR_HPP__

#include "frame.hpp"
#include "protocol.pb.h"
#include "error_host.hpp"
#include <atomic>
#include <boost/asio.hpp>
#include <cstdint>
#include <expected>
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
 * MockApplet base class for emulated applets
 */
namespace MockApplets {
    class MockApplet {
    public:
        virtual ~MockApplet() = default;
        virtual const char* name() const noexcept = 0;
        virtual std::expected<void, ControllerError> start() noexcept = 0;
        virtual std::expected<void, ControllerError> stop() noexcept = 0;
        virtual std::expected<void, ControllerError> command(int argc, char** argv) noexcept = 0;
        virtual const char* help() const noexcept = 0;
        
        bool is_running() const noexcept { return running_; }
        
    protected:
        bool running_{false};
    };
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
    void handle_set_charger_config(const Request& req);
    void handle_set_backboost_config(const Request& req);

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
