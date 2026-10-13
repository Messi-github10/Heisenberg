#include "MainWidget/MediaInfoPanel.hpp"
#include "Backend/MediaInfoProbe.hpp"

#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

MediaInfoPanel::MediaInfoPanel(QWidget* parent)
    : QWidget(parent) {
    setObjectName("mediaInfo");
    setMinimumWidth(260);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto* title = new QLabel(tr("Media Info"), this);
    title->setStyleSheet("font-weight: 600;");
    layout->addWidget(title);

    message_ = new QLabel(this);
    message_->setWordWrap(true);
    message_->setStyleSheet("color: #999da8; padding: 4px;");
    layout->addWidget(message_);

    formHost_ = new QWidget(this);
    form_ = new QFormLayout(formHost_);
    form_->setContentsMargins(0, 0, 0, 0);
    form_->setHorizontalSpacing(10);
    form_->setVerticalSpacing(4);
    form_->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    form_->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    layout->addWidget(formHost_);
    layout->addStretch(1);

    showTimelineHint();
}

void MediaInfoPanel::showTimelineHint() {
    clearInfo();
    setMessage(tr("Timeline preview.\nDouble-click a clip in the media pool to inspect the source file."));
}

void MediaInfoPanel::showLoading(const QString& path) {
    clearInfo();
    setMessage(tr("Reading %1…").arg(QFileInfo(path).fileName()));
}

void MediaInfoPanel::showProbe(const heisenberg::ui::MediaInfoReport& report) {
    clearInfo();
    if (!report.loaded) {
        setMessage(report.error.isEmpty() ? tr("Could not read this file.") : report.error);
        return;
    }
    for (const auto& field : report.fields) {
        if (field.label == QLatin1Char('#')) addSection(field.value);
        else addRow(field.label, field.value);
    }
    formHost_->setVisible(form_->rowCount() > 0);
}

void MediaInfoPanel::clearInfo() {
    while (QLayoutItem* item = form_->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
    formHost_->hide();
    message_->clear();
    message_->hide();
}

void MediaInfoPanel::setMessage(const QString& text) {
    message_->setText(text);
    message_->setVisible(!text.isEmpty());
}

void MediaInfoPanel::addSection(const QString& title) {
    auto* label = new QLabel(title, formHost_);
    label->setStyleSheet("font-weight: 600; color: #d8d9df; padding-top: 8px;");
    form_->addRow(label);
}

void MediaInfoPanel::addRow(const QString& label, const QString& value) {
    const QString shown = value.trimmed();
    if (shown.isEmpty()) return;
    auto* name = new QLabel(label, formHost_);
    name->setStyleSheet("color: #999da8;");
    auto* text = new QLabel(shown, formHost_);
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text->setStyleSheet("color: #ececef;");
    form_->addRow(name, text);
}
