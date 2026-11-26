#pragma once

#include <QWidget>
#include <memory>

namespace heisenberg::ui { class EditorSession; }

// Persistent inspector for the primary selected clip. Time is in timeline frames.
class ClipPropertiesPanel final : public QWidget {
    Q_OBJECT
public:
    explicit ClipPropertiesPanel(heisenberg::ui::EditorSession* session, QWidget* parent = nullptr);
    ~ClipPropertiesPanel() override;
    void setClip(const QString& clipId);
    QString clipId() const;
    void setCurrentFrame(qint64 frame);
    qint64 currentFrame() const;
    void setSelectionCount(int count);
    void finishEditing();
    void cancelEditing();
signals:
    void editRequested();
    void editFailed(const QString& message);
    void seekFrameRequested(qint64 frame);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
