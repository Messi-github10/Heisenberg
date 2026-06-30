//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <QObject>
#include <QColor>

namespace heisenberg {
namespace ui {

class ColorGradeModel : public QObject {
    Q_OBJECT

    // === 色轮参数（Lift / Gamma / Gain） ===
    Q_PROPERTY(double  liftRed    READ liftRed    WRITE setLiftRed    NOTIFY liftChanged)
    Q_PROPERTY(double  liftGreen  READ liftGreen  WRITE setLiftGreen  NOTIFY liftChanged)
    Q_PROPERTY(double  liftBlue   READ liftBlue   WRITE setLiftBlue   NOTIFY liftChanged)
    Q_PROPERTY(QColor  liftColor  READ liftColor  WRITE setLiftColor  NOTIFY liftColorChanged)

    Q_PROPERTY(double  gammaRed   READ gammaRed   WRITE setGammaRed   NOTIFY gammaChanged)
    Q_PROPERTY(double  gammaGreen READ gammaGreen WRITE setGammaGreen NOTIFY gammaChanged)
    Q_PROPERTY(double  gammaBlue  READ gammaBlue  WRITE setGammaBlue  NOTIFY gammaChanged)
    Q_PROPERTY(QColor  gammaColor READ gammaColor WRITE setGammaColor NOTIFY gammaColorChanged)

    Q_PROPERTY(double  gainRed    READ gainRed    WRITE setGainRed    NOTIFY gainChanged)
    Q_PROPERTY(double  gainGreen  READ gainGreen  WRITE setGainGreen  NOTIFY gainChanged)
    Q_PROPERTY(double  gainBlue   READ gainBlue   WRITE setGainBlue   NOTIFY gainChanged)
    Q_PROPERTY(QColor  gainColor  READ gainColor  WRITE setGainColor  NOTIFY gainColorChanged)

    // === 整体调整 ===
    Q_PROPERTY(double  offset     READ offset     WRITE setOffset     NOTIFY offsetChanged)
    Q_PROPERTY(double  saturation READ saturation WRITE setSaturation NOTIFY saturationChanged)
    Q_PROPERTY(double  contrast   READ contrast   WRITE setContrast   NOTIFY contrastChanged)

    // === 白平衡 ===
    Q_PROPERTY(double  temperature READ temperature WRITE setTemperature NOTIFY temperatureChanged)
    Q_PROPERTY(double  tint        READ tint        WRITE setTint        NOTIFY tintChanged)

public:
    explicit ColorGradeModel(QObject* parent = nullptr);
    ~ColorGradeModel() override;

    // ---- getters ----
    double  liftRed()      const { return liftRed_; }
    double  liftGreen()    const { return liftGreen_; }
    double  liftBlue()     const { return liftBlue_; }
    QColor  liftColor()    const { return liftColor_; }
    double  gammaRed()     const { return gammaRed_; }
    double  gammaGreen()   const { return gammaGreen_; }
    double  gammaBlue()    const { return gammaBlue_; }
    QColor  gammaColor()   const { return gammaColor_; }
    double  gainRed()      const { return gainRed_; }
    double  gainGreen()    const { return gainGreen_; }
    double  gainBlue()     const { return gainBlue_; }
    QColor  gainColor()    const { return gainColor_; }
    double  offset()       const { return offset_; }
    double  saturation()   const { return saturation_; }
    double  contrast()     const { return contrast_; }
    double  temperature()  const { return temperature_; }
    double  tint()         const { return tint_; }

    // ---- setters ----
    void setLiftRed  (double v);
    void setLiftGreen(double v);
    void setLiftBlue (double v);
    void setLiftColor(const QColor& c);
    void setGammaRed  (double v);
    void setGammaGreen(double v);
    void setGammaBlue (double v);
    void setGammaColor(const QColor& c);
    void setGainRed   (double v);
    void setGainGreen (double v);
    void setGainBlue  (double v);
    void setGainColor (const QColor& c);
    void setOffset    (double v);
    void setSaturation(double v);
    void setContrast  (double v);
    void setTemperature(double v);
    void setTint      (double v);

public slots:
    // 重置所有参数到默认值
    void resetAll();

signals:
    void liftChanged();
    void liftColorChanged();
    void gammaChanged();
    void gammaColorChanged();
    void gainChanged();
    void gainColorChanged();
    void offsetChanged();
    void saturationChanged();
    void contrastChanged();
    void temperatureChanged();
    void tintChanged();

    // 任何参数变化都触发，用于通知渲染管线重建 LUT
    void gradeChanged();

private:
    double  liftRed_      = 0.0;
    double  liftGreen_    = 0.0;
    double  liftBlue_     = 0.0;
    QColor  liftColor_    = {0, 0, 0};
    double  gammaRed_     = 0.0;
    double  gammaGreen_   = 0.0;
    double  gammaBlue_    = 0.0;
    QColor  gammaColor_   = {0, 0, 0};
    double  gainRed_      = 0.0;
    double  gainGreen_    = 0.0;
    double  gainBlue_     = 0.0;
    QColor  gainColor_    = {0, 0, 0};
    double  offset_       = 0.0;
    double  saturation_   = 1.0;
    double  contrast_     = 1.0;
    double  temperature_  = 0.0;
    double  tint_         = 0.0;

    // TODO: Phase 2 — 与 libplacebo 的 pl_renderer 参数对接
    //       ColorGradeModel → 构建 3D LUT / 调整矩阵 → pl_renderer
};

} // namespace ui
} // namespace heisenberg
