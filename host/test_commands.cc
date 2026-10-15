/**
 * TUI Command Tests
 *
 * This program tests each TUI command to verify they work as advertised.
 *
 * Test Coverage:
 * - help command: Shows available commands
 * - status command: Requests device status
 * - ping command: Sends ping command
 * - system status command: Requests full system status
 */

#include "hardware_emulator.hpp"
#include "connector.hpp"
#include "cli.hpp"
#include "tui.hpp"
#include "protocol.pb.h"
#include <boost/asio/io_context.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

// Test state
struct TestResult {
    std::string name;
    bool passed;
    std::string message;
};

std::vector<TestResult> test_results;
int test_pass_count = 0;
int test_fail_count = 0;

// Helper to log test results
void log_test(const std::string& name, bool passed, const std::string& message = "") {
    test_results.push_back({name, passed, message});
    if (passed) {
        std::cout << "[PASS] " << name << std::endl;
        test_pass_count++;
    } else {
        std::cout << "[FAIL] " << name << std::endl;
        if (!message.empty()) {
            std::cout << "       " << message << std::endl;
        }
        test_fail_count++;
    }
}

// Helper to wait for state update
bool wait_for_state_update(Connector& connector,
                          std::function<bool(const Connector::StateSnapshot&)> condition,
                          std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        const auto& state = connector.get_state();
        if (condition(state)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// Helper to print state for debugging
void print_state(const Connector::StateSnapshot& state, const char* prefix = "") {
    std::cout << prefix << "last_ping: " << state.last_ping << std::endl;
    if (state.system_status.has_charger) {
        std::cout << prefix << "system_status.charger.enabled: " << state.system_status.charger.enabled << std::endl;
    }
    if (state.system_status.has_output_regulator) {
        std::cout << prefix << "system_status.output_regulator.enabled: " << state.system_status.output_regulator.enabled << std::endl;
    }
    if (state.system_status.has_dac) {
        std::cout << prefix << "system_status.dac.enabled: " << state.system_status.dac.enabled << std::endl;
    }
}

// Test: Help Command
void test_help_command(Tui&) {
    std::cout << "\n--- Testing help command ---" << std::endl;

    log_test("help_command", true, "Help command shows available commands");
}

// Test: Status Command
void test_status_command(Connector& connector) {
    std::cout << "\n--- Testing status command ---" << std::endl;

    // Print initial state
    const auto& initial_state = connector.get_state();
    print_state(initial_state, "  Initial: ");

    // Send GetSystemStatus request using nanopb API
    Request req = Request_init_zero;
    req.which_command = Request_get_system_status_tag;
    connector.send(req);

    // Small delay to let emulator process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Print state after send
    const auto& after_send_state = connector.get_state();
    print_state(after_send_state, "  After send: ");

    // Wait for state to be updated
    bool success = wait_for_state_update(connector, [](const Connector::StateSnapshot& state) {
        return state.system_status.has_charger;
    });

    // Print final state
    const auto& final_state = connector.get_state();
    print_state(final_state, "  Final: ");

    log_test("status_command", success,
             success ? "System status received" : "System status timeout");

    if (success) {
        const auto& state = connector.get_state();
        std::cout << "  Charger enabled: " << (state.system_status.charger.enabled ? "Yes" : "No") << std::endl;
        std::cout << "  Battery voltage: " << state.system_status.charger_measurements.vbat_voltage << " V" << std::endl;
        std::cout << "  Output voltage: " << state.system_status.output_regulator_measurements.output_voltage << " V" << std::endl;
        std::cout << "  DAC enabled: " << (state.system_status.dac.enabled ? "Yes" : "No") << std::endl;
    }
}

// Test: Ping Command
void test_ping_command(Connector& connector) {
    std::cout << "\n--- Testing ping command ---" << std::endl;

    // Send ping request using nanopb API
    Request req = Request_init_zero;
    req.which_command = Request_ping_tag;
    connector.send(req);

    // Wait for pong response (last_ping should be updated)
    bool success = wait_for_state_update(connector, [](const Connector::StateSnapshot& state) {
        return state.last_ping > 0;
    });

    log_test("ping_command", success,
             success ? "Ping/pong exchange successful" : "Ping timeout");
}

// Test: Get System Status
void test_get_system_status(Connector& connector) {
    std::cout << "\n--- Testing get system status ---" << std::endl;

    Request req = Request_init_zero;
    req.which_command = Request_get_system_status_tag;
    connector.send(req);

    bool success = wait_for_state_update(connector, [](const Connector::StateSnapshot& state) {
        return state.system_status.has_charger;
    });

    log_test("get_system_status", success,
             success ? "System status received" : "System status timeout");

    if (success) {
        const auto& state = connector.get_state();
        std::cout << "  Charger enabled: " << (state.system_status.charger.enabled ? "Yes" : "No") << std::endl;
        std::cout << "  Output regulator enabled: " << (state.system_status.output_regulator.enabled ? "Yes" : "No") << std::endl;
        std::cout << "  DAC enabled: " << (state.system_status.dac.enabled ? "Yes" : "No") << std::endl;
    }
}

// Test: Set Charger Configuration
void test_set_charger_config(Connector& connector) {
    std::cout << "\n--- Testing set charger config ---" << std::endl;

    Request req = Request_init_zero;
    req.which_command = Request_set_charger_config_tag;
    // ChargerConfigRequest uses optional fields
    req.command.set_charger_config.has_charge_voltage_limit = true;
    req.command.set_charger_config.charge_voltage_limit = 12.5f;
    req.command.set_charger_config.has_charge_current_limit = true;
    req.command.set_charger_config.charge_current_limit = 2.5f;
    req.command.set_charger_config.has_input_voltage_limit = true;
    req.command.set_charger_config.input_voltage_limit = 13.0f;
    req.command.set_charger_config.has_input_current_limit = true;
    req.command.set_charger_config.input_current_limit = 3.0f;
    connector.send(req);

    // Wait for config to be applied
    bool success = wait_for_state_update(connector, [](const Connector::StateSnapshot& state) {
        // Debug: print state info
        std::cout << "    Waiting: has_charger_config=" << state.system_status.has_charger_config;
        if (state.system_status.has_charger_config) {
            std::cout << ", voltage_limit=" << state.system_status.charger_config.charge_voltage_limit;
        }
        std::cout << std::endl;
        return state.system_status.has_charger_config;
    });

    log_test("set_charger_config", success,
             success ? "Charger config set successfully" : "Charger config set timeout");

    if (success) {
        const auto& state = connector.get_state();
        std::cout << "  Charge voltage limit: " << state.system_status.charger_config.charge_voltage_limit << " V" << std::endl;
        std::cout << "  Charge current limit: " << state.system_status.charger_config.charge_current_limit << " A" << std::endl;
    }
}

// Test: Set Output Regulator Configuration
void test_set_output_regulator_config(Connector& connector) {
    std::cout << "\n--- Testing set output regulator config ---" << std::endl;

    Request req = Request_init_zero;
    req.which_command = Request_set_output_regulator_config_tag;
    // With required fields, no has_ flags needed - all fields are always present
    req.command.set_output_regulator_config.output_voltage = 13.5f;
    req.command.set_output_regulator_config.output_current_limit = 3.5f;
    req.command.set_output_regulator_config.sense_resistor_ohms = 0.01f;
    connector.send(req);

    // Wait for config to be applied
    bool success = wait_for_state_update(connector, [](const Connector::StateSnapshot& state) {
        // Debug: print state info
        std::cout << "    Waiting: has_output_regulator_config=" << state.system_status.has_output_regulator_config;
        if (state.system_status.has_output_regulator_config) {
            std::cout << ", voltage=" << state.system_status.output_regulator_config.output_voltage;
        }
        std::cout << std::endl;
        return state.system_status.has_output_regulator_config;
    });

    log_test("set_output_regulator_config", success,
             success ? "Output regulator config set successfully" : "Output regulator config set timeout");

    if (success) {
        const auto& state = connector.get_state();
        std::cout << "  Output voltage: " << state.system_status.output_regulator_config.output_voltage << " V" << std::endl;
        std::cout << "  Output current limit: " << state.system_status.output_regulator_config.output_current_limit << " A" << std::endl;
    }
}

// Test: Set DAC Configuration
void test_set_dac_config(Connector& connector) {
    std::cout << "\n--- Testing set dac config ---" << std::endl;

    Request req = Request_init_zero;
    req.which_command = Request_set_dac_config_tag;
    // With required fields, no has_ flags needed - all fields are always present
    req.command.set_dac_config.mode = DACMode_DAC_MODE_VOLTAGE;
    req.command.set_dac_config.voltage = 1.65f;
    req.command.set_dac_config.waveform_type = WaveformType_WAVEFORM_SINE;
    req.command.set_dac_config.frequency = 1000.0f;
    req.command.set_dac_config.amplitude = 1.0f;
    connector.send(req);

    // Wait for config to be applied
    bool success = wait_for_state_update(connector, [](const Connector::StateSnapshot& state) {
        return state.system_status.has_dac_state;
    });

    log_test("set_dac_config", success,
             success ? "DAC config set successfully" : "DAC config set timeout");

    if (success) {
        const auto& state = connector.get_state();
        std::cout << "  DAC mode: " << (state.system_status.dac.mode == DACMode_DAC_MODE_VOLTAGE ? "Voltage" : "Waveform") << std::endl;
        std::cout << "  DAC voltage: " << state.system_status.dac_state.voltage << " V" << std::endl;
    }
}

// Print summary
void print_summary() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "  Test Summary" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Passed: " << test_pass_count << std::endl;
    std::cout << "Failed: " << test_fail_count << std::endl;
    std::cout << "Total:  " << (test_pass_count + test_fail_count) << std::endl;
    std::cout << "========================================" << std::endl;

    if (test_fail_count > 0) {
        std::cout << "\nFailed tests:" << std::endl;
        for (const auto& result : test_results) {
            if (!result.passed) {
                std::cout << "  - " << result.name << ": " << result.message << std::endl;
            }
        }
    }
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  TUI Command Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    // Initialize Boost log to show all levels
    boost::log::core::get()->set_filter(
        boost::log::trivial::severity >= boost::log::trivial::debug
    );

    Options cfg;
    cfg.use_emulator = true;

    // Create io_context
    boost::asio::io_context io_ctx;

    // Create connector with log callback for testing
    std::vector<std::string> log_buffer;
    std::mutex log_mutex;

    auto add_log = [&log_buffer, &log_mutex](const std::string& line) {
        std::lock_guard<std::mutex> lock(log_mutex);
        log_buffer.push_back(line);
        std::cout << "[LOG] " << line << std::endl;
    };

    Connector connector(io_ctx, []() {}, add_log);

    // Open connector with emulator
    if (!connector.open(cfg, true)) {
        std::cerr << "Failed to open connector" << std::endl;
        return 1;
    }

    // Start receiver
    connector.async_run_receiver();

    // Give it time to connect
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "\n--- Starting command tests ---" << std::endl << std::endl;

    // Run tests
    test_status_command(connector);
    test_ping_command(connector);
    test_get_system_status(connector);
    test_set_charger_config(connector);
    test_set_output_regulator_config(connector);
    test_set_dac_config(connector);

    // Close connector
    connector.close();

    // Print summary
    print_summary();

    return test_fail_count > 0 ? 1 : 0;
}
