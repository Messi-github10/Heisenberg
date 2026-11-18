#include <Utiles/Logger.hpp>
#include <gtest/gtest.h>
#include <array>
#include <barrier>
#include <future>
#include <string>

TEST(LoggerConcurrencyTest, ConcurrentFirstUsePublishesOneStableModuleLogger) {
    constexpr size_t workers = 12;
    // Fresh module names ensure this tests first publication, even when a
    // combined test process has already initialized the usual module loggers.
    for (int round = 0; round < 3; ++round) {
        const auto file = "logger-concurrency-regression/module-" + std::to_string(round) + "/worker.cpp";
        std::barrier start(static_cast<std::ptrdiff_t>(workers));
        std::array<std::future<spdlog::logger*>,workers> pending;
        for (auto& future : pending) {
            future = std::async(std::launch::async,[&] {
                start.arrive_and_wait();
                return heisenberg::Logger::ModuleLogger(file.c_str());
            });
        }
        std::array<spdlog::logger*,workers> loggers{};
        for (size_t i = 0; i < workers; ++i) loggers[i] = pending[i].get();
        auto* published = heisenberg::Logger::ModuleLogger(file.c_str());
        ASSERT_NE(published,nullptr);
        // Compare every borrowed pointer without dereferencing a potentially
        // destroyed logger: the old check/create/replace race failed here.
        for (auto* logger : loggers) EXPECT_EQ(logger,published) << "round " << round;
        EXPECT_EQ(heisenberg::Logger::ModuleLogger(file.c_str()),published);
    }
}
