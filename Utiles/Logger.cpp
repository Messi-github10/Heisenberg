#include <Utiles/Logger.hpp>

#include <spdlog/sinks/rotating_file_sink.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace heisenberg {

namespace {

std::once_flag   g_InitFlag;
std::atomic_bool g_InitDone{false};
std::once_flag   g_AtexitFlag;

std::mutex g_ModuleMutex;
std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> g_ModuleLoggers;

std::string g_RunId;

std::string GetExeDirectory() {
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return ".";
    }
    std::string full(path, len);
    auto pos = full.find_last_of("\\/");
    if (pos != std::string::npos) {
        return full.substr(0, pos);
    }
#endif
    return ".";
}

std::string GenerateRunId() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y_%m_%d_%H_%M", &tm);
#ifdef _WIN32
    return std::to_string(GetCurrentProcessId()) + "_" + buf;
#else
    return std::to_string(getpid()) + "_" + buf;
#endif
}

std::string ExtractModule(const char* file) {
    std::string path(file);
    for (auto& c : path) {
        if (c == '\\') c = '/';
    }

    std::string_view view(path);
    std::string_view root(HEISENBERG_SOURCE_DIR);

    if (view.starts_with(root)) {
        view.remove_prefix(root.size());
        if (view.starts_with('/')) {
            view.remove_prefix(1);
        }
    }

    auto pos = view.find_last_of('/');
    if (pos == std::string_view::npos) {
        return "heisenberg";
    }

    view = view.substr(0, pos);
    if (view.empty()) {
        return "heisenberg";
    }

    return std::string(view);
}

std::shared_ptr<spdlog::logger> CreateModuleLogger(const std::string& module) {
    auto logDir = std::filesystem::path(GetExeDirectory()) / "Log" / module;
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);

    auto logName = g_RunId + ".log";
    auto logPath = (logDir / logName).string();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        logPath, 10 * 1024 * 1024, 5);

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    auto logger = std::make_shared<spdlog::logger>(
        module,
        spdlog::sinks_init_list{console_sink, file_sink});

    logger->set_pattern(Logger::kDefaultPattern);
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::err);

    return logger;
}

} // anonymous namespace

void Logger::Init(const std::string& pattern) {
    if (g_RunId.empty()) {
        g_RunId = GenerateRunId();
    }

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    auto logDir = std::filesystem::path(GetExeDirectory()) / "Log";
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);
    auto logPath = (logDir / (g_RunId + ".log")).string();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        logPath, 10 * 1024 * 1024, 5);

    auto logger = std::make_shared<spdlog::logger>(
        "heisenberg",
        spdlog::sinks_init_list{console_sink, file_sink});

    logger->set_pattern(pattern);
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::err);

    spdlog::set_default_logger(std::move(logger));

    g_InitDone.store(true, std::memory_order_release);

    std::call_once(g_AtexitFlag, []() {
        std::atexit([]() { spdlog::shutdown(); });
    });
}

void Logger::SetLevel(spdlog::level::level_enum level) {
    DefaultLogger()->set_level(level);
}

void Logger::Shutdown() {
    spdlog::shutdown();
}

spdlog::logger* Logger::DefaultLogger() {
    if (!g_InitDone.load(std::memory_order_acquire)) {
        std::call_once(g_InitFlag, []() { Init(); });
    }
    return spdlog::default_logger_raw();
}

spdlog::logger* Logger::ModuleLogger(const char* file) {
    if (g_RunId.empty()) {
        g_RunId = GenerateRunId();
    }

    std::string module = ExtractModule(file);

    {
        std::lock_guard lock(g_ModuleMutex);
        auto it = g_ModuleLoggers.find(module);
        if (it != g_ModuleLoggers.end()) {
            return it->second.get();
        }
    }

    auto logger = CreateModuleLogger(module);

    {
        std::lock_guard lock(g_ModuleMutex);
        g_ModuleLoggers[module] = logger;
    }

    return logger.get();
}

std::shared_ptr<spdlog::logger> Logger::CreateLogger(const std::string& name) {
    auto existing = spdlog::get(name);
    if (existing) {
        return existing;
    }

    auto logger = spdlog::stdout_color_mt(name);
    logger->set_pattern(kDefaultPattern);
    return logger;
}

std::shared_ptr<spdlog::logger> Logger::GetLogger(const std::string& name) {
    return spdlog::get(name);
}

} // namespace heisenberg
