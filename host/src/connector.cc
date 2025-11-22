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
#include <cstddef>
#include <cstring>
#include <functional>
#include <iterator>
#include <sys/stat.h>
#include <vector>
#include <boost/log/trivial.hpp>
#include <boost/crc.hpp>

Connector::Connector(boost::asio::io_context &io_context, std::function<void()> refresh)
    : port(io_context), refresh(std::move(refresh)) {
    state.last_ping = 0;
}

Connector::~Connector() {
    if (port.is_open())
        port.close();
}

void Connector::async_run_receiver() noexcept {
    port.async_read_some(
        boost::asio::buffer(data),
        std::bind(&Connector::process_data, this, std::placeholders::_1, std::placeholders::_2)
    );
}

void Connector::process_data(boost::system::error_code ec, std::size_t bytes_written) noexcept {
    if (ec) {
        eat_bytes(bytes_written);
        async_run_receiver();
    } else {
        BOOST_LOG_TRIVIAL(error) << "Failed to read from serial port";
        // TODO: handle cli
    }
}

void Connector::eat_bytes(std::size_t bytes_written) noexcept {
    parse_buffer.insert(parse_buffer.cend(), data.begin(), data.begin() + bytes_written);

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

    if (std::distance(start, parse_buffer.end()) > (int) MESSAGE_HEADER_SIZE) {
        auto body = std::next(start, MESSAGE_HEADER_SIZE);
        std::copy(start, body, msg.raw);

        if (std::distance(body, parse_buffer.end()) > msg.fields.size) {
            auto body_end = std::next(body, msg.fields.size);
            std::copy(body, body_end, msg.fields.data);

            boost::crc_32_type result;
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
        case Response_pong_tag:
            BOOST_LOG_TRIVIAL(debug) << "Received pong\n";
            state.last_ping = std::time(nullptr);
            break;
    }
}

void Connector::send(const Request &req) noexcept {
    message msg;
    pb_ostream_t stream = pb_ostream_from_buffer((pb_byte_t *) msg.fields.data, sizeof(msg.fields.data));
    bool status = pb_encode(&stream, Request_fields, &req);

    if (!status) {
        BOOST_LOG_TRIVIAL(error) << "Failed to encode request\n";
        return;
    }

    msg.fields.size = stream.bytes_written;

    boost::crc_32_type crc;
    crc.process_bytes(msg.fields.data, msg.fields.size);
    msg.fields.crc32 = crc.checksum();

    output_buffer.push_back(
        std::vector(&msg.raw[0], &msg.raw[GET_MESSAGE_SIZE(msg)])
    );

    auto it = output_buffer.cend();
    port.async_write_some(boost::asio::const_buffer(it->data(), it->size()), [=, this](boost::system::error_code ec, std::size_t ) {
        if (!ec)
            BOOST_LOG_TRIVIAL(error) << "Failed to send message\n";

        output_buffer.erase(it);
    });
}

bool Connector::open(const Options &cfg) {
    boost::system::error_code ec;
    port.open(cfg.port, ec);

    if (!ec) {
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
    port.cancel();
    port.close();
}

const Connector::State &Connector::get_state() const noexcept {
    return state;
}
