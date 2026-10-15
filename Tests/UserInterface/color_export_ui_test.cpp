#include <Backend/EditorSession.hpp>
#include <Backend/QtExportController.hpp>
#include <MainWidget/MainWindow.hpp>
#include <MainWidget/TimelineWidget.hpp>
#include <Models/ProjectCodec.hpp>
#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QMouseEvent>
#include <QLabel>
#include <QTimer>

namespace {
void selectFirst(TimelineWidget& widget) {
    const QPointF point(200, 48);
    QMouseEvent press(QEvent::MouseButtonPress, point, widget.mapToGlobal(point.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, point, widget.mapToGlobal(point.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &release);
}
}
TEST(ColorUiTest, MenuSignalsAndInputDialogCreateOneUndoTransaction) {
    heisenberg::ui::EditorSession session;
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    MainWindow window(&session);
    auto* exportAction = window.findChild<QAction*>("exportTimeline"); ASSERT_NE(exportAction, nullptr);
    auto* colorAction = window.findChild<QAction*>("colorSettings"); ASSERT_NE(colorAction, nullptr);
    int exports = 0;
    QObject::connect(&window, &MainWindow::exportTimelineRequested, [&]() { ++exports; });
    exportAction->trigger(); EXPECT_EQ(exports, 1);
    const auto before = session.editor().project(); const auto revision = session.editor().revision();
    QTimer::singleShot(0, [&]() {
        auto* dialog = window.findChild<QDialog*>("colorSettingsDialog");
        ASSERT_NE(dialog, nullptr);
        const auto inputName = QString("inputSpace_%1").arg(QString::fromStdString(session.editor().manifest().entries().front().id));
        auto* combo = dialog->findChild<QComboBox*>(inputName); ASSERT_NE(combo, nullptr);
        EXPECT_GE(combo->findData("Camera Rec.709"), 0);
        combo->setCurrentIndex(combo->findData("ACEScg")); dialog->accept();
    });
    colorAction->trigger();
    EXPECT_EQ(session.editor().revision(), revision + 1);
    EXPECT_EQ(session.editor().project().colorSettings.inputColorSpaces.begin()->second, "ACEScg");
    session.undo(); EXPECT_EQ(session.editor().project(), before);
}

TEST(ColorUiTest, LegacyModeIsPreservedUntilExplicitUpgradeAndOneUndoRestoresIt) {
    heisenberg::ui::EditorSession session;
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    const auto ref = session.editor().manifest().entries().front().id;
    auto legacy = heisenberg::legacyColorSettings();
    legacy.inputColorSpaces[ref] = "Camera Rec.709";
    legacy.inputColorSpaces["unavailable-media"] = "preserved custom Log";
    ASSERT_TRUE(session.setColorSettings(legacy).changed());
    MainWindow window(&session);
    auto* action = window.findChild<QAction*>("colorSettings"); ASSERT_NE(action, nullptr);
    const auto before = session.editor().project();
    const auto revision = session.editor().revision();
    QTimer::singleShot(0, [&]() {
        auto* dialog = window.findChild<QDialog*>("colorSettingsDialog"); ASSERT_NE(dialog, nullptr);
        auto* mode = dialog->findChild<QComboBox*>("colorMode"); ASSERT_NE(mode, nullptr);
        EXPECT_EQ(mode->currentData().toString(), QString("legacy"));
        auto* summary = dialog->findChild<QLabel*>("colorModeSummary"); ASSERT_NE(summary, nullptr);
        EXPECT_TRUE(summary->text().contains("Legacy ACES"));
        dialog->accept();
    });
    action->trigger();
    EXPECT_EQ(session.editor().revision(), revision);
    EXPECT_EQ(session.editor().project(), before);
    QTimer::singleShot(0, [&]() {
        auto* dialog = window.findChild<QDialog*>("colorSettingsDialog"); ASSERT_NE(dialog, nullptr);
        auto* mode = dialog->findChild<QComboBox*>("colorMode"); ASSERT_NE(mode, nullptr);
        mode->setCurrentIndex(mode->findData("sdr"));
        auto* preview = dialog->findChild<QLabel*>("previewColorOutput"); ASSERT_NE(preview, nullptr);
        EXPECT_TRUE(preview->text().contains("Un-tone-mapped"));
        const auto inputName = QString("inputSpace_%1").arg(QString::fromStdString(ref));
        auto* input = dialog->findChild<QComboBox*>(inputName); ASSERT_NE(input, nullptr);
        EXPECT_EQ(input->currentData().toString(), QString("Camera Rec.709"));
        EXPECT_TRUE(input->itemText(0).contains("v2"));
        dialog->accept();
    });
    action->trigger();
    auto upgraded = legacy;
    upgraded.inputPolicyVersion = 2; upgraded.semanticsVersion = 2;
    upgraded.previewOutput = heisenberg::ColorSettings{}.previewOutput;
    upgraded.exportOutput = heisenberg::ColorSettings{}.exportOutput;
    EXPECT_EQ(session.editor().project().colorSettings, upgraded);
    EXPECT_EQ(session.editor().revision(), revision + 1);
    heisenberg::ProjectFile reopened; std::string error;
    ASSERT_TRUE(heisenberg::ProjectCodec::decode(heisenberg::ProjectCodec::encode(session.editor().project()), reopened, &error)) << error;
    EXPECT_EQ(reopened.colorSettings, upgraded);
    session.undo(); EXPECT_EQ(session.editor().project(), before);
    QTimer::singleShot(0, [&]() {
        auto* dialog = window.findChild<QDialog*>("colorSettingsDialog"); ASSERT_NE(dialog, nullptr); dialog->accept();
    });
    action->trigger(); // No-op must not discard the pending mode-upgrade redo.
    session.redo(); EXPECT_EQ(session.editor().project().colorSettings, upgraded);
}

TEST(ColorUiTest, ModeSwitchCancelIsSafeAndAcesRenderingKeepsV2Inputs) {
    heisenberg::ui::EditorSession session;
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    const auto ref = QString::fromStdString(session.editor().manifest().entries().front().id);
    ASSERT_TRUE(session.setMediaInputColorSpace(ref, "ACEScg").changed());
    MainWindow window(&session);
    auto* action = window.findChild<QAction*>("colorSettings"); ASSERT_NE(action, nullptr);
    const auto before = session.editor().project();
    const auto revision = session.editor().revision();
    QTimer::singleShot(0, [&]() {
        auto* dialog = window.findChild<QDialog*>("colorSettingsDialog"); ASSERT_NE(dialog, nullptr);
        auto* mode = dialog->findChild<QComboBox*>("colorMode"); ASSERT_NE(mode, nullptr);
        EXPECT_EQ(mode->currentData().toString(), QString("sdr"));
        mode->setCurrentIndex(mode->findData("aces"));
        auto* input = dialog->findChild<QComboBox*>(QString("inputSpace_%1").arg(ref)); ASSERT_NE(input, nullptr);
        input->setCurrentIndex(input->findData("Camera Rec.709"));
        dialog->reject();
    });
    action->trigger();
    EXPECT_EQ(session.editor().project(), before);
    EXPECT_EQ(session.editor().revision(), revision);
    QTimer::singleShot(0, [&]() {
        auto* dialog = window.findChild<QDialog*>("colorSettingsDialog"); ASSERT_NE(dialog, nullptr);
        auto* mode = dialog->findChild<QComboBox*>("colorMode"); ASSERT_NE(mode, nullptr);
        mode->setCurrentIndex(mode->findData("aces")); dialog->accept();
    });
    action->trigger();
    const auto aces = session.editor().project().colorSettings;
    EXPECT_EQ(aces.inputPolicyVersion, 2); EXPECT_EQ(aces.semanticsVersion, 2);
    EXPECT_EQ(aces.previewOutput.view, "ACES 2.0 - SDR 100 nits (Rec.709)");
    EXPECT_EQ(aces.exportOutput.view, "ACES 2.0 - SDR 100 nits (Rec.709)");
    EXPECT_EQ(aces.inputColorSpaces, before.colorSettings.inputColorSpaces);
    EXPECT_EQ(session.editor().revision(), revision + 1);
    QTimer::singleShot(0, [&]() {
        auto* dialog = window.findChild<QDialog*>("colorSettingsDialog"); ASSERT_NE(dialog, nullptr); dialog->accept();
    });
    action->trigger(); EXPECT_EQ(session.editor().revision(), revision + 1);
    QTimer::singleShot(0, [&]() {
        auto* dialog = window.findChild<QDialog*>("colorSettingsDialog"); ASSERT_NE(dialog, nullptr);
        auto* mode = dialog->findChild<QComboBox*>("colorMode"); ASSERT_NE(mode, nullptr);
        mode->setCurrentIndex(mode->findData("sdr")); dialog->accept();
    });
    action->trigger();
    EXPECT_EQ(session.editor().project(), before);
    EXPECT_EQ(session.editor().revision(), revision + 2);
    session.undo(); EXPECT_EQ(session.editor().project().colorSettings, aces);
    session.undo(); EXPECT_EQ(session.editor().project(), before);
}

TEST(ColorUiTest, GradeNoOpAndEditingPreserveOtherImportedParameterTracks) {
    heisenberg::ui::EditorSession session;
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    const auto ref = QString::fromStdString(session.editor().manifest().entries().front().id);
    ASSERT_TRUE(session.placeMedia(ref, 0).changed());
    const auto id = QString::fromStdString(session.editor().activeTrack()->clips.front().id);
    auto effects = heisenberg::JsonValue::parse(R"([{"id":"grade","type":"video.basicGrade","enabled":true,"unknown":3,"params":{"exposure":{"value":0,"track":{"keyframes":[{"frame":0,"value":0},{"frame":10,"value":1}]}},"contrast":{"value":1,"track":{"keyframes":[{"frame":0,"value":1}]}},"saturation":{"value":1}}}])");
    ASSERT_TRUE(session.setClipEffects(id, effects).changed());
    TimelineWidget widget; widget.setSession(&session); widget.resize(1000, 210); widget.show();
    QCoreApplication::processEvents(); selectFirst(widget);
    ASSERT_EQ(widget.selectedClipId(), id);
    const auto revision = session.editor().revision();
    QTimer::singleShot(0, [&]() {
        auto* dialog = widget.findChild<QDialog*>("clipGradeDialog"); ASSERT_NE(dialog, nullptr); dialog->accept();
    });
    widget.editSelectedClipGrade(); EXPECT_EQ(session.editor().revision(), revision);
    QTimer::singleShot(0, [&]() {
        auto* dialog = widget.findChild<QDialog*>("clipGradeDialog"); ASSERT_NE(dialog, nullptr);
        auto* exposure = dialog->findChild<QDoubleSpinBox*>("grade_exposure"); ASSERT_NE(exposure, nullptr);
        exposure->setValue(1.5); dialog->accept();
    });
    widget.editSelectedClipGrade(); EXPECT_EQ(session.editor().revision(), revision + 1);
    const auto& updated = *session.editor().activeTimeline()->clipById(id.toStdString())->effects;
    EXPECT_EQ(updated[0]["unknown"], 3);
    EXPECT_FALSE(updated[0]["params"]["exposure"].contains("track"));
    EXPECT_EQ(updated[0]["params"]["contrast"], effects[0]["params"]["contrast"]);
    session.undo(); EXPECT_EQ(*session.editor().activeTimeline()->clipById(id.toStdString())->effects, effects);
}

TEST(ExportUiTest, CodecDefaultsFollowSelectionAndKeepCustomCrf) {
    heisenberg::ui::EditorSession session;
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    ASSERT_TRUE(session.placeMedia(QString::fromStdString(session.editor().manifest().entries().front().id), 0).changed());
    QWidget parent;
    heisenberg::ui::QtExportController controller(&session, nullptr);
    QTimer::singleShot(0, [&]() {
        auto* dialog = parent.findChild<QDialog*>("exportTimelineDialog"); ASSERT_NE(dialog, nullptr);
        auto* codec = dialog->findChild<QComboBox*>("exportCodec"); ASSERT_NE(codec, nullptr);
        auto* crf = dialog->findChild<QSpinBox*>("exportCrf"); ASSERT_NE(crf, nullptr);
        EXPECT_EQ(crf->value(), 20);
        codec->setCurrentIndex(1); EXPECT_EQ(crf->value(), 24);
        codec->setCurrentIndex(0); EXPECT_EQ(crf->value(), 20);
        crf->setValue(18); codec->setCurrentIndex(1); EXPECT_EQ(crf->value(), 18);
        dialog->reject();
    });
    controller.chooseAndExport(&parent); EXPECT_FALSE(controller.running());
}

TEST(ExportUiTest, ShutdownIsIdempotentAndRejectsNewJobs) {
    heisenberg::ui::EditorSession session;
    heisenberg::ui::QtExportController controller(&session, nullptr);
    controller.cancel(); controller.shutdown(); controller.shutdown();
    heisenberg::ExportRequest request; request.snapshot = session.snapshot();
    QString error;
    EXPECT_FALSE(controller.start(std::move(request), &error));
    EXPECT_FALSE(error.isEmpty()); EXPECT_FALSE(controller.running());
}
