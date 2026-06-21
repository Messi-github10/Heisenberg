//
// Created by NiceFold on 2026/6/21.
//

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace heisenberg {

class Packet;
class Stream;

namespace demuxer {

class IDemuxer {
public:
    virtual ~IDemuxer() = default;

    // ---- 生命周期 ----

    /// 打开媒体文件/URL。成功返回 0，失败返回负值。
    /// 成功后 streams() 即被填充。
    virtual int open(const std::string &url) = 0;

    /// 关闭并释放资源。可重复调用，幂等。
    virtual void close() = 0;

    // ---- 读包 ----

    /// 读取下一帧压缩数据包。
    /// 返回 nullptr 表示 EOF 或读取错误。
    /// 使用 shared_ptr 允许多个消费者（解码线程/渲染线程）共享同一帧数据。
    virtual std::shared_ptr<Packet> readPacket() = 0;

    // ---- 定位 ----

    /// 定位到指定秒数。
    /// @param seconds       目标位置（秒）
    /// @param streamIndex   参考流索引，-1 表示自动选择
    /// @param flags         FFmpeg seek 标志：
    ///                       AVSEEK_FLAG_BACKWARD (1) — 向后找关键帧（默认）
    ///                       AVSEEK_FLAG_BYTE     (2) — 按字节位置定位
    ///                       AVSEEK_FLAG_ANY      (4) — 任意帧，不限于关键帧
    ///                       AVSEEK_FLAG_FRAME    (8) — 按帧号定位
    /// @return 成功返回 >= 0，失败返回负值
    virtual int seek(double seconds, int streamIndex = -1, int flags = 1 /* AVSEEK_FLAG_BACKWARD */) = 0;

    // ---- 访问器 ----

    /// open() 后获取到的流列表。
    virtual const std::vector<Stream> &streams() const = 0;

    /// 文件时长（秒），未知时返回 -1.0。
    virtual double duration() const = 0;

    /// 是否支持定位。
    virtual bool seekable() const = 0;

    /// 是否已成功打开且未关闭。
    virtual bool isOpen() const = 0;

    /// 当前打开的 URL。
    virtual const std::string &url() const = 0;

    /// 起始时间偏移（秒），通常为 0，有 edit list 的容器可能非零。
    virtual double startTime() const = 0;

    /// 容器格式名称（如 "mov,mp4,m4a,3gp,3g2,mj2"），用于日志/诊断。
    virtual std::string formatName() const = 0;
};

} // namespace demuxer
} // namespace heisenberg
