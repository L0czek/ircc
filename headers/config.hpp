#ifndef __CONFIG_HPP__
#define __CONFIG_HPP__

#include "log.hpp"

#if   defined(LOG_LEVEL_TRACE)
constexpr os::LogLevel LOG_LEVEL = os::LogLevel::Trace;
#elif defined(LOG_LEVEL_DEBUG)
constexpr os::LogLevel LOG_LEVEL = os::LogLevel::Debug;
#elif defined(LOG_LEVEL_INFO)
constexpr os::LogLevel LOG_LEVEL = os::LogLevel::Info;
#elif defined(LOG_LEVEL_WARN)
constexpr os::LogLevel LOG_LEVEL = os::LogLevel::Warn;
#elif defined(LOG_LEVEL_ERROR)
constexpr os::LogLevel LOG_LEVEL = os::LogLevel::Error;
#endif

#endif
