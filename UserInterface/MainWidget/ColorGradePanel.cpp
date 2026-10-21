#include "ColorGradePanel.hpp"
#include "ColorGradeDialog.hpp"
#include <Backend/EditorSession.hpp>

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QScopedValueRollback>
#include <QTimer>
#include <QVBoxLayout>

using heisenberg::JsonValue;

ColorGradePanel::ColorGradePanel(heisenberg::ui::EditorSession* session, QWidget* parent)
    : QWidget(parent), session_(session), layout_(new QVBoxLayout(this)),
      title_(new QLabel(tr("Color Grade"), this)), hint_(new QLabel(this)), projectToken_(session->token()) {
    setObjectName("colorGradePanel");
    setMinimumWidth(340);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    layout_->setContentsMargins(12, 12, 12, 12);
    layout_->setSpacing(10);
    title_->setObjectName("colorGradeClipName");
    title_->setWordWrap(true);
    title_->setStyleSheet("font-weight: 600; font-size: 13px;");
    hint_->setObjectName("colorGradeSelectionHint");
    hint_->setWordWrap(true);
    layout_->addWidget(title_);
    layout_->addWidget(hint_, 1, Qt::AlignTop);
    setStyleSheet(R"(
        QWidget#colorGradePanel { background: #1e1f23; color: #ececef; }
        QWidget#colorGradePanel QLabel, QCheckBox, QGroupBox { color: #d8d9df; }
        QTabWidget::pane { border: 0; }
        QTabBar::tab { background: #25262b; color: #a9abb5; padding: 7px 9px; border-bottom: 2px solid transparent; }
        QTabBar::tab:selected { background: #303137; color: #ffffff; border-bottom-color: #ebad58; }
        QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; border: 0; }
        QDoubleSpinBox, QComboBox { background: #18191c; color: #ececef; border: 1px solid #41434a; border-radius: 3px; padding: 4px; }
        QTableWidget { background: #202126; color: #ececef; gridline-color: #37383e; border: 1px solid #37383e; }
        QHeaderView::section { background: #303137; color: #d8d9df; border: 0; padding: 4px; }
        QGroupBox { border: 1px solid #37383e; border-radius: 5px; margin-top: 9px; padding-top: 9px; }
    )");
    connect(session_, &heisenberg::ui::EditorSession::timelineChanged, this, [this] { refresh(); });
    connect(session_, &heisenberg::ui::EditorSession::projectChanged, this, [this] {
        if (projectToken_ != session_->token()) {
            transaction_ = false;
            clipId_.clear();
            projectToken_ = session_->token();
        }
        refresh();
    });
    connect(session_, &heisenberg::ui::EditorSession::mediaChanged, this, [this] { refresh(); });
    qApp->installEventFilter(this);
    refresh();
}

ColorGradePanel::~ColorGradePanel() {
    qApp->removeEventFilter(this);
    if (transaction_) {
        updating_ = true;
        session_->cancelClipEffectsTransaction();
    }
}

void ColorGradePanel::setClip(const QString& clipId) {
    if (clipId_ == clipId) { refresh(); return; }
    finishEditing();
    clipId_ = clipId;
    clearControls();
    refresh();
}

void ColorGradePanel::clearControls() {
    if (!controls_) return;
    currentTab_ = controls_->currentTab();
    disconnect(controls_, nullptr, this, nullptr);
    layout_->removeWidget(controls_);
    delete controls_;
    controls_ = nullptr;
}

void ColorGradePanel::refresh() {
    if (updating_ || transaction_) return;
    const auto* timeline = session_->editor().activeTimeline();
    const auto* clip = timeline ? timeline->clipById(clipId_.toStdString()) : nullptr;
    if (!clip || clip->mediaType != heisenberg::ClipType::Video) {
        clearControls();
        title_->setText(tr("Color Grade"));
        hint_->setText(clip ? tr("Select a video clip to adjust its color.") : tr("Select a clip in the timeline to adjust its color."));
        hint_->show();
        originalEffects_.reset();
        return;
    }
    QString name = QString::fromStdString(clip->id);
    for (const auto& asset : session_->editor().assets())
        if (asset.id == clip->mediaRef) { name = QString::fromStdString(asset.name); break; }
    title_->setText(name);
    title_->setToolTip(tr("Color grade for the selected timeline clip"));
    const auto effects = clip->effects.value_or(JsonValue::array());
    originalEffects_ = clip->effects;
    if (controls_ && effects == controls_->effects()) return;
    clearControls();
    if (!effects.is_array()) {
        hint_->setText(tr("This clip's effects cannot be edited."));
        hint_->show();
        return;
    }
    hint_->hide();
    controls_ = new ColorGradeControls(effects, this);
    controls_->setCurrentTab(currentTab_);
    controls_->setLutImporter([this](const QString& path, QString& id, QString& name, QString& error) {
        return session_->importGradeLut(path, id, name, error);
    });
    layout_->insertWidget(2, controls_, 1);
    connect(controls_, &ColorGradeControls::effectsChanged, this, &ColorGradePanel::previewEffects);
    connect(controls_, &ColorGradeControls::editFinished, this, &ColorGradePanel::commitEditing);
}

void ColorGradePanel::previewEffects() {
    if (updating_ || !controls_) return;
    const auto effects = controls_->effects();
    if (!transaction_ && effects == originalEffects_.value_or(JsonValue::array())) return;
    QScopedValueRollback guard(updating_, true);
    if (!transaction_) {
        const auto result = session_->beginClipEffectsTransaction(clipId_);
        if (!result.message.empty()) { emit editFailed(QString::fromStdString(result.message)); return; }
        transaction_ = true;
    }
    const auto value = effects == originalEffects_.value_or(JsonValue::array())
        ? originalEffects_ : std::optional<JsonValue>(effects);
    const auto result = session_->updateClipEffectsTransaction(value);
    if (!result.message.empty()) emit editFailed(QString::fromStdString(result.message));
}

void ColorGradePanel::commitEditing() {
    if (updating_ || !transaction_) return;
    QScopedValueRollback guard(updating_, true);
    const auto result = session_->commitClipEffectsTransaction();
    transaction_ = false;
    if (!result.message.empty()) emit editFailed(QString::fromStdString(result.message));
    const auto* timeline = session_->editor().activeTimeline();
    const auto* clip = timeline ? timeline->clipById(clipId_.toStdString()) : nullptr;
    if (clip) originalEffects_ = clip->effects;
    if (controls_) controls_->rebase();
}

void ColorGradePanel::finishEditing() {
    if (updating_ || finishing_) return;
    QScopedValueRollback guard(finishing_, true);
    if (controls_) controls_->finishEditing();
    commitEditing();
}

void ColorGradePanel::cancelEditing() {
    if (updating_) return;
    if (transaction_) {
        QScopedValueRollback guard(updating_, true);
        const auto result = session_->cancelClipEffectsTransaction();
        transaction_ = false;
        if (!result.message.empty()) emit editFailed(QString::fromStdString(result.message));
    }
    clearControls();
    refresh();
}

bool ColorGradePanel::eventFilter(QObject* watched, QEvent* event) {
    auto* widget = qobject_cast<QWidget*>(watched);
    const bool inside = widget && controls_ && (widget == controls_ || controls_->isAncestorOf(widget));
    if (event->type() == QEvent::MouseButtonPress && !inside) finishEditing();
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (inside && key->key() == Qt::Key_Escape && transaction_) {
            key->accept();
            if (event->type() == QEvent::KeyPress) QTimer::singleShot(0, this, &ColorGradePanel::cancelEditing);
            return true;
        }
        if (key->modifiers().testFlag(Qt::ControlModifier) &&
            (key->key() == Qt::Key_S || key->key() == Qt::Key_O || key->key() == Qt::Key_I ||
             key->key() == Qt::Key_Z || key->key() == Qt::Key_Y)) finishEditing();
    }
    return QWidget::eventFilter(watched, event);
}
