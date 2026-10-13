#include "MainWidget/RoundedPanel.hpp"
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

RoundedPanel::RoundedPanel(QWidget* content, QWidget* parent)
    : QWidget(parent), content_(content) {
    setAttribute(Qt::WA_StyledBackground, true);
    content_->setParent(this);
    if (content_->minimumWidth() > 0)
        setMinimumWidth(content_->minimumWidth() + margin_ * 2);
    if (content_->minimumHeight() > 0)
        setMinimumHeight(content_->minimumHeight() + margin_ * 2);
}

void RoundedPanel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor("#121316"));

    QPainterPath card;
    card.addRoundedRect(QRectF(rect()).adjusted(margin_, margin_, -margin_, -margin_),
                        radius_, radius_);
    painter.fillPath(card, QColor("#1e1f23"));
}

void RoundedPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    content_->setGeometry(rect().adjusted(margin_, margin_, -margin_, -margin_));
    applyClip();
}

void RoundedPanel::applyClip() {
    QPainterPath clip;
    clip.addRoundedRect(QRectF(content_->rect()), radius_, radius_);
    content_->setMask(QRegion(clip.toFillPolygon().toPolygon()));
}
