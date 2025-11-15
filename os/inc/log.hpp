#ifndef __LOG_HPP__
#define __LOG_HPP__

namespace os {

enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error
};

void init(LogLevel level);
void trace(const char *fmt, ...);
void debug(const char *fmt, ...);
void info(const char *fmt, ...);
void warn(const char *fmt, ...);
void error(const char *fmt, ...);

}

#endif
