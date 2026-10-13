#pragma once

#include <MultiMedia/MediaInfo/MediaInfoProbe.hpp>
#include <QList>
#include <QMetaType>
#include <QString>

namespace heisenberg::ui {

struct MediaInfoField {
    QString label;
    QString value;
};

struct MediaInfoReport {
    bool loaded = false;
    QString error;
    QList<MediaInfoField> fields;
};

// Formats a framework probe for the inspector. Section titles use label "#".
MediaInfoReport presentProbe(const heisenberg::mediainfo::ProbeResult& probe, const QString& path);

} // namespace heisenberg::ui

Q_DECLARE_METATYPE(heisenberg::ui::MediaInfoReport)
