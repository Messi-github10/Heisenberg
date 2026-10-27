#include "ColorGradeDialog.hpp"
#include <Compositing/Grade/ColorGrade.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QEnterEvent>
#include <QGridLayout>
#include <QStyle>
#include <QToolButton>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QPointer>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <string_view>

using heisenberg::JsonValue;
namespace {
constexpr double pi = 3.14159265358979323846;
const char* channels[] = {"master", "red", "green", "blue"};
class IgnoreWheelInput final : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Wheel) {
            event->ignore();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
};
QDoubleSpinBox* number(QWidget* parent, const QString& name, double low, double high, double value) {
    auto* field = new QDoubleSpinBox(parent);
    field->installEventFilter(new IgnoreWheelInput(field));
    field->setObjectName(name);
    field->setDecimals(4);
    field->setRange(low, high);
    field->setSingleStep(.1);
    field->setKeyboardTracking(false);
    field->setMinimumWidth(0);
    field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    field->setValue(value);
    return field;
}
JsonValue identityCurve() { return JsonValue::array({JsonValue::array({0.0, 0.0}), JsonValue::array({1.0, 1.0})}); }
QFormLayout* formLayout(QWidget* page) {
    auto* form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    return form;
}
void addPage(QTabWidget* tabs, QWidget* page, const QString& title) {
    auto* scroll = new QScrollArea(tabs);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(0);
    scroll->setWidget(page);
    tabs->addTab(scroll, title);
}
void configureTable(QTableWidget* table) {
    table->setMinimumWidth(0);
    table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setMinimumSectionSize(65);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setWordWrap(true);
}
}

ColorWheelWidget::ColorWheelWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(150, 150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(216);
    setCursor(Qt::CrossCursor);
    setAccessibleName(tr("Color wheel: hue and strength"));
    setToolTip(tr("Drag to set hue and strength. Use the numeric controls for exact values."));
}
QSize ColorWheelWidget::sizeHint() const { return {216, 216}; }
double ColorWheelWidget::wheelRadius() const { return std::max(1.0, std::min(width(), height()) / 2.0 - 22); }
void ColorWheelWidget::setColor(double hue, double strength) {
    hue_ = hue; strength_ = strength; update();
}
void ColorWheelWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPointF center(width() / 2.0, height() / 2.0);
    const double radius = wheelRadius();
    const double diameter = radius * 2;
    const double dpr = devicePixelRatioF();
    const int pixels = std::max(2, static_cast<int>(std::ceil(diameter * dpr)));
    if (disk_.size() != QSize(pixels, pixels) || disk_.devicePixelRatio() != dpr) {
        disk_ = QImage(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
        disk_.setDevicePixelRatio(dpr);
        const double middle = pixels / 2.0;
        for (int y = 0; y < pixels; ++y) {
            auto* row = reinterpret_cast<QRgb*>(disk_.scanLine(y));
            for (int x = 0; x < pixels; ++x) {
                const double dx = (x + .5 - middle) / middle;
                const double dy = (middle - y - .5) / middle;
                const double distance = std::hypot(dx, dy);
                const double coverage = std::clamp((1 - distance) * middle + .5, 0.0, 1.0);
                if (coverage == 0) { row[x] = 0; continue; }
                const double hue = std::fmod(std::atan2(dy, dx) / (2 * pi) + 1, 1.0);
                const double blend = std::pow(std::min(distance, 1.0), .85);
                const auto edge = QColor::fromHsvF(hue, .68, .76);
                const auto mix = [blend](int channel) { return static_cast<int>(std::lround(51 + (channel - 51) * blend)); };
                row[x] = qPremultiply(qRgba(mix(edge.red()), mix(edge.green()), mix(edge.blue()), static_cast<int>(coverage * 255)));
            }
        }
    }
    painter.setPen(Qt::NoPen);
    QRadialGradient shadow(center + QPointF(0, 3), radius + 11);
    shadow.setColorAt(0, Qt::transparent); shadow.setColorAt(.83, QColor(0, 0, 0, 70)); shadow.setColorAt(1, Qt::transparent);
    painter.setBrush(shadow); painter.drawEllipse(center + QPointF(0, 3), radius + 11, radius + 11);
    painter.setBrush(QColor(19, 20, 24)); painter.setPen(QPen(QColor(72, 74, 83), 1));
    painter.drawEllipse(center, radius + 5, radius + 5);
    painter.drawImage(QRectF(center.x() - radius, center.y() - radius, diameter, diameter), disk_);
    painter.setBrush(Qt::NoBrush); painter.setPen(QPen(QColor(255, 255, 255, 28), 1));
    painter.drawEllipse(center, radius, radius);
    for (int i = 0; i < 48; ++i) {
        const double angle = i * 2 * pi / 48;
        const QPointF direction(std::cos(angle), -std::sin(angle));
        const bool major = i % 8 == 0;
        painter.setPen(QPen(major ? QColor(164, 166, 177) : QColor(73, 75, 85), major ? 1.3 : 1));
        painter.drawLine(center + direction * (radius + 9), center + direction * (radius + (major ? 15 : 12)));
    }
    painter.setPen(QPen(QColor(255, 255, 255, 19), 1, Qt::DotLine));
    painter.drawLine(center - QPointF(radius - 2, 0), center + QPointF(radius - 2, 0));
    painter.drawLine(center - QPointF(0, radius - 2), center + QPointF(0, radius - 2));
    painter.setPen(QPen(QColor(255, 255, 255, 30), 1));
    painter.drawLine(center - QPointF(3, 0), center + QPointF(3, 0));
    painter.drawLine(center - QPointF(0, 3), center + QPointF(0, 3));
    const double angle = hue_ * pi / 180;
    const QPointF handle = center + QPointF(std::cos(angle), -std::sin(angle)) * (std::clamp(strength_, 0.0, 1.0) * radius);
    if (strength_ > .04) {
        painter.setPen(QPen(QColor(255, 255, 255, 40), 1));
        painter.drawLine(center, handle);
    }
    painter.setPen(Qt::NoPen); painter.setBrush(QColor(0, 0, 0, 90));
    painter.drawEllipse(handle + QPointF(0, 1.5), 8, 8);
    if (hovered_ || dragging_) {
        painter.setBrush(QColor(255, 255, 255, dragging_ ? 28 : 16));
        painter.drawEllipse(handle, 12, 12);
    }
    painter.setBrush(QColor(31, 32, 38, 210)); painter.setPen(QPen(QColor(245, 246, 250), 1.7));
    painter.drawEllipse(handle, 5.5, 5.5);
    painter.setPen(Qt::NoPen); painter.setBrush(QColor(245, 246, 250)); painter.drawEllipse(handle, 1.2, 1.2);
}
void ColorWheelWidget::moveHandle(const QPointF& position) {
    const QPointF delta = position - QPointF(width() / 2.0, height() / 2.0);
    hue_ = std::fmod(std::atan2(-delta.y(), delta.x()) * 180 / pi + 360, 360);
    strength_ = std::min(1.0, std::hypot(delta.x(), delta.y()) / wheelRadius());
    emit colorChanged(hue_, strength_); update();
}
void ColorWheelWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    const auto delta = event->position() - QPointF(width() / 2.0, height() / 2.0);
    if (std::hypot(delta.x(), delta.y()) > wheelRadius() + 6) return;
    dragging_ = true; moveHandle(event->position()); event->accept();
}
void ColorWheelWidget::mouseMoveEvent(QMouseEvent* event) { if (dragging_) moveHandle(event->position()); }
void ColorWheelWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ && event->button() == Qt::LeftButton) {
        moveHandle(event->position()); dragging_ = false; update(); event->accept(); emit editingFinished();
    }
}
void ColorWheelWidget::enterEvent(QEnterEvent* event) { hovered_ = true; update(); QWidget::enterEvent(event); }
void ColorWheelWidget::leaveEvent(QEvent* event) { hovered_ = false; update(); QWidget::leaveEvent(event); }

GradeCurveWidget::GradeCurveWidget(QWidget* parent) : QWidget(parent), color_(Qt::white) {
    setMinimumSize(260, 200); setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Piecewise linear curve editor"));
    setToolTip(tr("Drag points; double-click to add a point; Delete removes the selected interior point."));
}
QSize GradeCurveWidget::sizeHint() const { return {400, 270}; }
QRectF GradeCurveWidget::graphRect() const { return QRectF(rect()).adjusted(15, 15, -15, -15); }
QPointF GradeCurveWidget::toGraph(const QPointF& position) const {
    const auto area = graphRect();
    return {std::clamp((position.x() - area.left()) / area.width(), 0.0, 1.0),
        std::clamp((area.bottom() - position.y()) / area.height(), 0.0, 1.0)};
}
void GradeCurveWidget::setPoints(const QVector<QPointF>& points) { points_ = points; selected_ = -1; update(); }
void GradeCurveWidget::setChannelColor(const QColor& color) { color_ = color; update(); }
void GradeCurveWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    const auto area = graphRect(); painter.fillRect(area, QColor(25, 27, 31));
    painter.setPen(QPen(QColor(65, 68, 75), 1));
    for (int i = 0; i <= 4; ++i) {
        const double t = i / 4.0;
        painter.drawLine(QPointF(area.left() + t * area.width(), area.top()), QPointF(area.left() + t * area.width(), area.bottom()));
        painter.drawLine(QPointF(area.left(), area.top() + t * area.height()), QPointF(area.right(), area.top() + t * area.height()));
    }
    painter.setPen(QPen(QColor(105, 108, 115), 1, Qt::DashLine)); painter.drawLine(area.bottomLeft(), area.topRight());
    auto position = [&](const QPointF& p) { return QPointF(area.left() + p.x() * area.width(), area.bottom() - p.y() * area.height()); };
    QPainterPath path;
    if (!points_.isEmpty()) path.moveTo(position({0, points_.front().y()}));
    for (const auto& point : points_) path.lineTo(position(point));
    if (!points_.isEmpty()) path.lineTo(position({1, points_.back().y()}));
    painter.setPen(QPen(color_, 2)); painter.drawPath(path);
    for (qsizetype i = 0; i < points_.size(); ++i) {
        painter.setBrush(i == selected_ ? QColor(255, 190, 70) : color_);
        painter.setPen(QPen(QColor(20, 20, 20), 1)); painter.drawEllipse(position(points_[i]), 5, 5);
    }
}
void GradeCurveWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    setFocus(); selected_ = -1;
    const auto area = graphRect();
    for (qsizetype i = 0; i < points_.size(); ++i) {
        const QPointF p(area.left() + points_[i].x() * area.width(), area.bottom() - points_[i].y() * area.height());
        if (std::hypot(p.x() - event->position().x(), p.y() - event->position().y()) < 12) { selected_ = static_cast<int>(i); break; }
    }
    dragging_ = selected_ >= 0; update();
}
void GradeCurveWidget::movePoint(const QPointF& position) {
    if (selected_ < 0) return;
    auto point = toGraph(position);
    point.setX(std::clamp(point.x(), selected_ == 0 ? 0.0 : points_[selected_ - 1].x() + .0001,
        selected_ == points_.size() - 1 ? 1.0 : points_[selected_ + 1].x() - .0001));
    if (points_[selected_] == point) return;
    points_[selected_] = point; emit pointsChanged(); update();
}
void GradeCurveWidget::mouseMoveEvent(QMouseEvent* event) { if (dragging_) movePoint(event->position()); }
void GradeCurveWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ && event->button() == Qt::LeftButton) {
        movePoint(event->position()); dragging_ = false; event->accept(); emit editingFinished();
    }
}
void GradeCurveWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !graphRect().contains(event->position()) || points_.size() >= 64) return;
    const auto point = toGraph(event->position());
    auto at = std::lower_bound(points_.begin(), points_.end(), point.x(), [](const QPointF& p, double x) { return p.x() < x; });
    if (at == points_.begin() || at == points_.end() || std::abs(at->x() - point.x()) < .0001 || std::abs((at - 1)->x() - point.x()) < .0001) return;
    selected_ = static_cast<int>(at - points_.begin()); points_.insert(selected_, point); dragging_ = false;
    emit pointsChanged(); update(); emit editingFinished();
}
void GradeCurveWidget::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && selected_ > 0 && selected_ < points_.size() - 1) {
        points_.removeAt(selected_); selected_ = -1; dragging_ = false; emit pointsChanged(); update(); emit editingFinished();
    } else QWidget::keyPressEvent(event);
}

bool ColorGradeControls::supportsGradeSchema(const JsonValue& grade) {
    return grade.is_object() && grade.contains("schemaVersion") && grade["schemaVersion"].is_number_integer()
        && grade["schemaVersion"] == heisenberg::grade::kColorGradeSchemaVersion;
}

bool ColorGradeDialog::supportsGradeSchema(const JsonValue& grade) {
    return ColorGradeControls::supportsGradeSchema(grade);
}

void ColorGradeDialog::accept() {
    if (!controls_->isEditable()) { reject(); return; }
    controls_->finishEditing();
    QDialog::accept();
}

ColorGradeDialog::ColorGradeDialog(const JsonValue& effects, QWidget* parent) : QDialog(parent) {
    setObjectName("clipColorGradeDialog"); setWindowTitle(tr("Color Grade")); resize(720, 650);
    auto* layout = new QVBoxLayout(this);
    controls_ = new ColorGradeControls(effects, this);
    layout->addWidget(controls_, 1);
    connect(controls_, &ColorGradeControls::effectsChanged, this, &ColorGradeDialog::effectsChanged);
    connect(controls_, &ColorGradeControls::editFinished, this, &ColorGradeDialog::editFinished);
    if (controls_->isEditable()) {
        auto* hint = new QLabel(tr("Editing a numeric parameter sets a constant and replaces only that parameter’s animation. Untouched tracks and imported fields are preserved. Preview updates live; Cancel restores the original grade. OK creates one undo step."), this);
        hint->setObjectName("colorGradeConstantHint"); hint->setWordWrap(true); layout->addWidget(hint);
    }
    auto* buttons = new QDialogButtonBox(controls_->isEditable() ? QDialogButtonBox::Ok | QDialogButtonBox::Cancel : QDialogButtonBox::StandardButtons(QDialogButtonBox::Cancel), this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ColorGradeDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

ColorGradeControls::ColorGradeControls(const JsonValue& effects, QWidget* parent)
    : QWidget(parent), original_(effects), effects_(effects), index_(effects.size()) {
    setObjectName("colorGradeControls");
    setMinimumWidth(0);
    for (size_t i = 0; i < effects_.size(); ++i)
        if (effects_[i].is_object() && effects_[i].contains("type") && effects_[i]["type"] == "video.colorGrade") { index_ = i; break; }
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    if (index_ < effects_.size() && !supportsGradeSchema(effects_[index_])) {
        editable_ = false;
        auto* warning = new QLabel(tr("This color grade has an unsupported or invalid schemaVersion. Only integer schemaVersion 1 can be edited. The original effect is preserved unchanged."), this);
        warning->setObjectName("colorGradeUnsupportedSchema"); warning->setWordWrap(true); layout->addWidget(warning);
        layout->addStretch();
        return;
    }
    auto* enabled = new QCheckBox(tr("Enable color grade"), this); enabled->setObjectName("colorGradeEnabled");
    enabled->setChecked(index_ == effects_.size() || effects_[index_].value("enabled", true)); layout->addWidget(enabled);
    connect(enabled, &QCheckBox::toggled, this, [this](bool checked) { grade()["enabled"] = checked; emit effectsChanged(); requestEditFinished(); });
    auto* tabs = new QTabWidget(this); tabs_ = tabs;
    tabs->setObjectName("colorGradeTabs"); tabs->setTabPosition(QTabWidget::North);
    tabs->setUsesScrollButtons(true); tabs->setElideMode(Qt::ElideRight); tabs->tabBar()->setExpanding(false);
    tabs->setMinimumWidth(0); layout->addWidget(tabs, 1);
    auto* light = new QWidget(tabs); auto* lightForm = formLayout(light);
    const char* lightNames[] = {"exposure", "contrast", "highlights", "shadows", "blacks", "whites"};
    const QString lightLabels[] = {tr("Exposure (EV)"), tr("Contrast"), tr("Highlights"), tr("Shadows"), tr("Blacks"), tr("Whites")};
    const double lightMin[] = {-3, .5, -1, -1, -1, -1};
    const double lightMax[] = {3, 1.5, 1, 1, 1, 1};
    for (int i = 0; i < 6; ++i) lightForm->addRow(lightLabels[i], parameter(light, lightNames[i], lightMin[i], lightMax[i], i == 1 ? 1 : 0));
    auto* evHint = new QLabel(tr("Exposure uses linear EV: +1 EV doubles scene-linear light."), light); evHint->setWordWrap(true); lightForm->addRow(evHint);
    addPage(tabs, light, tr("Light"));
    auto* color = new QWidget(tabs); auto* colorForm = formLayout(color);
    const char* colorNames[] = {"temperature", "tint", "vibrance", "saturation"};
    const QString colorLabels[] = {tr("Temperature"), tr("Tint"), tr("Vibrance"), tr("Saturation")};
    const double colorMin[] = {2000, -100, -1, 0};
    const double colorMax[] = {11000, 100, 1, 2};
    const double colorDefault[] = {6500, 0, 0, 1};
    for (int i = 0; i < 4; ++i) colorForm->addRow(colorLabels[i], parameter(color, colorNames[i], colorMin[i], colorMax[i], colorDefault[i]));
    auto* colorHint = new QLabel(tr("Temperature is in kelvin. Positive tint shifts toward green."), color);
    colorHint->setWordWrap(true); colorForm->addRow(colorHint);
    addPage(tabs, color, tr("Color"));
    auto* wheels = new QWidget(tabs); auto* wheelLayout = new QVBoxLayout(wheels);
    wheels->setObjectName("colorGradeWheelsPage");
    wheels->setStyleSheet(R"(
        QWidget#colorGradeWheelsPage { background: transparent; }
        QWidget[wheelSection="true"] { background: transparent; }
        QLabel[wheelTitle="true"] { color: #f0f1f5; font-size: 12px; font-weight: 600; }
        QLabel[wheelFieldLabel="true"] { color: #9396a4; font-size: 10px; }
        QDoubleSpinBox { background: #202126; color: #e4e6ed; border: 1px solid #3a3c45; border-radius: 4px; padding: 5px 3px; }
        QDoubleSpinBox:focus { border-color: #aeb3c2; }
        QToolButton { background: transparent; border: 0; border-radius: 4px; padding: 3px; }
        QToolButton:hover { background: #383a43; }
    )");
    wheelLayout->setContentsMargins(8, 4, 8, 8);
    wheelLayout->setSpacing(18);
    const char* wheelNames[] = {"lift", "gamma", "gain"};
    const QString wheelTitles[] = {tr("Lift"), tr("Gamma"), tr("Gain")};
    for (int i = 0; i < 3; ++i) {
        const auto* wheelName = wheelNames[i];
        const std::string prefix(wheelName);
        auto* section = new QWidget(wheels); section->setProperty("wheelSection", true);
        auto* sectionLayout = new QVBoxLayout(section); sectionLayout->setContentsMargins(0, 0, 0, 0); sectionLayout->setSpacing(6);
        auto* header = new QHBoxLayout;
        auto* title = new QLabel(wheelTitles[i], section); title->setProperty("wheelTitle", true);
        auto* reset = new QToolButton(section); reset->setObjectName(QString("colorGrade_%1Reset").arg(wheelName));
        reset->setIcon(style()->standardIcon(QStyle::SP_BrowserReload)); reset->setIconSize(QSize(14, 14));
        reset->setFixedSize(24, 24); reset->setToolTip(tr("Reset %1").arg(wheelTitles[i])); reset->setAccessibleName(reset->toolTip());
        header->addWidget(title); header->addStretch(); header->addWidget(reset); sectionLayout->addLayout(header);
        auto* wheel = new ColorWheelWidget(section); wheel->setObjectName(QString("colorGrade_%1Wheel").arg(wheelName)); sectionLayout->addWidget(wheel);
        auto* hue = parameter(section, (prefix + "Hue").c_str(), 0, 360, 0);
        auto* strength = parameter(section, (prefix + "Strength").c_str(), 0, 1, 0);
        const bool lift = prefix == "lift";
        auto* master = parameter(section, (prefix + "Master").c_str(), lift ? -.5 : .5, lift ? .5 : (prefix == "gamma" ? 2 : 1.5), lift ? 0 : 1);
        hue->setSuffix(QString::fromUtf8("\xc2\xb0"));
        const QString labels[] = {tr("Hue"), tr("Strength"), tr("Master")};
        QDoubleSpinBox* fields[] = {hue, strength, master};
        auto* values = new QGridLayout; values->setHorizontalSpacing(8); values->setVerticalSpacing(4);
        for (int column = 0; column < 3; ++column) {
            auto* label = new QLabel(labels[column], section); label->setProperty("wheelFieldLabel", true); label->setBuddy(fields[column]);
            fields[column]->setButtonSymbols(QAbstractSpinBox::NoButtons); fields[column]->setAlignment(Qt::AlignCenter);
            fields[column]->setToolTip(labels[column]);
            values->addWidget(label, 0, column, Qt::AlignHCenter); values->addWidget(fields[column], 1, column); values->setColumnStretch(column, 1);
        }
        sectionLayout->addLayout(values);
        auto sync = [wheel, hue, strength]() { wheel->setColor(hue->value(), strength->value()); };
        connect(hue, &QDoubleSpinBox::valueChanged, wheel, sync); connect(strength, &QDoubleSpinBox::valueChanged, wheel, sync);
        connect(wheel, &ColorWheelWidget::colorChanged, this, [hue, strength](double h, double s) { hue->setValue(h); strength->setValue(s); }); sync();
        connect(wheel, &ColorWheelWidget::editingFinished, this, &ColorGradeControls::requestEditFinished);
        connect(reset, &QToolButton::clicked, this, [this, hue, strength, master, lift] {
            hue->setValue(0); strength->setValue(0); master->setValue(lift ? 0 : 1); requestEditFinished();
        });
        wheelLayout->addWidget(section);
    }
    wheelLayout->addStretch();
    addPage(tabs, wheels, tr("Wheels"));
    auto* curves = new QWidget(tabs); auto* curvesLayout = new QVBoxLayout(curves);
    channel_ = new QComboBox(curves); channel_->setObjectName("colorGradeCurveChannel"); channel_->addItems({tr("Master"), tr("Red"), tr("Green"), tr("Blue")}); curvesLayout->addWidget(channel_);
    curve_ = new GradeCurveWidget(curves); curve_->setObjectName("colorGradeCurveGraph"); curvesLayout->addWidget(curve_, 1);
    curveTable_ = new QTableWidget(curves); curveTable_->setObjectName("colorGradeCurvePoints"); curveTable_->setColumnCount(2); curveTable_->setHorizontalHeaderLabels({tr("Input"), tr("Output")}); configureTable(curveTable_); curveTable_->setMinimumHeight(110); curveTable_->setMaximumHeight(165); curvesLayout->addWidget(curveTable_);
    auto* curveButtons = new QVBoxLayout;
    auto* addPoint = new QPushButton(tr("Add point"), curves); addPoint->setObjectName("colorGradeCurveAdd");
    auto* removePoint = new QPushButton(tr("Remove point"), curves); removePoint->setObjectName("colorGradeCurveRemove");
    auto* resetCurve = new QPushButton(tr("Reset channel"), curves); resetCurve->setObjectName("colorGradeCurveReset");
    curveButtons->addWidget(addPoint); curveButtons->addWidget(removePoint); curveButtons->addWidget(resetCurve); curvesLayout->addLayout(curveButtons);
    connect(channel_, &QComboBox::currentIndexChanged, this, [this] { loadCurve(); });
    connect(curve_, &GradeCurveWidget::pointsChanged, this, [this] { saveCurve(); updateCurveTable(); });
    connect(curve_, &GradeCurveWidget::editingFinished, this, &ColorGradeControls::requestEditFinished);
    connect(addPoint, &QPushButton::clicked, this, [this] {
        auto points = curve_->points(); if (points.size() >= 64) return; int gap = 0;
        for (int i = 1; i < points.size() - 1; ++i) if (points[i + 1].x() - points[i].x() > points[gap + 1].x() - points[gap].x()) gap = i;
        if (points[gap + 1].x() - points[gap].x() < .0002) return;
        points.insert(gap + 1, (points[gap] + points[gap + 1]) / 2); curve_->setPoints(points); saveCurve(); updateCurveTable(); requestEditFinished();
    });
    connect(removePoint, &QPushButton::clicked, this, [this] {
        const int row = curveTable_->currentRow(); auto points = curve_->points();
        if (row <= 0 || row >= points.size() - 1) return;
        points.removeAt(row); curve_->setPoints(points); saveCurve(); updateCurveTable(); requestEditFinished();
    });
    connect(resetCurve, &QPushButton::clicked, this, [this] { curve_->setPoints({{0, 0}, {1, 1}}); saveCurve(); updateCurveTable(); requestEditFinished(); });
    loadCurve(); addPage(tabs, curves, tr("Curves"));
    auto* hues = new QWidget(tabs); auto* huesLayout = new QVBoxLayout(hues);
    auto* hueHint = new QLabel(tr("Target hue and rotation use degrees. Saturation is a multiplier; lightness is an offset."), hues); hueHint->setWordWrap(true); huesLayout->addWidget(hueHint);
    hueTable_ = new QTableWidget(hues); hueTable_->setObjectName("colorGradeHueTargets"); hueTable_->setColumnCount(4); hueTable_->setHorizontalHeaderLabels({tr("Hue (°)"), tr("Rotation (°)"), tr("Saturation"), tr("Lightness")}); configureTable(hueTable_); hueTable_->setMinimumHeight(180); huesLayout->addWidget(hueTable_, 1);
    auto* hueButtons = new QHBoxLayout; auto* addHue = new QPushButton(tr("Add target"), hues); addHue->setObjectName("colorGradeHueAdd"); auto* removeHue = new QPushButton(tr("Remove target"), hues); removeHue->setObjectName("colorGradeHueRemove"); hueButtons->addWidget(addHue); hueButtons->addWidget(removeHue); huesLayout->addLayout(hueButtons);
    connect(addHue, &QPushButton::clicked, this, [this] {
        auto& targets = grade()["params"]["hueTargets"]; if (!targets.is_array()) targets = JsonValue::array();
        if (targets.size() >= 64) return;
        targets.push_back({{"hue", std::fmod(targets.size() * 60.0, 360.0)}, {"rotation", 0.0}, {"saturation", 1.0}, {"lightness", 0.0}}); updateHueTable(); emit effectsChanged(); requestEditFinished();
    });
    connect(removeHue, &QPushButton::clicked, this, [this] {
        const int row = hueTable_->currentRow(); if (row < 0) return;
        auto& targets = grade()["params"]["hueTargets"]; if (targets.is_array() && row < targets.size()) { targets.erase(targets.begin() + row); updateHueTable(); emit effectsChanged(); requestEditFinished(); }
    });
    updateHueTable(); addPage(tabs, hues, tr("Hue Targets"));
    auto* lut = new QWidget(tabs); auto* lutForm = formLayout(lut);
    JsonValue savedLut = JsonValue::object();
    if (index_ < effects_.size() && effects_[index_].contains("params") && effects_[index_]["params"].is_object()) savedLut = effects_[index_]["params"].value("lut", JsonValue::object());
    if (!savedLut.is_object()) savedLut = JsonValue::object();
    lutName_ = new QLabel(QString::fromStdString(savedLut.value("name", std::string("No LUT selected"))), lut); lutName_->setObjectName("colorGradeLutName"); lutName_->setWordWrap(true); lutForm->addRow(tr("LUT"), lutName_);
    auto* import = new QPushButton(tr("Import .cube..."), lut); import->setObjectName("colorGradeLutImport"); lutForm->addRow(import); connect(import, &QPushButton::clicked, this, &ColorGradeControls::importLut);
    auto* domain = new QComboBox(lut); domain->setObjectName("colorGradeLutDomain"); domain->addItem(tr("Rec.709"), "rec709"); domain->addItem(tr("ACEScct"), "acescct"); domain->setCurrentIndex(std::max(0, domain->findData(QString::fromStdString(savedLut.value("domain", std::string("rec709")))))); lutForm->addRow(tr("Domain"), domain);
    lutDomain_ = domain;
    connect(domain, &QComboBox::currentIndexChanged, this, [this, domain] { grade()["params"]["lut"]["domain"] = domain->currentData().toString().toStdString(); emit effectsChanged(); requestEditFinished(); });
    double intensity = 1;
    if (savedLut.contains("intensity") && savedLut["intensity"].is_object()) intensity = savedLut["intensity"].value("value", 1.0);
    auto* lutIntensity = number(lut, "colorGradeLutIntensity", 0, 1, intensity); lutForm->addRow(tr("Intensity"), lutIntensity);
    lutIntensity_ = lutIntensity;
    watchEditing(lutIntensity);
    const bool hasLut = savedLut.contains("id") && savedLut["id"].is_string() && !savedLut["id"].get<std::string>().empty();
    lutDomain_->setEnabled(hasLut); lutIntensity_->setEnabled(hasLut);
    connect(lutIntensity, &QDoubleSpinBox::valueChanged, this, [this](double value) { auto& field = grade()["params"]["lut"]["intensity"]; if (!field.is_object()) field = JsonValue::object(); field["value"] = value; field.erase("track"); emit effectsChanged(); });
    auto* clearLut = new QPushButton(tr("Remove LUT"), lut); clearLut->setObjectName("colorGradeLutRemove"); lutForm->addRow(clearLut);
    connect(clearLut, &QPushButton::clicked, this, [this] { if (index_ >= effects_.size()) return; grade()["params"].erase("lut"); lutName_->setText(tr("No LUT selected")); lutDomain_->setEnabled(false); lutIntensity_->setEnabled(false); emit effectsChanged(); requestEditFinished(); });
    lutError_ = new QLabel(lut); lutError_->setObjectName("colorGradeLutError"); lutError_->setWordWrap(true); lutForm->addRow(lutError_); addPage(tabs, lut, tr("LUT"));
}

int ColorGradeControls::currentTab() const { return tabs_ ? tabs_->currentIndex() : -1; }

void ColorGradeControls::setCurrentTab(int index) {
    if (tabs_ && index >= 0 && index < tabs_->count()) tabs_->setCurrentIndex(index);
}

void ColorGradeControls::rebase() {
    original_ = effects_;
    for (auto* field : findChildren<QDoubleSpinBox*>())
        if (field->property("colorGradeParameterBaseline").isValid())
            field->setProperty("colorGradeParameterBaseline", field->value());
    editFinishPending_ = false;
}

bool ColorGradeControls::isDragging() const {
    if (curve_ && curve_->isDragging()) return true;
    for (auto* wheel : findChildren<ColorWheelWidget*>()) if (wheel->isDragging()) return true;
    return false;
}

void ColorGradeControls::watchEditing(QDoubleSpinBox* field) {
    connect(field, &QDoubleSpinBox::editingFinished, this, &ColorGradeControls::requestEditFinished);
}

void ColorGradeControls::requestEditFinished() {
    if (!editable_) return;
    editFinishPending_ = true;
    if (editFinishQueued_ || finishingEditing_ || importingLut_) return;
    editFinishQueued_ = true;
    // A focus-out can occur before a wheel press, or inside a table editor's event.
    // Wait until that event is over, and leave any drag pending until its release.
    QTimer::singleShot(0, this, [this] {
        editFinishQueued_ = false;
        if (!editFinishPending_ || finishingEditing_ || importingLut_ || isDragging()) return;
        editFinishPending_ = false;
        emit editFinished();
    });
}

void ColorGradeControls::finishEditing() {
    if (!editable_ || finishingEditing_) return;
    finishingEditing_ = true;
    QWidget* focus = QApplication::focusWidget();
    if (focus && isAncestorOf(focus)) {
        for (QWidget* editor = focus; editor && editor != this; editor = editor->parentWidget()) {
            if (auto* field = qobject_cast<QDoubleSpinBox*>(editor)) {
                field->interpretText();
                field->clearFocus();
                break;
            }
        }
    }
    finishingEditing_ = false;
    requestEditFinished();
}

JsonValue& ColorGradeControls::grade() {
    if (index_ == effects_.size()) effects_.push_back({{"id", "color-grade"}, {"type", "video.colorGrade"}, {"schemaVersion", 1}, {"enabled", true}, {"params", JsonValue::object()}});
    auto& effect = effects_[index_];
    if (!effect.contains("params") || !effect["params"].is_object()) effect["params"] = JsonValue::object();
    return effect;
}
double ColorGradeControls::parameterValue(const char* name, double fallback) const {
    if (index_ >= effects_.size() || !effects_[index_].contains("params") || !effects_[index_]["params"].is_object()) return fallback;
    const auto& params = effects_[index_]["params"];
    if (!params.contains(name)) return fallback;
    const auto& parameter = params[name];
    if (parameter.is_number()) return parameter.get<double>();
    if (parameter.is_object() && parameter.contains("value") && parameter["value"].is_number()) return parameter["value"].get<double>();
    return fallback;
}
QDoubleSpinBox* ColorGradeControls::parameter(QWidget* parent, const char* name, double minimum, double maximum, double fallback) {
    for (const auto& spec : heisenberg::grade::colorGradeParameterRegistry()) {
        if (std::string_view(spec.name) == name) { minimum = spec.minimum; maximum = spec.maximum; fallback = spec.defaultValue; break; }
    }
    const double initial = parameterValue(name, fallback);
    auto* field = number(parent, QString("colorGrade_%1").arg(name), minimum, maximum, initial);
    if (std::string_view(name) == "temperature") field->setSingleStep(100);
    else if (std::string_view(name) == "tint" || std::string_view(name).ends_with("Hue")) field->setSingleStep(1);
    field->setProperty("colorGradeParameterBaseline", field->value());
    const std::string key(name);
    connect(field, &QDoubleSpinBox::valueChanged, this, [this, key, field](double value) { setParameter(key.c_str(), value, field->property("colorGradeParameterBaseline").toDouble()); });
    watchEditing(field);
    return field;
}
void ColorGradeControls::setParameter(const char* name, double value, double initial) {
    auto& params = grade()["params"];
    if (value == initial) {
        if (index_ < original_.size() && original_[index_].contains("params") && original_[index_]["params"].is_object() && original_[index_]["params"].contains(name)) params[name] = original_[index_]["params"][name];
        else params.erase(name);
    } else {
        auto& field = params[name]; if (!field.is_object()) field = JsonValue::object();
        field["value"] = value; field.erase("track");
    }
    if (index_ < original_.size() && (!original_[index_].contains("params") || !original_[index_]["params"].is_object()) && params.empty())
        effects_[index_].erase("params");
    if (index_ == original_.size() && params.empty() && effects_[index_].value("enabled", true)) effects_.erase(effects_.begin() + index_);
    emit effectsChanged();
}
void ColorGradeControls::loadCurve() {
    auto values = identityCurve();
    if (index_ < effects_.size() && effects_[index_].contains("params") && effects_[index_]["params"].is_object()) {
        const auto& params = effects_[index_]["params"];
        if (params.contains("curves") && params["curves"].is_object()) values = params["curves"].value(channels[channel_->currentIndex()], identityCurve());
    }
    QVector<QPointF> points;
    if (values.is_array()) for (const auto& point : values) if (point.is_array() && point.size() == 2 && point[0].is_number() && point[1].is_number()) points.push_back({point[0].get<double>(), point[1].get<double>()});
    bool valid = points.size() >= 2 && points.size() <= 64;
    for (int i = 0; i < points.size(); ++i) valid &= std::isfinite(points[i].x()) && std::isfinite(points[i].y()) && points[i].x() >= 0 && points[i].x() <= 1 && points[i].y() >= 0 && points[i].y() <= 1 && (i == 0 || points[i].x() > points[i - 1].x());
    if (!valid) points = {{0, 0}, {1, 1}};
    curve_->setPoints(points); const QColor colors[] = {Qt::white, QColor(255, 95, 95), QColor(90, 220, 130), QColor(100, 155, 255)}; curve_->setChannelColor(colors[channel_->currentIndex()]); updateCurveTable();
}
void ColorGradeControls::saveCurve() {
    const auto& points = curve_->points();
    const bool identity = points.size() >= 2 && points.front() == QPointF(0, 0) && points.back() == QPointF(1, 1)
        && std::all_of(points.begin(), points.end(), [](const QPointF& point) { return point.x() == point.y(); });
    const auto* channel = channels[channel_->currentIndex()];
    // Normalize only the channel the user edited, preserving every imported sibling.
    if (identity) {
        if (index_ < effects_.size()) {
            auto& effect = effects_[index_];
            if (effect.contains("params") && effect["params"].is_object()) {
                auto& params = effect["params"];
                if (params.contains("curves") && params["curves"].is_object()) {
                    params["curves"].erase(channel);
                    if (params["curves"].empty()) params.erase("curves");
                }
            }
        }
    } else {
        auto values = JsonValue::array();
        for (const auto& point : points) values.push_back(JsonValue::array({point.x(), point.y()}));
        auto& curves = grade()["params"]["curves"]; if (!curves.is_object()) curves = JsonValue::object();
        curves[channel] = std::move(values);
    }
    emit effectsChanged();
}
void ColorGradeControls::updateCurveTable() {
    const auto points = curve_->points(); const int selected = curveTable_->currentRow();
    // Keep editors alive while they deliver valueChanged/editingFinished. Only retire
    // removed rows, silencing focus-out signals before Qt deletes their widgets.
    for (int row = static_cast<int>(points.size()); row < curveTable_->rowCount(); ++row)
        for (int column = 0; column < 2; ++column)
            if (auto* old = curveTable_->cellWidget(row, column)) {
                old->blockSignals(true); old->setObjectName({}); disconnect(old, nullptr, this, nullptr);
            }
    curveTable_->setRowCount(static_cast<int>(points.size()));
    for (int row = 0; row < points.size(); ++row) for (int column = 0; column < 2; ++column) {
        auto* field = qobject_cast<QDoubleSpinBox*>(curveTable_->cellWidget(row, column));
        if (!field) {
            field = number(curveTable_, QString(column == 0 ? "curveInput_%1" : "curveOutput_%1").arg(row), 0, 1, 0);
            curveTable_->setCellWidget(row, column, field);
            connect(field, &QDoubleSpinBox::valueChanged, this, [this, row, column](double value) {
                auto points = curve_->points();
                if (row >= points.size()) return;
                if (column == 0) points[row].setX(value); else points[row].setY(value);
                curve_->setPoints(points); saveCurve(); updateCurveTable();
            });
            watchEditing(field);
        }
        field->setObjectName(QString(column == 0 ? "curveInput_%1" : "curveOutput_%1").arg(row));
        const QSignalBlocker blocker(field);
        field->setRange(column == 0 && row > 0 ? points[row - 1].x() + .0001 : 0,
            column == 0 && row < points.size() - 1 ? points[row + 1].x() - .0001 : 1);
        field->setValue(column == 0 ? points[row].x() : points[row].y());
    }
    if (selected >= 0 && selected < points.size()) curveTable_->selectRow(selected);
}
void ColorGradeControls::updateHueTable() {
    JsonValue targets = JsonValue::array();
    if (index_ < effects_.size() && effects_[index_].contains("params") && effects_[index_]["params"].is_object()) targets = effects_[index_]["params"].value("hueTargets", JsonValue::array());
    if (!targets.is_array()) targets = JsonValue::array();
    for (int row = 0; row < hueTable_->rowCount(); ++row) for (int column = 0; column < 4; ++column)
        if (auto* old = hueTable_->cellWidget(row, column)) {
            old->blockSignals(true); old->setObjectName({}); disconnect(old, nullptr, this, nullptr);
        }
    hueTable_->setRowCount(static_cast<int>(targets.size()));
    const char* keys[] = {"hue", "rotation", "saturation", "lightness"}; const double low[] = {0, -30, 0, -.5}; const double high[] = {360, 30, 2, .5};
    for (int row = 0; row < targets.size(); ++row) for (int column = 0; column < 4; ++column) {
        const auto& target = targets[row];
        const double fallback = column == 2 ? 1.0 : 0.0;
        double value = fallback;
        if (target.is_object() && target.contains(keys[column])) {
            const auto& item = target[keys[column]];
            if (item.is_number()) value = item.get<double>();
            else if (item.is_object() && item.contains("value") && item["value"].is_number()) value = item["value"].get<double>();
        }
        auto* field = number(hueTable_, QString("hueTarget_%1_%2").arg(row).arg(keys[column]), low[column], high[column], value); hueTable_->setCellWidget(row, column, field); const std::string key(keys[column]);
        connect(field, &QDoubleSpinBox::valueChanged, this, [this, row, key](double v) { changeHueCell(row, key.c_str(), v); });
        watchEditing(field);
    }
}
void ColorGradeControls::changeHueCell(int row, const char* key, double value) {
    auto& target = grade()["params"]["hueTargets"][row]; if (!target.is_object()) target = JsonValue::object();
    auto& field = target[key];
    if (field.is_object()) {
        if (field.contains("value") && field["value"].is_number() && field["value"].get<double>() == value) return;
        field["value"] = value; field.erase("track");
    } else {
        if (field.is_number() && field.get<double>() == value) return;
        field = value;
    }
    emit effectsChanged();
}
void ColorGradeControls::importLut() {
    importingLut_ = true;
    const QPointer<ColorGradeControls> guard(this);
    const auto path = QFileDialog::getOpenFileName(this, tr("Import 3D LUT"), {}, tr("Cube LUT (*.cube);;All files (*)"));
    if (!guard) return;
    importingLut_ = false;
    if (path.isEmpty()) { requestEditFinished(); return; }
    QString id, name, error;
    if (!lutImporter_ || !lutImporter_(path, id, name, error)) { lutError_->setText(error.isEmpty() ? tr("LUT library is unavailable.") : error); requestEditFinished(); return; }
    auto& lut = grade()["params"]["lut"]; if (!lut.is_object()) lut = JsonValue::object(); lut["id"] = id.toStdString(); lut["name"] = name.toStdString();
    if (!lut.contains("domain")) lut["domain"] = lutDomain_->currentData().toString().toStdString();
    if (!lut.contains("intensity")) lut["intensity"] = {{"value", lutIntensity_->value()}};
    lutDomain_->setEnabled(true); lutIntensity_->setEnabled(true);
    lutName_->setText(name); lutError_->clear(); emit effectsChanged(); requestEditFinished();
}
