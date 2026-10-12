#include "EditorSession.hpp"

#include <QDir>
#include <QFileInfo>
#include <QUuid>

namespace heisenberg::ui {
EditorSession::EditorSession(QObject* parent) : QObject(parent),
    token_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

void EditorSession::setDirty(bool dirty) {
    if (dirty_ == dirty) return;
    dirty_ = dirty;
    emit dirtyChanged(dirty);
}

bool EditorSession::loadPackage(const QString& path, QString* error) {
    std::string reason;
    if (!editor_.loadPackage(path.toUtf8().toStdString(), &reason)) {
        if (error) *error = QString::fromUtf8(reason.c_str());
        return false;
    }
    token_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    setDirty(false);
    emit projectChanged();
    emit mediaChanged();
    emit timelineChanged(0);
    return true;
}

bool EditorSession::savePackage(const QString& path, QString* error) {
    const auto previousRoot = editor_.projectDir();
    std::string reason;
    if (!editor_.savePackage(path.toUtf8().toStdString(), &reason)) {
        if (error) *error = QString::fromUtf8(reason.c_str());
        return false;
    }
    setDirty(false);
    if (previousRoot != editor_.projectDir()) emit mediaChanged();
    return true;
}

QStringList EditorSession::importFiles(const QStringList& paths) {
    QStringList errors;
    bool changed = false;
    for (const auto& path : paths) {
        const QFileInfo file(path);
        const QString absolute = file.canonicalFilePath();
        if (absolute.isEmpty() || !file.isFile()) {
            errors << tr("%1: file is unavailable").arg(file.fileName());
            continue;
        }
        bool duplicate = false;
        for (const auto& asset : editor_.assets()) {
            if (QFileInfo(QString::fromUtf8(asset.path.c_str())).canonicalFilePath()
                    .compare(absolute, Qt::CaseInsensitive) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        std::string reason;
        if (editor_.importMedia(QDir::fromNativeSeparators(absolute).toUtf8().toStdString(), &reason))
            changed = true;
        else errors << tr("%1: %2").arg(file.fileName(), QString::fromUtf8(reason.c_str()));
    }
    if (changed) {
        setDirty(true);
        emit mediaChanged();
    }
    return errors;
}

EditResult EditorSession::placeMedia(const QString& mediaRef, qint64 frame) {
    auto result = editor_.placeClip(mediaRef.toUtf8().toStdString(), frame);
    if (result.changed()) {
        setDirty(true);
        emit timelineChanged(frame);
    }
    return result;
}

void EditorSession::undo() {
    if (editor_.undo()) { setDirty(true); emit timelineChanged(-1); }
}

void EditorSession::redo() {
    if (editor_.redo()) { setDirty(true); emit timelineChanged(-1); }
}
} // namespace heisenberg::ui
