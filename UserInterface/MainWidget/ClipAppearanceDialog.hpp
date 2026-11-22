#pragma once

#include <Models/ClipAppearance.hpp>
#include <QDialog>
#include <vector>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QDoubleSpinBox;

namespace heisenberg::ui {

class ClipAppearanceDialog final : public QDialog {
public:
    explicit ClipAppearanceDialog(const Clip& clip, QWidget* parent = nullptr);
    ClipAppearancePatch changedPatch() const;
    void accept() override;

private:
    bool updateValidation();
    Clip original_;
    QLabel* validation_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    struct ScalarControl {
        QDoubleSpinBox* spin = nullptr;
        double displayedBase = 0.0;
        double units = 1.0;
        std::optional<double> ClipAppearancePatch::* field = nullptr;
    };
    std::vector<ScalarControl> scalars_;
    QComboBox* blendMode_ = nullptr;
    int originalBlendModeIndex_ = -1;
    QCheckBox* flipHorizontal_ = nullptr;
    QCheckBox* flipVertical_ = nullptr;
    bool originalFlipHorizontal_ = false;
    bool originalFlipVertical_ = false;
};

} // namespace heisenberg::ui
