#include "emulator_connector.hpp"
#include "connector.hpp"
#include "pb_encode.h"
#include "pb_decode.h"
#include "protocol.pb.h"
#include <chrono>
#include <boost/log/trivial.hpp>

EmulatorConnector::EmulatorConnector(boost::asio::io_context& ctx,
                                      HardwareEmulator& emulator,
                                      std::function<void()> refresh,
                                      std::function<void(std::string)> add_log)
    : emulator_(emulator),
      refresh_(std::move(refresh)),
      add_log_(std::move(add_log)),
      response_timer_(ctx) {}

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

void EmulatorConnector::send(const Request& req) {
    BOOST_LOG_TRIVIAL(info) << "EmulatorConnector::send() called";
    if (add_log_) {
        add_log_("Sending request to emulator");
    }

    // First process the command to update emulator state
    emulator_.send_request(req);

    // Then get the response (which will include the updated state)
    auto response_bytes = emulator_.get_response_for_request(req);

    if (!response_bytes.empty()) {
        // Get the size from the header (size field is at index 8)
        if (response_bytes.size() > 8) {
            uint8_t encoded_size = static_cast<uint8_t>(response_bytes[8]);
            BOOST_LOG_TRIVIAL(debug) << "EmulatorConnector: response_size=" << response_bytes.size() << ", encoded_size=" << (int)encoded_size << ", min_required=" << (12 + encoded_size);

            // Debug: print first few bytes of header
            BOOST_LOG_TRIVIAL(debug) << "EmulatorConnector: header bytes [0-11]:";
            for (int i = 0; i < 12; i++) {
                BOOST_LOG_TRIVIAL(debug) << "  [" << i << "] = 0x" << std::hex << (int)(uint8_t)response_bytes[i] << std::dec;
            }

            BOOST_LOG_TRIVIAL(info) << "EmulatorConnector: Checking condition: encoded_size=" << (int)encoded_size << " > 0 && response_bytes.size()=" << response_bytes.size() << " >= " << (12 + encoded_size);
            BOOST_LOG_TRIVIAL(info) << "EmulatorConnector: encoded_size > 0: " << (encoded_size > 0);
            BOOST_LOG_TRIVIAL(info) << "EmulatorConnector: size check: " << response_bytes.size() << " >= " << (12 + encoded_size) << " = " << (response_bytes.size() >= 12 + encoded_size);

            if (encoded_size > 0 && response_bytes.size() >= 12 + encoded_size) {
                // Decode and process the response
                BOOST_LOG_TRIVIAL(debug) << "EmulatorConnector: Decoding response (size=" << encoded_size << ")";
                pb_istream_t stream = pb_istream_from_buffer(
                    (pb_byte_t*)response_bytes.data() + 12, // Skip full header (12 bytes)
                    encoded_size
                );

                Response resp = Response_init_zero;
                if (pb_decode(&stream, Response_fields, &resp)) {
                    BOOST_LOG_TRIVIAL(debug) << "EmulatorConnector: Decoded response payload type=" << resp.which_payload;
                    // Update connector state directly based on response type
                    if (connector_) {
                        switch (resp.which_payload) {
                            case Response_pong_tag:
                                BOOST_LOG_TRIVIAL(info) << "EmulatorConnector: Processing pong response";
                                connector_->record_ping();
                                break;
                            case Response_system_status_tag:
                                BOOST_LOG_TRIVIAL(info) << "EmulatorConnector: Processing system_status response, has_charger_config=" << resp.payload.system_status.has_charger_config;
                                connector_->update_system_status(resp.payload.system_status);
                                break;
                            default:
                                BOOST_LOG_TRIVIAL(info) << "EmulatorConnector: Unknown response payload type=" << resp.which_payload;
                                break;
                        }
                        // Trigger UI refresh
                        refresh_();
                    } else {
                        BOOST_LOG_TRIVIAL(error) << "EmulatorConnector: connector_ is null!";
                    }
                } else {
                    BOOST_LOG_TRIVIAL(error) << "Failed to decode response";
                }
            } else {
                BOOST_LOG_TRIVIAL(error) << "Response too small: size=" << response_bytes.size() << ", required=" << (12 + encoded_size);
            }
        } else {
            BOOST_LOG_TRIVIAL(error) << "Response too small";
        }
    }

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
