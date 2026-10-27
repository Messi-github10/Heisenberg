#include <Backend/EditorSession.hpp>
#include <Backend/PlayerController.hpp>
#include <MainWidget/MainWindow.hpp>
#include <MainWidget/MediaPoolWidget.hpp>
#include <MainWidget/TimelineWidget.hpp>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QThread>
#include <gtest/gtest.h>
#include <functional>

using heisenberg::ui::EditorSession;
using heisenberg::ui::PlayerController;
namespace {
bool waitFor(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return condition();
}
void wireMarks(MainWindow& window, PlayerController& player) {
    QObject::connect(&player, &PlayerController::previewModeChanged, &window, &MainWindow::setSourcePreview);
    QObject::connect(&player, &PlayerController::sourceMarkingChanged, &window, [&window, &player] {
        window.setSourceMarking(player.currentSourceMediaRef(), player.sourceMarkingAvailable());
    });
    QObject::connect(&window, &MainWindow::markSourceInRequested, &player, &PlayerController::markSourceIn);
    QObject::connect(&window, &MainWindow::markSourceOutRequested, &player, &PlayerController::markSourceOut);
    QObject::connect(&window, &MainWindow::clearSourceRangeRequested, &player, &PlayerController::clearSourceRange);
}
}

TEST(SourceEditWorkflowTest, PreviewMarkActionsRestoreRangeAndDisableOnTimelineReturn) {
    EditorSession session;
    PlayerController player(&session);
    player.setHardwareDecode(false);
    MainWindow window(&session);
    wireMarks(window, player);
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    const auto ref = QString::fromStdString(session.editor().manifest().entries().front().id);
    const bool dirtyBeforeMarks = session.isDirty();
    const auto revisionBeforeMarks = session.editor().revision();
    auto* in = window.findChild<QAction*>("markSourceIn");
    auto* out = window.findChild<QAction*>("markSourceOut");
    auto* clear = window.findChild<QAction*>("clearSourceRange");
    ASSERT_NE(in, nullptr); ASSERT_NE(out, nullptr); ASSERT_NE(clear, nullptr);
    EXPECT_FALSE(in->isEnabled());
    player.previewAsset(ref);
    ASSERT_TRUE(waitFor([&] { return player.sourceMarkingAvailable() && in->isEnabled(); }));
    player.seek(.25);
    ASSERT_TRUE(waitFor([&] { return player.currentTime() >= .24; }));
    in->trigger();
    ASSERT_TRUE(session.sourceRange(ref).inSeconds.has_value());
    EXPECT_NEAR(*session.sourceRange(ref).inSeconds, .25, .001);
    player.seek(.5);
    ASSERT_TRUE(waitFor([&] { return player.currentTime() >= .49; }));
    out->trigger();
    ASSERT_TRUE(session.sourceRange(ref).outSeconds.has_value());
    EXPECT_NEAR(*session.sourceRange(ref).outSeconds, 13.0 / 24, .001);
    EXPECT_TRUE(clear->isEnabled());
    EXPECT_TRUE(window.findChild<QLabel*>("sourceRangeLabel")->text().contains("0.250"));
    EXPECT_EQ(session.isDirty(), dirtyBeforeMarks);
    EXPECT_EQ(session.editor().revision(), revisionBeforeMarks);
    player.showTimeline();
    EXPECT_FALSE(in->isEnabled());
    const auto range = session.sourceRange(ref);
    in->trigger();
    EXPECT_EQ(session.sourceRange(ref).inSeconds, range.inSeconds);
    player.previewAsset(ref);
    ASSERT_TRUE(waitFor([&] { return in->isEnabled(); }));
    clear->trigger();
    EXPECT_FALSE(session.sourceRange(ref).inSeconds.has_value());
    EXPECT_FALSE(session.sourceRange(ref).outSeconds.has_value());
    EXPECT_FALSE(clear->isEnabled());
}

TEST(SourceEditWorkflowTest, OpeningMarkedSourceKeepsTimelineInsertionPlayhead) {
    EditorSession session;
    PlayerController player(&session);
    player.setHardwareDecode(false);
    MainWindow window(&session);
    wireMarks(window, player);
    QObject::connect(&player, &PlayerController::currentTimeChanged, &window, [&] {
        window.setCurrentTime(player.currentTime());
    });
    QObject::connect(&player, &PlayerController::durationChanged, &window, [&] {
        window.setDuration(player.duration());
    });
    QObject::connect(&player, &PlayerController::frameCountChanged, &window, [&] {
        window.setFrameCount(player.frameCount());
    });
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    const auto ref = QString::fromStdString(session.editor().manifest().entries().front().id);
    ASSERT_TRUE(session.placeMedia(ref, 60).changed());
    ASSERT_TRUE(waitFor([&] { return player.isSeekable() && player.currentTime() >= 1.99; }));
    ASSERT_EQ(window.timelineWidget()->currentFrame(), 60);
    player.previewAsset(ref);
    ASSERT_TRUE(waitFor([&] { return player.sourceMarkingAvailable(); }));
    EXPECT_EQ(window.timelineWidget()->currentFrame(), 60);
    player.seek(.5);
    ASSERT_TRUE(waitFor([&] { return player.currentTime() >= .49; }));
    window.findChild<QAction*>("markSourceIn")->trigger();
    EXPECT_EQ(window.timelineWidget()->currentFrame(), 60);
}

TEST(SourceEditWorkflowTest, PoolInsertAndOverwriteUseMarkedRangeAndToolbarSelectsDropMode) {
    EditorSession session;
    MainWindow window(&session);
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    const auto ref = QString::fromStdString(session.editor().manifest().entries().front().id);
    ASSERT_TRUE(session.placeMedia(ref, 0).changed());
    const auto originalId = session.editor().activeTrack()->clips.front().id;
    ASSERT_TRUE(session.setSourceIn(ref, .25));
    ASSERT_TRUE(session.setSourceOut(ref, .75));
    auto* list = window.findChild<MediaListWidget*>();
    ASSERT_NE(list, nullptr); ASSERT_EQ(list->count(), 1);
    list->setCurrentRow(0);
    window.timelineWidget()->setCurrentFrame(10);
    auto* mode = window.findChild<QComboBox*>("mediaPlacementMode");
    ASSERT_NE(mode, nullptr);
    EXPECT_FALSE(window.timelineWidget()->insertMode());
    mode->setCurrentIndex(1);
    EXPECT_TRUE(window.timelineWidget()->insertMode());
    auto* overwriteMode = window.findChild<QAction*>("overwritePlacementMode");
    ASSERT_NE(overwriteMode, nullptr);
    overwriteMode->trigger();
    EXPECT_EQ(mode->currentIndex(), 0);
    EXPECT_FALSE(window.timelineWidget()->insertMode());
    auto* insert = window.findChild<QPushButton*>("insertSelectedMedia");
    auto* overwrite = window.findChild<QPushButton*>("addSelectedMedia");
    ASSERT_NE(insert, nullptr); ASSERT_NE(overwrite, nullptr);
    ASSERT_TRUE(insert->isEnabled());
    insert->click();
    const auto& clips = session.editor().activeTrack()->clips;
    ASSERT_EQ(clips.size(), 3u);
    EXPECT_EQ(clips[0].id, originalId);
    EXPECT_EQ(clips[0].durationFrames, 10);
    EXPECT_EQ(clips[1].startFrame, 10);
    EXPECT_EQ(clips[1].trimStartFrame, 8);
    EXPECT_EQ(clips[1].durationFrames, 15);
    EXPECT_EQ(clips[2].startFrame, 25);
    session.undo();
    ASSERT_EQ(session.editor().activeTrack()->clips.size(), 1u);
    overwrite->click();
    EXPECT_EQ(session.editor().activeTrack()->endFrame(), 30);
    ASSERT_EQ(session.editor().activeTrack()->clips.size(), 3u);
    EXPECT_EQ(session.editor().activeTrack()->clips[1].durationFrames, 15);
}

TEST(SourceEditWorkflowTest, MarkShortcutsStayInsidePreviewAndDoNotConsumeSearchTyping) {
    EditorSession session;
    MainWindow window(&session);
    window.show();
    window.setSourcePreview(true, "source");
    window.setSourceMarking("registered-source", true);
    int inCount = 0, outCount = 0;
    QObject::connect(&window, &MainWindow::markSourceInRequested, [&] { ++inCount; });
    QObject::connect(&window, &MainWindow::markSourceOutRequested, [&] { ++outCount; });
    auto* preview = window.findChild<QWidget*>("previewPanel");
    auto* search = window.findChild<QLineEdit*>("mediaSearch");
    ASSERT_NE(preview, nullptr); ASSERT_NE(search, nullptr);
    preview->setFocus();
    QCoreApplication::processEvents();
    for (int key : {Qt::Key_I, Qt::Key_O}) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, key == Qt::Key_I ? "i" : "o");
        QApplication::sendEvent(preview, &press);
    }
    EXPECT_EQ(inCount, 1); EXPECT_EQ(outCount, 1);
    search->setFocus();
    for (int key : {Qt::Key_I, Qt::Key_O}) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, key == Qt::Key_I ? "i" : "o");
        QApplication::sendEvent(search, &press);
    }
    EXPECT_EQ(search->text(), "io");
    EXPECT_EQ(inCount, 1); EXPECT_EQ(outCount, 1);
}
