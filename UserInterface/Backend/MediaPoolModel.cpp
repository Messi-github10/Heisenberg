//
// Created by NiceFold on 2026/6/30.
//

#include "MediaPoolModel.hpp"

#include <Demuxer/DemuxerFactory.hpp>
#include <Demuxer/IDemuxer.hpp>
#include <Common/Codec.hpp>
#include <Common/Stream.hpp>
#include <Utiles/Logger.hpp>

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

namespace heisenberg {
namespace ui {

// ---- 工具函数 ----

static QString codecName(const CodecParams& cp)
{
    switch (cp.codecId) {
        case CodecParams::H264:  return "H.264";
        case CodecParams::HEVC:  return "HEVC";
        case CodecParams::VP9:   return "VP9";
        case CodecParams::AV1:   return "AV1";
        case CodecParams::AAC:   return "AAC";
        case CodecParams::MP3:   return "MP3";
        case CodecParams::OPUS:  return "Opus";
        case CodecParams::FLAC:  return "FLAC";
        case CodecParams::VORBIS:return "Vorbis";
        default:                 return "Unknown";
    }
}

static QString formatDuration(double seconds)
{
    if (seconds <= 0.0) return "--:--";
    int totalSec = static_cast<int>(seconds);
    int h = totalSec / 3600;
    int m = (totalSec % 3600) / 60;
    int s = totalSec % 60;
    if (h > 0)
        return QString("%1:%2:%3")
            .arg(h, 2, 10, QChar('0'))
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0'));
    return QString("%1:%2")
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'));
}

static const QStringList& supportedVideoExtensions()
{
    static const QStringList exts = {
        "*.mp4", "*.mov", "*.mkv", "*.avi", "*.webm",
        "*.mxf", "*.m4v", "*.mpg", "*.mpeg", "*.wmv",
        "*.flv", "*.ts",  "*.mts", "*.m2ts", "*.3gp",
        "*.ogv", "*.vob", "*.asf"
    };
    return exts;
}

// ---- 构造 / 析构 ----

MediaPoolModel::MediaPoolModel(QObject* parent)
    : QAbstractListModel(parent)
{
    LOG_INFO("MediaPoolModel created");
}

MediaPoolModel::~MediaPoolModel() = default;

// ---- QAbstractListModel 接口 ----

int MediaPoolModel::rowCount(const QModelIndex&) const
{
    return static_cast<int>(items_.size());
}

QVariant MediaPoolModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(items_.size()))
        return {};

    const auto& item = items_[index.row()];

    switch (role) {
        case NameRole:       return item.name;
        case ThumbnailRole:  return item.thumbnailPath;
        case DurationRole:   return formatDuration(item.duration);
        case ResolutionRole: return item.resolution;
        case FilePathRole:   return item.filePath;
        default:             return {};
    }
}

QHash<int, QByteArray> MediaPoolModel::roleNames() const
{
    return {
        { NameRole,       "name" },
        { ThumbnailRole,  "thumbnail" },
        { DurationRole,   "duration" },
        { ResolutionRole, "resolution" },
        { FilePathRole,   "filePath" }
    };
}

// ---- 操作接口 ----

void MediaPoolModel::importFile(const QUrl& url)
{
    const QString filePath = url.toLocalFile();
    if (filePath.isEmpty()) {
        LOG_WARN("importFile: empty URL or non-local file");
        return;
    }

    QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile()) {
        LOG_WARN("importFile: file not found — {}", filePath.toStdString());
        return;
    }

    LOG_INFO("Importing file: {}", filePath.toStdString());

    // 使用 Demuxer 探测文件元数据
    auto demuxer = demuxer::createDemuxer();
    int ret = demuxer->open(filePath.toStdString());
    if (ret < 0) {
        LOG_WARN("importFile: failed to open — {} (error code: {})", filePath.toStdString(), ret);
        return;
    }

    MediaItem item;
    item.name     = fi.fileName();
    item.filePath = filePath;
    item.duration = demuxer->duration();
    item.thumbnailPath = "";  // Phase 2: 生成缩略图

    // 查找视频流提取分辨率
    const auto& streams = demuxer->streams();
    bool haveVideo = false;
    for (const auto& st : streams) {
        if (st.isVideo()) {
            item.resolution = QString("%1×%2")
                .arg(st.codec.width)
                .arg(st.codec.height);
            haveVideo = true;
            LOG_INFO("  Video: {} ({}) @ {:.2f} fps",
                item.resolution.toStdString(),
                codecName(st.codec).toStdString(),
                st.codec.fps());
            break;
        }
    }
    if (!haveVideo) {
        item.resolution = "—";
        LOG_INFO("  No video stream found");
    }

    LOG_INFO("  Duration: {}s, Streams: {}", demuxer->duration(), streams.size());

    demuxer->close();

    // 插入模型
    int row = static_cast<int>(items_.size());
    beginInsertRows(QModelIndex(), row, row);
    items_.push_back(std::move(item));
    endInsertRows();

    LOG_INFO("Imported: {} [{}]", items_.back().name.toStdString(),
             items_.back().resolution.toStdString());
}

void MediaPoolModel::importFolder(const QUrl& url)
{
    const QString dirPath = url.toLocalFile();
    if (dirPath.isEmpty()) {
        LOG_WARN("importFolder: empty URL");
        return;
    }

    QDir dir(dirPath);
    if (!dir.exists()) {
        LOG_WARN("importFolder: directory not found — {}", dirPath.toStdString());
        return;
    }

    LOG_INFO("Scanning folder: {}", dirPath.toStdString());

    const auto& filters = supportedVideoExtensions();
    QDirIterator it(dirPath, filters, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);

    int count = 0;
    while (it.hasNext()) {
        it.next();
        importFile(QUrl::fromLocalFile(it.filePath()));
        ++count;
    }

    LOG_INFO("importFolder: {} file(s) imported from {}", count, dirPath.toStdString());
}

void MediaPoolModel::removeItem(int index)
{
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        LOG_WARN("removeItem: index {} out of range (size={})", index, items_.size());
        return;
    }

    LOG_INFO("Removing item {}: {}", index, items_[index].name.toStdString());

    beginRemoveRows(QModelIndex(), index, index);
    items_.erase(items_.begin() + index);
    endRemoveRows();
}

void MediaPoolModel::clear()
{
    if (items_.empty()) return;

    LOG_INFO("Clearing all {} media item(s)", items_.size());

    beginResetModel();
    items_.clear();
    endResetModel();
}

} // namespace ui
} // namespace heisenberg
