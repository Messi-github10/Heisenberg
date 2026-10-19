#pragma once

#include <Project/VideoProject.hpp>
#include <Editor/ProjectSettings.hpp>
#include <QObject>
#include <QStringList>

namespace heisenberg::ui {

// The UI owns one editor; widgets read it and submit intents through this session.
class EditorSession final : public QObject {
    Q_OBJECT
public:
    explicit EditorSession(QObject* parent = nullptr);
    const EditorViewModel& editor() const {
        return project_.editor();
    }
    const VideoProject& videoProject() const {
        return project_;
    }
    ProjectSnapshot snapshot() const {
        return project_.snapshot();
    }
    EditResult applyTimelineSettings(const TimelineSettings& settings);
    const QString& token() const {
        return token_;
    }
    bool isDirty() const {
        return project_.isDirty();
    }
    bool loadPackage(const QString& path, QString* error = nullptr);
    bool savePackage(const QString& path, QString* error = nullptr);
    QStringList importFiles(const QStringList& paths);
    EditResult placeMedia(const QString& mediaRef, qint64 frame, const QString& targetTrackId = {});
    EditResult setClipAudio(const QString& clipId, double volume, qint64 fadeInFrames, qint64 fadeOutFrames);
    EditResult setTrackMuted(const QString& trackId, bool muted);
    EditResult setColorSettings(const ColorSettings& settings);
    EditResult setMediaInputColorSpace(const QString& mediaRef, const QString& space);
    EditResult setClipEffects(const QString& clipId, std::optional<JsonValue> effects);
    EditResult removeClips(const QStringList& clipIds);
    EditResult moveClip(const QString& clipId, qint64 frame);
    EditResult trimClip(const QString& clipId, TrimEdge edge, qint64 deltaFrames);
    EditResult splitClip(const QString& clipId, qint64 frame);
    void undo();
    void redo();

signals:
    void projectChanged();
    void mediaChanged();
    void timelineChanged(qint64 focusFrame);
    void dirtyChanged(bool dirty);

private:
    void syncDirty();
    VideoProject project_;
    QString token_;
    bool dirty_ = false;
};

inline constexpr const char* kMediaMimeType = "application/x-heisenberg-media";
} // namespace heisenberg::ui
