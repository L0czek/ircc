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

#include "cli.hpp"
#include "protocol.pb.h"
#include "message.hpp"

class Connector {
public:
    struct State {
        std::time_t last_ping{0};
        ChargerStatus charger_status{};
        ChargerMeasurements charger_measurements{};
        ChargerConfig charger_config{};

        BackBoostStatus backboost_status{};
        BackBoostMeasurements backboost_measurements{};
        BackBoostConfig backboost_config{};

        DACStatus dac_status{};
        DACMeasurements dac_measurements{};
        DACConfig dac_config{};
    };

    Connector(boost::asio::io_context &ctx, std::function<void()> refresh, std::function<void(std::string)> add_log = nullptr);
    ~Connector();

    bool open(const Options &cfg);
    const State& get_state() const noexcept;
    void async_run_receiver() noexcept;
    void send(const Request &req) noexcept;
    void close() noexcept;
private:
    boost::asio::serial_port port;
    std::array<std::byte, MESSAGE_SIZE> data;
    std::vector<std::byte> parse_buffer;
    std::list<std::vector<std::byte>> output_buffer;
    std::function<void()> refresh;
    std::function<void(std::string)> add_log;
    State state;

    void process_data(boost::system::error_code ec, std::size_t bytes_written) noexcept;
    void eat_bytes(std::size_t bytes_written) noexcept;
    std::vector<std::byte>::iterator try_parse_message(std::vector<std::byte>::iterator start);
    void handle_response(const Response &resp) noexcept;
};

#endif

