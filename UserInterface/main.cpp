#include <Utiles/Logger.hpp>

int main() {

    LOG_TRACE("Hello, spdlog! (trace)");
    LOG_DEBUG("Hello, spdlog! (debug)");
    LOG_INFO("Hello, spdlog! (info)");
    LOG_WARN("Hello, spdlog! (warning)");
    LOG_ERROR("Hello, spdlog! (error)");
    LOG_CRITICAL("Hello, spdlog! (critical)");

    auto console = heisenberg::Logger::CreateLogger("console");
    LOG_NAMED_INFO(console.get(), "Colored console output from named logger!");

    return 0;
}
