#include "connector.hpp"
#include "cli.hpp"
#include "message.hpp"
#include "pb.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include "protocol.pb.h"
#include <algorithm>
#include <array>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/registered_buffer.hpp>
#include <boost/asio/serial_port_base.hpp>
#include <boost/system/detail/error_code.hpp>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <execution>
#include <functional>
#include <iterator>
#include <sys/stat.h>
#include <vector>
#include <boost/log/trivial.hpp>
#include <boost/crc.hpp>

#if USE_EMULATOR
#include "emulator_connector.hpp"
#include "hardware_emulator.hpp"
#endif

// Define EmulatorStateWrapper for pimpl pattern
#if USE_EMULATOR
struct EmulatorStateWrapper {
    std::unique_ptr<HardwareEmulator> emulator;
    std::unique_ptr<EmulatorConnector> emulator_connector;
};
#endif

// STM32 hardware accelerated CRC32 config
using CRC32 = boost::crc_optimal<
    32,
    0x04C11DB7,
    0xFFFFFFFF,
    0x00000000,
    false,  // reflect input
    false   // reflect output
>;

Connector::Connector(boost::asio::io_context &io_context, std::function<void()> refresh, std::function<void(std::string)> add_log)
    : port(io_context) {
    state.last_ping = 0;

    this->refresh = std::move(refresh);
    this->add_log = std::move(add_log);

#if USE_EMULATOR
    // Emulator mode is controlled at runtime via open() parameter
    emulator_state = std::make_unique<EmulatorStateWrapper>();
    emulator_state->emulator = std::make_unique<HardwareEmulator>(
        [](const std::string& msg) {
            BOOST_LOG_TRIVIAL(info) << "[Emulator] " << msg;
        },
        [](const std::string& msg) {
            BOOST_LOG_TRIVIAL(debug) << "[Packet] " << msg;
        }
    );
    emulator_state->emulator_connector = std::make_unique<EmulatorConnector>(
        io_context, *emulator_state->emulator, this->refresh, this->add_log
    );
    
    // Set the connector pointer so emulator can update state
    emulator_state->emulator_connector->set_connector(this);

    BOOST_LOG_TRIVIAL(info) << "Using Hardware Emulator mode";
#else
    BOOST_LOG_TRIVIAL(info) << "Using Serial Port mode";
#endif
}

Connector::~Connector() {
    close();
}

void Connector::async_run_receiver() noexcept {
#if USE_EMULATOR
    if (use_emulator && emulator_state && emulator_state->emulator_connector) {
        emulator_state->emulator_connector->async_run_receiver();
        return;
    }
#endif
    port.async_read_some(
        boost::asio::buffer(data),
        std::bind(&Connector::process_data, this, std::placeholders::_1, std::placeholders::_2)
    );
}

void Connector::process_data(boost::system::error_code ec, std::size_t bytes_written) noexcept {
    if (!ec) {
        eat_bytes(bytes_written);
        async_run_receiver();
    } else {
        BOOST_LOG_TRIVIAL(error) << "Failed to read from serial port";
        // TODO: handle cli
    }
}

void Connector::eat_bytes(std::size_t bytes_written) noexcept {
    parse_buffer.insert(parse_buffer.cend(), data.begin(), data.begin() + bytes_written);

    auto id_pos = std::search(parse_buffer.cbegin(), parse_buffer.cend(), &MESSAGE_HEADER_ID[0], &MESSAGE_HEADER_ID[3]);
    if (id_pos != parse_buffer.cbegin())
        parse_buffer.erase(parse_buffer.cbegin(), id_pos);

    auto it = parse_buffer.begin();
    auto parsed_it = try_parse_message(it);

    while (it != parsed_it) {
        it = parsed_it;
        parsed_it = try_parse_message(it);
    }

    parse_buffer.erase(parse_buffer.begin(), parsed_it);
}
std::vector<std::byte>::iterator Connector::try_parse_message(std::vector<std::byte>::iterator start) {
    message msg;

    if (std::distance(start, parse_buffer.end()) >= (int) MESSAGE_HEADER_SIZE) {
        auto body = std::next(start, MESSAGE_HEADER_SIZE);
        std::copy(start, body, msg.raw);

        if (std::distance(body, parse_buffer.end()) >= msg.fields.size) {
            auto body_end = std::next(body, msg.fields.size);
            std::copy(body, body_end, msg.fields.data);

            CRC32 result;
            result.process_bytes(msg.fields.data, msg.fields.size);

            if (result.checksum() != msg.fields.crc32) {
                BOOST_LOG_TRIVIAL(error) << "Message has an invalid crc32 checksum " <<
                    std::hex << msg.fields.crc32 << " != " << result.checksum() << "\n";

                return body_end;
            }

            Response resp = Response_init_zero;
            pb_istream_t stream = pb_istream_from_buffer((pb_byte_t *) msg.fields.data, msg.fields.size);
            bool status = pb_decode(&stream, Response_fields, &resp);

            if (!status) {
                BOOST_LOG_TRIVIAL(error) << "Failed to decode message";

                return body_end;
            }

            handle_response(resp);

            return body_end;
        }
    }

    return start;
}

void Connector::handle_response(const Response &resp) noexcept {
    switch (resp.which_payload) {
        case Response_pong_tag: {
            BOOST_LOG_TRIVIAL(debug) << "Received pong\n";
            record_ping();
            refresh();
            if (add_log) {
                add_log("Received: pong");
            }
            break;
        }

        case Response_system_status_tag: {
            BOOST_LOG_TRIVIAL(debug) << "Received system status\n";
            update_system_status(resp.payload.system_status);
            refresh();
            break;
        }

        case Response_applet_status_tag: {
            BOOST_LOG_TRIVIAL(debug) << "Received applet status: exists="
                << resp.payload.applet_status.exists
                << " running=" << resp.payload.applet_status.running
                << " status_code=" << (int)resp.payload.applet_status.status_code
                << " text=" << resp.payload.applet_status.text << "\n";
            if (add_log) {
                std::string msg = "Applet: ";
                if (resp.payload.applet_status.exists) {
                    msg += resp.payload.applet_status.running ? "running" : "stopped";
                } else {
                    msg += "not found";
                }
                msg += " - " + std::string(resp.payload.applet_status.text);
                add_log(msg);
            }
            refresh();
            break;
        }

        case Response_applet_list_response_tag: {
            BOOST_LOG_TRIVIAL(debug) << "Received applet list with "
                << resp.payload.applet_list_response.applet_names_count << " applets\n";
            if (add_log) {
                std::string msg = "Applet list: ";
                if (resp.payload.applet_list_response.applet_names_count == 0) {
                    msg += "No applets registered";
                } else {
                    msg += "Available: ";
                    for (uint32_t i = 0; i < resp.payload.applet_list_response.applet_names_count; ++i) {
                        if (i > 0) msg += ", ";
                        msg += std::string(resp.payload.applet_list_response.applet_names[i]);
                    }
                }
                add_log(msg);
            }
            refresh();
            break;
        }
    }
}

void Connector::send(const Request &req) noexcept {
    message msg;
    std::memset(msg.raw, 0, sizeof(msg.raw));
    std::memcpy(msg.fields.header, MESSAGE_HEADER_ID, sizeof(MESSAGE_HEADER_ID));
    pb_ostream_t stream = pb_ostream_from_buffer((pb_byte_t *) msg.fields.data, sizeof(msg.fields.data));
    bool status = pb_encode(&stream, Request_fields, &req);

    if (!status) {
        BOOST_LOG_TRIVIAL(error) << "Failed to encode request\n";
        return;
    }

    msg.fields.size = stream.bytes_written;

    CRC32 crc;
    crc.process_bytes(msg.fields.data, msg.fields.size);
    msg.fields.crc32 = crc.checksum();

#if USE_EMULATOR
    if (use_emulator && emulator_state && emulator_state->emulator_connector) {
        emulator_state->emulator_connector->send(req);
        return;
    }
#endif
    auto it = output_buffer.insert(output_buffer.cend(),
        std::vector(&msg.raw[0], &msg.raw[GET_MESSAGE_SIZE(msg)])
    );
    port.async_write_some(boost::asio::const_buffer(it->data(), it->size()), [=, this](boost::system::error_code ec, std::size_t ) {
        if (ec)
            BOOST_LOG_TRIVIAL(error) << "Failed to send message" << ec << "\n";

        output_buffer.erase(it);
    });
}

bool Connector::open(const Options &cfg, bool use_emulator) {
    this->use_emulator = use_emulator;
    BOOST_LOG_TRIVIAL(debug) << "Connector::open() use_emulator=" << use_emulator;

#if USE_EMULATOR
    if (use_emulator && emulator_state && emulator_state->emulator_connector) {
        BOOST_LOG_TRIVIAL(debug) << "Using emulator connector";
        return emulator_state->emulator_connector->open(cfg);
    }
    // If not using emulator or emulator_state/emulator_connector is null, use serial port mode
    BOOST_LOG_TRIVIAL(debug) << "Falling through to serial port mode";
#endif

    // Serial port mode (always available)
    boost::system::error_code ec;
    port.open(cfg.port, ec);

    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "Failed to open port " << ec;
        return false;
    }

    port.set_option(boost::asio::serial_port_base::baud_rate(cfg.baudrate));
    port.set_option(boost::asio::serial_port_base::character_size(cfg.databits));

    boost::asio::serial_port_base::stop_bits::type stop_bits;
    switch (cfg.stopbits) {
        case 1: stop_bits = boost::asio::serial_port_base::stop_bits::one; break;
        case 2: stop_bits = boost::asio::serial_port_base::stop_bits::two; break;
    }
    port.set_option(boost::asio::serial_port_base::stop_bits(stop_bits));

    boost::asio::serial_port_base::parity::type parity =
        boost::asio::serial_port_base::parity::none;
    if (cfg.parity == "odd") {
        parity = boost::asio::serial_port_base::parity::odd;
    } else if (cfg.parity == "even") {
        parity = boost::asio::serial_port_base::parity::even;
    }
    port.set_option(boost::asio::serial_port_base::parity(parity));

    port.set_option(boost::asio::serial_port_base::flow_control(
        boost::asio::serial_port_base::flow_control::none));

    return true;
}

void Connector::close() noexcept {
#if USE_EMULATOR
    if (use_emulator && emulator_state && emulator_state->emulator_connector) {
        emulator_state->emulator_connector->close();
    } else {
        boost::system::error_code ec;
        port.cancel(ec);
        port.close(ec);
    }
#else
    boost::system::error_code ec;
    port.cancel(ec);
    port.close(ec);
#endif
}

Connector::StateSnapshot Connector::get_state() const noexcept {
    std::lock_guard<std::mutex> lock(state.mutex);
    return {state.last_ping, state.system_status};
}

void Connector::record_ping() noexcept {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.last_ping = std::time(nullptr);
}

void Connector::update_system_status(const SystemStatus &status) noexcept {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.system_status = status;
}
