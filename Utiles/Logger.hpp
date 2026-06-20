#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <string>

namespace heisenberg {

class Logger {
public:
    Logger() = delete;

    static void Init(const std::string& pattern = kDefaultPattern);
    static void SetLevel(spdlog::level::level_enum level);
    static void Shutdown();
    static spdlog::logger* DefaultLogger();
    static spdlog::logger* ModuleLogger(const char* file);
    static std::shared_ptr<spdlog::logger> CreateLogger(const std::string& name);
    static std::shared_ptr<spdlog::logger> GetLogger(const std::string& name);

    static constexpr const char* kDefaultPattern =
        "[%Y-%m-%d %H:%M:%S.%e] [%^%8l%$] [%t] [%s:%#] %v";
};

} // namespace heisenberg

#define LOG_TRACE(...) \
    SPDLOG_LOGGER_CALL(::heisenberg::Logger::ModuleLogger(__FILE__), spdlog::level::trace, __VA_ARGS__)

#define LOG_DEBUG(...) \
    SPDLOG_LOGGER_CALL(::heisenberg::Logger::ModuleLogger(__FILE__), spdlog::level::debug, __VA_ARGS__)

#define LOG_INFO(...) \
    SPDLOG_LOGGER_CALL(::heisenberg::Logger::ModuleLogger(__FILE__), spdlog::level::info, __VA_ARGS__)

#define LOG_WARN(...) \
    SPDLOG_LOGGER_CALL(::heisenberg::Logger::ModuleLogger(__FILE__), spdlog::level::warn, __VA_ARGS__)

#define LOG_ERROR(...) \
    SPDLOG_LOGGER_CALL(::heisenberg::Logger::ModuleLogger(__FILE__), spdlog::level::err, __VA_ARGS__)

#define LOG_CRITICAL(...) \
    SPDLOG_LOGGER_CALL(::heisenberg::Logger::ModuleLogger(__FILE__), spdlog::level::critical, __VA_ARGS__)

#define LOG_NAMED_TRACE(logger, ...)    SPDLOG_LOGGER_CALL(logger, spdlog::level::trace, __VA_ARGS__)
#define LOG_NAMED_DEBUG(logger, ...)    SPDLOG_LOGGER_CALL(logger, spdlog::level::debug, __VA_ARGS__)
#define LOG_NAMED_INFO(logger, ...)     SPDLOG_LOGGER_CALL(logger, spdlog::level::info, __VA_ARGS__)
#define LOG_NAMED_WARN(logger, ...)     SPDLOG_LOGGER_CALL(logger, spdlog::level::warn, __VA_ARGS__)
#define LOG_NAMED_ERROR(logger, ...)    SPDLOG_LOGGER_CALL(logger, spdlog::level::err, __VA_ARGS__)
#define LOG_NAMED_CRITICAL(logger, ...) SPDLOG_LOGGER_CALL(logger, spdlog::level::critical, __VA_ARGS__)
