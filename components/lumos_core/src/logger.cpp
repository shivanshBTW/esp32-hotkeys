#include "lumos/core/logger.hpp"

#include <esp_log.h>

#include <cstdio>
#include <cstring>

namespace lumos {

Logger::Logger(const char* tag) : tag_(tag) {}

void Logger::log(LogLevel level, const char* fmt, va_list args) const {
    esp_log_level_t esp_level = ESP_LOG_INFO;
    switch (level) {
    case LogLevel::Error:
        esp_level = ESP_LOG_ERROR;
        break;
    case LogLevel::Warn:
        esp_level = ESP_LOG_WARN;
        break;
    case LogLevel::Info:
        esp_level = ESP_LOG_INFO;
        break;
    case LogLevel::Debug:
        esp_level = ESP_LOG_DEBUG;
        break;
    case LogLevel::Verbose:
        esp_level = ESP_LOG_VERBOSE;
        break;
    }
    // esp_log_writev does not add a newline (ESP_LOG* macros do).
    if (fmt != nullptr && fmt[0] != '\0' && fmt[std::strlen(fmt) - 1] == '\n') {
        esp_log_writev(esp_level, tag_, fmt, args);
        return;
    }
    char fmt_nl[256];
    if (std::snprintf(fmt_nl, sizeof(fmt_nl), "%s\n", fmt ? fmt : "") >=
        static_cast<int>(sizeof(fmt_nl))) {
        esp_log_writev(esp_level, tag_, fmt, args);
        esp_log_write(esp_level, tag_, "\n");
        return;
    }
    esp_log_writev(esp_level, tag_, fmt_nl, args);
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
