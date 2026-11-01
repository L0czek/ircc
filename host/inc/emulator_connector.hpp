#ifndef __EMULATOR_CONNECTOR_HPP__
#define __EMULATOR_CONNECTOR_HPP__

#include "hardware_emulator.hpp"
#include "protocol.pb.h"
#include <boost/asio.hpp>
#include <atomic>
#include <functional>
#include <queue>
#include <vector>
#include <mutex>

#include "cli.hpp"

// Forward declaration for Connector
class Connector;

/**
 * EmulatorConnector provides a serial_port-like interface to the HardwareEmulator
 * so the existing TUI code can work without modification.
 *
 * This class bridges the gap between the Connector class (which expects a serial port)
 * and the HardwareEmulator (which simulates the STM32 controller).
 */
class EmulatorConnector {
public:
    EmulatorConnector(boost::asio::io_context& ctx,
                      HardwareEmulator& emulator,
                      std::function<void()> refresh,
                      std::function<void(std::string)> add_log = nullptr);
    ~EmulatorConnector();

    // Simulate opening a port (actually starts the emulator)
    bool open(const Options& cfg);

    // Start receiving responses
    void async_run_receiver();

    // Send a request; the emulator's frames are fed back through Connector::feed()
    void send(const Request& req, uint16_t seq);

    // Get current state
    HardwareEmulator::State get_state() const;

    // Close (stop)
    void close();

    // Set the Connector for response handling
    void set_connector(Connector* conn);

private:
    HardwareEmulator& emulator_;
    std::function<void()> refresh_;
    std::function<void(std::string)> add_log_;
    Connector* connector_{nullptr};
    std::atomic<bool> open_{false};

    // Response queue
    std::queue<std::vector<std::byte>> response_queue_;
    std::mutex queue_mutex_;

    // Async read handler
    void process_response();

    // Timer for async simulation
    boost::asio::steady_timer response_timer_;
    boost::asio::io_context& io_;
};

#endif
