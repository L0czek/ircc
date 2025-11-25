#ifndef __CLI_HPP__
#define __CLI_HPP__

#include <boost/program_options.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <boost/log/trivial.hpp>

struct Options {
    std::string port;
    unsigned int baudrate;
    unsigned int databits;
    unsigned int stopbits;
    std::string parity;
    std::optional<std::size_t> keepalive;
    boost::log::trivial::severity_level loglevel;
};

void add_serial_options(boost::program_options::options_description &desc);
Options parse_serial_options(const boost::program_options::variables_map &vm);

#endif
