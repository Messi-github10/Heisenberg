#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

// libplacebo is C++-compatible with PL_API_BEGIN/PL_API_END
#include <libplacebo/config.h>
#include <libplacebo/log.h>

// Vulkan
#include <vulkan/vulkan.h>

#include <cstdio>
#include <string>

namespace {

void pl_log_callback(void* /*log_priv*/, pl_log_level level, const char* msg) {
    const char* prefix = "";
    switch (level) {
        case PL_LOG_FATAL: prefix = "[FATAL] "; break;
        case PL_LOG_ERR:   prefix = "[ERR]   "; break;
        case PL_LOG_WARN:  prefix = "[WARN]  "; break;
        case PL_LOG_INFO:  prefix = "[INFO]  "; break;
        case PL_LOG_DEBUG: prefix = "[DEBUG] "; break;
        case PL_LOG_TRACE: prefix = "[TRACE] "; break;
        default: break;
    }
    std::printf("%s%s\n", prefix, msg);
}

} // anonymous namespace

int main() {
    // === FFmpeg 验证 ===
    LOG_INFO("=== FFmpeg 版本信息 ===");
    LOG_INFO("av_version_info: {}", av_version_info());
    LOG_INFO("avutil_version:   {}", avutil_version());
    LOG_INFO("avcodec_version:  {}", avcodec_version());
    LOG_INFO("avformat_version: {}", avformat_version());

    LOG_INFO("=== 默认级别 info — info 及以上可见 ===");
    LOG_TRACE("看不到我 (trace)");
    LOG_DEBUG("看不到我 (debug)");
    LOG_INFO("看得到我 (info)");
    LOG_WARN("看得到我 (warn)");
    LOG_ERROR("看得到我 (error)");

    heisenberg::Logger::SetLevel(spdlog::level::warn);
    LOG_INFO("=== 切换到 warn — 只有 warn 及以上 ===");
    LOG_INFO("看不到我 (info)");
    LOG_DEBUG("看不到我 (debug)");
    LOG_WARN("看得到我 (warn)");
    LOG_ERROR("看得到我 (error)");

    heisenberg::Logger::SetLevel(spdlog::level::trace);
    LOG_INFO("=== 切换到 trace — 全部可见 ===");
    LOG_TRACE("看得到我 (trace)");
    LOG_DEBUG("看得到我 (debug)");
    LOG_INFO("看得到我 (info)");

    // === libplacebo 验证 ===
    LOG_INFO("=== libplacebo 版本信息 ===");
    LOG_INFO("PL_MAJOR_VER:   {}", PL_MAJOR_VER);
    LOG_INFO("PL_API_VER:     {}", PL_API_VER);
    LOG_INFO("pl_fix_ver():   {}", pl_fix_ver());
    LOG_INFO("pl_version():   {}", pl_version());

    LOG_INFO("=== libplacebo 特性支持 ===");
    LOG_INFO("Vulkan:  {}", PL_HAVE_VULKAN  ? "✓" : "✗");
    LOG_INFO("OpenGL:  {}", PL_HAVE_OPENGL  ? "✓" : "✗");
    LOG_INFO("D3D11:   {}", PL_HAVE_D3D11   ? "✓" : "✗");
    LOG_INFO("DOVI:    {}", PL_HAVE_DOVI    ? "✓" : "✗");
    LOG_INFO("LCMS:    {}", PL_HAVE_LCMS    ? "✓" : "✗");
    LOG_INFO("SHADERC: {}", PL_HAVE_SHADERC ? "✓" : "✗");
    LOG_INFO("XXHASH:  {}", PL_HAVE_XXHASH  ? "✓" : "✗");

    LOG_INFO("=== libplacebo 日志系统测试 ===");
    {
        pl_log_params params = {
            .log_cb     = pl_log_callback,
            .log_priv   = nullptr,
            .log_level  = PL_LOG_INFO,
        };
        pl_log log = pl_log_create(PL_API_VER, &params);
        if (log) {
            LOG_INFO("pl_log_create 成功 ✓");

            // 测试日志等级切换 (不产生输出，仅验证 API 无崩溃)
            pl_log_level_update(log, PL_LOG_DEBUG);
            pl_log_level_update(log, PL_LOG_INFO);
            LOG_INFO("pl_log_level_update 无崩溃 ✓");

            // 测试日志参数更新
            pl_log_params new_params = {
                .log_cb     = pl_log_simple,
                .log_priv   = stdout,
                .log_level  = PL_LOG_INFO,
            };
            pl_log_update(log, &new_params);
            LOG_INFO("pl_log_update (切换为 pl_log_simple) 成功 ✓");

            pl_log_destroy(&log);
            LOG_INFO("pl_log_destroy 成功 ✓");
        } else {
            LOG_ERROR("pl_log_create 失败 ✗");
        }
    }

    // === Vulkan 验证 ===
    LOG_INFO("=== Vulkan 版本信息 ===");
    LOG_INFO("Vulkan Header Version: {}.{}.{}",
        VK_API_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE),
        VK_API_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE),
        VK_API_VERSION_PATCH(VK_HEADER_VERSION_COMPLETE));
    LOG_INFO("VK_HEADER_VERSION:     {}", VK_HEADER_VERSION);
    LOG_INFO("VK_API_VERSION_1_0:    {}", (bool)VK_API_VERSION_1_0);
    LOG_INFO("VK_API_VERSION_1_1:    {}", (bool)VK_API_VERSION_1_1);
    LOG_INFO("VK_API_VERSION_1_2:    {}", (bool)VK_API_VERSION_1_2);
    LOG_INFO("VK_API_VERSION_1_3:    {}", (bool)VK_API_VERSION_1_3);
#ifdef VK_API_VERSION_1_4
    LOG_INFO("VK_API_VERSION_1_4:    {}", (bool)VK_API_VERSION_1_4);
#endif

    return 0;
}
