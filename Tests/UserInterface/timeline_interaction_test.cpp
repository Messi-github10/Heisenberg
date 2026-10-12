#include <Backend/EditorSession.hpp>
#include <Backend/PlayerController.hpp>
#include <MainWidget/MainWindow.hpp>
#include <MainWidget/TimelineWidget.hpp>

#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QWheelEvent>
#include <functional>

namespace {
void mouse(TimelineWidget& widget, QEvent::Type type, QPointF position,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, position, widget.mapToGlobal(position.toPoint()), button, buttons, modifiers);
    QApplication::sendEvent(&widget, &event);
}

void drag(TimelineWidget& widget, QPointF from, QPointF to,
          Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    mouse(widget, QEvent::MouseButtonPress, from, modifiers);
    mouse(widget, QEvent::MouseMove, to, modifiers);
    mouse(widget, QEvent::MouseButtonRelease, to, modifiers);
}

bool waitFor(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return condition();
}

class TimelineInteractionTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
        ref = QString::fromStdString(session.editor().manifest().entries().front().id);
        ASSERT_TRUE(session.placeMedia(ref, 30).changed());
        ASSERT_TRUE(session.placeMedia(ref, 90).changed());
        firstId = QString::fromStdString(session.editor().activeTrack()->clips.front().id);
        secondId = QString::fromStdString(session.editor().activeTrack()->clips.back().id);
        ASSERT_TRUE(directory.isValid());
        QString error;
        ASSERT_TRUE(session.savePackage(directory.path() + "/edit.palmier", &error)) << error.toStdString();
        timeline.setSession(&session);
        timeline.resize(1000, 210);
        timeline.show();
        QCoreApplication::processEvents();
    }
    std::string json() const { return heisenberg::ProjectCodec::encode(session.editor().project()); }
    const heisenberg::Clip* first() const { return session.editor().activeTimeline()->clipById(firstId.toStdString()); }
    const heisenberg::Clip* second() const { return session.editor().activeTimeline()->clipById(secondId.toStdString()); }
    // Default geometry: 160px header, 4px/frame. The grab is ten frames into the first clip.
    QPointF target(qint64 start, int y = 48) const { return {160.0 + (start + 10) * 4.0, double(y)}; }
    heisenberg::ui::EditorSession session;
    TimelineWidget timeline;
    QTemporaryDir directory;
    QString ref, firstId, secondId;
};

TEST_F(TimelineInteractionTest, ClickAndSubThresholdMotionSelectWithoutScrubbingOrEditing) {
    const auto before = json();
    int scrubs = 0, changes = 0;
    QObject::connect(&timeline, &TimelineWidget::scrubStarted, [&]() { ++scrubs; });
    QObject::connect(&session, &heisenberg::ui::EditorSession::timelineChanged, [&](qint64) { ++changes; });
    mouse(timeline, QEvent::MouseButtonPress, target(30));
    mouse(timeline, QEvent::MouseMove, target(30) + QPointF(3, 0));
    mouse(timeline, QEvent::MouseButtonRelease, target(30) + QPointF(3, 0));
    EXPECT_EQ(timeline.selectedClipId(), firstId);
    EXPECT_EQ(timeline.currentFrame(), 0);
    EXPECT_EQ(scrubs, 0);
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(TimelineInteractionTest, DraftIsLocalAndReleaseMovesPastEndWithOneUndoStep) {
    const auto before = json();
    const auto original = *first();
    int changes = 0;
    QObject::connect(&session, &heisenberg::ui::EditorSession::timelineChanged, [&](qint64) { ++changes; });
    mouse(timeline, QEvent::MouseButtonPress, target(30));
    mouse(timeline, QEvent::MouseMove, target(70));
    mouse(timeline, QEvent::MouseMove, target(150));
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
    EXPECT_EQ(changes, 0);
    mouse(timeline, QEvent::MouseButtonRelease, target(150));
    ASSERT_NE(first(), nullptr);
    auto expected = original;
    expected.startFrame = 150;
    EXPECT_EQ(*first(), expected);
    EXPECT_EQ(session.editor().activeTimeline()->totalFrames(), 180);
    EXPECT_EQ(timeline.selectedClipId(), firstId);
    EXPECT_EQ(changes, 1);
    EXPECT_TRUE(session.isDirty());
    const auto after = json();
    session.undo();
    EXPECT_EQ(json(), before);
    EXPECT_EQ(timeline.selectedClipId(), firstId);
    session.redo();
    EXPECT_EQ(json(), after);
}

TEST_F(TimelineInteractionTest, OverwriteMoveTrimsOtherClipAndDoesNotCoverItself) {
    const auto before = json();
    timeline.setSnappingEnabled(false);
    drag(timeline, target(30), target(100));
    ASSERT_NE(first(), nullptr);
    ASSERT_NE(second(), nullptr);
    EXPECT_EQ(first()->startFrame, 100);
    EXPECT_EQ(first()->durationFrames, 30);
    EXPECT_EQ(first()->trimStartFrame, 0);
    EXPECT_EQ(second()->startFrame, 90);
    EXPECT_EQ(second()->durationFrames, 10);
    EXPECT_EQ(second()->trimEndFrame, 20);
    EXPECT_EQ(session.editor().activeTrack()->clips.size(), 2u);
    const auto after = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), after);
}

TEST_F(TimelineInteractionTest, EitherEdgeSnapsToOtherClipBoundaryWithinEightPixels) {
    drag(timeline, target(30), target(118));
    ASSERT_NE(first(), nullptr);
    EXPECT_EQ(first()->startFrame, 120); // Leading edge -> second clip end.
    session.undo();
    drag(timeline, target(30), target(62));
    EXPECT_EQ(first()->startFrame, 60); // Trailing edge -> second clip start.
    EXPECT_EQ(second()->startFrame, 90);
    session.undo();
    drag(timeline, target(30), target(117));
    EXPECT_EQ(first()->startFrame, 117); // Twelve pixels away: no snap.
}

TEST_F(TimelineInteractionTest, PlayheadAndTimelineOriginSnapWithShiftBypassAndToggle) {
    timeline.setCurrentFrame(75);
    drag(timeline, target(30), target(73));
    EXPECT_EQ(first()->startFrame, 75);
    session.undo();
    timeline.setCurrentFrame(75);
    drag(timeline, target(30), target(73), Qt::ShiftModifier);
    EXPECT_EQ(first()->startFrame, 73);
    session.undo();
    timeline.setCurrentFrame(75);
    timeline.setSnappingEnabled(false);
    drag(timeline, target(30), target(73));
    EXPECT_EQ(first()->startFrame, 73);
    session.undo();
    timeline.setSnappingEnabled(true);
    timeline.setCurrentFrame(75);
    drag(timeline, target(30), target(2));
    EXPECT_EQ(first()->startFrame, 0);
}

TEST_F(TimelineInteractionTest, MovingClipDoesNotSnapToItsOwnOldEdges) {
    // Add vertical motion to exceed the drag threshold, while remaining in the video lane.
    drag(timeline, target(30), target(32, 62));
    EXPECT_EQ(first()->startFrame, 32);
}

TEST_F(TimelineInteractionTest, EscapeOutsideLaneAndWindowDeactivationCancelDraft) {
    const auto before = json();
    mouse(timeline, QEvent::MouseButtonPress, target(30));
    mouse(timeline, QEvent::MouseMove, target(150));
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &escape);
    mouse(timeline, QEvent::MouseButtonRelease, target(150));
    EXPECT_EQ(json(), before);
    drag(timeline, target(30), target(150, 120));
    EXPECT_EQ(json(), before);
    mouse(timeline, QEvent::MouseButtonPress, target(30));
    mouse(timeline, QEvent::MouseMove, target(150));
    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(&timeline, &deactivate);
    mouse(timeline, QEvent::MouseButtonRelease, target(150));
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(TimelineInteractionTest, NoOpMoveKeepsRedoAndDoesNotDirtySavedProject) {
    ASSERT_TRUE(session.moveClip(firstId, 0).changed());
    const auto moved = json();
    session.undo();
    QString error;
    ASSERT_TRUE(session.savePackage(QString::fromStdString(session.editor().projectDir()), &error));
    const auto before = json();
    // Exceed drag threshold, then return to the original start.
    mouse(timeline, QEvent::MouseButtonPress, target(30));
    mouse(timeline, QEvent::MouseMove, target(70));
    mouse(timeline, QEvent::MouseButtonRelease, target(30));
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
    session.redo();
    EXPECT_EQ(json(), moved);
}

TEST_F(TimelineInteractionTest, RulerAndEmptySpaceScrubWithoutMovingOrMisSelectingLastClip) {
    const auto before = json();
    drag(timeline, target(30), target(30));
    int scrubs = 0;
    QObject::connect(&timeline, &TimelineWidget::scrubStarted, [&]() { ++scrubs; });
    drag(timeline, QPointF(400, 15), QPointF(440, 15));
    EXPECT_EQ(timeline.currentFrame(), 70);
    EXPECT_EQ(timeline.selectedClipId(), firstId);
    drag(timeline, QPointF(760, 48), QPointF(760, 48)); // Past totalFrames=120.
    EXPECT_TRUE(timeline.selectedClipId().isEmpty());
    EXPECT_EQ(timeline.currentFrame(), 119);
    EXPECT_EQ(scrubs, 2);
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(TimelineInteractionTest, MovementUsesZoomAndHorizontalScrollCoordinates) {
    timeline.setSnappingEnabled(false);
    timeline.zoomIn(); // 5 px/frame; short timeline stays at scrollOffset=0.
    drag(timeline, QPointF(360, 48), QPointF(960, 48));
    EXPECT_EQ(first()->startFrame, 150);
    session.undo();
    timeline.zoomOut(); // Back to 4 px/frame.
    ASSERT_TRUE(session.placeMedia(ref, 300).changed());
    QWheelEvent wheel(QPointF(600, 48), timeline.mapToGlobal(QPoint(600, 48)),
        QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&timeline, &wheel); // ScrollOffset=80.
    drag(timeline, QPointF(240, 48), QPointF(412, 48));
    EXPECT_EQ(first()->startFrame, 73);
}

TEST_F(TimelineInteractionTest, MultiTrackSelectionIsAllowedButMovementIsRejected) {
    auto project = session.editor().project();
    heisenberg::Track extra;
    extra.id = heisenberg::makeUuid();
    project.timelines.front().tracks.push_back(extra);
    const auto text = heisenberg::ProjectCodec::encode(project);
    const auto package = QString::fromStdString(session.editor().projectDir());
    QFile file(package + "/project.json");
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(text.data(), static_cast<qint64>(text.size())), static_cast<qint64>(text.size()));
    file.close();
    QString error;
    ASSERT_TRUE(session.loadPackage(package, &error)) << error.toStdString();
    const auto before = json();
    int failures = 0;
    QObject::connect(&timeline, &TimelineWidget::editFailed, [&](const QString&) { ++failures; });
    drag(timeline, target(30), target(150));
    EXPECT_EQ(timeline.selectedClipId(), firstId);
    EXPECT_EQ(json(), before);
    EXPECT_EQ(failures, 1);
    EXPECT_EQ(session.moveClip(firstId, 150).status, heisenberg::EditStatus::Unsupported);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(TimelineInteractionTest, MoveReopensUnsavedPlaybackSnapshotAndUpdatesDuration) {
    heisenberg::ui::PlayerController player(&session);
    player.setHardwareDecode(false);
    player.showTimeline(0);
    ASSERT_TRUE(waitFor([&]() { return player.isSeekable(); }));
    drag(timeline, target(30), target(150));
    ASSERT_TRUE(waitFor([&]() { return player.frameCount() == 180 && player.currentTime() >= 4.99; }));
    EXPECT_NEAR(player.duration(), 6.0, 0.01);
    EXPECT_TRUE(session.isDirty());
    QFile saved(QString::fromStdString(session.editor().projectDir()) + "/project.json");
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    EXPECT_NE(saved.readAll().toStdString(), json()); // No save was required for playback.
}

TEST_F(TimelineInteractionTest, WindowActionsCancelDraftAndUndoRedoCommittedMove) {
    MainWindow window(&session);
    window.show();
    QCoreApplication::processEvents();
    auto* widget = window.timelineWidget();
    auto action = [&](const QString& text) -> QAction* {
        for (auto* candidate : window.findChildren<QAction*>())
            if (candidate->text() == text) return candidate;
        return nullptr;
    };
    auto* undo = action("Undo");
    auto* redo = action("Redo");
    auto* escape = action("Return to Timeline");
    auto* snap = action("Snap");
    ASSERT_NE(undo, nullptr);
    ASSERT_NE(redo, nullptr);
    ASSERT_NE(escape, nullptr);
    ASSERT_NE(snap, nullptr);
    snap->setChecked(false);
    EXPECT_FALSE(widget->snappingEnabled());
    const auto before = json();
    mouse(*widget, QEvent::MouseButtonPress, target(30));
    mouse(*widget, QEvent::MouseMove, target(150));
    int returns = 0;
    QObject::connect(&window, &MainWindow::timelinePreviewRequested, [&]() { ++returns; });
    escape->trigger();
    mouse(*widget, QEvent::MouseButtonRelease, target(150));
    EXPECT_EQ(json(), before);
    EXPECT_EQ(returns, 0);
    drag(*widget, target(30), target(150));
    const auto after = json();
    EXPECT_NE(after, before);
    undo->trigger();
    EXPECT_EQ(json(), before);
    redo->trigger();
    EXPECT_EQ(json(), after);
    // Prevent the test window's close prompt; destructor tears it down without saving.
    window.hide();
}
} // namespace
