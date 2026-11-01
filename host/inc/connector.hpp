#ifndef __CONNECTOR_HPP__
#define __CONNECTOR_HPP__

#include <boost/asio/io_context.hpp>
#include <boost/asio/serial_port.hpp>
#include <boost/system/detail/error_code.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cli.hpp"
#include "frame.hpp"
#include "protocol.pb.h"

// Forward declarations - always needed for pimpl pattern
class HardwareEmulator;
class EmulatorConnector;
struct EmulatorStateWrapper;

/**
 * @brief Host side of the controller link (protocol/inc/frame.hpp framing)
 *
 * Requests go out on the control port as REQUEST frames with an increasing seq; RESPONSE,
 * EVENT and DATA frames are parsed from the control port and, if opened, from the aux data
 * port. All callbacks (hooks, refresh, logging) run on the io_context thread. send() may
 * be called from any thread.
 */
class Connector {
public:
    struct StateSnapshot {
        std::time_t last_ping{0};
        std::time_t last_status{0};
        SystemStatus system_status{};
        bool has_scope_status{false};
        ScopeStatus scope_status{};
        uint32_t aux_baudrate{0};          // device side, from DataLinkStatus (0 = unknown)
        frame::ParserStats link_stats{};   // control port
        frame::ParserStats aux_stats{};    // aux data port
    };

    using MessageHook = std::function<void(const Response&, frame::Type type, uint16_t seq)>;
    using DataHook = std::function<void(const frame::View&)>;

    Connector(boost::asio::io_context &ctx, std::function<void()> refresh, std::function<void(std::string)> add_log = nullptr);
    ~Connector();

    bool open(const Options &cfg, bool use_emulator = false);
    void async_run_receiver() noexcept;
    void close() noexcept;

    /** @brief Encode and queue a request. @return its frame seq (correlates the response) */
    uint16_t send(const Request &req) noexcept;

    /** @brief Change the host baud rate of the aux data port (no-op if it is not open) */
    bool set_aux_baudrate(unsigned int baudrate) noexcept;
    bool aux_open() const noexcept { return aux_port.is_open(); }

    /** @brief Extra consumers of decoded messages / DATA frames (set before open) */
    void set_message_hook(MessageHook hook) { message_hook = std::move(hook); }
    void set_data_hook(DataHook hook) { data_hook = std::move(hook); }

    /** @brief Feed raw bytes as if received on the control (aux=false) or aux port */
    void feed(const uint8_t *bytes, std::size_t len, bool aux = false) noexcept;

    boost::asio::io_context &io() noexcept { return io_ctx; }

    /** @brief The emulator in emulator mode (tests), nullptr otherwise */
    HardwareEmulator *emulator() noexcept;

    StateSnapshot get_state() const noexcept;
    void record_ping() noexcept;
    void update_system_status(const SystemStatus &status) noexcept;

private:
    boost::asio::io_context &io_ctx;
    std::function<void()> refresh;
    std::function<void(std::string)> add_log;
    MessageHook message_hook;
    DataHook data_hook;

    mutable std::mutex state_mutex;
    StateSnapshot state;

    bool use_emulator{false};
    std::atomic<uint16_t> next_seq{1};

    boost::asio::serial_port port;
    boost::asio::serial_port aux_port;
    std::array<uint8_t, 4096> rx_buffer{};
    std::array<uint8_t, 4096> aux_rx_buffer{};
    frame::Parser parser;
    frame::Parser aux_parser;

    std::deque<std::vector<uint8_t>> write_queue;   // io thread only
    bool writing{false};

#if USE_EMULATOR
    std::unique_ptr<EmulatorStateWrapper> emulator_state;
#endif

    bool configure_port(boost::asio::serial_port &p, const std::string &name, const Options &cfg, unsigned int baud);
    void read_control() noexcept;
    void read_aux() noexcept;
    void start_write() noexcept;
    void on_frame(const frame::View &view, bool aux) noexcept;
    void handle_response(const Response &resp, frame::Type type) noexcept;
};

#endif
