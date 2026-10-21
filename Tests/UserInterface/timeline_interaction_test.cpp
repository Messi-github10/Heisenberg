#include <Backend/EditorSession.hpp>
#include <Models/ProjectCodec.hpp>
#include <Backend/PlayerController.hpp>
#include <MainWidget/MainWindow.hpp>
#include <MainWidget/TimelineWidget.hpp>

#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <cmath>
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
        ASSERT_TRUE(session.savePackage(directory.path() + "/edit.berg", &error)) << error.toStdString();
        timeline.setSession(&session);
        timeline.resize(1000, 210);
        timeline.show();
        QCoreApplication::processEvents();
    }
    std::string json() const {
        return heisenberg::ProjectCodec::encode(session.editor().project());
    }
    const heisenberg::Clip* first() const {
        return session.editor().activeTimeline()->clipById(firstId.toStdString());
    }
    const heisenberg::Clip* second() const {
        return session.editor().activeTimeline()->clipById(secondId.toStdString());
    }
    bool reloadProject(const heisenberg::ProjectFile& project) {
        const auto text = heisenberg::ProjectCodec::encode(project);
        const auto package = QString::fromStdString(session.editor().projectDir());
        QFile file(package + "/project.json");
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(text.data(), static_cast<qint64>(text.size())) != static_cast<qint64>(text.size())) return false;
        file.close();
        return session.loadPackage(package);
    }
    // Default geometry: 160px header, 4px/frame. The grab is ten frames into the first clip.
    QPointF target(qint64 start, int y = 48) const {
        return {160.0 + (start + 10) * 4.0, double(y)};
    }
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
    EXPECT_EQ(session.trimClip(firstId, heisenberg::TrimEdge::Left, 10).status, heisenberg::EditStatus::Unsupported);
    EXPECT_EQ(session.splitClip(firstId, 45).status, heisenberg::EditStatus::Unsupported);
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(TimelineInteractionTest, EdgeDraftIsLocalAndEitherTrimCommitsOneUndoStep) {
    timeline.setSnappingEnabled(false);
    const auto before = json();
    int changes = 0;
    QObject::connect(&session, &heisenberg::ui::EditorSession::timelineChanged, [&](qint64) { ++changes; });
    mouse(timeline, QEvent::MouseButtonPress, {281, 48});
    mouse(timeline, QEvent::MouseMove, {321, 48});
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
    EXPECT_EQ(changes, 0);
    mouse(timeline, QEvent::MouseButtonRelease, {321, 48});
    ASSERT_NE(first(), nullptr);
    EXPECT_EQ(first()->startFrame, 40);
    EXPECT_EQ(first()->durationFrames, 20);
    EXPECT_EQ(first()->trimStartFrame, 10);
    EXPECT_EQ(first()->trimEndFrame, 0);
    EXPECT_EQ(timeline.currentFrame(), 40);
    EXPECT_EQ(changes, 1);
    EXPECT_EQ(timeline.selectedClipId(), firstId);
    const auto afterLeft = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), afterLeft);
    session.undo();

    drag(timeline, {399, 48}, {359, 48});
    EXPECT_EQ(first()->startFrame, 30);
    EXPECT_EQ(first()->durationFrames, 20);
    EXPECT_EQ(first()->trimStartFrame, 0);
    EXPECT_EQ(first()->trimEndFrame, 10);
    EXPECT_EQ(timeline.currentFrame(), 49); // End is exclusive; preview the last included frame.
    const auto afterRight = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), afterRight);
}

TEST_F(TimelineInteractionTest, EdgesRestoreAvailableSourceAndClampToOneFrame) {
    timeline.setSnappingEnabled(false);
    const auto before = json();
    drag(timeline, {281, 48}, {201, 48}); // No unused head: cannot extend to frame 10.
    drag(timeline, {399, 48}, {479, 48}); // No unused tail: cannot extend to frame 80.
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());

    drag(timeline, {281, 48}, {321, 48}); // Trim ten frames, then restore only that head.
    drag(timeline, {321, 48}, {241, 48});
    EXPECT_EQ(json(), before);
    drag(timeline, {399, 48}, {359, 48});
    drag(timeline, {359, 48}, {479, 48});
    EXPECT_EQ(json(), before);

    drag(timeline, {281, 48}, {481, 48});
    EXPECT_EQ(first()->startFrame, 59);
    EXPECT_EQ(first()->durationFrames, 1);
    EXPECT_EQ(first()->trimStartFrame, 29);
    session.undo();
    EXPECT_EQ(json(), before);
    drag(timeline, {399, 48}, {199, 48});
    EXPECT_EQ(first()->startFrame, 30);
    EXPECT_EQ(first()->durationFrames, 1);
    EXPECT_EQ(first()->trimEndFrame, 29);
}

TEST_F(TimelineInteractionTest, TrimCancellationAndNoOpPreserveSavedDataAndRedo) {
    ASSERT_TRUE(session.trimClip(firstId, heisenberg::TrimEdge::Left, 10).changed());
    const auto trimmed = json();
    session.undo();
    ASSERT_TRUE(session.savePackage(QString::fromStdString(session.editor().projectDir())));
    const auto before = json();
    mouse(timeline, QEvent::MouseButtonPress, {281, 48});
    mouse(timeline, QEvent::MouseMove, {321, 48});
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &escape);
    mouse(timeline, QEvent::MouseButtonRelease, {321, 48});
    drag(timeline, {399, 48}, {359, 120});
    mouse(timeline, QEvent::MouseButtonPress, {399, 48});
    mouse(timeline, QEvent::MouseMove, {359, 48});
    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(&timeline, &deactivate);
    mouse(timeline, QEvent::MouseButtonRelease, {359, 48});
    mouse(timeline, QEvent::MouseButtonPress, {281, 48});
    mouse(timeline, QEvent::MouseMove, {321, 48});
    mouse(timeline, QEvent::MouseButtonRelease, {281, 48});
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
    session.redo();
    EXPECT_EQ(json(), trimmed);
}

TEST_F(TimelineInteractionTest, MovedTrimEdgeSnapsToPlayheadWithShiftAndToggleBypass) {
    timeline.setCurrentFrame(42);
    drag(timeline, {281, 48}, {321, 48}); // Desired frame 40 snaps to 42.
    EXPECT_EQ(first()->startFrame, 42);
    session.undo();
    timeline.setCurrentFrame(42);
    drag(timeline, {399, 48}, {335, 48}); // Desired end 44 snaps to 42.
    EXPECT_EQ(first()->endFrame(), 42);
    session.undo();
    timeline.setCurrentFrame(42);
    drag(timeline, {281, 48}, {321, 48}, Qt::ShiftModifier);
    EXPECT_EQ(first()->startFrame, 40);
    session.undo();
    timeline.setCurrentFrame(42);
    timeline.setSnappingEnabled(false);
    drag(timeline, {399, 48}, {335, 48});
    EXPECT_EQ(first()->endFrame(), 44);
}

TEST_F(TimelineInteractionTest, TailExtensionSnapsAndOverwritesNeighborInOneUndoStep) {
    ASSERT_TRUE(session.trimClip(firstId, heisenberg::TrimEdge::Right, -15).changed());
    ASSERT_TRUE(session.moveClip(secondId, 50).changed());
    timeline.setCurrentFrame(0);
    const auto before = json();
    drag(timeline, {339, 48}, {351, 48}); // End 48 snaps to neighbor start 50.
    EXPECT_EQ(first()->endFrame(), 50);
    EXPECT_EQ(second()->startFrame, 50);
    session.undo();
    EXPECT_EQ(json(), before);
    drag(timeline, {339, 48}, {351, 48}, Qt::ShiftModifier);
    EXPECT_EQ(first()->endFrame(), 48);
    session.undo();
    timeline.setSnappingEnabled(false);
    drag(timeline, {339, 48}, {399, 48});
    EXPECT_EQ(first()->endFrame(), 60);
    EXPECT_EQ(first()->trimEndFrame, 0);
    EXPECT_EQ(second()->startFrame, 60);
    EXPECT_EQ(second()->durationFrames, 20);
    EXPECT_EQ(second()->trimStartFrame, 10);
    const auto after = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), after);
}

TEST_F(TimelineInteractionTest, FractionalSpeedTrimRestoresRoundedSourceBudget) {
    auto project = session.editor().project();
    project.timelines.front().tracks.front().clips.front().speed = 0.6;
    ASSERT_TRUE(reloadProject(project));
    timeline.setSnappingEnabled(false);
    const auto before = json();
    drag(timeline, {281, 48}, {289, 62}); // Two timeline frames consume round(1.2) = one source frame.
    EXPECT_EQ(first()->startFrame, 32);
    EXPECT_EQ(first()->trimStartFrame, 1);
    drag(timeline, {289, 48}, {281, 62});
    EXPECT_EQ(json(), before); // floor(1 / 0.6) would incorrectly allow only one restored frame.
    drag(timeline, {399, 48}, {391, 62});
    EXPECT_EQ(first()->endFrame(), 58);
    EXPECT_EQ(first()->trimEndFrame, 1);
    drag(timeline, {391, 48}, {399, 62});
    EXPECT_EQ(json(), before);
}

TEST_F(TimelineInteractionTest, EdgeTrimUsesZoomAndScrollCoordinates) {
    timeline.setSnappingEnabled(false);
    const auto before = json();
    timeline.zoomIn(); // Five pixels/frame.
    drag(timeline, {311, 48}, {361, 48});
    EXPECT_EQ(first()->startFrame, 40);
    EXPECT_EQ(first()->trimStartFrame, 10);
    session.undo();
    EXPECT_EQ(json(), before);
    timeline.zoomOut();
    ASSERT_TRUE(session.placeMedia(ref, 300).changed());
    QWheelEvent wheel(QPointF(600, 48), timeline.mapToGlobal(QPoint(600, 48)),
        QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&timeline, &wheel); // ScrollOffset = 80.
    drag(timeline, {319, 48}, {279, 48});
    EXPECT_EQ(first()->endFrame(), 50);
    EXPECT_EQ(first()->trimEndFrame, 10);
}

TEST_F(TimelineInteractionTest, SplitShortcutCreatesAdjacentClipsAndStableRedoIds) {
    drag(timeline, target(30), target(30));
    timeline.setCurrentFrame(45);
    const auto before = json();
    QKeyEvent split(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier, "s");
    QApplication::sendEvent(&timeline, &split);
    ASSERT_EQ(session.editor().activeTrack()->clips.size(), 3u);
    const auto& right = session.editor().activeTrack()->clips[1];
    const auto rightId = right.id;
    EXPECT_NE(rightId, firstId.toStdString());
    EXPECT_EQ(rightId.size(), 36u);
    EXPECT_EQ(first()->durationFrames, 15);
    EXPECT_EQ(first()->trimEndFrame, 15);
    EXPECT_EQ(right.startFrame, 45);
    EXPECT_EQ(right.durationFrames, 15);
    EXPECT_EQ(right.trimStartFrame, 15);
    EXPECT_EQ(right.mediaRef, first()->mediaRef);
    EXPECT_EQ(right.speed, first()->speed);
    EXPECT_EQ(session.editor().activeTimeline()->totalFrames(), 120);
    EXPECT_EQ(timeline.selectedClipId(), firstId);
    const auto after = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), after);
    EXPECT_NE(session.editor().activeTimeline()->clipById(rightId), nullptr);
}

TEST_F(TimelineInteractionTest, SplitRequiresSelectedClipAndInteriorFrameAndKeepsRedoOnFailure) {
    int failures = 0;
    QObject::connect(&timeline, &TimelineWidget::editFailed, [&](const QString&) { ++failures; });
    timeline.setCurrentFrame(45);
    timeline.splitSelectedClip(); // No selection.
    EXPECT_EQ(failures, 1);
    drag(timeline, target(30), target(30));
    ASSERT_TRUE(session.splitClip(firstId, 45).changed());
    const auto after = json();
    session.undo();
    ASSERT_TRUE(session.savePackage(QString::fromStdString(session.editor().projectDir())));
    const auto before = json();
    for (const auto frame : {0, 30, 60, 75}) {
        timeline.setCurrentFrame(frame);
        timeline.splitSelectedClip();
        EXPECT_EQ(json(), before);
    }
    timeline.setCurrentFrame(45);
    QKeyEvent repeated(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier, "s", true);
    QApplication::sendEvent(&timeline, &repeated);
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
    EXPECT_EQ(failures, 5);
    session.redo();
    EXPECT_EQ(json(), after);
}

TEST_F(TimelineInteractionTest, SplitCancelsPendingTrimAndCutsOriginalSnapshot) {
    timeline.setCurrentFrame(45);
    mouse(timeline, QEvent::MouseButtonPress, {281, 48});
    mouse(timeline, QEvent::MouseMove, {321, 48});
    const auto before = json();
    QKeyEvent split(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &split);
    mouse(timeline, QEvent::MouseButtonRelease, {321, 48});
    EXPECT_EQ(first()->startFrame, 30);
    EXPECT_EQ(first()->durationFrames, 15);
    EXPECT_EQ(first()->trimStartFrame, 0);
    session.undo();
    EXPECT_EQ(json(), before);
}

TEST_F(TimelineInteractionTest, SplitToolbarActionWorksAndDoesNotStealSearchTyping) {
    MainWindow window(&session);
    window.show();
    QCoreApplication::processEvents();
    auto* widget = window.timelineWidget();
    drag(*widget, target(30), target(30));
    widget->setCurrentFrame(45);
    const auto before = json();
    auto* search = window.findChild<QLineEdit*>();
    ASSERT_NE(search, nullptr);
    search->setFocus();
    QKeyEvent key(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier, "s");
    QApplication::sendEvent(search, &key);
    EXPECT_EQ(search->text(), "s");
    EXPECT_EQ(json(), before);
    auto* split = window.findChild<QAction*>("splitAtPlayhead");
    ASSERT_NE(split, nullptr);
    split->trigger();
    EXPECT_EQ(first()->durationFrames, 15);
    session.undo();
    EXPECT_EQ(json(), before);
    window.hide();
}

TEST_F(TimelineInteractionTest, TrimAndSplitRefreshUnsavedPlaybackAndPersistOnReopen) {
    heisenberg::ui::PlayerController player(&session);
    player.setHardwareDecode(false);
    player.showTimeline(0);
    ASSERT_TRUE(waitFor([&]() { return player.isSeekable(); }));
    timeline.setSnappingEnabled(false);
    drag(timeline, {639, 48}, {579, 48}); // Last clip end 120 -> 105.
    ASSERT_TRUE(waitFor([&]() { return player.frameCount() == 105 && player.currentTime() > 3.45; }));
    EXPECT_NEAR(player.duration(), 3.5, 0.01);
    drag(timeline, target(30), target(30));
    timeline.setCurrentFrame(45);
    timeline.splitSelectedClip();
    ASSERT_TRUE(waitFor([&]() { return player.frameCount() == 105 && std::abs(player.currentTime() - 1.5) < 0.01; }));
    QFile saved(QString::fromStdString(session.editor().projectDir()) + "/project.json");
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    EXPECT_NE(saved.readAll().toStdString(), json());
    saved.close();
    const auto after = json();
    const auto package = QString::fromStdString(session.editor().projectDir());
    ASSERT_TRUE(session.savePackage(package));
    ASSERT_TRUE(session.loadPackage(package));
    EXPECT_EQ(json(), after);
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
class AudioTimelineInteractionTest : public TimelineInteractionTest {
protected:
    void SetUp() override {
        TimelineInteractionTest::SetUp();
        ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_LONG_AUDIO}).isEmpty());
        audioRef = QString::fromStdString(session.editor().manifest().entries().back().id);
        ASSERT_TRUE(session.placeMedia(audioRef, 30).changed());
        ASSERT_EQ(session.editor().activeTimeline()->tracks.size(), 2u);
        audioTrackId = QString::fromStdString(session.editor().activeTimeline()->tracks[1].id);
        audioId = QString::fromStdString(session.editor().activeTimeline()->tracks[1].clips.front().id);
        ASSERT_TRUE(session.savePackage(QString::fromStdString(session.editor().projectDir())));
    }
    const heisenberg::Clip* audio() const {
        return session.editor().activeTimeline()->clipById(audioId.toStdString());
    }
    const heisenberg::Track* audioTrack() const {
        return session.editor().activeTimeline()->trackById(audioTrackId.toStdString());
    }
    QString audioRef, audioId, audioTrackId;
};

TEST_F(AudioTimelineInteractionTest, SecondLaneMovesOnlyOnOwnTrackSnapsTrimsSplitsAndUndoRedo) {
    const auto before = json();
    const auto video = *first();
    drag(timeline, target(30, 92), target(150, 48)); // Cross-track move is not supported.
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
    drag(timeline, target(30, 92), target(118, 92)); // Snap audio start to video end at 120.
    ASSERT_NE(audio(), nullptr);
    EXPECT_EQ(audio()->startFrame, 120);
    EXPECT_EQ(*first(), video);
    const auto moved = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), moved);
    session.undo();
    timeline.setSnappingEnabled(false);
    mouse(timeline, QEvent::MouseButtonPress, {281, 92});
    mouse(timeline, QEvent::MouseMove, {321, 92});
    EXPECT_EQ(json(), before);
    mouse(timeline, QEvent::MouseButtonRelease, {321, 92});
    EXPECT_EQ(audio()->startFrame, 40);
    EXPECT_EQ(audio()->durationFrames, 80);
    EXPECT_EQ(audio()->trimStartFrame, 10);
    EXPECT_EQ(*first(), video);
    const auto trimmed = json();
    drag(timeline, {639, 92}, {599, 48}); // Wrong-lane trim cancels too.
    EXPECT_EQ(json(), trimmed);
    timeline.setCurrentFrame(70);
    timeline.splitSelectedClip();
    ASSERT_EQ(audioTrack()->clips.size(), 2u);
    EXPECT_EQ(audio()->durationFrames, 30);
    const auto rightId = audioTrack()->clips.back().id;
    EXPECT_EQ(audioTrack()->clips.back().startFrame, 70);
    EXPECT_EQ(audioTrack()->clips.back().trimStartFrame, 40);
    const auto split = json();
    session.undo();
    EXPECT_EQ(json(), trimmed);
    session.redo();
    EXPECT_EQ(json(), split);
    EXPECT_NE(session.editor().activeTimeline()->clipById(rightId), nullptr);
    const QString package = QString::fromStdString(session.editor().projectDir());
    ASSERT_TRUE(session.savePackage(package));
    ASSERT_TRUE(session.loadPackage(package));
    EXPECT_EQ(json(), split);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(AudioTimelineInteractionTest, SecondLaneGeometryUsesActualTrackHeight) {
    auto project = session.editor().project();
    project.timelines.front().tracks[0].displayHeight = 80.0;
    project.timelines.front().tracks[1].displayHeight = 60.0;
    ASSERT_TRUE(reloadProject(project));
    timeline.setSnappingEnabled(false);
    const auto before = json();
    drag(timeline, target(30, 130), target(150, 92)); // y92 is now still the video lane.
    EXPECT_EQ(json(), before);
    drag(timeline, target(30, 130), target(150, 130));
    EXPECT_EQ(timeline.selectedClipId(), audioId);
    ASSERT_NE(audio(), nullptr);
    EXPECT_EQ(audio()->startFrame, 150);
    EXPECT_EQ(first()->startFrame, 30);
    session.undo();
    EXPECT_EQ(json(), before);
    mouse(timeline, QEvent::MouseButtonPress, {140, 130});
    mouse(timeline, QEvent::MouseButtonRelease, {140, 130});
    EXPECT_TRUE(audioTrack()->muted);
    EXPECT_FALSE(session.editor().activeTimeline()->tracks[0].muted);
}

TEST_F(AudioTimelineInteractionTest, AudioSettingsCommitOnceCancelNoOpMuteDeleteAndPersistence) {
    drag(timeline, target(30, 92), target(30, 92));
    const auto before = json();
    int changes = 0;
    QObject::connect(&session, &heisenberg::ui::EditorSession::timelineChanged, [&](qint64) { ++changes; });
    // Context menu selects the second-lane clip and exposes the audio dialog action.
    bool menuSeen = false;
    QTimer::singleShot(0, [&]() {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu) {
            menuSeen = menu->findChild<QAction*>("clipAudioSettings") != nullptr;
            menu->close();
        }
    });
    const QPoint contextPoint = target(30, 92).toPoint();
    QContextMenuEvent context(QContextMenuEvent::Mouse, contextPoint, timeline.mapToGlobal(contextPoint));
    QApplication::sendEvent(&timeline, &context);
    EXPECT_TRUE(menuSeen);
    EXPECT_EQ(timeline.selectedClipId(), audioId);
    bool draftUnchanged = false, dialogSeen = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto* gain = dialog->findChild<QDoubleSpinBox*>("clipAudioGain");
        auto* fadeIn = dialog->findChild<QDoubleSpinBox*>("clipAudioFadeIn");
        auto* fadeOut = dialog->findChild<QDoubleSpinBox*>("clipAudioFadeOut");
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (!gain || !fadeIn || !fadeOut || !buttons) { dialog->reject(); return; }
        dialogSeen = true;
        EXPECT_DOUBLE_EQ(gain->minimum(), 0.0);
        EXPECT_DOUBLE_EQ(gain->maximum(), 200.0);
        gain->setValue(150.0);
        fadeIn->setValue(2.0);
        fadeOut->setValue(2.0);
        EXPECT_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        fadeIn->setValue(0.5);
        fadeOut->setValue(0.25);
        draftUnchanged = json() == before && changes == 0;
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    timeline.editSelectedClipAudio();
    ASSERT_TRUE(dialogSeen);
    EXPECT_TRUE(draftUnchanged);
    EXPECT_EQ(changes, 1);
    EXPECT_DOUBLE_EQ(audio()->volume, 1.5);
    EXPECT_EQ(audio()->fadeInFrames, 15);
    EXPECT_EQ(audio()->fadeOutFrames, 8);
    const auto configured = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), configured);
    const int configuredChanges = changes;
    QTimer::singleShot(0, [&]() {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            dialog->findChild<QDoubleSpinBox*>("clipAudioGain")->setValue(200.0);
            dialog->reject();
        }
    });
    timeline.editSelectedClipAudio();
    EXPECT_EQ(json(), configured);
    EXPECT_EQ(changes, configuredChanges);
    QTimer::singleShot(0, [&]() {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    timeline.editSelectedClipAudio(); // Unchanged confirmation does not create history.
    EXPECT_EQ(json(), configured);
    EXPECT_EQ(changes, configuredChanges);
    mouse(timeline, QEvent::MouseButtonPress, {140, 92});
    mouse(timeline, QEvent::MouseButtonRelease, {140, 92});
    EXPECT_TRUE(audioTrack()->muted);
    const auto muted = json();
    session.undo();
    EXPECT_EQ(json(), configured);
    session.redo();
    EXPECT_EQ(json(), muted);
    QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &remove);
    EXPECT_EQ(audio(), nullptr);
    EXPECT_TRUE(audioTrack()->clips.empty());
    EXPECT_EQ(session.editor().activeTimeline()->tracks[0].clips.size(), 2u);
    const auto deleted = json();
    session.undo();
    EXPECT_EQ(json(), muted);
    session.redo();
    EXPECT_EQ(json(), deleted);
    session.undo();
    const QString package = QString::fromStdString(session.editor().projectDir());
    ASSERT_TRUE(session.savePackage(package));
    ASSERT_TRUE(session.loadPackage(package));
    EXPECT_EQ(json(), muted);
    EXPECT_DOUBLE_EQ(audio()->volume, 1.5);
    EXPECT_TRUE(audioTrack()->muted);
    EXPECT_FALSE(session.isDirty());
    int noOpChanges = 0;
    QObject::connect(&session, &heisenberg::ui::EditorSession::timelineChanged, [&](qint64) { ++noOpChanges; });
    EXPECT_EQ(session.setTrackMuted(audioTrackId, true).status, heisenberg::EditStatus::NoOp);
    EXPECT_EQ(session.setClipAudio(audioId, 1.5, 15, 8).status, heisenberg::EditStatus::NoOp);
    EXPECT_EQ(session.removeClips({"missing"}).status, heisenberg::EditStatus::NoOp);
    EXPECT_EQ(session.setClipAudio(audioId, 2.01, 0, 0).status, heisenberg::EditStatus::Invalid);
    EXPECT_EQ(noOpChanges, 0);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(AudioTimelineInteractionTest, BackspaceDeletesTimelineSelectionButSearchKeysStayLocal) {
    MainWindow window(&session);
    window.show();
    QCoreApplication::processEvents();
    auto* widget = window.timelineWidget();
    drag(*widget, target(30, 92), target(30, 92));
    const auto before = json();
    auto* search = window.findChild<QLineEdit*>();
    ASSERT_NE(search, nullptr);
    search->setText("audio");
    search->setFocus();
    QKeyEvent backspace(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QApplication::sendEvent(search, &backspace);
    EXPECT_EQ(search->text(), "audi");
    QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(search, &remove);
    EXPECT_EQ(json(), before);
    widget->setFocus();
    QApplication::sendEvent(widget, &backspace);
    EXPECT_EQ(audio(), nullptr);
    session.undo();
    EXPECT_EQ(json(), before);
    window.hide();
}
} // namespace
