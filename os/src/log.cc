#include "log.hpp"
#include <cstdarg>
#include <cstdio>

#include "stm32g4xx_hal.h"

namespace os {

static LogLevel max_level;
static LogSink sink = nullptr;

void init(LogLevel max_level) {
    os::max_level = max_level;
}

void set_sink(LogSink s) {
    sink = s;
}

void log(LogLevel level, const char *fmt, std::va_list list) {
    if (level < max_level) {
        return;
    }

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

    // Format once for both the console and the sink (longer messages are truncated).
    char text[128];
    std::vsnprintf(text, sizeof(text), fmt, list);
    const uint32_t tick = HAL_GetTick();
    std::printf("%lu:%s[%s]\033[0m  %s", static_cast<unsigned long>(tick), color, tag, text);
    if (LogSink s = sink) {
        s(level, tick, text);
    }
}

void trace(const char *fmt, ...) {
    va_list list;
    va_start(list, fmt);

    log(LogLevel::Trace, fmt, list);
    va_end(list);
}

void debug(const char *fmt, ...) {
    va_list list;
    va_start(list, fmt);

    log(LogLevel::Debug, fmt, list);
    va_end(list);
}

void info(const char *fmt, ...) {
    va_list list;
    va_start(list, fmt);

    log(LogLevel::Info, fmt, list);
    va_end(list);
}

void warn(const char *fmt, ...) {
    va_list list;
    va_start(list, fmt);

    log(LogLevel::Warn, fmt, list);
    va_end(list);
}

void error(const char *fmt, ...) {
    va_list list;
    va_start(list, fmt);

    log(LogLevel::Error, fmt, list);
    va_end(list);
}

uint32_t timestamp_ms() {
    return HAL_GetTick();
}

void delay(uint32_t ms) {
    HAL_Delay(ms);
}

}
