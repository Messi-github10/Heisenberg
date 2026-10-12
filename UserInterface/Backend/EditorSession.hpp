#pragma once

#include <Editor/EditorViewModel.hpp>
#include <QObject>
#include <QStringList>

namespace heisenberg::ui {

// The UI owns one editor; widgets read it and submit intents through this session.
class EditorSession final : public QObject {
    Q_OBJECT
public:
    explicit EditorSession(QObject* parent = nullptr);
    const EditorViewModel& editor() const { return editor_; }
    const QString& token() const { return token_; }
    bool isDirty() const { return dirty_; }
    bool loadPackage(const QString& path, QString* error = nullptr);
    bool savePackage(const QString& path, QString* error = nullptr);
    QStringList importFiles(const QStringList& paths);
    EditResult placeMedia(const QString& mediaRef, qint64 frame);
    EditResult moveClip(const QString& clipId, qint64 frame);
    void undo();
    void redo();

signals:
    void projectChanged();
    void mediaChanged();
    void timelineChanged(qint64 focusFrame);
    void dirtyChanged(bool dirty);

private:
    void setDirty(bool dirty);
    EditorViewModel editor_;
    QString token_;
    bool dirty_ = false;
};

inline constexpr const char* kMediaMimeType = "application/x-heisenberg-media";
} // namespace heisenberg::ui
