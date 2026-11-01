#ifndef __LOG_HPP__
#define __LOG_HPP__

#include <cstdint>

namespace os {

enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error
};

void init(LogLevel level);

/**
 * @brief Extra log consumer (e.g. forwarding to the host). Called for every message that
 *        passes the level filter, with the formatted text (no colour codes, may end in
 *        '\n'). Must not block and must not log; may be called from any thread.
 */
using LogSink = void (*)(LogLevel level, uint32_t tick_ms, const char *text);
void set_sink(LogSink sink);
void trace(const char *fmt, ...);
void debug(const char *fmt, ...);
void info(const char *fmt, ...);
void warn(const char *fmt, ...);
void error(const char *fmt, ...);

// Time utilities
uint32_t timestamp_ms();
void delay(uint32_t ms);

}

#endif
