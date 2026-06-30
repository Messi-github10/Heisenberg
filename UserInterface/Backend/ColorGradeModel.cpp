//
// Created by NiceFold on 2026/6/30.
//

#include "ColorGradeModel.hpp"

namespace heisenberg {
namespace ui {

ColorGradeModel::ColorGradeModel(QObject* parent)
    : QObject(parent)
{
}

ColorGradeModel::~ColorGradeModel() = default;

// ============================================================
// 色轮 setter — 每修改一个参数触发两次信号：
//   1. 具体信号 (如 liftChanged)
//   2. 聚合信号 gradeChanged（通知渲染管线重建 LUT）
// ============================================================

void ColorGradeModel::setLiftRed(double v)   { if (liftRed_ != v) { liftRed_ = v; emit liftChanged(); emit gradeChanged(); } }
void ColorGradeModel::setLiftGreen(double v) { if (liftGreen_ != v) { liftGreen_ = v; emit liftChanged(); emit gradeChanged(); } }
void ColorGradeModel::setLiftBlue(double v)  { if (liftBlue_ != v) { liftBlue_ = v; emit liftChanged(); emit gradeChanged(); } }
void ColorGradeModel::setLiftColor(const QColor& c) {
    if (liftColor_ != c) {
        liftColor_ = c;
        liftRed_   = c.redF();
        liftGreen_ = c.greenF();
        liftBlue_  = c.blueF();
        emit liftColorChanged();
        emit liftChanged();
        emit gradeChanged();
    }
}

void ColorGradeModel::setGammaRed(double v)   { if (gammaRed_ != v) { gammaRed_ = v; emit gammaChanged(); emit gradeChanged(); } }
void ColorGradeModel::setGammaGreen(double v) { if (gammaGreen_ != v) { gammaGreen_ = v; emit gammaChanged(); emit gradeChanged(); } }
void ColorGradeModel::setGammaBlue(double v)  { if (gammaBlue_ != v) { gammaBlue_ = v; emit gammaChanged(); emit gradeChanged(); } }
void ColorGradeModel::setGammaColor(const QColor& c) {
    if (gammaColor_ != c) {
        gammaColor_ = c;
        gammaRed_   = c.redF();
        gammaGreen_ = c.greenF();
        gammaBlue_  = c.blueF();
        emit gammaColorChanged();
        emit gammaChanged();
        emit gradeChanged();
    }
}

void ColorGradeModel::setGainRed(double v)   { if (gainRed_ != v) { gainRed_ = v; emit gainChanged(); emit gradeChanged(); } }
void ColorGradeModel::setGainGreen(double v) { if (gainGreen_ != v) { gainGreen_ = v; emit gainChanged(); emit gradeChanged(); } }
void ColorGradeModel::setGainBlue(double v)  { if (gainBlue_ != v) { gainBlue_ = v; emit gainChanged(); emit gradeChanged(); } }
void ColorGradeModel::setGainColor(const QColor& c) {
    if (gainColor_ != c) {
        gainColor_ = c;
        gainRed_   = c.redF();
        gainGreen_ = c.greenF();
        gainBlue_  = c.blueF();
        emit gainColorChanged();
        emit gainChanged();
        emit gradeChanged();
    }
}

void ColorGradeModel::setOffset(double v)     { if (offset_ != v) { offset_ = v; emit offsetChanged(); emit gradeChanged(); } }
void ColorGradeModel::setSaturation(double v) { if (saturation_ != v) { saturation_ = v; emit saturationChanged(); emit gradeChanged(); } }
void ColorGradeModel::setContrast(double v)   { if (contrast_ != v) { contrast_ = v; emit contrastChanged(); emit gradeChanged(); } }
void ColorGradeModel::setTemperature(double v){ if (temperature_ != v) { temperature_ = v; emit temperatureChanged(); emit gradeChanged(); } }
void ColorGradeModel::setTint(double v)       { if (tint_ != v) { tint_ = v; emit tintChanged(); emit gradeChanged(); } }

void ColorGradeModel::resetAll()
{
    setLiftColor({0, 0, 0});
    setGammaColor({0, 0, 0});
    setGainColor({0, 0, 0});
    setOffset(0.0);
    setSaturation(1.0);
    setContrast(1.0);
    setTemperature(0.0);
    setTint(0.0);
}

} // namespace ui
} // namespace heisenberg
