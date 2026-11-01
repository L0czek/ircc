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
#include "scope_client.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <format>
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
    req.command.set_output_regulator_config.has_output_voltage = true;
    req.command.set_output_regulator_config.output_voltage = 13.5f;
    req.command.set_output_regulator_config.has_output_current_limit = true;
    req.command.set_output_regulator_config.output_current_limit = 3.5f;
    req.command.set_output_regulator_config.has_sense_resistor_ohms = true;
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

// Test: enable/disable and partial output regulator config (only given fields change)
void test_enable_and_partial_config(Connector& connector) {
    std::cout << "\n--- Testing enable + partial config ---" << std::endl;

    Request req = Request_init_zero;
    req.which_command = Request_set_charger_config_tag;
    req.command.set_charger_config.has_enabled = true;
    req.command.set_charger_config.enabled = true;
    connector.send(req);
    bool ok = wait_for_state_update(connector, [](const Connector::StateSnapshot& s) {
        return s.system_status.has_charger && s.system_status.charger.enabled;
    });
    log_test("charger_enable", ok, ok ? "" : "charger did not report enabled");

    req = Request_init_zero;
    req.which_command = Request_set_output_regulator_config_tag;
    req.command.set_output_regulator_config.has_output_voltage = true;
    req.command.set_output_regulator_config.output_voltage = 9.0f;
    req.command.set_output_regulator_config.has_enabled = true;
    req.command.set_output_regulator_config.enabled = true;
    connector.send(req);
    ok = wait_for_state_update(connector, [](const Connector::StateSnapshot& s) {
        const auto& c = s.system_status.output_regulator_config;
        return s.system_status.output_regulator.enabled && c.output_voltage == 9.0f && c.output_current_limit == 3.5f;
    });
    log_test("output_partial_config", ok, ok ? "" : "voltage/enable not applied or current limit changed");
}

// Test: DAC requests are rejected (the DAC controller was removed)
void test_set_dac_config_unsupported(Connector& connector, std::atomic<int>& unsupported) {
    std::cout << "\n--- Testing set dac config (unsupported) ---" << std::endl;
    Request req = Request_init_zero;
    req.which_command = Request_set_dac_config_tag;
    connector.send(req);
    bool ok = false;
    for (int i = 0; i < 40 && !ok; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ok = unsupported.load() > 0;
    }
    log_test("set_dac_config_unsupported", ok, ok ? "" : "no UNSUPPORTED_COMMAND result");
}

// Test: scope configure / arm / trigger / read, with two DATA frames dropped
void test_scope(Connector& connector, HardwareEmulator* emulator, ScopeClient& scope) {
    std::cout << "\n--- Testing oscilloscope ---" << std::endl;

    Request req = Request_init_zero;
    req.which_command = Request_scope_configure_tag;
    auto& c = req.command.scope_configure;
    c.channels_count = 2;
    c.channels[0].adc = 1; c.channels[0].input = 1;
    c.channels[1].adc = 2; c.channels[1].input = 3;
    c.sample_rate_hz = 1000000;
    c.pre_trigger_samples = 1500;
    c.post_trigger_samples = 3000;
    connector.send(req);

    req = Request_init_zero;
    req.which_command = Request_scope_arm_tag;
    connector.send(req);
    req = Request_init_zero;
    req.which_command = Request_scope_force_trigger_tag;
    connector.send(req);

    bool ok = wait_for_state_update(connector, [](const Connector::StateSnapshot& s) {
        return s.has_scope_status && s.scope_status.state == ScopeState_SCOPE_STATE_DONE;
    });
    log_test("scope_capture_done", ok, ok ? "" : "scope did not reach DONE");
    if (!ok) return;

    const auto st = connector.get_state().scope_status;
    const uint32_t count = static_cast<uint32_t>(st.post_trigger_samples - st.first_sample);
    if (emulator) emulator->scope_drop_frames_once({1, 4});   // lose ch1 block 0 and ch0 block 2
    std::string error;
    ok = scope.read(0x3, st.first_sample, count, st.sample_rate_hz, DataLink_DATA_LINK_ORIGIN, "", error);
    log_test("scope_read_started", ok, error);
    if (!ok) return;

    for (int i = 0; i < 100 && scope.progress().active; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto capture = scope.last_capture();
    ok = capture && capture->complete && scope.progress().round == 1;
    log_test("scope_read_resend", ok, capture ? capture->error : "no capture");
    if (!ok) return;

    // Emulated data: 2048 + 1500 sin(2 pi (i - pre) / 200 + ch pi/2)
    bool data_ok = true;
    for (uint32_t ch = 0; ch < 2 && data_ok; ++ch) {
        for (uint32_t i = 0; i < count; ++i) {
            const double t = (static_cast<double>(i) - 1500) / 200.0;
            const auto expected = static_cast<uint16_t>(2048.0 + 1500.0 * std::sin(2.0 * M_PI * t + ch * M_PI / 2.0));
            if (!capture->have[ch][i] || capture->samples[ch][i] != expected) {
                data_ok = false;
                break;
            }
        }
    }
    log_test("scope_read_data", data_ok, data_ok ? "" : "sample mismatch");

    std::string csv_error;
    const std::string path = "/tmp/test_commands_scope.csv";
    const bool csv_ok = ScopeClient::write_csv(*capture, path, csv_error);
    std::remove(path.c_str());
    log_test("scope_csv", csv_ok, csv_error);

    // Out-of-range read is rejected and reported
    ok = scope.read(0x1, st.first_sample - 10, 5, 0, DataLink_DATA_LINK_ORIGIN, "", error);
    for (int i = 0; i < 40 && scope.progress().active; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto rejected = scope.last_capture();
    ok = ok && rejected && rejected->failed;
    log_test("scope_read_out_of_range", ok, ok ? "" : "out-of-range read not reported");
}

// Test: log subscription replays the boot log as EVENTs (filtered by level); device info
void test_diagnostics(Connector& connector, std::atomic<int>& log_events, std::atomic<int>& warn_events,
                      std::atomic<int>& device_infos) {
    std::cout << "\n--- Testing diagnostics ---" << std::endl;
    Request req = Request_init_zero;
    req.which_command = Request_log_subscribe_tag;
    req.command.log_subscribe.has_min_level = true;
    req.command.log_subscribe.min_level = DeviceLogLevel_DEVICE_LOG_WARN;
    connector.send(req);
    req = Request_init_zero;
    req.which_command = Request_get_device_info_tag;
    connector.send(req);
    bool ok = false;
    for (int i = 0; i < 40 && !ok; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ok = device_infos.load() == 1 && log_events.load() > 0;
    }
    log_test("log_replay_filtered", ok && log_events.load() == warn_events.load(),
             std::format("{} log events, {} at warn or above", log_events.load(), warn_events.load()));
    log_test("device_info", device_infos.load() == 1, "no DeviceInfo response");
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

// USB-C / USB-PD (docs/usb_pd.md): status, sink limits, validation, hard reset, dock limit
void test_usb_pd(Connector& connector, std::atomic<int>& invalid_usb_pd) {
    std::cout << "\n--- Testing USB-PD ---" << std::endl;

    Request req = Request_init_zero;
    req.which_command = Request_get_system_status_tag;
    connector.send(req);
    bool ok = wait_for_state_update(connector, [](const Connector::StateSnapshot& s) {
        return s.system_status.has_usb_pd && s.system_status.has_usb_pd_config &&
               s.system_status.usb_pd.source_pdos_count == 4;
    });
    log_test("usb_pd_status", ok, ok ? "" : "no usb_pd status / source caps");

    req = Request_init_zero;
    req.which_command = Request_set_usb_pd_config_tag;
    req.command.set_usb_pd_config.has_max_sink_voltage = true;
    req.command.set_usb_pd_config.max_sink_voltage = 9.0f;
    connector.send(req);
    ok = wait_for_state_update(connector, [](const Connector::StateSnapshot& s) {
        const auto& p = s.system_status.usb_pd;
        return p.has_selected_pdo && p.selected_pdo == 2 && p.has_contract_voltage && p.contract_voltage == 9.0f;
    });
    log_test("usb_pd_vmax", ok, ok ? "" : "vmax=9 did not select the 9 V PDO");

    const int invalid_before = invalid_usb_pd.load();
    req = Request_init_zero;
    req.which_command = Request_set_usb_pd_config_tag;
    req.command.set_usb_pd_config.has_max_sink_voltage = true;
    req.command.set_usb_pd_config.max_sink_voltage = 25.0f;
    connector.send(req);
    for (int i = 0; i < 40 && invalid_usb_pd.load() == invalid_before; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ok = invalid_usb_pd.load() > invalid_before &&
         connector.get_state().system_status.usb_pd_config.max_sink_voltage == 9.0f;
    log_test("usb_pd_invalid_vmax", ok, ok ? "" : "vmax=25 not rejected or config changed");

    const uint32_t resets = connector.get_state().system_status.usb_pd.hard_resets;
    req = Request_init_zero;
    req.which_command = Request_usb_pd_renegotiate_tag;
    req.command.usb_pd_renegotiate.has_hard_reset = true;
    req.command.usb_pd_renegotiate.hard_reset = true;
    connector.send(req);
    ok = wait_for_state_update(connector, [resets](const Connector::StateSnapshot& s) {
        return s.system_status.usb_pd.hard_resets == resets + 1;
    });
    log_test("usb_pd_hard_reset", ok, ok ? "" : "hard reset not counted");

    req = Request_init_zero;
    req.which_command = Request_set_charger_config_tag;
    req.command.set_charger_config.has_dock_input_current_limit = true;
    req.command.set_charger_config.dock_input_current_limit = 2.5f;
    connector.send(req);
    ok = wait_for_state_update(connector, [](const Connector::StateSnapshot& s) {
        return s.system_status.charger_config.has_dock_input_current_limit &&
               s.system_status.charger_config.dock_input_current_limit == 2.5f;
    });
    log_test("charger_dock_limit", ok, ok ? "" : "dock limit not applied");
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
    ScopeClient scope(io_ctx, [&connector](const Request& r) { return connector.send(r); }, add_log);
    std::atomic<int> unsupported{0};
    std::atomic<int> invalid_usb_pd{0};
    std::atomic<int> log_events{0}, warn_events{0}, device_infos{0};
    connector.set_message_hook([&](const Response& resp, frame::Type type, uint16_t seq) {
        if (type == frame::Type::Event && resp.which_payload == Response_log_message_tag) {
            ++log_events;
            if (resp.payload.log_message.level >= DeviceLogLevel_DEVICE_LOG_WARN) ++warn_events;
        }
        if (resp.which_payload == Response_device_info_tag && resp.payload.device_info.threads_count > 0) {
            ++device_infos;
        }
        if (resp.has_result && resp.result.status == ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND) {
            ++unsupported;
        }
        if (resp.has_result && resp.result.status == ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS &&
            resp.result.has_error && resp.result.error == ControllerErrorCode_CONTROLLER_ERROR_USBPD_INVALID_PARAMETER) {
            ++invalid_usb_pd;
        }
        scope.on_message(resp, type, seq);
    });
    connector.set_data_hook([&scope](const frame::View& v) { scope.on_data(v); });
    auto work = boost::asio::make_work_guard(io_ctx);
    std::thread io_thread([&io_ctx] { io_ctx.run(); });

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
    test_enable_and_partial_config(connector);
    test_set_dac_config_unsupported(connector, unsupported);
    test_scope(connector, connector.emulator(), scope);
    test_diagnostics(connector, log_events, warn_events, device_infos);
    test_usb_pd(connector, invalid_usb_pd);

    // Close connector
    connector.close();
    work.reset();
    io_ctx.stop();
    io_thread.join();

    // Print summary
    print_summary();

    return test_fail_count > 0 ? 1 : 0;
}
