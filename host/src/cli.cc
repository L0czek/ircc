#include "cli.hpp"
#include <boost/program_options.hpp>
#include <stdexcept>

using namespace boost::program_options;

boost::log::trivial::severity_level parse_log_level(const std::string& lvl) {
    using namespace boost::program_options;

    using boost::log::trivial::severity_level;

    if (lvl == "trace")   return severity_level::trace;
    if (lvl == "debug")   return severity_level::debug;
    if (lvl == "info")    return severity_level::info;
    if (lvl == "warning") return severity_level::warning;
    if (lvl == "error")   return severity_level::error;
    if (lvl == "fatal")   return severity_level::fatal;

    throw std::runtime_error("Invalid log level: " + lvl);
}

void add_serial_options(options_description &desc) {
    desc.add_options()
        ("port,p", value<std::string>()->required(), "Serial port device (e.g. /dev/ttyUSB0, COM3)")
        ("baud,b", value<unsigned int>()->default_value(115200), "Baud rate")
        ("databits,d", value<unsigned int>()->default_value(8), "Number of data bits (5,6,7,8)")
        ("stopbits,s", value<unsigned int>()->default_value(1), "Number of stop bits (1 or 2)")
        ("parity", value<std::string>()->default_value("none"), "Parity: none, odd, even")
        ("keepalive,k", value<std::size_t>(), "Run keepalive task with interval in seconds (optional)")
        ("log-level,l", value<std::string>()->default_value("info"),
            "Boost.Log trivial level: trace, debug, info, warning, error, fatal");

}

Options parse_serial_options(const variables_map &vm) {
    Options cfg;

    cfg.port = vm.at("port").as<std::string>();
    cfg.baudrate = vm.at("baud").as<unsigned int>();
    cfg.databits = vm.at("databits").as<unsigned int>();
    cfg.stopbits = vm.at("stopbits").as<unsigned int>();
    cfg.parity = vm.at("parity").as<std::string>();
    cfg.loglevel = parse_log_level(vm.at("log-level").as<std::string>());

    if (vm.count("keepalive")) {
        cfg.keepalive = vm.at("keepalive").as<std::size_t>();
    } else {
        cfg.keepalive = std::nullopt;
    }

    // Validation
    if (cfg.databits < 5 || cfg.databits > 8)
        throw std::runtime_error("Invalid data bits: must be 5, 6, 7, or 8");

    if (cfg.stopbits != 1 && cfg.stopbits != 2)
        throw std::runtime_error("Invalid stop bits: must be 1 or 2");

    if (cfg.parity != "none" && cfg.parity != "odd" && cfg.parity != "even")
        throw std::runtime_error("Invalid parity: must be none, odd, or even");

    return cfg;
}

