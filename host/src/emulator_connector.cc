#include "emulator_connector.hpp"
#include "connector.hpp"
#include "pb_encode.h"
#include "pb_decode.h"
#include "protocol.pb.h"
#include <boost/asio/post.hpp>
#include <chrono>
#include <boost/log/trivial.hpp>

EmulatorConnector::EmulatorConnector(boost::asio::io_context& ctx,
                                      HardwareEmulator& emulator,
                                      std::function<void()> refresh,
                                      std::function<void(std::string)> add_log)
    : emulator_(emulator),
      refresh_(std::move(refresh)),
      add_log_(std::move(add_log)),
      response_timer_(ctx),
      io_(ctx) {}

EmulatorConnector::~EmulatorConnector() {
    close();
}

bool EmulatorConnector::open(const Options&) {
    emulator_.start();
    open_.store(true);
    // Starting the emulator simulates opening a serial port.
    if (add_log_) {
        add_log_("Emulator: Connected to virtual STM32 controller");
    }
    BOOST_LOG_TRIVIAL(info) << "EmulatorConnector: Opened virtual port, connector_=" << (connector_ ? "not null" : "null");

    // Initialize connector state from emulator's initial state
    if (connector_) {
        HardwareEmulator::State emu_state = emulator_.get_state();
        connector_->record_ping();
        connector_->update_system_status(emu_state.system_status);

        BOOST_LOG_TRIVIAL(info) << "EmulatorConnector: Initialized state from emulator";

        // Don't call refresh_() here - the TUI isn't running yet
        // The first refresh will happen when the UI starts
    } else {
        BOOST_LOG_TRIVIAL(warning) << "EmulatorConnector: connector_ is null in open()";
    }

    return true;
}

void EmulatorConnector::set_connector(Connector* conn) {
    connector_ = conn;
}

void EmulatorConnector::async_run_receiver() {
    // Start the response processing loop
    process_response();
}

void EmulatorConnector::send(const Request& req, uint16_t seq) {
    // Run on the io thread like real serial traffic, so all Connector callbacks share a thread.
    boost::asio::post(io_, [this, req, seq] {
        if (!open_.load()) {
            return;
        }
        emulator_.send_request(req);
        const auto bytes = emulator_.get_response_for_request(req, seq);
        if (connector_ && !bytes.empty()) {
            connector_->feed(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        }
    });
}

void EmulatorConnector::process_response() {
    // The emulator processes requests immediately and sends responses back
    // In a real serial implementation, responses would come asynchronously

    // For simulation, we'll just trigger a refresh to show the updated state
    refresh_();

    response_timer_.expires_after(std::chrono::milliseconds(100));
    response_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        if (ec) {
            BOOST_LOG_TRIVIAL(error) << "Emulator response timer failed: " << ec.message();
            return;
        }
        process_response();
    });
}

HardwareEmulator::State EmulatorConnector::get_state() const {
    return emulator_.get_state();
}

void EmulatorConnector::close() {
    if (!open_.exchange(false)) {
        return;
    }
    response_timer_.cancel();
    emulator_.stop();
    if (add_log_) {
        add_log_("Emulator: Disconnected");
    }
    BOOST_LOG_TRIVIAL(info) << "EmulatorConnector: Closed";
}
