#pragma once

#include <Models/Clip.hpp>
#include <QWidget>
#include <optional>

class ColorGradeControls;
class QLabel;
class QVBoxLayout;
namespace heisenberg::ui { class EditorSession; }

class ColorGradePanel final : public QWidget {
    Q_OBJECT
public:
    explicit ColorGradePanel(heisenberg::ui::EditorSession* session, QWidget* parent = nullptr);
    ~ColorGradePanel() override;
    void setClip(const QString& clipId);
    void finishEditing();
    void cancelEditing();
    QString clipId() const { return clipId_; }
signals:
    void editFailed(const QString& message);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void refresh();
    void clearControls();
    void previewEffects();
    void commitEditing();
    heisenberg::ui::EditorSession* session_;
    QVBoxLayout* layout_;
    QLabel* title_;
    QLabel* hint_;
    ColorGradeControls* controls_ = nullptr;
    QString clipId_;
    QString projectToken_;
    std::optional<heisenberg::JsonValue> originalEffects_;
    int currentTab_ = 0;
    bool transaction_ = false;
    bool updating_ = false;
    bool finishing_ = false;
};
