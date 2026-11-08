#include "connector.hpp"
#include "cli.hpp"
#include "error_host.hpp"
#include "pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "protocol.pb.h"
#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/serial_port_base.hpp>
#include <boost/asio/write.hpp>
#include <boost/log/trivial.hpp>
#include <cstring>

#if USE_EMULATOR
#include "emulator_connector.hpp"
#include "hardware_emulator.hpp"
#endif

#if USE_EMULATOR
struct EmulatorStateWrapper {
    std::unique_ptr<HardwareEmulator> emulator;
    std::unique_ptr<EmulatorConnector> emulator_connector;
};
#endif

namespace {

const char* status_name(ResponseStatusCode status) {
    switch (status) {
        case ResponseStatusCode_RESPONSE_STATUS_SUCCESS:             return "success";
        case ResponseStatusCode_RESPONSE_STATUS_FAILURE:             return "failure";
        case ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND:           return "not found";
        case ResponseStatusCode_RESPONSE_STATUS_ALREADY_EXISTS:      return "already exists";
        case ResponseStatusCode_RESPONSE_STATUS_INVALID_STATE:       return "invalid state";
        case ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS:  return "invalid parameters";
        case ResponseStatusCode_RESPONSE_STATUS_TIMEOUT:             return "timeout";
        case ResponseStatusCode_RESPONSE_STATUS_NO_MEMORY:           return "no memory";
        case ResponseStatusCode_RESPONSE_STATUS_NO_PERMISSION:       return "no permission";
        case ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND: return "unsupported command";
        default:                                                     return "unknown status";
    }
}

} // namespace

Connector::Connector(boost::asio::io_context &io_context, std::function<void()> refresh, std::function<void(std::string)> add_log)
    : io_ctx(io_context),
      refresh(std::move(refresh)),
      add_log(std::move(add_log)),
      port(io_context),
      aux_port(io_context) {
#if USE_EMULATOR
    emulator_state = std::make_unique<EmulatorStateWrapper>();
    emulator_state->emulator = std::make_unique<HardwareEmulator>(
        [](const std::string& msg) { BOOST_LOG_TRIVIAL(info) << "[Emulator] " << msg; },
        [](const std::string& msg) { BOOST_LOG_TRIVIAL(debug) << "[Packet] " << msg; }
    );
    emulator_state->emulator_connector = std::make_unique<EmulatorConnector>(
        io_context, *emulator_state->emulator, this->refresh, this->add_log
    );
    emulator_state->emulator_connector->set_connector(this);
#endif
}

Connector::~Connector() {
    close();
}

HardwareEmulator *Connector::emulator() noexcept {
#if USE_EMULATOR
    if (use_emulator && emulator_state) {
        return emulator_state->emulator.get();
    }
#endif
    return nullptr;
}

// =============================================================================
// Open / close
// =============================================================================

bool Connector::configure_port(boost::asio::serial_port &p, const std::string &name, const Options &cfg, unsigned int baud) {
    boost::system::error_code ec;
    p.open(name, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "Failed to open port " << name << ": " << ec.message();
        return false;
    }

    boost::asio::serial_port_base::stop_bits::type stop_bits =
        cfg.stopbits == 2 ? boost::asio::serial_port_base::stop_bits::two : boost::asio::serial_port_base::stop_bits::one;
    boost::asio::serial_port_base::parity::type parity = boost::asio::serial_port_base::parity::none;
    if (cfg.parity == "odd") {
        parity = boost::asio::serial_port_base::parity::odd;
    } else if (cfg.parity == "even") {
        parity = boost::asio::serial_port_base::parity::even;
    }

    p.set_option(boost::asio::serial_port_base::baud_rate(baud), ec);
    if (!ec) p.set_option(boost::asio::serial_port_base::character_size(cfg.databits), ec);
    if (!ec) p.set_option(boost::asio::serial_port_base::stop_bits(stop_bits), ec);
    if (!ec) p.set_option(boost::asio::serial_port_base::parity(parity), ec);
    if (!ec) p.set_option(boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::none), ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "Failed to configure port " << name << ": " << ec.message();
        p.close(ec);
        return false;
    }
    return true;
}

bool Connector::open(const Options &cfg, bool use_emulator) {
    this->use_emulator = use_emulator;

#if USE_EMULATOR
    if (use_emulator && emulator_state && emulator_state->emulator_connector) {
        return emulator_state->emulator_connector->open(cfg);
    }
#endif

    if (!configure_port(port, cfg.port, cfg, cfg.baudrate)) {
        return false;
    }
    if (!cfg.aux_port.empty() && !configure_port(aux_port, cfg.aux_port, cfg, cfg.aux_baudrate)) {
        if (add_log) {
            add_log("Aux data port " + cfg.aux_port + " could not be opened; DATA must use the control link");
        }
    }
    return true;
}

void Connector::close() noexcept {
#if USE_EMULATOR
    if (use_emulator && emulator_state && emulator_state->emulator_connector) {
        emulator_state->emulator_connector->close();
        return;
    }
#endif
    boost::system::error_code ec;
    for (auto *p : {&port, &aux_port}) {
        if (p->is_open()) {
            p->cancel(ec);
            p->close(ec);
        }
    }
}

bool Connector::set_aux_baudrate(unsigned int baudrate) noexcept {
    if (!aux_port.is_open()) {
        return false;
    }
    boost::system::error_code ec;
    aux_port.set_option(boost::asio::serial_port_base::baud_rate(baudrate), ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "Failed to set aux baud rate " << baudrate << ": " << ec.message();
        return false;
    }
    aux_parser.reset();
    return true;
}

// =============================================================================
// Receive
// =============================================================================

void Connector::async_run_receiver() noexcept {
#if USE_EMULATOR
    if (use_emulator && emulator_state && emulator_state->emulator_connector) {
        emulator_state->emulator_connector->async_run_receiver();
        return;
    }
#endif
    read_control();
    if (aux_port.is_open()) {
        read_aux();
    }
}

void Connector::read_control() noexcept {
    port.async_read_some(boost::asio::buffer(rx_buffer), [this](boost::system::error_code ec, std::size_t n) {
        if (ec) {
            if (ec != boost::asio::error::operation_aborted) {
                BOOST_LOG_TRIVIAL(error) << "Failed to read from serial port: " << ec.message();
                if (add_log) add_log("Control port read failed: " + ec.message());
            }
            return;
        }
        feed(rx_buffer.data(), n, false);
        read_control();
    });
}

void Connector::read_aux() noexcept {
    aux_port.async_read_some(boost::asio::buffer(aux_rx_buffer), [this](boost::system::error_code ec, std::size_t n) {
        if (ec) {
            if (ec != boost::asio::error::operation_aborted) {
                BOOST_LOG_TRIVIAL(error) << "Failed to read from aux port: " << ec.message();
                if (add_log) add_log("Aux port read failed: " + ec.message());
            }
            return;
        }
        feed(aux_rx_buffer.data(), n, true);
        read_aux();
    });
}

void Connector::feed(const uint8_t *bytes, std::size_t len, bool aux) noexcept {
    frame::Parser &p = aux ? aux_parser : parser;
    p.feed(bytes, len, [this, aux](const frame::View &view) { on_frame(view, aux); });
    std::lock_guard lock(state_mutex);
    (aux ? state.aux_stats : state.link_stats) = p.stats();
}

void Connector::on_frame(const frame::View &view, bool aux) noexcept {
    switch (view.type) {
        case frame::Type::Data:
            if (data_hook) {
                data_hook(view);
            }
            return;

        case frame::Type::Response:
        case frame::Type::Event: {
            static Response resp;   // io thread only; Response is large
            resp = Response_init_zero;
            pb_istream_t stream = pb_istream_from_buffer(view.payload, view.length);
            if (!pb_decode(&stream, Response_fields, &resp)) {
                BOOST_LOG_TRIVIAL(error) << "Failed to decode message: " << PB_GET_ERROR(&stream);
                return;
            }
            handle_response(resp, view.type);
            if (message_hook) {
                message_hook(resp, view.type, view.seq);
            }
            return;
        }

        case frame::Type::Request:
        default:
            BOOST_LOG_TRIVIAL(warning) << "Unexpected frame type " << static_cast<int>(view.type)
                                       << (aux ? " on aux port" : "");
            return;
    }
}

void Connector::handle_response(const Response &resp, frame::Type type) noexcept {
    (void)type;
    if (resp.has_result && resp.result.status != ResponseStatusCode_RESPONSE_STATUS_SUCCESS) {
        std::string msg = std::string("Controller: ") + status_name(resp.result.status);
        if (resp.result.has_error) {
            msg += " - " + get_error_message(static_cast<ControllerError>(resp.result.error)) +
                   " (" + std::to_string(static_cast<int>(resp.result.error)) + ")";
        }
        BOOST_LOG_TRIVIAL(warning) << msg;
        if (add_log) {
            add_log(msg);
        }
    }

    switch (resp.which_payload) {
        case Response_pong_tag:
            record_ping();
            if (add_log) add_log("Received: pong");
            break;

        case Response_system_status_tag:
            update_system_status(resp.payload.system_status);
            break;

        case Response_scope_status_tag: {
            std::lock_guard lock(state_mutex);
            state.has_scope_status = true;
            state.scope_status = resp.payload.scope_status;
            break;
        }

        case Response_data_link_status_tag: {
            std::lock_guard lock(state_mutex);
            state.aux_baudrate = resp.payload.data_link_status.baudrate;
            break;
        }

        case Response_applet_status_tag:
            if (add_log) {
                const auto &st = resp.payload.applet_status;
                std::string msg = "Applet: ";
                msg += st.exists ? (st.running ? "running" : "stopped") : "not found";
                // params / get answer one line per parameter
                std::string text(st.text);
                while (!text.empty() && text.back() == '\n') text.pop_back();
                const auto nl = text.find('\n');
                if (nl == std::string::npos) {
                    add_log(msg + " - " + text);
                } else {
                    add_log(msg);
                    size_t start = 0;
                    while (start <= text.size()) {
                        const auto end = text.find('\n', start);
                        add_log("  " + text.substr(start, end == std::string::npos ? std::string::npos : end - start));
                        if (end == std::string::npos) break;
                        start = end + 1;
                    }
                }
            }
            break;

        case Response_applet_list_response_tag:
            if (add_log) {
                const auto &list = resp.payload.applet_list_response;
                std::string msg = "Applet list: ";
                if (list.applet_names_count == 0) {
                    msg += "No applets registered";
                } else {
                    for (pb_size_t i = 0; i < list.applet_names_count; ++i) {
                        msg += (i ? ", " : "") + std::string(list.applet_names[i]);
                    }
                }
                add_log(msg);
            }
            break;

        default:
            break;
    }
    if (refresh) {
        refresh();
    }
}

// =============================================================================
// Transmit
// =============================================================================

uint16_t Connector::send(const Request &req) noexcept {
    const uint16_t seq = next_seq.fetch_add(1);

#if USE_EMULATOR
    if (use_emulator && emulator_state && emulator_state->emulator_connector) {
        emulator_state->emulator_connector->send(req, seq);
        return seq;
    }
#endif

    std::vector<uint8_t> buffer(frame::kMaxFrame);
    pb_ostream_t stream = pb_ostream_from_buffer(frame::payload_area(buffer.data()), frame::kMaxPayload);
    if (!pb_encode(&stream, Request_fields, &req)) {
        BOOST_LOG_TRIVIAL(error) << "Failed to encode request: " << PB_GET_ERROR(&stream);
        return seq;
    }
    const size_t n = frame::finish(buffer.data(), buffer.size(), frame::Type::Request, 0, seq, stream.bytes_written);
    buffer.resize(n);

    boost::asio::post(io_ctx, [this, buffer = std::move(buffer)]() mutable {
        write_queue.push_back(std::move(buffer));
        if (!writing) {
            start_write();
        }
    });
    return seq;
}

void Connector::start_write() noexcept {
    if (write_queue.empty() || !port.is_open()) {
        writing = false;
        write_queue.clear();
        return;
    }
    writing = true;
    boost::asio::async_write(port, boost::asio::buffer(write_queue.front()),
                             [this](boost::system::error_code ec, std::size_t) {
                                 if (ec) {
                                     BOOST_LOG_TRIVIAL(error) << "Failed to send message: " << ec.message();
                                 }
                                 write_queue.pop_front();
                                 start_write();
                             });
}

// =============================================================================
// State
// =============================================================================

Connector::StateSnapshot Connector::get_state() const noexcept {
    std::lock_guard lock(state_mutex);
    return state;
}

void Connector::record_ping() noexcept {
    std::lock_guard lock(state_mutex);
    state.last_ping = std::time(nullptr);
}

void Connector::update_system_status(const SystemStatus &status) noexcept {
    std::lock_guard lock(state_mutex);
    state.system_status = status;
    state.last_status = std::time(nullptr);
}
