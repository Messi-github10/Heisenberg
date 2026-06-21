#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

// libplacebo is C++-compatible with PL_API_BEGIN/PL_API_END
#include <libplacebo/config.h>
#include <libplacebo/log.h>

// Vulkan — VK_NO_PROTOTYPES 必须在 vulkan.h 之前定义，volk 会提供自己的函数加载
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <volk.h>

#include <QCoreApplication>
#include <QVersionNumber>
#include <QString>

#include <Demuxer/DemuxerFactory.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>

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

    // === Volk 验证 ===
    LOG_INFO("=== Volk 验证 ===");
    {
        VkResult result = volkInitialize();
        if (result == VK_SUCCESS) {
            LOG_INFO("volkInitialize 成功 ✓");
        } else {
            LOG_WARN("volkInitialize 返回 {}", (int)result);
            LOG_WARN("(没有 Vulkan 驱动时这是正常的)");
        }
    }

    // === Qt 6 验证 ===
    LOG_INFO("=== Qt 6 版本信息 ===");
    LOG_INFO("Qt Version: {}", qVersion());
    LOG_INFO("QT_VERSION_STR: {}", QT_VERSION_STR);
    LOG_INFO("QT_VERSION: {:x}", QT_VERSION);
    LOG_INFO("QT_VERSION_MAJOR: {}", QT_VERSION_MAJOR);
    LOG_INFO("QT_VERSION_MINOR: {}", QT_VERSION_MINOR);
    LOG_INFO("QT_VERSION_PATCH: {}", QT_VERSION_PATCH);

    // === Demuxer 单元自测 ===
    {
        // 文件路径：优先命令行参数，否则用默认名
        const char *testFile = R"(C:\Users\NiceFold\Videos\test.mp4)";
        LOG_INFO("=== Demuxer 单元自测 ===");
        LOG_INFO("目标文件: {}", testFile);

        auto demuxer = heisenberg::demuxer::createDemuxer();

        // ---- 1. 打开 ----
        int ret = demuxer->open(testFile);
        if (ret < 0) {
            LOG_ERROR("demuxer->open() 失败，返回 {}。请确保文件存在: {}", ret, testFile);
        } else {
            LOG_INFO("demuxer->open() 成功 ✓");
            LOG_INFO("isOpen(): {}", demuxer->isOpen());
            LOG_INFO("url():     {}", demuxer->url());

            // ---- 2. 流信息 ----
            const auto &streams = demuxer->streams();
            LOG_INFO("stream 数量: {}", streams.size());
            const char *codecNames[] = {
                "H264", "HEVC", "VP9", "AV1",
                "AAC", "MP3", "OPUS", "FLAC", "VORBIS", "UNKNOWN"
            };
            for (const auto &s : streams) {
                const auto &c = s.codec;
                const char *typeStr = s.isVideo() ? "VIDEO" : "AUDIO";
                const char *codecName = codecNames[static_cast<int>(c.codecId)];
                LOG_INFO("  stream[{}]: {} id={} codec={} tb={}/{}",
                    s.index, typeStr, s.demuxerId, codecName,
                    c.tbNum, c.tbDen);
                if (s.isVideo()) {
                    LOG_INFO("    {}x{} {:.3f}fps sar={}:{}",
                        c.width, c.height, c.fps(), c.sarNum, c.sarDen);
                } else {
                    LOG_INFO("    {}Hz {}ch bitDepth={} blockAlign={}",
                        c.sampleRate, c.channels, c.bitDepth, c.blockAlign);
                }
                if (!s.language.empty())
                    LOG_INFO("    language: {}", s.language);
                if (!s.title.empty())
                    LOG_INFO("    title: {}", s.title);
                LOG_INFO("    default={} forced={}", s.isDefault, s.isForced);
            }

            // ---- 3. 元数据 ----
            LOG_INFO("duration:   {:.3f}s", demuxer->duration());
            LOG_INFO("seekable:   {}", demuxer->seekable());
            LOG_INFO("startTime:  {:.3f}s", demuxer->startTime());
            LOG_INFO("formatName: {}", demuxer->formatName());

            // ---- 4. 顺序读包（前 50 个） ----
            LOG_INFO("--- 顺序读取前 50 个包 ---");
            int pktCount = 0;
            while (pktCount < 50) {
                auto pkt = demuxer->readPacket();
                if (!pkt) {
                    LOG_INFO("读包结束（EOF 或错误），共 {} 个", pktCount);
                    break;
                }
                // 仅打印前 10 个以及每第 10 个
                if (pktCount < 10 || pktCount % 10 == 0) {
                    LOG_INFO("  pkt[{}]: stream={} pts={:.3f} dts={:.3f} dur={:.3f} {} size={}",
                        pktCount, pkt->streamIndex,
                        pkt->pts, pkt->dts, pkt->duration,
                        pkt->keyframe ? "K" : " ",
                        pkt->size());
                }
                pktCount++;
            }
            if (pktCount == 50)
                LOG_INFO("已读取 {} 个包，不再继续", pktCount);

            // ---- 5. 定位测试 ----
            if (demuxer->seekable() && demuxer->duration() > 5.0) {
                LOG_INFO("--- Seek 到 5.0s ---");
                ret = demuxer->seek(5.0);
                LOG_INFO("seek() 返回: {}", ret);

                int postSeekCount = 0;
                while (postSeekCount < 5) {
                    auto pkt = demuxer->readPacket();
                    if (!pkt) break;
                    LOG_INFO("  seek后pkt[{}]: stream={} pts={:.3f} {} size={}",
                        postSeekCount, pkt->streamIndex,
                        pkt->pts,
                        pkt->keyframe ? "K" : " ",
                        pkt->size());
                    postSeekCount++;
                }
            } else {
                LOG_INFO("--- 跳过 Seek 测试（不可 seek 或时长不足）---");
            }

            // ---- 6. 关闭 ----
            demuxer->close();
            LOG_INFO("close() 后 isOpen(): {}", demuxer->isOpen());
        }

        LOG_INFO("=== Demuxer 自测完成 ===");
    }

    return 0;
}
