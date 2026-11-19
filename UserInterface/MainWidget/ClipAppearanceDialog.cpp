#include "MainWidget/ClipAppearanceDialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <algorithm>
#include <cmath>
#include <limits>

namespace heisenberg::ui {

ClipAppearanceDialog::ClipAppearanceDialog(const Clip& clip, QWidget* parent)
    : QDialog(parent), original_(clip), originalFlipHorizontal_(clip.transform.flipHorizontal),
      originalFlipVertical_(clip.transform.flipVertical) {
    setObjectName("clipAppearanceDialog");
    setWindowTitle(tr("Clip Appearance"));
    auto* form = new QFormLayout(this);
    const auto scalar = [&](const char* name, const QString& label, double value,
                            std::optional<double> ClipAppearancePatch::* field,
                            double units, double minimum, double maximum) {
        auto* spin = new QDoubleSpinBox(this);
        spin->setObjectName(name);
        spin->setDecimals(4);
        spin->setSingleStep(1.0);
        spin->setSuffix(units == 100.0 ? tr(" %") : tr(" °"));
        double displayed = value * units;
        if (!std::isfinite(displayed)) {
            // Overflow/nonfinite imported values remain stored until this control
            // changes. Tell the user when the display cannot express that value.
            displayed = std::isnan(displayed) ? 0.0 :
                std::copysign(std::numeric_limits<double>::max(), displayed);
            spin->setToolTip(tr("Stored value: %1. Unchanged controls preserve the original value.")
                .arg(QString::number(value, 'g', 17)));
        }
        spin->setRange(std::min(minimum, displayed), std::max(maximum, displayed));
        spin->setValue(displayed);
        spin->setMinimumWidth(140);
        // Compare against Qt's actual rounded display, never a reconstructed model
        // value: untouched controls must not introduce precision/range changes.
        scalars_.push_back({spin, spin->value(), units, field});
        form->addRow(label, spin);
    };
    scalar("clipCenterX", tr("Center X"), clip.transform.centerX, &ClipAppearancePatch::centerX, 100.0, -1000000.0, 1000000.0);
    scalar("clipCenterY", tr("Center Y"), clip.transform.centerY, &ClipAppearancePatch::centerY, 100.0, -1000000.0, 1000000.0);
    scalar("clipWidth", tr("Width scale"), clip.transform.width, &ClipAppearancePatch::width, 100.0, 0.0001, 1000000.0);
    scalar("clipHeight", tr("Height scale"), clip.transform.height, &ClipAppearancePatch::height, 100.0, 0.0001, 1000000.0);
    scalar("clipRotation", tr("Rotation"), clip.transform.rotation, &ClipAppearancePatch::rotation, 1.0, -360000.0, 360000.0);
    flipHorizontal_ = new QCheckBox(tr("Flip horizontally"), this);
    flipHorizontal_->setObjectName("clipFlipHorizontal");
    flipHorizontal_->setChecked(originalFlipHorizontal_);
    form->addRow(flipHorizontal_);
    flipVertical_ = new QCheckBox(tr("Flip vertically"), this);
    flipVertical_->setObjectName("clipFlipVertical");
    flipVertical_->setChecked(originalFlipVertical_);
    form->addRow(flipVertical_);
    scalar("clipCropLeft", tr("Crop left"), clip.crop.left, &ClipAppearancePatch::cropLeft, 100.0, 0.0, 100.0);
    scalar("clipCropTop", tr("Crop top"), clip.crop.top, &ClipAppearancePatch::cropTop, 100.0, 0.0, 100.0);
    scalar("clipCropRight", tr("Crop right"), clip.crop.right, &ClipAppearancePatch::cropRight, 100.0, 0.0, 100.0);
    scalar("clipCropBottom", tr("Crop bottom"), clip.crop.bottom, &ClipAppearancePatch::cropBottom, 100.0, 0.0, 100.0);
    scalar("clipEdgeRounding", tr("Edge rounding"), clip.edgeRounding, &ClipAppearancePatch::edgeRounding, 100.0, 0.0, 100.0);
    scalar("clipEdgeSoftness", tr("Edge softness"), clip.edgeSoftness, &ClipAppearancePatch::edgeSoftness, 100.0, 0.0, 100.0);
    scalar("clipOpacity", tr("Opacity"), clip.opacity, &ClipAppearancePatch::opacity, 100.0, 0.0, 100.0);
    auto* hint = new QLabel(tr("Position is the clip center as a percentage of the frame. "
        "Width and height scale the default fitted size; positive rotation is clockwise. "
        "These controls set fixed values; existing animation is preserved."), this);
    hint->setWordWrap(true);
    hint->setMaximumWidth(440);
    form->addRow(hint);
    validation_ = new QLabel(this);
    validation_->setObjectName("clipAppearanceValidation");
    validation_->setWordWrap(true);
    validation_->setMaximumWidth(440);
    form->addRow(validation_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    form->addRow(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, &ClipAppearanceDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (const auto& control : scalars_)
        connect(control.spin, &QDoubleSpinBox::valueChanged, this, [this] { updateValidation(); });
    connect(flipHorizontal_, &QCheckBox::toggled, this, [this] { updateValidation(); });
    connect(flipVertical_, &QCheckBox::toggled, this, [this] { updateValidation(); });
    updateValidation();
}

ClipAppearancePatch ClipAppearanceDialog::changedPatch() const {
    ClipAppearancePatch patch;
    for (const auto& control : scalars_) {
        const double value = control.spin->value();
        if (value != control.displayedBase) patch.*(control.field) = value / control.units;
    }
    if (flipHorizontal_->isChecked() != originalFlipHorizontal_)
        patch.flipHorizontal = flipHorizontal_->isChecked();
    if (flipVertical_->isChecked() != originalFlipVertical_)
        patch.flipVertical = flipVertical_->isChecked();
    return patch;
}

bool ClipAppearanceDialog::updateValidation() {
    std::string error;
    const bool valid = validateClipAppearancePatch(original_, changedPatch(), &error);
    validation_->setText(QString::fromStdString(error));
    validation_->setVisible(!valid);
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(valid);
    return valid;
}

void ClipAppearanceDialog::accept() {
    if (updateValidation()) QDialog::accept();
}

} // namespace heisenberg::ui
