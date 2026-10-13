#include "Backend/MediaInfoProbe.hpp"

#include <QFileInfo>
#include <QLocale>

#include <cmath>

namespace heisenberg::ui {
namespace {

QString valueOf(const heisenberg::mediainfo::StreamInfo& stream, const char* name) {
    for (const auto& field : stream.fields) {
        if (field.name == name) return QString::fromUtf8(field.value.c_str());
    }
    return {};
}

QString formatBytes(const QString& raw) {
    bool ok = false;
    const qulonglong bytes = raw.toULongLong(&ok);
    if (!ok || raw.isEmpty()) return raw;
    const double value = static_cast<double>(bytes);
    const QLocale locale;
    if (value >= 1024.0 * 1024.0 * 1024.0)
        return locale.toString(value / (1024.0 * 1024.0 * 1024.0), 'f', 2) + QStringLiteral(" GB");
    if (value >= 1024.0 * 1024.0)
        return locale.toString(value / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
    if (value >= 1024.0)
        return locale.toString(value / 1024.0, 'f', 0) + QStringLiteral(" KB");
    return locale.toString(bytes) + QStringLiteral(" B");
}

QString formatDuration(const QString& raw) {
    bool ok = false;
    const double ms = raw.toDouble(&ok);
    if (!ok || ms < 0.0) return raw;
    const int total = static_cast<int>(std::llround(ms / 1000.0));
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int seconds = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

QString formatBitRate(const QString& raw) {
    bool ok = false;
    const double bps = raw.toDouble(&ok);
    if (!ok || bps <= 0.0) return raw;
    const QLocale locale;
    if (bps >= 1000000.0)
        return locale.toString(bps / 1000000.0, 'f', 2) + QStringLiteral(" Mbps");
    if (bps >= 1000.0)
        return locale.toString(bps / 1000.0, 'f', 0) + QStringLiteral(" kbps");
    return locale.toString(bps, 'f', 0) + QStringLiteral(" bps");
}

QString formatHertz(const QString& raw) {
    bool ok = false;
    const double hz = raw.toDouble(&ok);
    if (!ok || hz <= 0.0) return raw;
    const QLocale locale;
    if (hz >= 1000.0)
        return locale.toString(hz / 1000.0, 'f', hz >= 10000.0 ? 1 : 3) + QStringLiteral(" kHz");
    return locale.toString(hz, 'f', 0) + QStringLiteral(" Hz");
}

QString formatFrameRate(const QString& raw) {
    bool ok = false;
    const double fps = raw.toDouble(&ok);
    if (!ok || fps <= 0.0) return raw;
    const int digits = std::abs(fps - std::round(fps)) < 0.001 ? 0 : 3;
    return QLocale().toString(fps, 'f', digits) + QStringLiteral(" fps");
}

QString joinPresent(std::initializer_list<QString> parts, const QString& separator) {
    QStringList kept;
    for (const QString& part : parts) {
        if (!part.isEmpty()) kept.push_back(part);
    }
    return kept.join(separator);
}

void addField(MediaInfoReport& report, const QString& label, const QString& value) {
    if (!value.isEmpty()) report.fields.push_back(MediaInfoField{label, value});
}

void addSection(MediaInfoReport& report, const QString& title) {
    report.fields.push_back(MediaInfoField{QStringLiteral("#"), title});
}

} // namespace

MediaInfoReport presentProbe(const heisenberg::mediainfo::ProbeResult& probe, const QString& path) {
    MediaInfoReport report;
    if (!probe.loaded) {
        report.error = probe.error.empty()
            ? QStringLiteral("Could not read this file.")
            : QString::fromUtf8(probe.error.c_str());
        return report;
    }

    int videoCount = 0;
    int audioCount = 0;
    for (const auto& stream : probe.streams) {
        if (stream.kind == heisenberg::mediainfo::StreamKind::Video) ++videoCount;
        if (stream.kind == heisenberg::mediainfo::StreamKind::Audio) ++audioCount;
    }

    int videoIndex = 0;
    int audioIndex = 0;
    for (const auto& stream : probe.streams) {
        using heisenberg::mediainfo::StreamKind;
        if (stream.kind == StreamKind::General) {
            addSection(report, QStringLiteral("General"));
            addField(report, QStringLiteral("File"), QFileInfo(path).absoluteFilePath());
            addField(report, QStringLiteral("Container"), valueOf(stream, "Format"));
            addField(report, QStringLiteral("Size"), formatBytes(valueOf(stream, "FileSize")));
            addField(report, QStringLiteral("Duration"), formatDuration(valueOf(stream, "Duration")));
            addField(report, QStringLiteral("Bit rate"), formatBitRate(valueOf(stream, "OverallBitRate")));
        } else if (stream.kind == StreamKind::Video) {
            ++videoIndex;
            addSection(report, videoCount == 1
                ? QStringLiteral("Video")
                : QStringLiteral("Video %1").arg(videoIndex));
            addField(report, QStringLiteral("Codec"),
                     joinPresent({valueOf(stream, "Format"), valueOf(stream, "Format_Profile")},
                                 QStringLiteral(" ")));
            addField(report, QStringLiteral("Size"),
                     joinPresent({valueOf(stream, "Width"), valueOf(stream, "Height")},
                                 QStringLiteral(" × ")));
            addField(report, QStringLiteral("Frame rate"), formatFrameRate(valueOf(stream, "FrameRate")));
            addField(report, QStringLiteral("Bit rate"), formatBitRate(valueOf(stream, "BitRate")));
            const QString depth = valueOf(stream, "BitDepth");
            addField(report, QStringLiteral("Bit depth"),
                     depth.isEmpty() ? QString() : depth + QStringLiteral(" bit"));
            addField(report, QStringLiteral("Color"),
                     joinPresent({valueOf(stream, "ColorSpace"), valueOf(stream, "ChromaSubsampling")},
                                 QStringLiteral(" ")));
        } else if (stream.kind == StreamKind::Audio) {
            ++audioIndex;
            addSection(report, audioCount == 1
                ? QStringLiteral("Audio")
                : QStringLiteral("Audio %1").arg(audioIndex));
            addField(report, QStringLiteral("Codec"),
                     joinPresent({valueOf(stream, "Format"), valueOf(stream, "Format_Profile")},
                                 QStringLiteral(" ")));
            addField(report, QStringLiteral("Channels"), valueOf(stream, "Channels"));
            addField(report, QStringLiteral("Sample rate"), formatHertz(valueOf(stream, "SamplingRate")));
            addField(report, QStringLiteral("Bit rate"), formatBitRate(valueOf(stream, "BitRate")));
            addField(report, QStringLiteral("Language"), valueOf(stream, "Language"));
        }
    }

    report.loaded = true;
    return report;
}

} // namespace heisenberg::ui
