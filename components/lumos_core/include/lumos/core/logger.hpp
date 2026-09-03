#pragma once

#include <cstdarg>

namespace lumos {

enum class LogLevel {
    Error,
    Warn,
    Info,
    Debug,
    Verbose,
};

class Logger {
public:
    explicit Logger(const char* tag);

    void error(const char* fmt, ...) const __attribute__((format(printf, 2, 3)));
    void warn(const char* fmt, ...) const __attribute__((format(printf, 2, 3)));
    void info(const char* fmt, ...) const __attribute__((format(printf, 2, 3)));
    void debug(const char* fmt, ...) const __attribute__((format(printf, 2, 3)));
    void verbose(const char* fmt, ...) const __attribute__((format(printf, 2, 3)));

private:
    void log(LogLevel level, const char* fmt, va_list args) const;
    const char* tag_;
};

} // namespace lumos
