#pragma once

#include <Models/Clip.hpp>
#include <QColor>
#include <QImage>
#include <QDialog>
#include <QWidget>
#include <QVector>
#include <QPointF>
#include <QRectF>
#include <functional>
#include <utility>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QTabWidget;
class QTableWidget;

// Hue is in degrees; distance from the center is the wheel's strength.
class ColorWheelWidget final : public QWidget {
    Q_OBJECT
public:
    explicit ColorWheelWidget(QWidget* parent = nullptr);
    void setColor(double hue, double strength);
    bool isDragging() const { return dragging_; }
    QSize sizeHint() const override;
signals:
    void colorChanged(double hue, double strength);
    void editingFinished();
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    double wheelRadius() const;
    void moveHandle(const QPointF& position);
    QImage disk_;
    double hue_ = 0.0;
    double strength_ = 0.0;
    bool dragging_ = false;
    bool hovered_ = false;
};

// The graph draws the exact piecewise-linear function sent to the GPU.
class GradeCurveWidget final : public QWidget {
    Q_OBJECT
public:
    explicit GradeCurveWidget(QWidget* parent = nullptr);
    void setPoints(const QVector<QPointF>& points);
    const QVector<QPointF>& points() const { return points_; }
    void setChannelColor(const QColor& color);
    bool isDragging() const { return dragging_; }
    QSize sizeHint() const override;
signals:
    void pointsChanged();
    void editingFinished();
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
private:
    QRectF graphRect() const;
    QPointF toGraph(const QPointF& position) const;
    void movePoint(const QPointF& position);
    QVector<QPointF> points_{{0, 0}, {1, 1}};
    QColor color_;
    int selected_ = -1;
    bool dragging_ = false;
};

// Button-free controls shared by the modal dialog and the grading sidebar.
class ColorGradeControls final : public QWidget {
    Q_OBJECT
public:
    explicit ColorGradeControls(const heisenberg::JsonValue& effects, QWidget* parent = nullptr);
    static bool supportsGradeSchema(const heisenberg::JsonValue& grade);
    const heisenberg::JsonValue& effects() const { return effects_; }
    // The importer returns the framework's content-addressed LUT identity/display name.
    using LutImporter = std::function<bool(const QString&, QString&, QString&, QString&)>;
    void setLutImporter(LutImporter importer) { lutImporter_ = std::move(importer); }
    bool isEditable() const { return editable_; }
    int currentTab() const;
    void setCurrentTab(int index);
    // Adopt the committed state as the restoration baseline without rebuilding editors.
    void rebase();
    // Synchronously interpret the focused numeric editor before reading effects().
    // editFinished remains deferred, and is never emitted in the middle of a drag.
    void finishEditing();
signals:
    void effectsChanged();
    // Delivered after the originating editor event, so a consumer may replace controls.
    void editFinished();
private:
    heisenberg::JsonValue& grade();
    double parameterValue(const char* name, double fallback) const;
    QDoubleSpinBox* parameter(QWidget* parent, const char* name, double minimum, double maximum, double fallback);
    void setParameter(const char* name, double value, double initial);
    void watchEditing(QDoubleSpinBox* field);
    void requestEditFinished();
    bool isDragging() const;
    void updateCurveTable();
    void saveCurve();
    void loadCurve();
    void updateHueTable();
    void changeHueCell(int row, const char* key, double value);
    void importLut();
    heisenberg::JsonValue original_;
    heisenberg::JsonValue effects_;
    size_t index_ = 0;
    bool editable_ = true;
    bool finishingEditing_ = false;
    bool editFinishPending_ = false;
    bool editFinishQueued_ = false;
    bool importingLut_ = false;
    QTabWidget* tabs_ = nullptr;
    QComboBox* channel_ = nullptr;
    GradeCurveWidget* curve_ = nullptr;
    QTableWidget* curveTable_ = nullptr;
    QTableWidget* hueTable_ = nullptr;
    QLabel* lutName_ = nullptr;
    QLabel* lutError_ = nullptr;
    QComboBox* lutDomain_ = nullptr;
    QDoubleSpinBox* lutIntensity_ = nullptr;
    LutImporter lutImporter_;
};

// Compatibility wrapper: the controls own grading state, the dialog owns its actions.
class ColorGradeDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ColorGradeDialog(const heisenberg::JsonValue& effects, QWidget* parent = nullptr);
    static bool supportsGradeSchema(const heisenberg::JsonValue& grade);
    void accept() override;
    const heisenberg::JsonValue& effects() const { return controls_->effects(); }
    using LutImporter = ColorGradeControls::LutImporter;
    void setLutImporter(LutImporter importer) { controls_->setLutImporter(std::move(importer)); }
    ColorGradeControls* controls() const { return controls_; }
signals:
    void effectsChanged();
    void editFinished();
private:
    ColorGradeControls* controls_ = nullptr;
};
