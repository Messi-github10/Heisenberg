#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

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

    return 0;
}
