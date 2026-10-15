#ifndef __CONNECTOR_HPP__
#define __CONNECTOR_HPP__

#include <boost/asio/io_context.hpp>
#include <boost/asio/serial_port.hpp>
#include <boost/system/detail/error_code.hpp>
#include <cstddef>
#include <ctime>
#include <functional>
#include <list>
#include <vector>
#include <memory>
#include <mutex>

#include "cli.hpp"
#include "protocol.pb.h"
#include "message.hpp"

// Forward declarations - always needed for pimpl pattern
class HardwareEmulator;
class EmulatorConnector;

// Forward declaration of internal state structure for pimpl when USE_EMULATOR=1
// (Definition is in connector.cc to avoid redefinition with hardware_emulator.hpp)
struct EmulatorStateWrapper;

class Connector {
public:
    struct State {
        std::time_t last_ping{0};
        SystemStatus system_status{};

        mutable std::mutex mutex;
    };

    struct StateSnapshot {
        std::time_t last_ping{0};
        SystemStatus system_status{};
    };

    Connector(boost::asio::io_context &ctx, std::function<void()> refresh, std::function<void(std::string)> add_log = nullptr);
    ~Connector();

    bool open(const Options &cfg, bool use_emulator = false);
    StateSnapshot get_state() const noexcept;
    void record_ping() noexcept;
    void update_system_status(const SystemStatus &status) noexcept;
    void async_run_receiver() noexcept;
    void send(const Request &req) noexcept;
    void close() noexcept;

private:
    std::array<std::byte, MESSAGE_SIZE> data;
    std::vector<std::byte> parse_buffer;
    std::list<std::vector<std::byte>> output_buffer;
    std::function<void()> refresh;
    std::function<void(std::string)> add_log;
    State state;

    bool use_emulator{false};

    // Both modes always available - only one is used at runtime
    boost::asio::serial_port port;
    
    // Emulator members - only declared when USE_EMULATOR=1
#if USE_EMULATOR
    std::unique_ptr<EmulatorStateWrapper> emulator_state;
#endif

    void process_data(boost::system::error_code ec, std::size_t bytes_written) noexcept;
    void eat_bytes(std::size_t bytes_written) noexcept;
    std::vector<std::byte>::iterator try_parse_message(std::vector<std::byte>::iterator start);
    void handle_response(const Response &resp) noexcept;
};

#endif
