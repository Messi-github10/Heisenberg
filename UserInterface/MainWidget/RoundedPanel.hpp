#pragma once

#include <QWidget>

// Clips one module to rounded corners and paints the gap around it with the
// window background, so self-painted and native children cannot square it off.
class RoundedPanel final : public QWidget {
public:
    explicit RoundedPanel(QWidget* content, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void applyClip();

    QWidget* content_ = nullptr;
    int radius_ = 10;
    int margin_ = 6;
};
