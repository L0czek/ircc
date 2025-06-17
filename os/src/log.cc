#include "log.hpp"
#include <cstdarg>
#include <cstdio>

#include "stm32g4xx_hal.h"

namespace os {

static LogLevel max_level;

void init(LogLevel max_level) {
    os::max_level = max_level;
}

void log(LogLevel level, const char *fmt, std::va_list list) {
    if (level >= max_level) {

        const char* tag;
        const char* color;

        switch (level) {
            case LogLevel::Trace:
                tag = "TRACE";
                color = "\033[90m"; // Bright Black (Gray)
                break;
            case LogLevel::Debug:
                tag = "DEBUG";
                color = "\033[36m"; // Cyan
                break;
            case LogLevel::Info:
                tag = "INFO";
                color = "\033[32m"; // Green
                break;
            case LogLevel::Warn:
                tag = "WARN";
                color = "\033[33m"; // Yellow
                break;
            case LogLevel::Error:
                tag = "ERROR";
                color = "\033[31m"; // Red
                break;
            default:
                tag = "UNKNOWN";
                color = "\033[0m";  // Reset
                break;
        }
        std::printf("%lu:%s[%s]\033[0m  ", HAL_GetTick(), color, tag);
        std::vprintf(fmt, list);
    }
}

void trace(const char *fmt, ...) {
    va_list list;
    va_start(list, fmt);

    log(LogLevel::Trace, fmt, list);
}

void debug(const char *fmt, ...) {
    va_list list;
    va_start(list, fmt);

    log(LogLevel::Debug, fmt, list);
}

void info(const char *fmt, ...) {
    va_list list;
    va_start(list, fmt);

    log(LogLevel::Info, fmt, list);
}

void warn(const char *fmt, ...) {
    va_list list;
    va_start(list, fmt);

    log(LogLevel::Warn, fmt, list);
}

void error(const char *fmt, ...) {
    va_list list;
    va_start(list, fmt);

    log(LogLevel::Error, fmt, list);
}

}
