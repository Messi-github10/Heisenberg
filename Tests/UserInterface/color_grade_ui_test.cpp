#include <Backend/EditorSession.hpp>
#include <MainWidget/ColorGradeDialog.hpp>
#include <MainWidget/TimelineWidget.hpp>
#include <Compositing/Grade/ColorGrade.hpp>
#include <Compositing/Grade/LutLibrary.hpp>
#include <gtest/gtest.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QFile>
#include <QMouseEvent>
#include <QPushButton>
#include <QPixmap>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <filesystem>

using heisenberg::JsonValue;
using heisenberg::ui::EditorSession;
namespace {
JsonValue importedEffects() {
    return JsonValue::parse(R"([{"type":"video.custom","payload":{"keep":true}},{"id":"grade","type":"video.colorGrade","schemaVersion":1,"enabled":true,"unknown":3,"params":{"exposure":{"value":0,"label":"preserved","track":{"keyframes":[{"frame":0,"value":0},{"frame":10,"value":1}]}},"contrast":{"value":1,"track":{"keyframes":[{"frame":0,"value":1}]}},"curves":{"master":[[0,0],[1,1]],"red":[[0,0],[0.5,0.6],[1,1]],"future":"preserve"},"hueTargets":[{"hue":30,"rotation":{"value":0,"track":{"keyframes":[{"frame":0,"value":0}]}},"saturation":{"value":1,"track":{"keyframes":[{"frame":0,"value":1}]}},"lightness":0,"unknown":8}],"lut":{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","name":"Imported","domain":"acescct","intensity":{"value":0.5,"track":{"keyframes":[{"frame":0,"value":0.5}]}},"extra":4}}}])");
}
void selectFirstGradeClip(TimelineWidget& widget) {
    const QPointF point(200, 48);
    QMouseEvent press(QEvent::MouseButtonPress, point, widget.mapToGlobal(point.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, point, widget.mapToGlobal(point.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &release);
}
}

TEST(ColorGradeUiTest, SixTabsUseFrameworkRangesAndOpeningPreservesEveryField) {
    const auto effects = importedEffects();
    ColorGradeDialog dialog(effects);
    EXPECT_EQ(dialog.effects(), effects);
    auto* tabs = dialog.findChild<QTabWidget*>("colorGradeTabs"); ASSERT_NE(tabs, nullptr); EXPECT_EQ(tabs->count(), 6);
    for (const auto& spec : heisenberg::grade::colorGradeParameterRegistry()) {
        auto* field = dialog.findChild<QDoubleSpinBox*>(QString("colorGrade_%1").arg(spec.name));
        ASSERT_NE(field, nullptr) << spec.name;
        EXPECT_DOUBLE_EQ(field->minimum(), spec.minimum); EXPECT_DOUBLE_EQ(field->maximum(), spec.maximum);
    }
    EXPECT_EQ(dialog.findChildren<ColorWheelWidget*>().size(), 3);
    const auto screenshot = qEnvironmentVariable("HEISENBERG_UI_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        dialog.show(); QCoreApplication::processEvents();
        EXPECT_TRUE(dialog.grab().save(screenshot));
        tabs->setCurrentIndex(2); QCoreApplication::processEvents(); EXPECT_TRUE(dialog.grab().save(screenshot + ".wheels.png"));
        tabs->setCurrentIndex(3); QCoreApplication::processEvents(); EXPECT_TRUE(dialog.grab().save(screenshot + ".curves.png"));
    }
    dialog.accept(); EXPECT_EQ(dialog.effects(), effects);
}

TEST(ColorGradeUiTest, UnsupportedSchemaCannotOpenTransactionOrEditAndPreservesPayload) {
    EditorSession session;
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    ASSERT_TRUE(session.placeMedia(QString::fromStdString(session.editor().manifest().entries().front().id), 0).changed());
    const auto id = QString::fromStdString(session.editor().activeTrack()->clips.front().id);
    TimelineWidget widget; widget.setSession(&session); widget.resize(1000, 210); widget.show();
    QCoreApplication::processEvents(); selectFirstGradeClip(widget); ASSERT_EQ(widget.selectedClipId(), id);
    for (const auto& version : JsonValue::array({2, nullptr, "1", 1.0, true, 0, -1, JsonValue::object()})) {
        SCOPED_TRACE(version.dump());
        auto effects = importedEffects();
        effects[1]["enabled"] = false; // Bypassing render compilation must not permit editing an unknown schema.
        effects[1]["schemaVersion"] = version;
        if (version.is_null()) effects[1].erase("schemaVersion");
        ASSERT_TRUE(session.setClipEffects(id, effects).changed());
        const auto revision = session.editor().revision(); const bool dirty = session.isDirty();
        int failures = 0; bool opened = false;
        const auto connection = QObject::connect(&widget, &TimelineWidget::editFailed, &widget, [&](const QString& message) { ++failures; EXPECT_TRUE(message.contains("schemaVersion")); });
        QTimer fallback; fallback.setSingleShot(true);
        QObject::connect(&fallback, &QTimer::timeout, &widget, [&] { if (auto* dialog = widget.findChild<ColorGradeDialog*>("clipColorGradeDialog")) { opened = true; dialog->reject(); } });
        fallback.start(0); widget.editSelectedClipColorGrade(); fallback.stop(); QObject::disconnect(connection);
        EXPECT_EQ(failures, 1); EXPECT_FALSE(opened);
        EXPECT_EQ(session.editor().revision(), revision); EXPECT_EQ(session.isDirty(), dirty);
        EXPECT_EQ(*session.editor().activeTimeline()->clipById(id.toStdString())->effects, effects);
        // No transaction was left pending by the rejected entry point.
        EXPECT_TRUE(session.beginClipEffectsTransaction(id).changed()); session.cancelClipEffectsTransaction();
        ColorGradeDialog direct(effects);
        auto* warning = direct.findChild<QLabel*>("colorGradeUnsupportedSchema"); ASSERT_NE(warning, nullptr);
        EXPECT_TRUE(warning->text().contains("schemaVersion"));
        EXPECT_EQ(direct.findChild<QTabWidget*>("colorGradeTabs"), nullptr);
        EXPECT_TRUE(direct.findChildren<QDoubleSpinBox*>().isEmpty());
        auto* buttons = direct.findChild<QDialogButtonBox*>(); ASSERT_NE(buttons, nullptr);
        EXPECT_EQ(buttons->standardButtons(), QDialogButtonBox::StandardButtons(QDialogButtonBox::Cancel));
        direct.accept(); EXPECT_EQ(direct.result(), QDialog::Rejected); EXPECT_EQ(direct.effects(), effects);
    }
}

TEST(ColorGradeUiTest, NumericEditRemovesOnlyItsTrackAndReturningToOriginalRestoresIt) {
    const auto effects = importedEffects(); ColorGradeDialog dialog(effects);
    auto* exposure = dialog.findChild<QDoubleSpinBox*>("colorGrade_exposure"); ASSERT_NE(exposure, nullptr);
    exposure->setValue(1.5);
    EXPECT_DOUBLE_EQ(dialog.effects()[1]["params"]["exposure"]["value"].get<double>(), 1.5);
    EXPECT_FALSE(dialog.effects()[1]["params"]["exposure"].contains("track"));
    EXPECT_EQ(dialog.effects()[1]["params"]["exposure"]["label"], "preserved");
    EXPECT_EQ(dialog.effects()[1]["params"]["contrast"], effects[1]["params"]["contrast"]);
    EXPECT_EQ(dialog.effects()[0], effects[0]);
    exposure->setValue(0); EXPECT_EQ(dialog.effects(), effects);
    auto* enabled = dialog.findChild<QCheckBox*>("colorGradeEnabled"); ASSERT_NE(enabled, nullptr); enabled->setChecked(false);
    auto expected = effects; expected[1]["enabled"] = false; EXPECT_EQ(dialog.effects(), expected);
}

TEST(ColorGradeUiTest, ReturningToRoundedDisplayRestoresExactImportedParameterAndFallback) {
    auto effects = importedEffects(); effects[1]["params"]["exposure"]["value"] = .123456;
    ColorGradeDialog dialog(effects);
    auto* exposure = dialog.findChild<QDoubleSpinBox*>("colorGrade_exposure"); ASSERT_NE(exposure, nullptr);
    const double displayedInitial = exposure->value(); EXPECT_DOUBLE_EQ(displayedInitial, .1235);
    exposure->setValue(.5); EXPECT_FALSE(dialog.effects()[1]["params"]["exposure"].contains("track"));
    exposure->setValue(displayedInitial); EXPECT_EQ(dialog.effects(), effects);
    EXPECT_DOUBLE_EQ(dialog.effects()[1]["params"]["exposure"]["value"].get<double>(), .123456);
    auto* highlights = dialog.findChild<QDoubleSpinBox*>("colorGrade_highlights"); ASSERT_NE(highlights, nullptr);
    const double fallback = highlights->value(); highlights->setValue(.25); highlights->setValue(fallback);
    EXPECT_EQ(dialog.effects(), effects);
}

TEST(ColorGradeUiTest, WheelGraphAndTableStaySynchronizedAndCurveChannelsRemainIndependent) {
    const auto effects = importedEffects(); ColorGradeDialog dialog(effects);
    auto* wheel = dialog.findChild<ColorWheelWidget*>("colorGrade_liftWheel"); ASSERT_NE(wheel, nullptr);
    wheel->colorChanged(120, .5);
    EXPECT_DOUBLE_EQ(dialog.findChild<QDoubleSpinBox*>("colorGrade_liftHue")->value(), 120);
    EXPECT_DOUBLE_EQ(dialog.findChild<QDoubleSpinBox*>("colorGrade_liftStrength")->value(), .5);
    EXPECT_DOUBLE_EQ(dialog.effects()[1]["params"]["liftStrength"]["value"].get<double>(), .5);
    auto* tabs = dialog.findChild<QTabWidget*>("colorGradeTabs"); tabs->setCurrentIndex(3);
    auto* graph = dialog.findChild<GradeCurveWidget*>("colorGradeCurveGraph"); ASSERT_NE(graph, nullptr);
    dialog.findChild<QPushButton*>("colorGradeCurveAdd")->click();
    ASSERT_EQ(graph->points().size(), 3); EXPECT_EQ(graph->points()[1], QPointF(.5, .5));
    auto* output = dialog.findChild<QDoubleSpinBox*>("curveOutput_1"); ASSERT_NE(output, nullptr); output->setValue(.75);
    EXPECT_EQ(graph->points()[1], QPointF(.5, .75));
    EXPECT_EQ(dialog.effects()[1]["params"]["curves"]["master"][1], JsonValue::array({.5, .75}));
    EXPECT_EQ(dialog.effects()[1]["params"]["curves"]["red"], effects[1]["params"]["curves"]["red"]);
    EXPECT_EQ(dialog.effects()[1]["params"]["curves"]["future"], "preserve");
    auto* channel = dialog.findChild<QComboBox*>("colorGradeCurveChannel"); channel->setCurrentIndex(1); EXPECT_EQ(graph->points()[1], QPointF(.5, .6));
    channel->setCurrentIndex(0); EXPECT_EQ(graph->points()[1], QPointF(.5, .75));
}

TEST(ColorGradeUiTest, GraphMouseDragWritesExactLinearPointAndUpdatesTable) {
    ColorGradeDialog dialog(JsonValue::array());
    dialog.findChild<QTabWidget*>("colorGradeTabs")->setCurrentIndex(3);
    dialog.show(); QCoreApplication::processEvents();
    dialog.findChild<QPushButton*>("colorGradeCurveAdd")->click();
    auto* graph = dialog.findChild<GradeCurveWidget*>("colorGradeCurveGraph"); ASSERT_NE(graph, nullptr);
    const QRectF area = QRectF(graph->rect()).adjusted(15, 15, -15, -15);
    const QPointF from(area.center()); const QPointF to(area.left() + area.width() * .6, area.bottom() - area.height() * .8);
    QMouseEvent press(QEvent::MouseButtonPress, from, graph->mapToGlobal(from.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(graph, &press);
    QMouseEvent move(QEvent::MouseMove, to, graph->mapToGlobal(to.toPoint()), Qt::NoButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(graph, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, graph->mapToGlobal(to.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier); QApplication::sendEvent(graph, &release);
    EXPECT_NEAR(graph->points()[1].x(), .6, .0001); EXPECT_NEAR(graph->points()[1].y(), .8, .0001);
    const auto& points = dialog.effects()[0]["params"]["curves"]["master"]; ASSERT_EQ(points.size(), 3u);
    EXPECT_NEAR(points[1][0].get<double>(), .6, .0001); EXPECT_NEAR(points[1][1].get<double>(), .8, .0001);
}

TEST(ColorGradeUiTest, ResetAndDragToIdentityOmitOnlyEditedChannelAfterAcceptAndReopen) {
    for (const bool reset : {true, false}) {
        SCOPED_TRACE(reset ? "reset" : "drag with extra diagonal knots");
        EditorSession session;
        ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
        ASSERT_TRUE(session.placeMedia(QString::fromStdString(session.editor().manifest().entries().front().id), 0).changed());
        const auto id = QString::fromStdString(session.editor().activeTrack()->clips.front().id);
        auto effects = importedEffects();
        effects[1]["params"]["curves"]["red"] = JsonValue::array({
            JsonValue::array({0, 0}), JsonValue::array({.25, .25}), JsonValue::array({.5, .6}),
            JsonValue::array({.75, .75}), JsonValue::array({1, 1})});
        effects.push_back(JsonValue::parse(R"({"type":"video.basicGrade","params":{"exposure":{"value":0,"track":{"keyframes":[{"frame":0,"value":1}]}}}})"));
        ASSERT_TRUE(session.setClipEffects(id, effects).changed());
        auto expected = effects; expected[1]["params"]["curves"].erase("red");
        TimelineWidget widget; widget.setSession(&session); widget.resize(1000, 210); widget.show();
        QCoreApplication::processEvents(); selectFirstGradeClip(widget); ASSERT_EQ(widget.selectedClipId(), id);
        QTimer::singleShot(0, [&] {
            auto* dialog = widget.findChild<ColorGradeDialog*>("clipColorGradeDialog"); ASSERT_NE(dialog, nullptr);
            dialog->findChild<QTabWidget*>("colorGradeTabs")->setCurrentIndex(3);
            dialog->findChild<QComboBox*>("colorGradeCurveChannel")->setCurrentIndex(1);
            QCoreApplication::processEvents();
            if (reset) dialog->findChild<QPushButton*>("colorGradeCurveReset")->click();
            else {
                auto* graph = dialog->findChild<GradeCurveWidget*>("colorGradeCurveGraph"); ASSERT_NE(graph, nullptr);
                const QRectF area = QRectF(graph->rect()).adjusted(15, 15, -15, -15);
                const QPointF from(area.center().x(), area.bottom() - area.height() * .6);
                const QPointF to = area.center();
                QMouseEvent press(QEvent::MouseButtonPress, from, graph->mapToGlobal(from.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(graph, &press);
                QMouseEvent move(QEvent::MouseMove, to, graph->mapToGlobal(to.toPoint()), Qt::NoButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(graph, &move);
                QMouseEvent release(QEvent::MouseButtonRelease, to, graph->mapToGlobal(to.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier); QApplication::sendEvent(graph, &release);
                EXPECT_EQ(graph->points().size(), 5); EXPECT_EQ(graph->points()[2], QPointF(.5, .5));
            }
            EXPECT_EQ(dialog->effects(), expected); dialog->accept();
        });
        widget.editSelectedClipColorGrade();
        EXPECT_EQ(*session.editor().activeTimeline()->clipById(id.toStdString())->effects, expected);
        QTimer::singleShot(0, [&] {
            auto* dialog = widget.findChild<ColorGradeDialog*>("clipColorGradeDialog"); ASSERT_NE(dialog, nullptr);
            EXPECT_EQ(dialog->effects(), expected);
            dialog->findChild<QComboBox*>("colorGradeCurveChannel")->setCurrentIndex(1);
            const auto* graph = dialog->findChild<GradeCurveWidget*>("colorGradeCurveGraph"); ASSERT_NE(graph, nullptr);
            ASSERT_EQ(graph->points().size(), 2); EXPECT_EQ(graph->points().front(), QPointF(0, 0)); EXPECT_EQ(graph->points().back(), QPointF(1, 1));
            dialog->accept();
        });
        widget.editSelectedClipColorGrade();
        EXPECT_EQ(*session.editor().activeTimeline()->clipById(id.toStdString())->effects, expected);
        session.undo(); EXPECT_EQ(*session.editor().activeTimeline()->clipById(id.toStdString())->effects, effects);
    }
}

TEST(ColorGradeUiTest, IdentityCleanupDropsEmptyCurvesButRetainsPartialDiagonalAndUnknownFields) {
    auto effects = importedEffects();
    effects[1]["params"]["curves"] = {{"master", JsonValue::array({JsonValue::array({0, 0}), JsonValue::array({.5, .8}), JsonValue::array({1, 1})})}};
    ColorGradeDialog dialog(effects);
    dialog.findChild<QPushButton*>("colorGradeCurveReset")->click();
    auto expected = effects; expected[1]["params"].erase("curves"); EXPECT_EQ(dialog.effects(), expected);
    dialog.accept(); ColorGradeDialog reopened(dialog.effects()); EXPECT_EQ(reopened.effects(), expected);
    auto* graph = reopened.findChild<GradeCurveWidget*>("colorGradeCurveGraph"); ASSERT_NE(graph, nullptr);
    graph->setPoints({{.25, .25}, {.75, .75}}); graph->pointsChanged();
    EXPECT_EQ(reopened.effects()[1]["params"]["curves"]["master"], JsonValue::array({JsonValue::array({.25, .25}), JsonValue::array({.75, .75})}));
    ColorGradeDialog fresh(JsonValue::array()); fresh.findChild<QPushButton*>("colorGradeCurveReset")->click(); EXPECT_TRUE(fresh.effects().empty());
}

TEST(ColorGradeUiTest, HueAndLutEditsPreserveSiblingTracksAndUnknownMetadata) {
    const auto effects = importedEffects(); ColorGradeDialog dialog(effects);
    auto* rotation = dialog.findChild<QDoubleSpinBox*>("hueTarget_0_rotation"); ASSERT_NE(rotation, nullptr); rotation->setValue(12);
    const auto& target = dialog.effects()[1]["params"]["hueTargets"][0];
    EXPECT_FALSE(target["rotation"].contains("track")); EXPECT_EQ(target["rotation"]["value"], 12);
    EXPECT_EQ(target["saturation"], effects[1]["params"]["hueTargets"][0]["saturation"]); EXPECT_EQ(target["unknown"], 8);
    auto* intensity = dialog.findChild<QDoubleSpinBox*>("colorGradeLutIntensity"); ASSERT_NE(intensity, nullptr); intensity->setValue(.75);
    const auto& lut = dialog.effects()[1]["params"]["lut"];
    EXPECT_FALSE(lut["intensity"].contains("track")); EXPECT_EQ(lut["intensity"]["value"], .75); EXPECT_EQ(lut["extra"], 4); EXPECT_EQ(lut["domain"], "acescct");
    dialog.findChild<QPushButton*>("colorGradeLutRemove")->click(); EXPECT_FALSE(dialog.effects()[1]["params"].contains("lut"));
}

TEST(ColorGradeUiTest, ModalPreviewCancelAndAcceptPreserveHistoryWithOneUndo) {
    EditorSession session;
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    ASSERT_TRUE(session.placeMedia(QString::fromStdString(session.editor().manifest().entries().front().id), 0).changed());
    const auto id = QString::fromStdString(session.editor().activeTrack()->clips.front().id);
    const auto effects = importedEffects(); ASSERT_TRUE(session.setClipEffects(id, effects).changed());
    TimelineWidget widget; widget.setSession(&session); widget.resize(1000, 210); widget.show(); QCoreApplication::processEvents(); selectFirstGradeClip(widget); ASSERT_EQ(widget.selectedClipId(), id);
    QTimer::singleShot(0, [&] {
        auto* dialog = widget.findChild<ColorGradeDialog*>("clipColorGradeDialog"); ASSERT_NE(dialog, nullptr);
        auto* field = dialog->findChild<QDoubleSpinBox*>("colorGrade_exposure"); field->setValue(1);
        EXPECT_EQ((*session.editor().activeTimeline()->clipById(id.toStdString())->effects)[1]["params"]["exposure"]["value"], 1);
        dialog->reject();
    });
    widget.editSelectedClipColorGrade(); EXPECT_EQ(*session.editor().activeTimeline()->clipById(id.toStdString())->effects, effects);
    QTimer::singleShot(0, [&] {
        auto* dialog = widget.findChild<ColorGradeDialog*>("clipColorGradeDialog"); ASSERT_NE(dialog, nullptr);
        auto* field = dialog->findChild<QDoubleSpinBox*>("colorGrade_exposure"); field->setValue(.5); field->setValue(1); field->setValue(1.5); dialog->accept();
    });
    widget.editSelectedClipColorGrade(); const auto edited = *session.editor().activeTimeline()->clipById(id.toStdString())->effects;
    EXPECT_EQ(edited[1]["params"]["exposure"]["value"], 1.5); session.undo(); EXPECT_EQ(*session.editor().activeTimeline()->clipById(id.toStdString())->effects, effects);
    QTimer::singleShot(0, [&] { auto* dialog = widget.findChild<ColorGradeDialog*>("clipColorGradeDialog"); ASSERT_NE(dialog, nullptr); dialog->accept(); });
    widget.editSelectedClipColorGrade(); session.redo(); EXPECT_EQ(*session.editor().activeTimeline()->clipById(id.toStdString())->effects, edited);
}

TEST(ColorGradeUiTest, SessionImportsLutIntoAppLibraryWithoutPersistingSourcePath) {
    EditorSession session; QTemporaryDir temporary; ASSERT_TRUE(temporary.isValid());
    const auto source = temporary.filePath("identity.cube"); QFile file(source); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("TITLE \"UI test identity\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n"); file.close();
    QString id, name, error; ASSERT_TRUE(session.importGradeLut(source, id, name, error)) << error.toStdString();
    EXPECT_EQ(id.size(), 64); EXPECT_EQ(name, "UI test identity");
    auto asset = heisenberg::grade::LutLibrary::resolveConfigured(id.toStdString()); ASSERT_NE(asset, nullptr);
    EXPECT_EQ(asset->metadata.size, 2u); EXPECT_NE(asset->resourcePath, std::filesystem::path(source.toStdString()));
    QFile invalid(temporary.filePath("invalid.cube")); ASSERT_TRUE(invalid.open(QIODevice::WriteOnly)); invalid.write("LUT_3D_SIZE 2\n0 0 0\n"); invalid.close();
    EXPECT_FALSE(session.importGradeLut(invalid.fileName(), id, name, error)); EXPECT_FALSE(error.isEmpty());
}
