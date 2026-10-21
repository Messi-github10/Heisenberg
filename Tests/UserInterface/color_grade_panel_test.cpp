#include <Backend/EditorSession.hpp>
#include <Compositing/Grade/ColorGrade.hpp>
#include <MainWidget/ColorGradeDialog.hpp>
#include <MainWidget/ColorGradePanel.hpp>
#include <MainWidget/MainWindow.hpp>
#include <MainWidget/TimelineWidget.hpp>

#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <optional>
#include <string>

using heisenberg::JsonValue;
using heisenberg::ui::EditorSession;

namespace {
JsonValue panelEffects(double exposure) {
    auto effects = JsonValue::parse(R"([
        {"type":"video.custom","payload":{"keep":true}},
        {"id":"grade","type":"video.colorGrade","schemaVersion":1,"enabled":true,"unknown":3,
         "params":{
            "exposure":{"value":0.125,"label":"preserved","track":{"keyframes":[{"frame":0,"value":0.125},{"frame":10,"value":1}]}},
            "contrast":{"value":1,"track":{"keyframes":[{"frame":0,"value":1}]}},
            "saturation":{"value":1},
            "curves":{"red":[[0,0],[0.5,0.6],[1,1]],"future":"preserve"},
            "hueTargets":[{"hue":30,"rotation":{"value":0,"track":{"keyframes":[{"frame":0,"value":0}]}},"saturation":1,"lightness":0,"unknown":8}]}},
        {"type":"video.basicGrade","enabled":false,"params":{"exposure":{"value":0,"track":{"keyframes":[{"frame":0,"value":2}]}}}}
    ])");
    effects[1]["params"]["exposure"]["value"] = exposure;
    return effects;
}

void setConstant(JsonValue& effects, const char* parameter, double value) {
    auto& field = effects[1]["params"][parameter];
    field["value"] = value;
    field.erase("track");
}

void selectClip(TimelineWidget& timeline, QPointF point = QPointF(200, 48)) {
    QMouseEvent press(QEvent::MouseButtonPress, point, timeline.mapToGlobal(point.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, point, timeline.mapToGlobal(point.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &release);
}

// Probe the public transaction entry point rather than inspecting history revisions
// or relying on the framework's private transaction-state accessor.
void expectNoPendingTransaction(EditorSession& session, const QString& id) {
    const auto result = session.beginClipEffectsTransaction(id);
    EXPECT_TRUE(result.changed()) << result.message;
    if (result.changed()) EXPECT_TRUE(session.cancelClipEffectsTransaction().message.empty());
}

void expectPendingTransaction(EditorSession& session, const QString& id) {
    const auto result = session.beginClipEffectsTransaction(id);
    EXPECT_EQ(result.status, heisenberg::EditStatus::Invalid);
    EXPECT_NE(result.message.find("pending"), std::string::npos);
    if (result.changed()) session.cancelClipEffectsTransaction();
}

class ColorGradePanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
        const auto ref = QString::fromStdString(session.editor().manifest().entries().front().id);
        auto placed = session.placeMedia(ref, 0);
        ASSERT_TRUE(placed.changed()) << placed.message;
        ASSERT_FALSE(placed.affectedIds.empty());
        firstId = QString::fromStdString(placed.affectedIds.front());
        placed = session.placeMedia(ref, 60);
        ASSERT_TRUE(placed.changed()) << placed.message;
        ASSERT_FALSE(placed.affectedIds.empty());
        secondId = QString::fromStdString(placed.affectedIds.front());
        ASSERT_NE(firstId, secondId);
        ASSERT_TRUE(session.setClipEffects(firstId, firstEffects).changed());
        ASSERT_TRUE(session.setClipEffects(secondId, secondEffects).changed());
    }

    std::optional<JsonValue> effects(const QString& id) const {
        const auto* timeline = session.editor().activeTimeline();
        const auto* clip = timeline ? timeline->clipById(id.toStdString()) : nullptr;
        EXPECT_NE(clip, nullptr);
        return clip ? clip->effects : std::nullopt;
    }

    EditorSession session;
    QString firstId, secondId;
    const JsonValue firstEffects = panelEffects(.125);
    const JsonValue secondEffects = panelEffects(-.25);
};
} // namespace

TEST_F(ColorGradePanelTest, TimelineSelectionUsesPersistentInspectorAndNeverOpensGradeDialog) {
    MainWindow window(&session);
    window.show();
    QCoreApplication::processEvents();
    auto* inspector = window.findChild<QTabWidget*>("previewInspector");
    auto* panel = window.findChild<ColorGradePanel*>("colorGradePanel");
    ASSERT_NE(inspector, nullptr);
    ASSERT_NE(panel, nullptr);
    ASSERT_EQ(inspector->count(), 2);
    EXPECT_EQ(inspector->tabText(0), QString("Color Grade"));
    EXPECT_EQ(inspector->tabText(1), QString("Media Info"));
    EXPECT_EQ(inspector->widget(0), panel);
    EXPECT_TRUE(panel->clipId().isEmpty());
    EXPECT_EQ(panel->findChild<ColorGradeControls*>(), nullptr);

    QStringList selections;
    QObject::connect(window.timelineWidget(), &TimelineWidget::selectedClipChanged,
                     &window, [&](const QString& id) { selections.push_back(id); });
    selectClip(*window.timelineWidget());
    EXPECT_EQ(selections, QStringList{firstId});
    EXPECT_EQ(window.timelineWidget()->selectedClipId(), firstId);
    EXPECT_EQ(panel->clipId(), firstId);
    auto* controls = panel->findChild<ColorGradeControls*>();
    ASSERT_NE(controls, nullptr);
    EXPECT_EQ(controls->effects(), firstEffects);
    EXPECT_EQ(panel->findChild<QDialogButtonBox*>(), nullptr);
    auto* tabs = panel->findChild<QTabWidget*>("colorGradeTabs");
    ASSERT_NE(tabs, nullptr);
    ASSERT_EQ(tabs->count(), 6);
    EXPECT_EQ(tabs->tabPosition(), QTabWidget::North);
    const QStringList names{"Light", "Color", "Wheels", "Curves", "Hue Targets", "LUT"};
    for (int i = 0; i < names.size(); ++i) EXPECT_EQ(tabs->tabText(i), names[i]);
    for (const auto& spec : heisenberg::grade::colorGradeParameterRegistry()) {
        auto* field = controls->findChild<QDoubleSpinBox*>(QString("colorGrade_%1").arg(spec.name));
        ASSERT_NE(field, nullptr) << spec.name;
        EXPECT_DOUBLE_EQ(field->minimum(), spec.minimum);
        EXPECT_DOUBLE_EQ(field->maximum(), spec.maximum);
    }
    EXPECT_EQ(controls->findChildren<ColorWheelWidget*>().size(), 3);
    controls->setCurrentTab(3);
    EXPECT_EQ(controls->currentTab(), 3);
    inspector->setCurrentIndex(1);
    EXPECT_EQ(panel->clipId(), firstId);
    EXPECT_EQ(panel->findChild<ColorGradeControls*>(), controls);

    // If routing regresses to the old modal entry point, fail without hanging.
    bool openedDialog = false;
    QTimer fallback;
    fallback.setSingleShot(true);
    QObject::connect(&fallback, &QTimer::timeout, &window, [&] {
        if (auto* dialog = window.findChild<ColorGradeDialog*>("clipColorGradeDialog")) {
            openedDialog = true;
            dialog->reject();
        }
    });
    fallback.start(0);
    window.timelineWidget()->editSelectedClipColorGrade();
    fallback.stop();
    EXPECT_FALSE(openedDialog);
    EXPECT_EQ(window.findChild<ColorGradeDialog*>(), nullptr);
    EXPECT_EQ(inspector->currentWidget(), panel);
    EXPECT_EQ(panel->findChild<ColorGradeControls*>(), controls);
    EXPECT_EQ(controls->currentTab(), 3);
    selectClip(*window.timelineWidget());
    EXPECT_EQ(selections, QStringList{firstId}); // Re-selecting is not a new binding.
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEffects));
    expectNoPendingTransaction(session, firstId);

    const auto screenshot = qEnvironmentVariable("HEISENBERG_PANEL_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QCoreApplication::processEvents();
        EXPECT_TRUE(window.grab().save(screenshot));
        for (int i = 0; i < tabs->count(); ++i) {
            controls->setCurrentTab(i);
            QCoreApplication::processEvents();
            EXPECT_TRUE(window.grab().save(screenshot + QString(".tab-%1.png").arg(i)));
        }
        window.resize(900, 600);
        controls->setCurrentTab(0);
        QCoreApplication::processEvents();
        EXPECT_GT(panel->width(), 0);
        EXPECT_GT(panel->height(), 0);
        EXPECT_TRUE(window.grab().save(screenshot + ".small.png"));
    }

    selectClip(*window.timelineWidget(), QPointF(900, 48));
    EXPECT_TRUE(window.timelineWidget()->selectedClipId().isEmpty());
    EXPECT_TRUE(panel->clipId().isEmpty());
    EXPECT_EQ(panel->findChild<ColorGradeControls*>(), nullptr);
    EXPECT_EQ(selections, (QStringList{firstId, QString()}));
    expectNoPendingTransaction(session, firstId);
}

TEST_F(ColorGradePanelTest, MultiplePreviewValuesCommitAsOneUndoAndRedoRefreshesControls) {
    ColorGradePanel panel(&session);
    panel.setClip(firstId);
    auto* controls = panel.findChild<ColorGradeControls*>();
    ASSERT_NE(controls, nullptr);
    controls->setCurrentTab(1);
    auto* exposure = panel.findChild<QDoubleSpinBox*>("colorGrade_exposure");
    auto* contrast = panel.findChild<QDoubleSpinBox*>("colorGrade_contrast");
    auto* saturation = panel.findChild<QDoubleSpinBox*>("colorGrade_saturation");
    ASSERT_NE(exposure, nullptr);
    ASSERT_NE(contrast, nullptr);
    ASSERT_NE(saturation, nullptr);
    exposure->setValue(.5);
    exposure->setValue(1);
    exposure->setValue(1.5);
    contrast->setValue(1.25);
    saturation->setValue(1.2);
    auto expected = firstEffects;
    setConstant(expected, "exposure", 1.5);
    setConstant(expected, "contrast", 1.25);
    setConstant(expected, "saturation", 1.2);
    EXPECT_EQ(controls->effects(), expected);
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(expected));
    EXPECT_EQ(effects(secondId), std::optional<JsonValue>(secondEffects));
    expectPendingTransaction(session, firstId);
    panel.finishEditing();
    expectNoPendingTransaction(session, firstId);
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(expected));

    session.undo();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEffects));
    // Undo/redo can replace the controls; never reuse editor pointers across them.
    controls = panel.findChild<ColorGradeControls*>();
    ASSERT_NE(controls, nullptr);
    EXPECT_EQ(controls->effects(), firstEffects);
    EXPECT_EQ(controls->currentTab(), 1);
    exposure = panel.findChild<QDoubleSpinBox*>("colorGrade_exposure");
    ASSERT_NE(exposure, nullptr);
    EXPECT_DOUBLE_EQ(exposure->value(), .125);
    session.redo();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(expected));
    controls = panel.findChild<ColorGradeControls*>();
    ASSERT_NE(controls, nullptr);
    EXPECT_EQ(controls->effects(), expected);
    exposure = panel.findChild<QDoubleSpinBox*>("colorGrade_exposure");
    contrast = panel.findChild<QDoubleSpinBox*>("colorGrade_contrast");
    ASSERT_NE(exposure, nullptr);
    ASSERT_NE(contrast, nullptr);
    EXPECT_DOUBLE_EQ(exposure->value(), 1.5);
    EXPECT_DOUBLE_EQ(contrast->value(), 1.25);
    expectNoPendingTransaction(session, firstId);
}

TEST_F(ColorGradePanelTest, SwitchingClipsCommitsOldBindingBeforeEditingNewBinding) {
    ColorGradePanel panel(&session);
    panel.setClip(firstId);
    auto* controls = panel.findChild<ColorGradeControls*>();
    ASSERT_NE(controls, nullptr);
    controls->setCurrentTab(3);
    auto* exposure = panel.findChild<QDoubleSpinBox*>("colorGrade_exposure");
    ASSERT_NE(exposure, nullptr);
    exposure->setValue(1.5);
    expectPendingTransaction(session, firstId);
    auto firstEdited = firstEffects;
    setConstant(firstEdited, "exposure", 1.5);
    panel.setClip(secondId);
    EXPECT_EQ(panel.clipId(), secondId);
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEdited));
    EXPECT_EQ(effects(secondId), std::optional<JsonValue>(secondEffects));
    expectNoPendingTransaction(session, secondId);
    controls = panel.findChild<ColorGradeControls*>();
    ASSERT_NE(controls, nullptr);
    EXPECT_EQ(controls->effects(), secondEffects);
    EXPECT_EQ(controls->currentTab(), 3);
    exposure = panel.findChild<QDoubleSpinBox*>("colorGrade_exposure");
    ASSERT_NE(exposure, nullptr);
    EXPECT_DOUBLE_EQ(exposure->value(), -.25);
    exposure->setValue(-1);
    auto secondEdited = secondEffects;
    setConstant(secondEdited, "exposure", -1);
    panel.finishEditing();
    session.undo();
    EXPECT_EQ(effects(secondId), std::optional<JsonValue>(secondEffects));
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEdited));
    session.undo();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEffects));
    session.redo();
    session.redo();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEdited));
    EXPECT_EQ(effects(secondId), std::optional<JsonValue>(secondEdited));
}

TEST_F(ColorGradePanelTest, CancelAndNoOpFinishPreserveFullPayloadAndExistingRedo) {
    ColorGradePanel panel(&session);
    panel.setClip(firstId);
    auto* exposure = panel.findChild<QDoubleSpinBox*>("colorGrade_exposure");
    ASSERT_NE(exposure, nullptr);
    exposure->setValue(1.5);
    panel.finishEditing();
    const auto committed = effects(firstId);
    session.undo();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEffects));
    exposure = panel.findChild<QDoubleSpinBox*>("colorGrade_exposure");
    ASSERT_NE(exposure, nullptr);
    exposure->setValue(2);
    expectPendingTransaction(session, firstId);
    panel.cancelEditing();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEffects));
    auto* controls = panel.findChild<ColorGradeControls*>();
    ASSERT_NE(controls, nullptr);
    EXPECT_EQ(controls->effects(), firstEffects);
    expectNoPendingTransaction(session, firstId);
    exposure = panel.findChild<QDoubleSpinBox*>("colorGrade_exposure");
    ASSERT_NE(exposure, nullptr);
    exposure->setValue(.75);
    exposure->setValue(.125);
    panel.finishEditing();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEffects));
    expectNoPendingTransaction(session, firstId);
    session.redo();
    EXPECT_EQ(effects(firstId), committed);
}

TEST_F(ColorGradePanelTest, SuccessiveCommitsRebaseWithoutResurrectingImportedAnimation) {
    ColorGradePanel panel(&session);
    panel.setClip(firstId);
    auto* exposure = panel.findChild<QDoubleSpinBox*>("colorGrade_exposure");
    ASSERT_NE(exposure, nullptr);
    exposure->setValue(1.5);
    panel.finishEditing();
    auto firstEdited = firstEffects;
    setConstant(firstEdited, "exposure", 1.5);
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEdited));
    exposure = panel.findChild<QDoubleSpinBox*>("colorGrade_exposure");
    ASSERT_NE(exposure, nullptr);
    exposure->setValue(.125); // The old displayed value is now a new constant edit.
    panel.finishEditing();
    auto secondEdited = firstEffects;
    setConstant(secondEdited, "exposure", .125);
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(secondEdited));
    session.undo();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEdited));
    session.redo();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(secondEdited));
    expectNoPendingTransaction(session, firstId);
}

TEST_F(ColorGradePanelTest, NumericEditingFinishedCommitsAfterOriginatingEditorEvent) {
    ColorGradePanel panel(&session);
    panel.setClip(firstId);
    auto* controls = panel.findChild<ColorGradeControls*>();
    auto* exposure = panel.findChild<QDoubleSpinBox*>("colorGrade_exposure");
    ASSERT_NE(controls, nullptr);
    ASSERT_NE(exposure, nullptr);
    int changes = 0, finishes = 0;
    QObject::connect(controls, &ColorGradeControls::effectsChanged, &panel, [&] { ++changes; });
    QObject::connect(controls, &ColorGradeControls::editFinished, &panel, [&] { ++finishes; });
    exposure->setValue(.5);
    exposure->setValue(1.5);
    EXPECT_EQ(changes, 2);
    exposure->editingFinished();
    EXPECT_EQ(finishes, 0);
    expectPendingTransaction(session, firstId);
    QCoreApplication::processEvents();
    EXPECT_EQ(finishes, 1);
    expectNoPendingTransaction(session, firstId);
    session.undo();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEffects));
}

TEST_F(ColorGradePanelTest, EmptyMissingAndAudioSelectionsAreSafeAndCanReturnToVideo) {
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_WAV}).isEmpty());
    const auto audioRef = QString::fromStdString(session.editor().manifest().entries().back().id);
    const auto placed = session.placeMedia(audioRef, 0);
    ASSERT_TRUE(placed.changed()) << placed.message;
    ASSERT_FALSE(placed.affectedIds.empty());
    const auto audioId = QString::fromStdString(placed.affectedIds.front());
    const auto before = session.editor().project();
    ColorGradePanel panel(&session);
    QStringList failures;
    QObject::connect(&panel, &ColorGradePanel::editFailed, &panel,
                     [&](const QString& message) { failures.push_back(message); });
    for (const auto& id : {QString(), QString("not-a-clip"), audioId}) {
        SCOPED_TRACE(id.toStdString());
        panel.setClip(id);
        EXPECT_EQ(panel.clipId(), id);
        EXPECT_EQ(panel.findChild<ColorGradeControls*>(), nullptr);
        EXPECT_TRUE(panel.findChildren<QDoubleSpinBox*>().isEmpty());
        auto* hint = panel.findChild<QLabel*>("colorGradeSelectionHint");
        ASSERT_NE(hint, nullptr);
        EXPECT_FALSE(hint->text().isEmpty());
        panel.finishEditing();
        panel.cancelEditing();
        EXPECT_EQ(session.editor().project(), before);
        expectNoPendingTransaction(session, firstId);
    }
    panel.setClip(firstId);
    auto* controls = panel.findChild<ColorGradeControls*>();
    ASSERT_NE(controls, nullptr);
    EXPECT_EQ(controls->effects(), firstEffects);
    EXPECT_TRUE(failures.isEmpty());
}

TEST_F(ColorGradePanelTest, UnsupportedSchemasStayReadOnlyWithoutStartingTransactions) {
    ColorGradePanel panel(&session);
    for (const auto& version : JsonValue::array({2, nullptr, "1", 1.0, true, 0, -1, JsonValue::object()})) {
        SCOPED_TRACE(version.dump());
        auto unsupported = firstEffects;
        unsupported[1]["schemaVersion"] = version;
        unsupported[1]["enabled"] = false;
        if (version.is_null()) unsupported[1].erase("schemaVersion");
        ASSERT_TRUE(session.setClipEffects(firstId, unsupported).changed());
        panel.setClip(firstId);
        auto* controls = panel.findChild<ColorGradeControls*>();
        ASSERT_NE(controls, nullptr);
        EXPECT_FALSE(controls->isEditable());
        EXPECT_EQ(controls->effects(), unsupported);
        EXPECT_EQ(panel.findChild<QTabWidget*>("colorGradeTabs"), nullptr);
        EXPECT_TRUE(panel.findChildren<QDoubleSpinBox*>().isEmpty());
        auto* warning = panel.findChild<QLabel*>("colorGradeUnsupportedSchema");
        ASSERT_NE(warning, nullptr);
        EXPECT_TRUE(warning->text().contains("schemaVersion"));
        panel.finishEditing();
        panel.cancelEditing();
        EXPECT_EQ(effects(firstId), std::optional<JsonValue>(unsupported));
        EXPECT_EQ(effects(secondId), std::optional<JsonValue>(secondEffects));
        expectNoPendingTransaction(session, firstId);
    }
}

TEST_F(ColorGradePanelTest, DestructionCancelsPreviewAndRestoresAbsentEffectsWithoutDirtyingSave) {
    ASSERT_TRUE(session.setClipEffects(firstId, std::nullopt).changed());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString error;
    ASSERT_TRUE(session.savePackage(directory.filePath("cancel.berg"), &error)) << error.toStdString();
    ASSERT_FALSE(session.isDirty());
    {
        ColorGradePanel panel(&session);
        panel.setClip(firstId);
        auto* exposure = panel.findChild<QDoubleSpinBox*>("colorGrade_exposure");
        ASSERT_NE(exposure, nullptr);
        exposure->setValue(1);
        ASSERT_TRUE(effects(firstId).has_value());
        expectPendingTransaction(session, firstId);
    }
    EXPECT_EQ(effects(firstId), std::nullopt);
    EXPECT_FALSE(session.isDirty());
    expectNoPendingTransaction(session, firstId);
}

TEST_F(ColorGradePanelTest, ExportActionCommitsPendingGradeBeforeNotifyingConsumers) {
    MainWindow window(&session);
    auto* panel = window.findChild<ColorGradePanel*>("colorGradePanel");
    auto* action = window.findChild<QAction*>("exportTimeline");
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(action, nullptr);
    panel->setClip(firstId);
    auto* exposure = panel->findChild<QDoubleSpinBox*>("colorGrade_exposure");
    auto* contrast = panel->findChild<QDoubleSpinBox*>("colorGrade_contrast");
    ASSERT_NE(exposure, nullptr);
    ASSERT_NE(contrast, nullptr);
    exposure->setValue(.5);
    exposure->setValue(1.5);
    contrast->setValue(1.25);
    auto expected = firstEffects;
    setConstant(expected, "exposure", 1.5);
    setConstant(expected, "contrast", 1.25);
    expectPendingTransaction(session, firstId);
    int exports = 0;
    QObject::connect(&window, &MainWindow::exportTimelineRequested, &window, [&] {
        ++exports;
        EXPECT_EQ(effects(firstId), std::optional<JsonValue>(expected));
        expectNoPendingTransaction(session, firstId);
    });
    action->trigger();
    EXPECT_EQ(exports, 1);
    expectNoPendingTransaction(session, firstId);
    session.undo();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEffects));
    session.redo();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(expected));
}

TEST_F(ColorGradePanelTest, SaveFlushesPendingGradeAndReloadRestoresCommittedValues) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto package = directory.filePath("grade.berg");
    QString error;
    ASSERT_TRUE(session.savePackage(package, &error)) << error.toStdString();
    MainWindow window(&session);
    auto* panel = window.findChild<ColorGradePanel*>("colorGradePanel");
    ASSERT_NE(panel, nullptr);
    panel->setClip(firstId);
    auto* exposure = panel->findChild<QDoubleSpinBox*>("colorGrade_exposure");
    auto* saturation = panel->findChild<QDoubleSpinBox*>("colorGrade_saturation");
    ASSERT_NE(exposure, nullptr);
    ASSERT_NE(saturation, nullptr);
    exposure->setValue(1.5);
    saturation->setValue(1.2);
    auto expected = firstEffects;
    setConstant(expected, "exposure", 1.5);
    setConstant(expected, "saturation", 1.2);
    expectPendingTransaction(session, firstId);

    // A regression must not leave a critical-error modal blocking the test run.
    QTimer fallback;
    fallback.setSingleShot(true);
    QObject::connect(&fallback, &QTimer::timeout, &window, [&] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
    });
    fallback.start(0);
    const bool saved = window.saveProjectPackage(); // Existing root: no file chooser.
    fallback.stop();
    ASSERT_TRUE(saved);
    EXPECT_FALSE(session.isDirty());
    expectNoPendingTransaction(session, firstId);
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(expected));
    EditorSession reopened;
    ASSERT_TRUE(reopened.loadPackage(package, &error)) << error.toStdString();
    const auto* restored = reopened.editor().activeTimeline()->clipById(firstId.toStdString());
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->effects, std::optional<JsonValue>(expected));
    EXPECT_EQ(reopened.editor().project(), session.editor().project());
    session.undo();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(firstEffects));
    session.redo();
    EXPECT_EQ(effects(firstId), std::optional<JsonValue>(expected));
    ASSERT_TRUE(window.loadProjectPackage(package, false));
    EXPECT_TRUE(panel->clipId().isEmpty());
    EXPECT_EQ(panel->findChild<ColorGradeControls*>(), nullptr);
    panel->setClip(firstId);
    auto* controls = panel->findChild<ColorGradeControls*>();
    ASSERT_NE(controls, nullptr);
    EXPECT_EQ(controls->effects(), expected);
    exposure = panel->findChild<QDoubleSpinBox*>("colorGrade_exposure");
    ASSERT_NE(exposure, nullptr);
    EXPECT_DOUBLE_EQ(exposure->value(), 1.5);
    expectNoPendingTransaction(session, firstId);
}
