/**
 * Hardware Emulator Test Program
 *
 * This program demonstrates how to use the HardwareEmulator to test the TUI
 * without physical hardware.
 *
 * Usage:
 *   ./test_emulator [--packet-log]
 *
 * The emulator simulates:
 * - STM32 resonance converter controller behavior
 * - Protocol buffer message encoding/decoding
 * - Realistic hardware measurements (with noise and drift)
 * - Packet exchange logging
 */

#include "hardware_emulator.hpp"
#include "cli.hpp"
#include "connector.hpp"
#include "tui.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/log/trivial.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <iomanip>

// Forward declaration
std::unique_ptr<HardwareEmulator> g_emulator;

// Packet callback to display exchanged packets
void packet_log_callback(const std::string& msg) {
    std::cout << msg << std::endl;
}

// Log callback for emulator
void emulator_log_callback(const std::string& msg) {
    std::cout << "[Emulator] " << msg << std::endl;
}

int main(int argc, char** argv) {
    bool show_packet_log = false;

    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--packet-log") {
            show_packet_log = true;
        }
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  Hardware Emulator Test Program" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    // Create the emulator
    g_emulator = std::make_unique<HardwareEmulator>(
        emulator_log_callback,
        show_packet_log ? packet_log_callback : nullptr
    );

    // Start the emulator
    g_emulator->start();

    // Get initial state
    auto state = g_emulator->get_state();

    std::cout << "Initial State (SystemStatus):" << std::endl;
    std::cout << "  Charger Enabled: " << (state.system_status.charger.enabled ? "Yes" : "No") << std::endl;
    std::cout << "  Battery Voltage: " << state.system_status.charger_measurements.vbat_voltage << " V" << std::endl;
    std::cout << "  Charge Current:  " << state.system_status.charger_measurements.ibus_current << " A" << std::endl;
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "  To test with TUI, run:" << std::endl;
    std::cout << "  1. Start this emulator: ./test_emulator --packet-log" << std::endl;
    std::cout << "  2. In another terminal, start hostctl with a named pipe:" << std::endl;
    std::cout << "     mkfifo /tmp/emulator_serial" << std::endl;
    std::cout << "     ./hostctl --port /tmp/emulator_serial --baud 115200" << std::endl;
    std::cout << std::endl;
    std::cout << "  Or modify connector.cc to use the emulator directly." << std::endl;
    std::cout << "========================================" << std::endl;

    // Run until interrupted
    std::cout << std::endl;
    std::cout << "Emulator running. Press Ctrl+C to stop..." << std::endl;

    try {
        // Keep running
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Periodically print state
            state = g_emulator->get_state();

            std::cout << "\r[Status] VBAT: "
                      << std::fixed << std::setprecision(2)
                      << state.system_status.charger_measurements.vbat_voltage
                      << " V, I: " << state.system_status.charger_measurements.ibus_current
                      << " A      " << std::flush;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    // Stop the emulator
    g_emulator->stop();

    // Print packet log if requested
    if (show_packet_log) {
        std::cout << std::endl << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "  Packet Exchange Log (Last 50)" << std::endl;
        std::cout << "========================================" << std::endl;

        auto log = g_emulator->get_packet_log();
        size_t start = (log.size() > 50) ? log.size() - 50 : 0;

        for (size_t i = start; i < log.size(); i++) {
            std::cout << log[i] << std::endl;
        }
    }

    std::cout << std::endl << "Emulator stopped." << std::endl;

    return 0;
}
