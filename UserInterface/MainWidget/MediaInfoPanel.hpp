#pragma once

#include "Backend/MediaInfoProbe.hpp"
#include <QWidget>

class QLabel;
class QFormLayout;

class MediaInfoPanel final : public QWidget {
    Q_OBJECT

public:
    explicit MediaInfoPanel(QWidget* parent = nullptr);

    void showTimelineHint();
    void showLoading(const QString& path);
    void showProbe(const heisenberg::ui::MediaInfoReport& report);

private:
    void clearInfo();
    void setMessage(const QString& text);
    void addSection(const QString& title);
    void addRow(const QString& label, const QString& value);

    QLabel* message_ = nullptr;
    QWidget* formHost_ = nullptr;
    QFormLayout* form_ = nullptr;
};
