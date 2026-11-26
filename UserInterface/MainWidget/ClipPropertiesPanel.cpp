#include "ClipPropertiesPanel.hpp"

#include <Backend/EditorSession.hpp>
#include <Models/BlendMode.hpp>
#include <Models/ClipAppearance.hpp>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

using namespace heisenberg;

namespace {
// Wheel events scroll the inspector and never silently alter a focused number.
class Number final : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;
    std::function<void()> finished;
    void flush() { interpretText(); }
protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
    void mouseReleaseEvent(QMouseEvent* event) override {
        QDoubleSpinBox::mouseReleaseEvent(event);
        if (finished) finished();
    }
    void keyReleaseEvent(QKeyEvent* event) override {
        QDoubleSpinBox::keyReleaseEvent(event);
        if ((event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) &&
            !event->isAutoRepeat() && finished) finished();
    }
};

class ScrubHandle final : public QToolButton {
public:
    explicit ScrubHandle(Number* number, QWidget* parent) : QToolButton(parent), number_(number) {
        setText(QString::fromUtf8("↔"));
        setCursor(Qt::SizeHorCursor);
        setFocusPolicy(Qt::NoFocus);
        setToolTip(tr("Drag horizontally to adjust; Shift for fine adjustment. Escape cancels."));
        setFixedWidth(25);
    }
    void cancelDrag() { dragging_ = false; setDown(false); }
protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) { event->ignore(); return; }
        number_->setFocus(Qt::MouseFocusReason);
        number_->flush();
        origin_ = event->globalPosition().x();
        base_ = number_->value();
        dragging_ = true;
        setDown(true);
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!dragging_) return;
        const double factor = event->modifiers().testFlag(Qt::ShiftModifier) ? .1 : 1.0;
        number_->setValue(base_ + (event->globalPosition().x() - origin_) * number_->singleStep() * factor);
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !dragging_) return;
        dragging_ = false;
        setDown(false);
        if (number_->finished) number_->finished();
        event->accept();
    }
private:
    Number* number_;
    double origin_ = 0, base_ = 0;
    bool dragging_ = false;
};
using Field = std::optional<double> ClipAppearancePatch::*;
}

struct ClipPropertiesPanel::Impl {
    struct Scalar {
        Number* spin;
        QWidget* row;
        Field field;
        double units, minimum, maximum;
        double displayed = 0, original = 0;
        int group = -1;
    };
    struct Group {
        AppearanceProperty property;
        QGroupBox* box;
        QToolButton *previous, *key, *next;
        QComboBox* interpolation;
        QLabel* hint;
        PreparedAppearanceProperty prepared;
        ClipAppearancePropertyState state;
        QString error;
        bool valid = false;
        bool preparedValid = false;
        std::optional<qint64> previousFrame, nextFrame;
    };
    ClipPropertiesPanel* panel;
    QPointer<ui::EditorSession> session;
    QLabel *title, *hint, *time;
    QScrollArea* scroll;
    QWidget* controls;
    QFormLayout* extras;
    QCheckBox *flipHorizontal, *flipVertical;
    QComboBox* blend;
    std::vector<Group> groups;
    std::vector<Scalar> scalars;
    QString clipId, token, gestureClip, gestureToken;
    qint64 frame = 0, localFrame = 0, gestureFrame = 0;
    int selectionCount = 0;
    bool updating = false, finishing = false, flushing = false, beginning = false, transaction = false, available = false;
    quint64 generation = 0;
    Number* gestureNumber = nullptr;

    const Clip* clip() const {
        const auto* timeline = session ? session->editor().activeTimeline() : nullptr;
        return timeline ? timeline->clipById(clipId.toStdString()) : nullptr;
    }
    void failure(const EditResult& result) {
        if (!result.message.empty()) emit panel->editFailed(QString::fromStdString(result.message));
    }
    void deferFinish() {
        const auto expected = generation;
        QTimer::singleShot(0, panel, [this, expected] {
            if (generation == expected) panel->finishEditing();
        });
    }
    bool begin(Number* number = nullptr) {
        if (updating || finishing || beginning || !session || !available) return false;
        if (transaction && gestureNumber != number) {
            const double entered = number ? number->value() : 0;
            panel->finishEditing();
            if (number) {
                const QSignalBlocker blocker(number);
                number->setValue(entered);
            }
        }
        if (transaction) return gestureToken == session->token() && gestureClip == clipId;
        const auto target = clipId;
        const auto project = session->token();
        const auto targetFrame = localFrame;
        // The grade inspector must flush before we reserve the document transaction.
        // Block only refresh/reentry; grade commit notifications must not reset the
        // number that originated this gesture.
        QScopedValueRollback beginGuard(beginning, true);
        QScopedValueRollback guard(updating, true);
        emit panel->editRequested();
        if (!session || project != session->token() || target != clipId || targetFrame != localFrame || !clip()) return false;
        const auto result = session->beginClipAppearanceTransaction(target, targetFrame);
        failure(result);
        if (!result.message.empty()) return false;
        transaction = true;
        gestureClip = target;
        gestureToken = project;
        gestureFrame = targetFrame;
        gestureNumber = number;
        ++generation;
        return true;
    }
    void update(ClipAppearanceEdit edit, Number* number = nullptr) {
        if (!begin(number)) return;
        edit.localFrame = gestureFrame;
        bool invalid = false;
        QString error;
        {
            QScopedValueRollback guard(updating, true);
            const auto result = session->updateClipAppearanceTransaction(edit);
            invalid = !result.message.empty();
            error = QString::fromStdString(result.message);
            failure(result);
        }
        if (invalid) {
            panel->cancelEditing();
            for (auto& group : groups)
                if (edit.property == group.property) group.hint->setText(error);
        }
    }
    void discrete(ClipAppearanceEdit edit) {
        panel->finishEditing();
        update(std::move(edit));
        // Keep controls alive throughout the originating checkbox/button event.
        deferFinish();
    }
    void scalarChanged(std::size_t index) {
        if (updating) return;
        auto& scalar = scalars[index];
        if (!scalar.spin->isEnabled()) return;
        ClipAppearanceEdit edit;
        if (scalar.group >= 0) edit.property = groups[scalar.group].property;
        edit.patch.*(scalar.field) = scalar.spin->value() == scalar.displayed
            ? scalar.original : scalar.spin->value() / scalar.units;
        update(std::move(edit), scalar.spin);
    }
    void addScalar(QFormLayout* form, int group, const char* name, const QString& label,
                   Field field, double units, double minimum, double maximum) {
        auto* row = new QWidget(controls);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(3);
        auto* spin = new Number(row);
        spin->setObjectName(name);
        spin->setDecimals(4);
        spin->setSingleStep(1.0);
        spin->setKeyboardTracking(false);
        spin->setSuffix(units == 100 ? panel->tr(" %") : panel->tr(" °"));
        spin->setMinimumWidth(100);
        layout->addWidget(spin, 1);
        auto* handle = new ScrubHandle(spin, row);
        handle->setObjectName(QString::fromLatin1(name) + "Scrub");
        layout->addWidget(handle);
        form->addRow(label, row);
        const auto index = scalars.size();
        scalars.push_back({spin, row, field, units, minimum, maximum, 0, 0, group});
        QObject::connect(spin, &QDoubleSpinBox::valueChanged, panel, [this, index] { scalarChanged(index); });
        spin->finished = [this] { deferFinish(); };
        QObject::connect(spin, &QDoubleSpinBox::editingFinished, panel, [this] { deferFinish(); });
    }
    QFormLayout* addGroup(QVBoxLayout* layout, AppearanceProperty property, const char* prefix, const QString& label) {
        auto* box = new QGroupBox(label, controls);
        box->setObjectName(QString::fromLatin1(prefix) + "Group");
        auto* body = new QVBoxLayout(box);
        body->setContentsMargins(7, 9, 7, 7);
        auto* form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);
        form->setHorizontalSpacing(7);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        body->addLayout(form);
        auto* keys = new QHBoxLayout;
        auto button = [&](const char* suffix, const QString& text, const QString& tooltip) {
            auto* result = new QToolButton(box);
            result->setObjectName(QString::fromLatin1(prefix) + suffix);
            result->setText(text);
            result->setToolTip(tooltip);
            keys->addWidget(result);
            return result;
        };
        auto* previous = button("Previous", QString::fromUtf8("‹"), panel->tr("Previous keyframe in this clip"));
        auto* key = button("Key", QString::fromUtf8("◇"), panel->tr("Add or remove a keyframe at the current frame"));
        key->setCheckable(true);
        auto* next = button("Next", QString::fromUtf8("›"), panel->tr("Next keyframe in this clip"));
        auto* interpolation = new QComboBox(box);
        interpolation->setObjectName(QString::fromLatin1(prefix) + "Interpolation");
        interpolation->addItem(panel->tr("Linear"), int(Interpolation::Linear));
        interpolation->addItem(panel->tr("Hold"), int(Interpolation::Hold));
        interpolation->addItem(panel->tr("Smooth"), int(Interpolation::Smooth));
        interpolation->setToolTip(panel->tr("Outgoing interpolation of the keyframe at this frame"));
        keys->addWidget(interpolation, 1);
        body->addLayout(keys);
        auto* groupHint = new QLabel(box);
        groupHint->setObjectName(QString::fromLatin1(prefix) + "Hint");
        groupHint->setWordWrap(true);
        body->addWidget(groupHint);
        layout->addWidget(box);
        const auto index = groups.size();
        groups.push_back({property, box, previous, key, next, interpolation, groupHint, {}, {}, {}, false, false, {}, {}});
        QObject::connect(previous, &QToolButton::clicked, panel, [this, index] { navigate(index, false); });
        QObject::connect(next, &QToolButton::clicked, panel, [this, index] { navigate(index, true); });
        QObject::connect(key, &QToolButton::clicked, panel, [this, index] {
            if (updating) return;
            ClipAppearanceEdit edit;
            edit.property = groups[index].property;
            edit.action = AppearanceEditAction::ToggleKeyframe;
            discrete(edit);
        });
        QObject::connect(interpolation, &QComboBox::currentIndexChanged, panel, [this, index](int value) {
            if (updating || value < 0) return;
            ClipAppearanceEdit edit;
            edit.property = groups[index].property;
            edit.action = AppearanceEditAction::SetInterpolation;
            edit.interpolation = static_cast<Interpolation>(groups[index].interpolation->itemData(value).toInt());
            discrete(edit);
        });
        return form;
    }
    void navigate(std::size_t index, bool forward) {
        panel->finishEditing();
        const auto target = forward ? groups[index].nextFrame : groups[index].previousFrame;
        if (target) emit panel->seekFrameRequested(*target);
    }
    void display(Scalar& scalar, double value, bool enabled) {
        const QSignalBlocker blocker(scalar.spin);
        double displayed = value * scalar.units;
        if (!std::isfinite(displayed)) displayed = std::isnan(displayed) ? 0 :
            std::copysign(std::numeric_limits<double>::max(), displayed);
        scalar.spin->setRange(std::min(scalar.minimum, displayed), std::max(scalar.maximum, displayed));
        scalar.spin->setValue(displayed);
        scalar.original = value;
        scalar.displayed = scalar.spin->value();
        scalar.row->setEnabled(enabled);
        scalar.spin->setToolTip(panel->tr("Stored/sample value: %1. Drag the adjacent handle to adjust.")
            .arg(QString::number(value, 'g', 17)));
    }
    void refresh(bool prepare = true) {
        if (updating || transaction || !session) return;
        QScopedValueRollback guard(updating, true);
        const auto* target = clip();
        available = target && target->mediaType == ClipType::Video;
        scroll->setVisible(available);
        time->setVisible(available);
        if (!available) {
            title->setText(panel->tr("Clip Properties"));
            hint->setText(target ? panel->tr("Select a video clip to adjust its properties.") :
                panel->tr("Select a clip in the timeline to adjust its properties."));
            return;
        }
        QString name = QString::fromStdString(target->id);
        for (const auto& asset : session->editor().assets())
            if (asset.id == target->mediaRef) { name = QString::fromStdString(asset.name); break; }
        title->setText(name);
        // Saturate subtraction for imported extreme timeline positions.
        if (target->startFrame > 0 && frame < std::numeric_limits<qint64>::min() + target->startFrame)
            localFrame = std::numeric_limits<qint64>::min();
        else if (target->startFrame < 0 && frame > std::numeric_limits<qint64>::max() + target->startFrame)
            localFrame = std::numeric_limits<qint64>::max();
        else localFrame = frame - target->startFrame;
        const bool inRange = localFrame >= 0 && localFrame < target->durationFrames;
        hint->setText(selectionCount > 1 ? panel->tr("%1 clips selected. Editing the primary clip only.").arg(selectionCount) :
            panel->tr("Position and scale use frame percentages. Drag ↔ to adjust a number."));
        time->setText(inRange ? panel->tr("Timeline frame %1 · Clip frame %2").arg(frame).arg(localFrame) :
            panel->tr("Timeline frame %1 · Outside clip range — animated values are read-only").arg(frame));
        for (auto& group : groups) {
            std::string error;
            if (prepare) {
                group.preparedValid = prepareAppearanceProperty(*target, group.property, group.prepared, &error);
                group.error = QString::fromStdString(error);
            }
            group.valid = group.preparedValid;
            if (group.preparedValid) {
                group.valid = group.prepared.sample(localFrame, group.state, &error);
                if (!group.valid) group.error = QString::fromStdString(error);
            }
            group.previousFrame.reset();
            group.nextFrame.reset();
            if (group.preparedValid) {
                for (auto key : group.prepared.keyframes()) {
                    if (key < 0 || key >= target->durationFrames) continue;
                    const qint64 timelineKey = target->startFrame + key;
                    if (key < localFrame) group.previousFrame = timelineKey;
                    if (key > localFrame && !group.nextFrame) group.nextFrame = timelineKey;
                }
            }
            group.previous->setEnabled(group.previousFrame.has_value());
            group.next->setEnabled(group.nextFrame.has_value());
            const QSignalBlocker keyBlock(group.key), interpolationBlock(group.interpolation);
            group.key->setEnabled(group.valid && inRange);
            group.key->setChecked(group.valid && group.state.exactKey);
            group.key->setText(group.valid && group.state.exactKey ? QString::fromUtf8("◆") : QString::fromUtf8("◇"));
            group.key->setStyleSheet(group.valid && group.state.animated && !group.state.exactKey ? "color: #ebad58;" : QString());
            group.key->setToolTip(!group.valid ? group.error : group.state.exactKey ?
                panel->tr("Remove the keyframe at this frame") : group.state.animated ?
                panel->tr("Animated property: add a keyframe using the sampled value") :
                panel->tr("Constant property: add the first keyframe"));
            group.interpolation->setEnabled(group.valid && inRange && group.state.exactKey);
            group.interpolation->setCurrentIndex(group.valid ? group.interpolation->findData(int(group.state.interpolation)) : -1);
            const bool last = group.valid && group.state.exactKey && group.prepared.keyframes().size() == 1;
            group.hint->setText(!group.valid ? group.error : last ?
                panel->tr("Removing the last key returns to the stored constant value.") :
                group.state.animated ? panel->tr("Sampled animation · changes create or update a key at this frame") : panel->tr("Constant · add a diamond to animate"));
        }
        for (auto& scalar : scalars) {
            if (scalar.group >= 0) {
                const auto& group = groups[scalar.group];
                const auto value = group.state.sampled.*(scalar.field);
                if (group.valid && value) display(scalar, *value, inRange || !group.state.animated);
                else scalar.row->setEnabled(false);
            } else {
                const double value = scalar.field == &ClipAppearancePatch::edgeRounding ? target->edgeRounding : target->edgeSoftness;
                display(scalar, value, true);
            }
        }
        const QSignalBlocker flipHBlock(flipHorizontal), flipVBlock(flipVertical), blendBlock(blend);
        flipHorizontal->setChecked(target->transform.flipHorizontal);
        flipVertical->setChecked(target->transform.flipVertical);
        blend->clear();
        int selected = 0;
        const auto parsed = parseBlendMode(target->blendMode);
        for (const auto& mode : blendModes()) {
            blend->addItem(QString::fromUtf8(mode.displayName.data(), int(mode.displayName.size())),
                QString::fromUtf8(mode.name.data(), int(mode.name.size())));
            if (parsed && *parsed == mode.mode) selected = blend->count() - 1;
        }
        if (!parsed) {
            blend->addItem(panel->tr("Unsupported (%1)").arg(QString::fromStdString(target->blendMode.value_or(""))));
            selected = blend->count() - 1;
        }
        blend->setCurrentIndex(selected);
    }
};

ClipPropertiesPanel::ClipPropertiesPanel(ui::EditorSession* session, QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>()) {
    d->panel = this;
    d->session = session;
    d->token = session->token();
    setObjectName("clipPropertiesPanel");
    setMinimumWidth(340);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    d->title = new QLabel(tr("Clip Properties"), this);
    d->title->setObjectName("clipPropertiesClipName");
    d->title->setWordWrap(true);
    d->title->setStyleSheet("font-weight: 600; font-size: 13px;");
    d->hint = new QLabel(this);
    d->hint->setObjectName("clipPropertiesSelectionHint");
    d->hint->setWordWrap(true);
    d->time = new QLabel(this);
    d->time->setObjectName("clipPropertiesTime");
    d->time->setWordWrap(true);
    layout->addWidget(d->title);
    layout->addWidget(d->hint);
    layout->addWidget(d->time);
    d->scroll = new QScrollArea(this);
    d->scroll->setWidgetResizable(true);
    d->scroll->setFrameShape(QFrame::NoFrame);
    d->controls = new QWidget(d->scroll);
    auto* body = new QVBoxLayout(d->controls);
    body->setContentsMargins(0, 0, 4, 0);
    auto* position = d->addGroup(body, AppearanceProperty::Position, "clipPosition", tr("Position"));
    d->addScalar(position, 0, "clipCenterX", tr("Center X"), &ClipAppearancePatch::centerX, 100, -1000000, 1000000);
    d->addScalar(position, 0, "clipCenterY", tr("Center Y"), &ClipAppearancePatch::centerY, 100, -1000000, 1000000);
    auto* scale = d->addGroup(body, AppearanceProperty::Scale, "clipScale", tr("Scale"));
    d->addScalar(scale, 1, "clipWidth", tr("Width"), &ClipAppearancePatch::width, 100, .0001, 1000000);
    d->addScalar(scale, 1, "clipHeight", tr("Height"), &ClipAppearancePatch::height, 100, .0001, 1000000);
    auto* rotation = d->addGroup(body, AppearanceProperty::Rotation, "clipRotation", tr("Rotation"));
    d->addScalar(rotation, 2, "clipRotation", tr("Angle"), &ClipAppearancePatch::rotation, 1, -360000, 360000);
    auto* crop = d->addGroup(body, AppearanceProperty::Crop, "clipCrop", tr("Crop"));
    d->addScalar(crop, 3, "clipCropLeft", tr("Left"), &ClipAppearancePatch::cropLeft, 100, 0, 100);
    d->addScalar(crop, 3, "clipCropTop", tr("Top"), &ClipAppearancePatch::cropTop, 100, 0, 100);
    d->addScalar(crop, 3, "clipCropRight", tr("Right"), &ClipAppearancePatch::cropRight, 100, 0, 100);
    d->addScalar(crop, 3, "clipCropBottom", tr("Bottom"), &ClipAppearancePatch::cropBottom, 100, 0, 100);
    auto* opacity = d->addGroup(body, AppearanceProperty::Opacity, "clipOpacity", tr("Opacity"));
    d->addScalar(opacity, 4, "clipOpacity", tr("Opacity"), &ClipAppearancePatch::opacity, 100, 0, 100);
    auto* extrasBox = new QGroupBox(tr("Constant properties"), d->controls);
    d->extras = new QFormLayout(extrasBox);
    d->extras->setContentsMargins(7, 9, 7, 7);
    d->extras->setHorizontalSpacing(7);
    d->extras->setRowWrapPolicy(QFormLayout::WrapLongRows);
    d->extras->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    d->flipHorizontal = new QCheckBox(tr("Flip horizontally"), extrasBox);
    d->flipHorizontal->setObjectName("clipFlipHorizontal");
    d->flipVertical = new QCheckBox(tr("Flip vertically"), extrasBox);
    d->flipVertical->setObjectName("clipFlipVertical");
    d->extras->addRow(d->flipHorizontal);
    d->extras->addRow(d->flipVertical);
    d->addScalar(d->extras, -1, "clipEdgeRounding", tr("Edge rounding"), &ClipAppearancePatch::edgeRounding, 100, 0, 100);
    d->addScalar(d->extras, -1, "clipEdgeSoftness", tr("Edge softness"), &ClipAppearancePatch::edgeSoftness, 100, 0, 100);
    d->blend = new QComboBox(extrasBox);
    d->blend->setObjectName("clipBlendMode");
    d->extras->addRow(tr("Blend mode"), d->blend);
    body->addWidget(extrasBox);
    body->addStretch();
    d->scroll->setWidget(d->controls);
    layout->addWidget(d->scroll, 1);
    setStyleSheet(R"(
        QWidget#clipPropertiesPanel { background: #1e1f23; color: #ececef; }
        QWidget#clipPropertiesPanel QLabel, QCheckBox, QGroupBox { color: #d8d9df; }
        QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; border: 0; }
        QDoubleSpinBox, QComboBox, QToolButton { background: #18191c; color: #ececef; border: 1px solid #41434a; border-radius: 3px; padding: 4px; }
        QDoubleSpinBox:disabled, QComboBox:disabled, QToolButton:disabled { color: #696b74; }
        QToolButton:checked { color: #ebad58; border-color: #ebad58; }
        QGroupBox { border: 1px solid #37383e; border-radius: 5px; margin-top: 9px; padding-top: 9px; }
    )");
    connect(d->flipHorizontal, &QCheckBox::toggled, this, [this](bool value) {
        if (d->updating) return;
        ClipAppearanceEdit edit; edit.patch.flipHorizontal = value; d->discrete(edit);
    });
    connect(d->flipVertical, &QCheckBox::toggled, this, [this](bool value) {
        if (d->updating) return;
        ClipAppearanceEdit edit; edit.patch.flipVertical = value; d->discrete(edit);
    });
    connect(d->blend, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (d->updating || index < 0 || !d->blend->itemData(index).isValid()) return;
        ClipAppearanceEdit edit; edit.patch.blendMode = d->blend->itemData(index).toString().toStdString(); d->discrete(edit);
    });
    connect(session, &ui::EditorSession::timelineChanged, this, [this] { d->refresh(); });
    connect(session, &ui::EditorSession::mediaChanged, this, [this] { d->refresh(); });
    connect(session, &ui::EditorSession::projectChanged, this, [this] {
        if (d->token != d->session->token()) {
            d->transaction = false;
            d->gestureNumber = nullptr;
            ++d->generation;
            d->clipId.clear();
            d->token = d->session->token();
        }
        d->refresh();
    });
    qApp->installEventFilter(this);
    d->refresh();
}

ClipPropertiesPanel::~ClipPropertiesPanel() {
    qApp->removeEventFilter(this);
    // QWidget destroys its children after our private state. A focused spin box
    // can emit editingFinished while it is being destroyed; sever those callbacks.
    for (auto* child : findChildren<QObject*>()) disconnect(child, nullptr, this, nullptr);
    for (auto& scalar : d->scalars) scalar.spin->finished = {};
    if (d->transaction && d->session && d->gestureToken == d->session->token()) {
        d->updating = true;
        d->session->cancelClipAppearanceTransaction();
    }
}
QString ClipPropertiesPanel::clipId() const { return d->clipId; }
qint64 ClipPropertiesPanel::currentFrame() const { return d->frame; }
void ClipPropertiesPanel::setClip(const QString& id) {
    if (d->clipId == id) { d->refresh(false); return; }
    finishEditing();
    d->clipId = id;
    ++d->generation;
    d->refresh();
}
void ClipPropertiesPanel::setCurrentFrame(qint64 frame) {
    if (d->frame == frame) return;
    finishEditing();
    d->frame = frame;
    ++d->generation;
    d->refresh(false);
}
void ClipPropertiesPanel::setSelectionCount(int count) {
    d->selectionCount = std::max(0, count);
    d->refresh(false);
}
void ClipPropertiesPanel::finishEditing() {
    if (d->updating || d->finishing || !d->session) return;
    // interpretText may emit valueChanged; allow that to start a transaction first.
    if (!d->flushing) {
        QScopedValueRollback flushGuard(d->flushing, true);
        QWidget* focused = QApplication::focusWidget();
        while (focused && focused != this) {
            if (auto* number = dynamic_cast<Number*>(focused)) {
                if (isAncestorOf(number)) number->flush();
                break;
            }
            focused = focused->parentWidget();
        }
    }
    for (auto* button : findChildren<QToolButton*>())
        if (auto* handle = dynamic_cast<ScrubHandle*>(button)) handle->cancelDrag();
    QScopedValueRollback guard(d->finishing, true);
    const bool needsRefresh = d->transaction;
    if (d->transaction) {
        QScopedValueRollback updating(d->updating, true);
        if (d->gestureToken == d->session->token()) d->failure(d->session->commitClipAppearanceTransaction());
        d->transaction = false;
        d->gestureNumber = nullptr;
    }
    ++d->generation;
    if (needsRefresh) d->refresh();
}
void ClipPropertiesPanel::cancelEditing() {
    if (d->updating || !d->session) return;
    for (auto* button : findChildren<QToolButton*>())
        if (auto* handle = dynamic_cast<ScrubHandle*>(button)) handle->cancelDrag();
    {
        QScopedValueRollback updating(d->updating, true);
        if (d->transaction && d->gestureToken == d->session->token())
            d->failure(d->session->cancelClipAppearanceTransaction());
        d->transaction = false;
        d->gestureNumber = nullptr;
        ++d->generation;
    }
    d->refresh();
}
bool ClipPropertiesPanel::eventFilter(QObject* watched, QEvent* event) {
    auto* widget = qobject_cast<QWidget*>(watched);
    const bool inside = widget && (widget == this || isAncestorOf(widget));
    if (event->type() == QEvent::MouseButtonPress && !inside) d->deferFinish();
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (inside && key->key() == Qt::Key_Escape) {
            key->accept();
            if (event->type() == QEvent::KeyPress) {
                const auto generation = d->generation;
                QTimer::singleShot(0, this, [this, generation] { if (d->generation == generation) cancelEditing(); });
            }
            return true;
        }
        if (key->modifiers().testFlag(Qt::ControlModifier) &&
            (key->key() == Qt::Key_S || key->key() == Qt::Key_O || key->key() == Qt::Key_I ||
             key->key() == Qt::Key_Z || key->key() == Qt::Key_Y)) finishEditing();
    }
    return QWidget::eventFilter(watched, event);
}
