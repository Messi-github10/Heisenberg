#include "ColorGradeDialog.hpp"
#include <Compositing/Grade/ColorGrade.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
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
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <string_view>

using heisenberg::JsonValue;
namespace {
constexpr double pi = 3.14159265358979323846;
const char* channels[] = {"master", "red", "green", "blue"};
QDoubleSpinBox* number(QWidget* parent, const QString& name, double low, double high, double value) {
    auto* field = new QDoubleSpinBox(parent);
    field->setObjectName(name);
    field->setDecimals(4);
    field->setRange(low, high);
    field->setSingleStep(.1);
    field->setKeyboardTracking(false);
    field->setValue(value);
    return field;
}
JsonValue identityCurve() { return JsonValue::array({JsonValue::array({0.0, 0.0}), JsonValue::array({1.0, 1.0})}); }
}

ColorWheelWidget::ColorWheelWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(130, 130);
    setAccessibleName(tr("Color wheel: hue and strength"));
    setToolTip(tr("Drag to set hue and strength. Use the numeric controls for exact values."));
}
QSize ColorWheelWidget::sizeHint() const { return {190, 190}; }
void ColorWheelWidget::setColor(double hue, double strength) {
    hue_ = hue; strength_ = strength; update();
}
void ColorWheelWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPointF center(width() / 2.0, height() / 2.0);
    const double radius = std::min(width(), height()) / 2.0 - 10;
    QConicalGradient hue(center, 0);
    for (int i = 0; i <= 12; ++i) hue.setColorAt(i / 12.0, QColor::fromHsvF(i / 12.0, 1, 1));
    painter.setPen(Qt::NoPen); painter.setBrush(hue);
    painter.drawEllipse(center, radius, radius);
    QRadialGradient neutral(center, radius);
    neutral.setColorAt(0, QColor(140, 140, 140)); neutral.setColorAt(1, QColor(140, 140, 140, 0));
    painter.setBrush(neutral); painter.drawEllipse(center, radius, radius);
    const double angle = hue_ * pi / 180;
    const double distance = std::clamp(strength_, 0.0, 1.0) * radius;
    const QPointF handle = center + QPointF(std::cos(angle), -std::sin(angle)) * distance;
    painter.setPen(QPen(Qt::black, 3)); painter.setBrush(Qt::NoBrush); painter.drawEllipse(handle, 6, 6);
    painter.setPen(QPen(Qt::white, 1.5)); painter.drawEllipse(handle, 6, 6);
}
void ColorWheelWidget::moveHandle(const QPointF& position) {
    const QPointF delta = position - QPointF(width() / 2.0, height() / 2.0);
    const double radius = std::max(1.0, std::min(width(), height()) / 2.0 - 10);
    hue_ = std::fmod(std::atan2(-delta.y(), delta.x()) * 180 / pi + 360, 360);
    strength_ = std::min(1.0, std::hypot(delta.x(), delta.y()) / radius);
    emit colorChanged(hue_, strength_); update();
}
void ColorWheelWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) { dragging_ = true; moveHandle(event->position()); event->accept(); }
}
void ColorWheelWidget::mouseMoveEvent(QMouseEvent* event) { if (dragging_) moveHandle(event->position()); }
void ColorWheelWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ && event->button() == Qt::LeftButton) { moveHandle(event->position()); dragging_ = false; }
}

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
    if (dragging_ && event->button() == Qt::LeftButton) { movePoint(event->position()); dragging_ = false; }
}
void GradeCurveWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !graphRect().contains(event->position()) || points_.size() >= 64) return;
    const auto point = toGraph(event->position());
    auto at = std::lower_bound(points_.begin(), points_.end(), point.x(), [](const QPointF& p, double x) { return p.x() < x; });
    if (at == points_.begin() || at == points_.end() || std::abs(at->x() - point.x()) < .0001 || std::abs((at - 1)->x() - point.x()) < .0001) return;
    selected_ = static_cast<int>(at - points_.begin()); points_.insert(selected_, point); dragging_ = false;
    emit pointsChanged(); update();
}
void GradeCurveWidget::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && selected_ > 0 && selected_ < points_.size() - 1) {
        points_.removeAt(selected_); selected_ = -1; emit pointsChanged(); update();
    } else QWidget::keyPressEvent(event);
}

bool ColorGradeDialog::supportsGradeSchema(const JsonValue& grade) {
    return grade.is_object() && grade.contains("schemaVersion") && grade["schemaVersion"].is_number_integer()
        && grade["schemaVersion"] == heisenberg::grade::kColorGradeSchemaVersion;
}

void ColorGradeDialog::accept() {
    if (index_ < effects_.size() && !supportsGradeSchema(effects_[index_])) { reject(); return; }
    QDialog::accept();
}

ColorGradeDialog::ColorGradeDialog(const JsonValue& effects, QWidget* parent)
    : QDialog(parent), original_(effects), effects_(effects), index_(effects.size()) {
    setObjectName("clipColorGradeDialog"); setWindowTitle(tr("Color Grade")); resize(720, 650);
    for (size_t i = 0; i < effects_.size(); ++i)
        if (effects_[i].is_object() && effects_[i].contains("type") && effects_[i]["type"] == "video.colorGrade") { index_ = i; break; }
    auto* layout = new QVBoxLayout(this);
    if (index_ < effects_.size() && !supportsGradeSchema(effects_[index_])) {
        auto* warning = new QLabel(tr("This color grade has an unsupported or invalid schemaVersion. Only integer schemaVersion 1 can be edited. The original effect is preserved unchanged."), this);
        warning->setObjectName("colorGradeUnsupportedSchema"); warning->setWordWrap(true); layout->addWidget(warning);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this); layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        return;
    }
    auto* enabled = new QCheckBox(tr("Enable color grade"), this); enabled->setObjectName("colorGradeEnabled");
    enabled->setChecked(index_ == effects_.size() || effects_[index_].value("enabled", true)); layout->addWidget(enabled);
    connect(enabled, &QCheckBox::toggled, this, [this](bool checked) { grade()["enabled"] = checked; emit effectsChanged(); });
    auto* tabs = new QTabWidget(this); tabs->setObjectName("colorGradeTabs"); layout->addWidget(tabs, 1);
    auto* light = new QWidget(tabs); auto* lightForm = new QFormLayout(light);
    const char* lightNames[] = {"exposure", "contrast", "highlights", "shadows", "blacks", "whites"};
    const QString lightLabels[] = {tr("Exposure (EV)"), tr("Contrast"), tr("Highlights"), tr("Shadows"), tr("Blacks"), tr("Whites")};
    const double lightMin[] = {-3, .5, -1, -1, -1, -1};
    const double lightMax[] = {3, 1.5, 1, 1, 1, 1};
    for (int i = 0; i < 6; ++i) lightForm->addRow(lightLabels[i], parameter(light, lightNames[i], lightMin[i], lightMax[i], i == 1 ? 1 : 0));
    auto* evHint = new QLabel(tr("Exposure uses linear EV: +1 EV doubles scene-linear light."), light); evHint->setWordWrap(true); lightForm->addRow(evHint);
    tabs->addTab(light, tr("Light"));
    auto* color = new QWidget(tabs); auto* colorForm = new QFormLayout(color);
    const char* colorNames[] = {"temperature", "tint", "vibrance", "saturation"};
    const QString colorLabels[] = {tr("Temperature"), tr("Tint"), tr("Vibrance"), tr("Saturation")};
    const double colorMin[] = {2000, -100, -1, 0};
    const double colorMax[] = {11000, 100, 1, 2};
    const double colorDefault[] = {6500, 0, 0, 1};
    for (int i = 0; i < 4; ++i) colorForm->addRow(colorLabels[i], parameter(color, colorNames[i], colorMin[i], colorMax[i], colorDefault[i]));
    colorForm->addRow(new QLabel(tr("Temperature is in kelvin. Positive tint shifts toward green."), color));
    tabs->addTab(color, tr("Color"));
    auto* wheels = new QWidget(tabs); auto* wheelLayout = new QHBoxLayout(wheels);
    const char* wheelNames[] = {"lift", "gamma", "gain"};
    for (const auto* wheelName : wheelNames) {
        const std::string prefix(wheelName); auto* group = new QGroupBox(QString::fromLatin1(wheelName), wheels); auto* form = new QFormLayout(group);
        auto* wheel = new ColorWheelWidget(group); wheel->setObjectName(QString("colorGrade_%1Wheel").arg(wheelName)); form->addRow(wheel);
        auto* hue = parameter(group, (prefix + "Hue").c_str(), 0, 360, 0);
        auto* strength = parameter(group, (prefix + "Strength").c_str(), 0, 1, 0);
        const bool lift = prefix == "lift";
        auto* master = parameter(group, (prefix + "Master").c_str(), lift ? -.5 : .5, lift ? .5 : (prefix == "gamma" ? 2 : 1.5), lift ? 0 : 1);
        form->addRow(tr("Hue (degrees)"), hue); form->addRow(tr("Strength"), strength); form->addRow(tr("Master"), master);
        auto sync = [wheel, hue, strength]() { wheel->setColor(hue->value(), strength->value()); };
        connect(hue, &QDoubleSpinBox::valueChanged, wheel, sync); connect(strength, &QDoubleSpinBox::valueChanged, wheel, sync);
        connect(wheel, &ColorWheelWidget::colorChanged, this, [hue, strength](double h, double s) { hue->setValue(h); strength->setValue(s); }); sync();
        wheelLayout->addWidget(group);
    }
    tabs->addTab(wheels, tr("Wheels"));
    auto* curves = new QWidget(tabs); auto* curvesLayout = new QVBoxLayout(curves);
    channel_ = new QComboBox(curves); channel_->setObjectName("colorGradeCurveChannel"); channel_->addItems({tr("Master"), tr("Red"), tr("Green"), tr("Blue")}); curvesLayout->addWidget(channel_);
    curve_ = new GradeCurveWidget(curves); curve_->setObjectName("colorGradeCurveGraph"); curvesLayout->addWidget(curve_, 1);
    curveTable_ = new QTableWidget(curves); curveTable_->setObjectName("colorGradeCurvePoints"); curveTable_->setColumnCount(2); curveTable_->setHorizontalHeaderLabels({tr("Input"), tr("Output")}); curveTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); curveTable_->setMaximumHeight(165); curvesLayout->addWidget(curveTable_);
    auto* curveButtons = new QHBoxLayout;
    auto* addPoint = new QPushButton(tr("Add point"), curves); addPoint->setObjectName("colorGradeCurveAdd");
    auto* removePoint = new QPushButton(tr("Remove point"), curves); removePoint->setObjectName("colorGradeCurveRemove");
    auto* resetCurve = new QPushButton(tr("Reset channel"), curves); resetCurve->setObjectName("colorGradeCurveReset");
    curveButtons->addWidget(addPoint); curveButtons->addWidget(removePoint); curveButtons->addWidget(resetCurve); curvesLayout->addLayout(curveButtons);
    connect(channel_, &QComboBox::currentIndexChanged, this, [this] { loadCurve(); });
    connect(curve_, &GradeCurveWidget::pointsChanged, this, [this] { saveCurve(); updateCurveTable(); });
    connect(addPoint, &QPushButton::clicked, this, [this] {
        auto points = curve_->points(); if (points.size() >= 64) return; int gap = 0;
        for (int i = 1; i < points.size() - 1; ++i) if (points[i + 1].x() - points[i].x() > points[gap + 1].x() - points[gap].x()) gap = i;
        if (points[gap + 1].x() - points[gap].x() < .0002) return;
        points.insert(gap + 1, (points[gap] + points[gap + 1]) / 2); curve_->setPoints(points); saveCurve(); updateCurveTable();
    });
    connect(removePoint, &QPushButton::clicked, this, [this] {
        const int row = curveTable_->currentRow(); auto points = curve_->points();
        if (row <= 0 || row >= points.size() - 1) return;
        points.removeAt(row); curve_->setPoints(points); saveCurve(); updateCurveTable();
    });
    connect(resetCurve, &QPushButton::clicked, this, [this] { curve_->setPoints({{0, 0}, {1, 1}}); saveCurve(); updateCurveTable(); });
    loadCurve(); tabs->addTab(curves, tr("Curves"));
    auto* hues = new QWidget(tabs); auto* huesLayout = new QVBoxLayout(hues);
    auto* hueHint = new QLabel(tr("Target hue and rotation use degrees. Saturation is a multiplier; lightness is an offset."), hues); hueHint->setWordWrap(true); huesLayout->addWidget(hueHint);
    hueTable_ = new QTableWidget(hues); hueTable_->setObjectName("colorGradeHueTargets"); hueTable_->setColumnCount(4); hueTable_->setHorizontalHeaderLabels({tr("Hue (degrees)"), tr("Rotation (degrees)"), tr("Saturation"), tr("Lightness")}); hueTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); huesLayout->addWidget(hueTable_, 1);
    auto* hueButtons = new QHBoxLayout; auto* addHue = new QPushButton(tr("Add target"), hues); addHue->setObjectName("colorGradeHueAdd"); auto* removeHue = new QPushButton(tr("Remove target"), hues); removeHue->setObjectName("colorGradeHueRemove"); hueButtons->addWidget(addHue); hueButtons->addWidget(removeHue); huesLayout->addLayout(hueButtons);
    connect(addHue, &QPushButton::clicked, this, [this] {
        auto& targets = grade()["params"]["hueTargets"]; if (!targets.is_array()) targets = JsonValue::array();
        if (targets.size() >= 64) return;
        targets.push_back({{"hue", std::fmod(targets.size() * 60.0, 360.0)}, {"rotation", 0.0}, {"saturation", 1.0}, {"lightness", 0.0}}); updateHueTable(); emit effectsChanged();
    });
    connect(removeHue, &QPushButton::clicked, this, [this] {
        const int row = hueTable_->currentRow(); if (row < 0) return;
        auto& targets = grade()["params"]["hueTargets"]; if (targets.is_array() && row < targets.size()) { targets.erase(targets.begin() + row); updateHueTable(); emit effectsChanged(); }
    });
    updateHueTable(); tabs->addTab(hues, tr("Hue Targets"));
    auto* lut = new QWidget(tabs); auto* lutForm = new QFormLayout(lut);
    JsonValue savedLut = JsonValue::object();
    if (index_ < effects_.size() && effects_[index_].contains("params") && effects_[index_]["params"].is_object()) savedLut = effects_[index_]["params"].value("lut", JsonValue::object());
    if (!savedLut.is_object()) savedLut = JsonValue::object();
    lutName_ = new QLabel(QString::fromStdString(savedLut.value("name", std::string("No LUT selected"))), lut); lutName_->setObjectName("colorGradeLutName"); lutName_->setWordWrap(true); lutForm->addRow(tr("LUT"), lutName_);
    auto* import = new QPushButton(tr("Import .cube..."), lut); import->setObjectName("colorGradeLutImport"); lutForm->addRow(import); connect(import, &QPushButton::clicked, this, &ColorGradeDialog::importLut);
    auto* domain = new QComboBox(lut); domain->setObjectName("colorGradeLutDomain"); domain->addItem(tr("Rec.709"), "rec709"); domain->addItem(tr("ACEScct"), "acescct"); domain->setCurrentIndex(std::max(0, domain->findData(QString::fromStdString(savedLut.value("domain", std::string("rec709")))))); lutForm->addRow(tr("Domain"), domain);
    lutDomain_ = domain;
    connect(domain, &QComboBox::currentIndexChanged, this, [this, domain] { grade()["params"]["lut"]["domain"] = domain->currentData().toString().toStdString(); emit effectsChanged(); });
    double intensity = 1;
    if (savedLut.contains("intensity") && savedLut["intensity"].is_object()) intensity = savedLut["intensity"].value("value", 1.0);
    auto* lutIntensity = number(lut, "colorGradeLutIntensity", 0, 1, intensity); lutForm->addRow(tr("Intensity"), lutIntensity);
    lutIntensity_ = lutIntensity;
    const bool hasLut = savedLut.contains("id") && savedLut["id"].is_string() && !savedLut["id"].get<std::string>().empty();
    lutDomain_->setEnabled(hasLut); lutIntensity_->setEnabled(hasLut);
    connect(lutIntensity, &QDoubleSpinBox::valueChanged, this, [this](double value) { auto& field = grade()["params"]["lut"]["intensity"]; if (!field.is_object()) field = JsonValue::object(); field["value"] = value; field.erase("track"); emit effectsChanged(); });
    auto* clearLut = new QPushButton(tr("Remove LUT"), lut); clearLut->setObjectName("colorGradeLutRemove"); lutForm->addRow(clearLut);
    connect(clearLut, &QPushButton::clicked, this, [this] { if (index_ >= effects_.size()) return; grade()["params"].erase("lut"); lutName_->setText(tr("No LUT selected")); lutDomain_->setEnabled(false); lutIntensity_->setEnabled(false); emit effectsChanged(); });
    lutError_ = new QLabel(lut); lutError_->setObjectName("colorGradeLutError"); lutError_->setWordWrap(true); lutForm->addRow(lutError_); tabs->addTab(lut, tr("LUT"));
    auto* hint = new QLabel(tr("Editing a numeric parameter sets a constant and replaces only that parameter’s animation. Untouched tracks and imported fields are preserved. Preview updates live; Cancel restores the original grade. OK creates one undo step."), this); hint->setObjectName("colorGradeConstantHint"); hint->setWordWrap(true); layout->addWidget(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

JsonValue& ColorGradeDialog::grade() {
    if (index_ == effects_.size()) effects_.push_back({{"id", "color-grade"}, {"type", "video.colorGrade"}, {"schemaVersion", 1}, {"enabled", true}, {"params", JsonValue::object()}});
    auto& effect = effects_[index_];
    if (!effect.contains("params") || !effect["params"].is_object()) effect["params"] = JsonValue::object();
    return effect;
}
double ColorGradeDialog::parameterValue(const char* name, double fallback) const {
    if (index_ >= effects_.size() || !effects_[index_].contains("params") || !effects_[index_]["params"].is_object()) return fallback;
    const auto& params = effects_[index_]["params"];
    if (!params.contains(name)) return fallback;
    const auto& parameter = params[name];
    if (parameter.is_number()) return parameter.get<double>();
    if (parameter.is_object() && parameter.contains("value") && parameter["value"].is_number()) return parameter["value"].get<double>();
    return fallback;
}
QDoubleSpinBox* ColorGradeDialog::parameter(QWidget* parent, const char* name, double minimum, double maximum, double fallback) {
    for (const auto& spec : heisenberg::grade::colorGradeParameterRegistry()) {
        if (std::string_view(spec.name) == name) { minimum = spec.minimum; maximum = spec.maximum; fallback = spec.defaultValue; break; }
    }
    const double initial = parameterValue(name, fallback);
    auto* field = number(parent, QString("colorGrade_%1").arg(name), minimum, maximum, initial);
    if (std::string_view(name) == "temperature") field->setSingleStep(100);
    else if (std::string_view(name) == "tint" || std::string_view(name).ends_with("Hue")) field->setSingleStep(1);
    const double displayedInitial = field->value();
    const std::string key(name);
    connect(field, &QDoubleSpinBox::valueChanged, this, [this, key, displayedInitial](double value) { setParameter(key.c_str(), value, displayedInitial); });
    return field;
}
void ColorGradeDialog::setParameter(const char* name, double value, double initial) {
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
void ColorGradeDialog::loadCurve() {
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
void ColorGradeDialog::saveCurve() {
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
void ColorGradeDialog::updateCurveTable() {
    const auto points = curve_->points(); const int selected = curveTable_->currentRow();
    for (int row = 0; row < curveTable_->rowCount(); ++row) for (int column = 0; column < 2; ++column)
        if (auto* old = curveTable_->cellWidget(row, column)) old->setObjectName({});
    curveTable_->setRowCount(static_cast<int>(points.size()));
    for (int row = 0; row < points.size(); ++row) {
        auto* input = number(curveTable_, QString("curveInput_%1").arg(row), row == 0 ? 0 : points[row - 1].x() + .0001, row == points.size() - 1 ? 1 : points[row + 1].x() - .0001, points[row].x());
        auto* output = number(curveTable_, QString("curveOutput_%1").arg(row), 0, 1, points[row].y());
        auto setCurveCoordinate = [this, row](int coordinate, double value) {
            auto points = curve_->points(); points[row].setX(coordinate == 0 ? value : points[row].x()); points[row].setY(coordinate == 1 ? value : points[row].y());
            curve_->setPoints(points); saveCurve();
            QTimer::singleShot(0, this, [this] { updateCurveTable(); });
        };
        curveTable_->setCellWidget(row, 0, input); curveTable_->setCellWidget(row, 1, output);
        connect(input, &QDoubleSpinBox::valueChanged, this, [setCurveCoordinate](double value) { setCurveCoordinate(0, value); });
        connect(output, &QDoubleSpinBox::valueChanged, this, [setCurveCoordinate](double value) { setCurveCoordinate(1, value); });
    }
    if (selected >= 0 && selected < points.size()) curveTable_->selectRow(selected);
}
void ColorGradeDialog::updateHueTable() {
    JsonValue targets = JsonValue::array();
    if (index_ < effects_.size() && effects_[index_].contains("params") && effects_[index_]["params"].is_object()) targets = effects_[index_]["params"].value("hueTargets", JsonValue::array());
    if (!targets.is_array()) targets = JsonValue::array();
    for (int row = 0; row < hueTable_->rowCount(); ++row) for (int column = 0; column < 4; ++column)
        if (auto* old = hueTable_->cellWidget(row, column)) old->setObjectName({});
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
    }
}
void ColorGradeDialog::changeHueCell(int row, const char* key, double value) {
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
void ColorGradeDialog::importLut() {
    const auto path = QFileDialog::getOpenFileName(this, tr("Import 3D LUT"), {}, tr("Cube LUT (*.cube);;All files (*)")); if (path.isEmpty()) return;
    QString id, name, error;
    if (!lutImporter_ || !lutImporter_(path, id, name, error)) { lutError_->setText(error.isEmpty() ? tr("LUT library is unavailable.") : error); return; }
    auto& lut = grade()["params"]["lut"]; if (!lut.is_object()) lut = JsonValue::object(); lut["id"] = id.toStdString(); lut["name"] = name.toStdString();
    if (!lut.contains("domain")) lut["domain"] = lutDomain_->currentData().toString().toStdString();
    if (!lut.contains("intensity")) lut["intensity"] = {{"value", lutIntensity_->value()}};
    lutDomain_->setEnabled(true); lutIntensity_->setEnabled(true);
    lutName_->setText(name); lutError_->clear(); emit effectsChanged();
}
