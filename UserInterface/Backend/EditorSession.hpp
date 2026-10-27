#pragma once

#include <Project/VideoProject.hpp>
#include <Editor/ProjectSettings.hpp>
#include <QObject>
#include <QHash>
#include <QStringList>
#include <optional>

namespace heisenberg::ui {

// Source marks are exclusive-boundary seconds, local to this editing session.
struct SourceRange {
    std::optional<double> inSeconds;
    std::optional<double> outSeconds;
    bool operator==(const SourceRange&) const = default;
};

struct SourcePlacementRange {
    qint64 trimStartFrame = 0;
    qint64 durationFrames = 0;
};

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
    SourceRange sourceRange(const QString& mediaRef) const;
    bool setSourceIn(const QString& mediaRef, double seconds);
    bool setSourceOut(const QString& mediaRef, double seconds);
    bool clearSourceRange(const QString& mediaRef);
    bool sourcePlacementRange(const QString& mediaRef, qint64& trimStart, qint64& duration,
                              QString* error = nullptr) const;
    EditResult placeMedia(const QString& mediaRef, qint64 frame, const QString& targetTrackId = {});
    EditResult insertMedia(const QString& mediaRef, qint64 frame, const QString& targetTrackId = {});
    EditResult rippleDeleteClip(const QString& clipId);
    EditResult setClipAudio(const QString& clipId, double volume, qint64 fadeInFrames, qint64 fadeOutFrames);
    EditResult setTrackMuted(const QString& trackId, bool muted);
    EditResult setColorSettings(const ColorSettings& settings);
    EditResult setMediaInputColorSpace(const QString& mediaRef, const QString& space);
    EditResult setClipEffects(const QString& clipId, std::optional<JsonValue> effects);
    EditResult beginClipEffectsTransaction(const QString& clipId);
    EditResult updateClipEffectsTransaction(std::optional<JsonValue> effects);
    EditResult commitClipEffectsTransaction();
    EditResult cancelClipEffectsTransaction();
    bool importGradeLut(const QString& path, QString& id, QString& displayName, QString& error);
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
    void sourceRangeChanged(const QString& mediaRef);

private:
    bool setSourceBoundary(const QString& mediaRef, double seconds, bool in);
    void syncDirty();
    QHash<QString, SourceRange> sourceRanges_;
    VideoProject project_;
    QString token_;
    bool dirty_ = false;
};

inline constexpr const char* kMediaMimeType = "application/x-heisenberg-media";
} // namespace heisenberg::ui
