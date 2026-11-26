#include <Backend/EditorSession.hpp>
#include <Backend/PlayerController.hpp>
#include <MainWidget/ClipPropertiesPanel.hpp>
#include <MainWidget/ColorGradePanel.hpp>
#include <MainWidget/MainWindow.hpp>
#include <MainWidget/TimelineWidget.hpp>

#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <cmath>
#include <functional>

using namespace heisenberg;
using heisenberg::ui::EditorSession;
using heisenberg::ui::PlayerController;

namespace {
void selectPropertiesClip(TimelineWidget* timeline, double x) {
    const QPointF point(x, 68);
    QMouseEvent press(QEvent::MouseButtonPress, point, timeline->mapToGlobal(point.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(timeline, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, point, timeline->mapToGlobal(point.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(timeline, &release);
}
bool waitProperties(const std::function<bool()>& ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return ready();
}
class ClipPropertiesWorkflowTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
        ref = QString::fromStdString(session.editor().manifest().entries().front().id);
        auto result = session.placeMedia(ref, 0);
        ASSERT_TRUE(result.changed()) << result.message;
        id = QString::fromStdString(result.affectedIds.front());
    }
    void addOpacityKey(int64_t frame) {
        ClipAppearanceEdit edit;
        edit.property = AppearanceProperty::Opacity;
        edit.localFrame = frame;
        edit.action = AppearanceEditAction::ToggleKeyframe;
        ASSERT_TRUE(session.editClipAppearance(id, edit).changed());
    }
    EditorSession session;
    QString id, ref;
};
}

TEST_F(ClipPropertiesWorkflowTest, InspectorRetainsTabAndAppearanceMenuRoutesWithoutModal) {
    MainWindow window(&session);
    window.show();
    QCoreApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>("previewInspector");
    auto* panel = window.findChild<ClipPropertiesPanel*>("clipPropertiesPanel");
    auto* grade = window.findChild<ColorGradePanel*>("colorGradePanel");
    ASSERT_NE(tabs, nullptr);
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(grade, nullptr);
    EXPECT_EQ(tabs->currentWidget(), panel);
    selectPropertiesClip(window.timelineWidget(), 200);
    EXPECT_EQ(panel->clipId(), id);
    EXPECT_EQ(grade->clipId(), id);
    tabs->setCurrentWidget(grade);
    const auto placed = session.placeMedia(ref, 60);
    ASSERT_TRUE(placed.changed());
    selectPropertiesClip(window.timelineWidget(), 420);
    EXPECT_EQ(tabs->currentWidget(), grade);
    window.timelineWidget()->editSelectedClipAppearance();
    EXPECT_EQ(tabs->currentWidget(), panel);
    EXPECT_EQ(panel->clipId(), QString::fromStdString(placed.affectedIds.front()));
    const auto screenshot = qEnvironmentVariable("HEISENBERG_PROPERTIES_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        window.resize(1200, 760);
        QCoreApplication::processEvents();
        EXPECT_TRUE(window.grab().save(screenshot));
    }
}

TEST_F(ClipPropertiesWorkflowTest, ExportAndSaveFlushAppearanceGestureAndReopenAnimation) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString package = directory.filePath("properties.berg");
    QString error;
    ASSERT_TRUE(session.savePackage(package, &error)) << error.toStdString();
    addOpacityKey(0);
    MainWindow window(&session);
    auto* panel = window.findChild<ClipPropertiesPanel*>("clipPropertiesPanel");
    ASSERT_NE(panel, nullptr);
    panel->setClip(id);
    auto* opacity = panel->findChild<QDoubleSpinBox*>("clipOpacity");
    ASSERT_NE(opacity, nullptr);
    opacity->setValue(75);
    opacity->setValue(50);
    ASSERT_TRUE(session.editor().hasPendingDocumentTransaction());
    auto* action = window.findChild<QAction*>("exportTimeline");
    ASSERT_NE(action, nullptr);
    int exports = 0;
    QObject::connect(&window, &MainWindow::exportTimelineRequested, &window, [&] {
        ++exports;
        EXPECT_FALSE(session.editor().hasPendingDocumentTransaction());
        EXPECT_EQ(session.snapshot().project(), session.editor().project());
    });
    action->trigger();
    EXPECT_EQ(exports, 1);
    session.undo();
    EXPECT_DOUBLE_EQ(session.editor().activeTimeline()->clipById(id.toStdString())
                         ->opacityTrack->value["keyframes"][0]["value"].get<double>(), 1);
    session.redo();
    opacity->setValue(25);
    ASSERT_TRUE(session.editor().hasPendingDocumentTransaction());
    ASSERT_TRUE(window.saveProjectPackage());
    EXPECT_FALSE(session.isDirty());
    EXPECT_FALSE(session.editor().hasPendingDocumentTransaction());
    EditorSession reopened;
    ASSERT_TRUE(reopened.loadPackage(package, &error)) << error.toStdString();
    EXPECT_EQ(reopened.editor().project(), session.editor().project());
    EXPECT_DOUBLE_EQ(reopened.editor().activeTimeline()->clipById(id.toStdString())
                         ->opacityTrack->value["keyframes"][0]["value"].get<double>(), .25);
}

TEST_F(ClipPropertiesWorkflowTest, GradeAndAppearanceCommitEachOtherBeforePreviewing) {
    MainWindow window(&session);
    auto* panel = window.findChild<ClipPropertiesPanel*>("clipPropertiesPanel");
    auto* grade = window.findChild<ColorGradePanel*>("colorGradePanel");
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(grade, nullptr);
    panel->setClip(id);
    grade->setClip(id);
    auto* opacity = panel->findChild<QDoubleSpinBox*>("clipOpacity");
    auto* exposure = grade->findChild<QDoubleSpinBox*>("colorGrade_exposure");
    ASSERT_NE(opacity, nullptr);
    ASSERT_NE(exposure, nullptr);
    opacity->setValue(50);
    ASSERT_TRUE(session.editor().hasPendingDocumentTransaction());
    exposure->setValue(1);
    ASSERT_TRUE(session.editor().hasPendingDocumentTransaction());
    grade->finishEditing();
    opacity->setValue(25);
    ASSERT_TRUE(session.editor().hasPendingDocumentTransaction());
    panel->finishEditing();
    session.undo();
    EXPECT_DOUBLE_EQ(session.editor().activeTimeline()->clipById(id.toStdString())->opacity, .5);
    session.undo();
    EXPECT_FALSE(session.editor().activeTimeline()->clipById(id.toStdString())->effects.has_value());
    session.undo();
    EXPECT_DOUBLE_EQ(session.editor().activeTimeline()->clipById(id.toStdString())->opacity, 1);
}

TEST_F(ClipPropertiesWorkflowTest, NavigationSeeksActualPreviewAndReturnsFromSourceMode) {
    addOpacityKey(0);
    addOpacityKey(15);
    PlayerController player(&session);
    player.setHardwareDecode(false);
    MainWindow window(&session);
    QObject::connect(&window, &MainWindow::timelineSeekRequested, &player, &PlayerController::seekTimelineFrame);
    QObject::connect(&player, &PlayerController::previewModeChanged, &window, &MainWindow::setSourcePreview);
    QObject::connect(&player, &PlayerController::durationChanged, &window, [&] { window.setDuration(player.duration()); });
    QObject::connect(&player, &PlayerController::frameCountChanged, &window, [&] { window.setFrameCount(player.frameCount()); });
    QObject::connect(&player, &PlayerController::currentTimeChanged, &window, [&] { window.setCurrentTime(player.currentTime()); });
    auto* panel = window.findChild<ClipPropertiesPanel*>("clipPropertiesPanel");
    ASSERT_NE(panel, nullptr);
    panel->setClip(id);
    auto* next = panel->findChild<QToolButton*>("clipOpacityNext");
    ASSERT_NE(next, nullptr);
    player.showTimeline(0);
    ASSERT_TRUE(waitProperties([&] { return player.isSeekable(); }));
    const auto revision = session.editor().revision();
    const auto project = session.editor().project();
    next->click();
    ASSERT_TRUE(waitProperties([&] { return std::abs(player.currentTime() - .5) < .001; }));
    EXPECT_EQ(window.timelineWidget()->currentFrame(), 15);
    EXPECT_EQ(panel->currentFrame(), 15);
    player.previewAsset(ref);
    ASSERT_TRUE(waitProperties([&] { return player.sourceMarkingAvailable(); }));
    auto* previous = panel->findChild<QToolButton*>("clipOpacityPrevious");
    ASSERT_NE(previous, nullptr);
    previous->click();
    ASSERT_TRUE(waitProperties([&] { return !player.previewingSource() && player.isSeekable() && player.currentTime() < .001; }));
    EXPECT_EQ(window.timelineWidget()->currentFrame(), 0);
    EXPECT_EQ(session.editor().revision(), revision);
    EXPECT_EQ(session.editor().project(), project);
    player.shutdown();
}
