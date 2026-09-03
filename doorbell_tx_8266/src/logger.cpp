#include "lumos/core/logger.hpp"

#include <Arduino.h>

#include <cstdio>

namespace lumos {

Logger::Logger(const char* tag) : tag_(tag) {}

void Logger::log(LogLevel level, const char* fmt, va_list args) const {
    const char* mark = "I";
    switch (level) {
    case LogLevel::Error:
        mark = "E";
        break;
    case LogLevel::Warn:
        mark = "W";
        break;
    case LogLevel::Info:
        mark = "I";
        break;
    case LogLevel::Debug:
        mark = "D";
        break;
    case LogLevel::Verbose:
        mark = "V";
        break;
    }
    char buf[192];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    Serial.printf("[%s][%s] %s\n", mark, tag_ != nullptr ? tag_ : "db", buf);
}

void Logger::error(const char* fmt, ...) const {
    va_list args;
    va_start(args, fmt);
    log(LogLevel::Error, fmt, args);
    va_end(args);
}

void Logger::warn(const char* fmt, ...) const {
    va_list args;
    va_start(args, fmt);
    log(LogLevel::Warn, fmt, args);
    va_end(args);
}

void Logger::info(const char* fmt, ...) const {
    va_list args;
    va_start(args, fmt);
    log(LogLevel::Info, fmt, args);
    va_end(args);
}

void Logger::debug(const char* fmt, ...) const {
    va_list args;
    va_start(args, fmt);
    log(LogLevel::Debug, fmt, args);
    va_end(args);
}

void Logger::verbose(const char* fmt, ...) const {
    va_list args;
    va_start(args, fmt);
    log(LogLevel::Verbose, fmt, args);
    va_end(args);
}

} // namespace lumos
