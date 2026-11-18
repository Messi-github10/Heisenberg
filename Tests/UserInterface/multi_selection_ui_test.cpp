#include <Backend/EditorSession.hpp>
#include <Compositing/CompositionBuilder.hpp>
#include <MainWidget/TimelineWidget.hpp>
#include <MainWidget/MainWindow.hpp>
#include <Models/ProjectCodec.hpp>

#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScrollBar>
#include <QStringList>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <algorithm>
#include <utility>

namespace {
using heisenberg::ClipType;
using heisenberg::ui::EditorSession;

// Match the event helpers in timeline_interaction_test.cpp. No decoder or timer
// is needed: the package uses the offline-media fixture from multitrack_ui_test.
void mouse(TimelineWidget& widget, QEvent::Type type, QPointF point,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, point, widget.mapToGlobal(point.toPoint()),
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
    QApplication::sendEvent(&widget, &event);
}
void click(TimelineWidget& widget, QPointF point,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    mouse(widget, QEvent::MouseButtonPress, point, modifiers);
    mouse(widget, QEvent::MouseButtonRelease, point, modifiers);
}
void drag(TimelineWidget& widget, QPointF from, QPointF to,
          Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    mouse(widget, QEvent::MouseButtonPress, from, modifiers);
    mouse(widget, QEvent::MouseMove, to, modifiers);
    mouse(widget, QEvent::MouseButtonRelease, to, modifiers);
}
void key(TimelineWidget& widget, int code,
         Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers);
    QApplication::sendEvent(&widget, &event);
}
QStringList sorted(QStringList ids) {
    ids.sort();
    return ids;
}

class MultiSelectionUiTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        package = directory.filePath("selection.berg");
        ASSERT_TRUE(session.savePackage(package));
        auto project = session.editor().project();
        heisenberg::Timeline active;
        active.id = "selection-timeline";
        active.name = "Multi-selection";
        // Storage is interleaved, but the UI groups video rows before audio.
        for (int i = 0; i < 4; ++i) {
            heisenberg::Track video;
            video.id = "v" + std::to_string(i);
            video.displayHeight = 44.0;
            if (i == 0) {
                video.clips = {makeClip("first", 30), makeClip("second", 90)};
            } else if (i == 1) {
                video.clips = {makeClip("middle", 50)};
            } else if (i == 2) {
                video.clips = {makeClip("lower", 30)};
            }
            active.tracks.push_back(video);
            if (i < 2) {
                heisenberg::Track audio;
                audio.id = "a" + std::to_string(i);
                audio.type = ClipType::Audio;
                audio.displayHeight = 44.0;
                audio.clips = {makeClip(i == 0 ? "audio" : "audio2", i == 0 ? 40 : 100,
                                        ClipType::Audio)};
                active.tracks.push_back(audio);
            }
        }
        project.timelines = {active};
        project.activeTimelineId = active.id;
        project.openTimelineIds = std::vector<std::string>{active.id};
        project.viewStates.reset();
        auto manifest = session.editor().manifest();
        for (const auto type : {ClipType::Video, ClipType::Audio}) {
            heisenberg::MediaManifestEntry entry;
            entry.id = type == ClipType::Video ? "video" : "audio";
            entry.name = entry.id;
            entry.type = type;
            entry.duration = 10.0;
            entry.source.path = directory.path().toStdString() + "/offline-" + entry.id;
            ASSERT_TRUE(manifest.add(entry));
        }
        for (const auto& document : {
                 std::pair{QString("project.json"), heisenberg::ProjectCodec::encode(project)},
                 std::pair{QString("media.json"), manifest.toJson()}}) {
            QFile file(package + '/' + document.first);
            ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            ASSERT_EQ(file.write(document.second.data(), static_cast<qint64>(document.second.size())),
                      static_cast<qint64>(document.second.size()));
        }
        QString error;
        ASSERT_TRUE(session.loadPackage(package, &error)) << error.toStdString();
        timeline.setSession(&session);
        timeline.setSnappingEnabled(false);
        timeline.resize(1000, 360);
        timeline.show();
        QCoreApplication::processEvents();
    }
    static heisenberg::Clip makeClip(const std::string& id, qint64 start,
                                    ClipType type = ClipType::Video) {
        heisenberg::Clip clip;
        clip.id = id;
        clip.mediaRef = type == ClipType::Audio ? "audio" : "video";
        clip.mediaType = type;
        clip.sourceClipType = type;
        clip.startFrame = start;
        clip.durationFrames = 30;
        clip.trimStartFrame = 5;
        clip.trimEndFrame = 7;
        clip.volume = .65;
        clip.fadeInFrames = 3;
        clip.fadeOutFrames = 4;
        clip.opacity = .75;
        clip.transform.rotation = 12;
        clip.effects = heisenberg::JsonValue::array({{
            {"id", "custom"}, {"type", "test.opaque"}, {"payload", {{"keep", true}}}
        }});
        return clip;
    }
    std::string json() const { return heisenberg::ProjectCodec::encode(session.editor().project()); }
    const heisenberg::Timeline* active() const { return session.editor().activeTimeline(); }
    const heisenberg::Clip* clip(const std::string& id) const { return active()->clipById(id); }
    std::string owner(const std::string& id) const {
        const auto* track = active()->trackForClip(id);
        return track ? track->id : std::string{};
    }
    // Header160, ruler30, new-track strip20, rows44, zoom4px/frame.
    static QPointF point(qint64 frame, int row = 0) { return {160.0 + frame * 4.0, 72.0 + row * 44.0}; }
    void selectPair() {
        click(timeline, point(40));
        click(timeline, point(100), Qt::ControlModifier);
        expectSelection({"first", "second"});
    }
    void expectSelection(const QStringList& ids) const {
        const auto actual = timeline.selectedClipIds();
        EXPECT_EQ(sorted(actual), sorted(ids));
        auto unique = sorted(actual);
        unique.removeDuplicates();
        EXPECT_EQ(actual.size(), unique.size());
        if (actual.isEmpty()) EXPECT_TRUE(timeline.selectedClipId().isEmpty());
        else EXPECT_TRUE(actual.contains(timeline.selectedClipId()));
    }
    const heisenberg::CompositionClip* planClip(const heisenberg::CompositionPlan& plan,
                                               const std::string& id) const {
        for (const auto& track : plan.tracks)
            for (const auto& value : track.clips)
                if (value.id == id) return &value;
        return nullptr;
    }
    EditorSession session;
    TimelineWidget timeline;
    QTemporaryDir directory;
    QString package;
};

class ModifierSelectionUiTest : public MultiSelectionUiTest,
                                public ::testing::WithParamInterface<Qt::KeyboardModifier> {};

TEST_P(ModifierSelectionUiTest, ToggleOnPressNeverStartsMoveOrScrub) {
    const auto before = json();
    int scrubs = 0, edits = 0, notifications = 0;
    QStringList notified;
    QObject::connect(&timeline, &TimelineWidget::scrubStarted, [&] { ++scrubs; });
    QObject::connect(&timeline, &TimelineWidget::editRequested, [&] { ++edits; });
    QObject::connect(&timeline, &TimelineWidget::selectedClipsChanged,
                     [&](const QStringList& ids) { ++notifications; notified = ids; });
    click(timeline, point(40));
    mouse(timeline, QEvent::MouseButtonPress, point(100), GetParam());
    expectSelection({"first", "second"});
    EXPECT_EQ(sorted(notified), sorted(timeline.selectedClipIds()));
    mouse(timeline, QEvent::MouseMove, point(160, 1), GetParam());
    mouse(timeline, QEvent::MouseButtonRelease, point(160, 1), GetParam());
    expectSelection({"first", "second"});
    mouse(timeline, QEvent::MouseButtonPress, point(40), GetParam());
    expectSelection({"second"});
    mouse(timeline, QEvent::MouseButtonRelease, point(40), GetParam());
    click(timeline, point(100), GetParam());
    expectSelection({});
    EXPECT_EQ(sorted(notified), QStringList{});
    EXPECT_GE(notifications, 4);
    EXPECT_EQ(scrubs, 0);
    EXPECT_EQ(edits, 0);
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(MultiSelectionUiTest, SelectedMemberPressPreservesGroupAndClickReleaseCollapses) {
    selectPair();
    const auto before = json();
    mouse(timeline, QEvent::MouseButtonPress, point(40));
    expectSelection({"first", "second"});
    EXPECT_EQ(timeline.selectedClipId(), "first");
    mouse(timeline, QEvent::MouseMove, point(40) + QPointF(2, 0));
    expectSelection({"first", "second"});
    mouse(timeline, QEvent::MouseButtonRelease, point(40) + QPointF(2, 0));
    expectSelection({"first"});
    click(timeline, point(60, 1)); // An unselected clip replaces membership.
    expectSelection({"middle"});
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(MultiSelectionUiTest, MarqueeIntersectsPaintedRectsAndCommitsOnlyOnRelease) {
    click(timeline, point(100, 5));
    timeline.setCurrentFrame(20);
    const auto before = json();
    int scrubs = 0;
    QObject::connect(&timeline, &TimelineWidget::scrubStarted, [&] { ++scrubs; });
    mouse(timeline, QEvent::MouseButtonPress, {260, 52});
    const auto baseline = timeline.grab().toImage();
    mouse(timeline, QEvent::MouseMove, {290, 165}); // Only the left 10px of first/lower.
    expectSelection({"audio2"});
    EXPECT_NE(timeline.grab().toImage(), baseline);
    EXPECT_EQ(timeline.currentFrame(), 20);
    mouse(timeline, QEvent::MouseButtonRelease, {290, 165});
    expectSelection({"first", "lower"});
    EXPECT_EQ(scrubs, 0);
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());

    // This rectangle occupies lane padding, above the painted clip top at y53.
    drag(timeline, {260, 51}, {420, 52});
    expectSelection({});
    // Reversed corners must normalize correctly; this press is beyond lower's right edge.
    drag(timeline, {410, 175}, {260, 52});
    expectSelection({"first", "middle", "lower"});
}

TEST_P(ModifierSelectionUiTest, MarqueeAddsToBaselineWithoutTogglingExistingHits) {
    click(timeline, point(40));
    drag(timeline, {260, 52}, {410, 175}, GetParam());
    expectSelection({"first", "middle", "lower"});
    drag(timeline, {260, 52}, {290, 80}, GetParam());
    expectSelection({"first", "middle", "lower"});
    EXPECT_FALSE(session.isDirty());
}

TEST_F(MultiSelectionUiTest, EscapeRestoresMarqueeBaselineAndIdleEscapeClears) {
    selectPair();
    const auto before = json();
    int scrubs = 0;
    QObject::connect(&timeline, &TimelineWidget::scrubStarted, [&] { ++scrubs; });
    mouse(timeline, QEvent::MouseButtonPress, {260, 52});
    const auto baseline = timeline.grab().toImage();
    mouse(timeline, QEvent::MouseMove, {410, 175});
    key(timeline, Qt::Key_Escape);
    expectSelection({"first", "second"});
    EXPECT_EQ(timeline.grab().toImage(), baseline);
    mouse(timeline, QEvent::MouseButtonRelease, {410, 175});
    expectSelection({"first", "second"});
    EXPECT_EQ(scrubs, 0);
    key(timeline, Qt::Key_Escape);
    expectSelection({});
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(MultiSelectionUiTest, BackgroundClickDefersClearAndSeekButRulerScrubsImmediately) {
    selectPair();
    timeline.setCurrentFrame(10);
    int started = 0, finished = 0;
    QObject::connect(&timeline, &TimelineWidget::scrubStarted, [&] { ++started; });
    QObject::connect(&timeline, &TimelineWidget::scrubFinished, [&](qint64) { ++finished; });
    mouse(timeline, QEvent::MouseButtonPress, point(75));
    expectSelection({"first", "second"});
    EXPECT_EQ(timeline.currentFrame(), 10);
    EXPECT_EQ(started, 0);
    mouse(timeline, QEvent::MouseButtonRelease, point(75));
    expectSelection({});
    EXPECT_EQ(timeline.currentFrame(), 75);
    EXPECT_EQ(started, 1);
    EXPECT_EQ(finished, 1);
    selectPair();
    mouse(timeline, QEvent::MouseButtonPress, {320, 15});
    EXPECT_EQ(timeline.currentFrame(), 40);
    EXPECT_EQ(started, 2);
    expectSelection({"first", "second"});
    mouse(timeline, QEvent::MouseMove, {400, 15});
    EXPECT_EQ(timeline.currentFrame(), 60);
    mouse(timeline, QEvent::MouseButtonRelease, {400, 15});
    EXPECT_EQ(finished, 2);
    expectSelection({"first", "second"});
}

TEST_F(MultiSelectionUiTest, BackgroundBelowLastLaneClearsAndSeeksOnlyOnRelease) {
    selectPair();
    timeline.setCurrentFrame(10);
    const auto before = json();
    int started = 0, finished = 0;
    QObject::connect(&timeline, &TimelineWidget::scrubStarted, [&] { ++started; });
    QObject::connect(&timeline, &TimelineWidget::scrubFinished, [&](qint64) { ++finished; });
    // Six rows end at y314; y345 is content background, below every lane.
    mouse(timeline, QEvent::MouseButtonPress, {460, 345});
    expectSelection({"first", "second"});
    EXPECT_EQ(timeline.currentFrame(), 10);
    EXPECT_EQ(started, 0);
    mouse(timeline, QEvent::MouseButtonRelease, {460, 345});
    expectSelection({});
    EXPECT_EQ(timeline.currentFrame(), 75);
    EXPECT_EQ(started, 1);
    EXPECT_EQ(finished, 1);
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(MultiSelectionUiTest, TrackDeletionPrunesSelectionAndFallsBackToSurvivingPrimary) {
    click(timeline, point(40));
    click(timeline, point(40, 2), Qt::ControlModifier);
    click(timeline, point(60, 1), Qt::ControlModifier);
    ASSERT_EQ(timeline.selectedClipId(), "middle");
    QStringList primaryChanges;
    std::vector<QStringList> membershipChanges;
    QObject::connect(&timeline, &TimelineWidget::selectedClipChanged,
                     [&](const QString& id) { primaryChanges.push_back(id); });
    QObject::connect(&timeline, &TimelineWidget::selectedClipsChanged,
                     [&](const QStringList& ids) { membershipChanges.push_back(sorted(ids)); });
    ASSERT_TRUE(session.removeTrack("v1").changed());
    expectSelection({"first", "lower"});
    EXPECT_EQ(timeline.selectedClipId(), "first");
    ASSERT_TRUE(session.removeTrack("v0").changed());
    expectSelection({"lower"});
    EXPECT_EQ(timeline.selectedClipId(), "lower");
    ASSERT_TRUE(session.removeTrack("v2").changed());
    expectSelection({});
    EXPECT_EQ(primaryChanges, (QStringList{"first", "lower", ""}));
    ASSERT_EQ(membershipChanges.size(), 3u);
    EXPECT_EQ(membershipChanges[0], (QStringList{"first", "lower"}));
    EXPECT_EQ(membershipChanges[1], (QStringList{"lower"}));
    EXPECT_TRUE(membershipChanges[2].isEmpty());
    session.undo();
    ASSERT_NE(clip("lower"), nullptr);
    expectSelection({}); // Undo restores model content without resurrecting UI membership.
}

TEST_F(MultiSelectionUiTest, RemovingNonPrimaryMembersKeepsPrimaryAndPrunesClipIds) {
    selectPair();
    click(timeline, point(40, 2), Qt::ControlModifier);
    ASSERT_EQ(timeline.selectedClipId(), "lower");
    int primaryChanges = 0;
    QObject::connect(&timeline, &TimelineWidget::selectedClipChanged, [&](const QString&) { ++primaryChanges; });
    ASSERT_TRUE(session.removeClips({"first"}).changed());
    expectSelection({"second", "lower"});
    EXPECT_EQ(timeline.selectedClipId(), "lower");
    ASSERT_TRUE(session.removeTrack("v0").changed());
    expectSelection({"lower"});
    EXPECT_EQ(timeline.selectedClipId(), "lower");
    EXPECT_EQ(primaryChanges, 0);
}

TEST_F(MultiSelectionUiTest, ProjectReplacementClearsSelectionAndCancelsOldPreview) {
    selectPair();
    mouse(timeline, QEvent::MouseButtonPress, point(40));
    mouse(timeline, QEvent::MouseMove, point(60));
    const auto before = json();
    QString error;
    ASSERT_TRUE(session.loadPackage(package, &error)) << error.toStdString();
    expectSelection({}); // Even matching clip IDs belong to the newly opened project.
    mouse(timeline, QEvent::MouseButtonRelease, point(60));
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
    selectPair();
    EditorSession empty;
    const auto emptyPackage = directory.filePath("empty.berg");
    ASSERT_TRUE(empty.savePackage(emptyPackage, &error)) << error.toStdString();
    ASSERT_TRUE(session.loadPackage(emptyPackage, &error)) << error.toStdString();
    expectSelection({});
    const auto* replacement = session.editor().activeTimeline();
    EXPECT_TRUE(!replacement || !replacement->clipById("first"));
    EXPECT_FALSE(session.isDirty());
}

TEST_F(MultiSelectionUiTest, MainWindowActionsRequireSingletonAndSearchKeepsItsOwnKeys) {
    MainWindow window(&session);
    window.resize(1400, 1000);
    window.show();
    window.activateWindow();
    QCoreApplication::processEvents();
    auto* widget = window.timelineWidget();
    auto* split = window.findChild<QAction*>("splitAtPlayhead");
    auto* ripple = window.findChild<QAction*>("rippleDeleteClip");
    auto* search = window.findChild<QLineEdit*>("mediaSearch");
    ASSERT_NE(split, nullptr);
    ASSERT_NE(ripple, nullptr);
    ASSERT_NE(search, nullptr);
    EXPECT_FALSE(split->isEnabled());
    EXPECT_FALSE(ripple->isEnabled());
    click(*widget, point(40));
    EXPECT_TRUE(split->isEnabled());
    EXPECT_TRUE(ripple->isEnabled());
    click(*widget, point(100), Qt::ControlModifier);
    EXPECT_FALSE(split->isEnabled());
    EXPECT_FALSE(ripple->isEnabled());
    const auto before = json();
    split->trigger();
    ripple->trigger();
    EXPECT_EQ(json(), before);
    click(*widget, point(100), Qt::ControlModifier);
    EXPECT_TRUE(split->isEnabled());
    EXPECT_TRUE(ripple->isEnabled());

    search->setFocus();
    QCoreApplication::processEvents();
    ASSERT_EQ(QApplication::focusWidget(), search);
    QKeyEvent letter(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier, "s");
    QApplication::sendEvent(search, &letter);
    EXPECT_EQ(search->text(), "s");
    QKeyEvent all(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
    QApplication::sendEvent(search, &all);
    EXPECT_EQ(search->selectedText(), "s");
    QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(search, &remove);
    EXPECT_TRUE(search->text().isEmpty());
    EXPECT_EQ(widget->selectedClipIds(), (QStringList{"first"}));
    EXPECT_EQ(json(), before);
    EXPECT_TRUE(split->isEnabled());
    EXPECT_TRUE(ripple->isEnabled());
    widget->setFocus();
    key(*widget, Qt::Key_Escape);
    EXPECT_FALSE(split->isEnabled());
    EXPECT_FALSE(ripple->isEnabled());
    window.hide();
}

TEST_F(MultiSelectionUiTest, FocusedTimelineOverridesEscapeWhileSelectionOrPreviewExists) {
    MainWindow window(&session);
    window.resize(1400, 1000);
    window.show();
    window.activateWindow();
    QCoreApplication::processEvents();
    auto* widget = window.timelineWidget();
    int returns = 0;
    QObject::connect(&window, &MainWindow::timelinePreviewRequested, [&] { ++returns; });
    auto overridesEscape = [&] {
        QKeyEvent event(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
        event.ignore();
        QApplication::sendEvent(widget, &event);
        return event.isAccepted();
    };
    click(*widget, point(40));
    click(*widget, point(100), Qt::ControlModifier);
    ASSERT_TRUE(widget->hasFocus());
    EXPECT_TRUE(overridesEscape());
    mouse(*widget, QEvent::MouseButtonPress, point(40));
    mouse(*widget, QEvent::MouseMove, point(60));
    EXPECT_TRUE(overridesEscape());
    const auto before = json();
    key(*widget, Qt::Key_Escape);
    mouse(*widget, QEvent::MouseButtonRelease, point(60));
    EXPECT_EQ(sorted(widget->selectedClipIds()), (QStringList{"first", "second"}));
    EXPECT_EQ(json(), before);
    EXPECT_EQ(returns, 0);
    EXPECT_TRUE(overridesEscape());
    key(*widget, Qt::Key_Escape);
    EXPECT_TRUE(widget->selectedClipIds().isEmpty());
    EXPECT_EQ(returns, 0);
    EXPECT_FALSE(overridesEscape()); // The window's Return to Timeline shortcut can now run.

    click(*widget, point(40));
    auto* search = window.findChild<QLineEdit*>("mediaSearch");
    ASSERT_NE(search, nullptr);
    search->setFocus();
    QCoreApplication::processEvents();
    ASSERT_EQ(QApplication::focusWidget(), search);
    EXPECT_FALSE(overridesEscape()); // Timeline selection cannot capture a search-field shortcut.
    EXPECT_EQ(widget->selectedClipIds(), (QStringList{"first"}));
    window.hide();
}

TEST_F(MultiSelectionUiTest, SelectAllIncludesEveryTypeAndNeverChangesProject) {
    const auto before = json();
    key(timeline, Qt::Key_A, Qt::ControlModifier);
    expectSelection({"first", "second", "middle", "lower", "audio", "audio2"});
    key(timeline, Qt::Key_A, Qt::ControlModifier);
    expectSelection({"first", "second", "middle", "lower", "audio", "audio2"});
    key(timeline, Qt::Key_Escape);
    expectSelection({});
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(MultiSelectionUiTest, GroupMovePreviewIsLocalAndOneUndoRestoresEveryPayload) {
    selectPair();
    const auto before = json();
    const auto first = *clip("first"), second = *clip("second");
    int changes = 0, requests = 0;
    QObject::connect(&session, &EditorSession::timelineChanged, [&](qint64) { ++changes; });
    QObject::connect(&timeline, &TimelineWidget::editRequested, [&] { ++requests; });
    mouse(timeline, QEvent::MouseButtonPress, point(40));
    const auto baseline = timeline.grab().toImage();
    mouse(timeline, QEvent::MouseMove, point(60));
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
    EXPECT_EQ(changes, 0);
    EXPECT_NE(timeline.grab().toImage(), baseline);
    mouse(timeline, QEvent::MouseButtonRelease, point(60));
    auto expectedFirst = first, expectedSecond = second;
    expectedFirst.startFrame += 20;
    expectedSecond.startFrame += 20;
    ASSERT_NE(clip("first"), nullptr);
    ASSERT_NE(clip("second"), nullptr);
    EXPECT_EQ(*clip("first"), expectedFirst);
    EXPECT_EQ(*clip("second"), expectedSecond);
    expectSelection({"first", "second"});
    EXPECT_EQ(changes, 1);
    EXPECT_EQ(requests, 1);
    const auto after = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), after);
}

TEST_F(MultiSelectionUiTest, GroupClampsAtFrameZeroUsingEarliestMember) {
    selectPair();
    const auto before = json();
    // Grab the later clip: clamping only that lead would lose the 60-frame gap.
    drag(timeline, point(100), point(10));
    ASSERT_NE(clip("first"), nullptr);
    ASSERT_NE(clip("second"), nullptr);
    EXPECT_EQ(clip("first")->startFrame, 0);
    EXPECT_EQ(clip("second")->startFrame, 60);
    expectSelection({"first", "second"});
    const auto after = json();
    session.undo(); EXPECT_EQ(json(), before);
    session.redo(); EXPECT_EQ(json(), after);
}

TEST_F(MultiSelectionUiTest, SnappingExcludesEverySelectedClipsOldBoundaries) {
    // Remove unselected nearby anchors; leave the playhead well away from the group.
    ASSERT_TRUE(session.removeClips({"middle", "lower", "audio", "audio2"}).changed());
    selectPair();
    timeline.setSnappingEnabled(true);
    timeline.setCurrentFrame(15);
    const auto before = json();
    // The leading clip's new end is frame89, one frame from selected second's
    // old start90. Snapping to that stale edge would change the shared delta.
    drag(timeline, point(40), point(69));
    ASSERT_NE(clip("first"), nullptr);
    ASSERT_NE(clip("second"), nullptr);
    EXPECT_EQ(clip("first")->startFrame, 59);
    EXPECT_EQ(clip("second")->startFrame, 119);
    session.undo(); EXPECT_EQ(json(), before);
}

TEST_F(MultiSelectionUiTest, MarqueeUsesZoomAndBothScrollOffsets) {
    timeline.zoomIn(); // The short timeline clamps scroll to zero at 5 pixels/frame.
    ASSERT_TRUE(session.moveClip("second", 300).changed()); // Enable horizontal panning.
    timeline.resize(1000, 210);
    QCoreApplication::processEvents();
    QWheelEvent wheel(QPointF(600, 68), timeline.mapToGlobal(QPoint(600, 68)),
        {}, QPoint(0, -120), Qt::NoButton, Qt::ShiftModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&timeline, &wheel); // Horizontal scrollOffset80.
    auto* scroll = timeline.findChild<QScrollBar*>("timelineVerticalScrollBar");
    ASSERT_NE(scroll, nullptr);
    scroll->setValue(88); // lower/v2 starts at y50 and is the first fully visible row.
    const auto before = json();
    drag(timeline, {220, 52}, {240, 80}); // lower rect x230..380, y53..91.
    expectSelection({"lower"});
    EXPECT_EQ(json(), before);
}

TEST_F(MultiSelectionUiTest, EscapeCancelsGroupMoveAndReturnToOriginKeepsRedo) {
    selectPair();
    const auto before = json();
    drag(timeline, point(40), point(60));
    const auto moved = json();
    session.undo();
    ASSERT_EQ(json(), before);
    ASSERT_TRUE(session.savePackage(package));
    selectPair();
    mouse(timeline, QEvent::MouseButtonPress, point(40));
    mouse(timeline, QEvent::MouseMove, point(80));
    key(timeline, Qt::Key_Escape);
    mouse(timeline, QEvent::MouseButtonRelease, point(80));
    expectSelection({"first", "second"});
    EXPECT_EQ(json(), before);
    mouse(timeline, QEvent::MouseButtonPress, point(40));
    mouse(timeline, QEvent::MouseMove, point(80));
    mouse(timeline, QEvent::MouseButtonRelease, point(40));
    EXPECT_EQ(json(), before);
    EXPECT_FALSE(session.isDirty());
    session.redo();
    EXPECT_EQ(json(), moved);
}

TEST_F(MultiSelectionUiTest, VideoLeadShiftsSameTypeTrackIndicesAndKeepsAudioOnItsTrack) {
    click(timeline, point(40));
    click(timeline, point(40, 2), Qt::ControlModifier);
    click(timeline, point(50, 4), Qt::ControlModifier);
    const auto before = json();
    const auto middle = *clip("middle");
    drag(timeline, point(40), point(140, 1));
    EXPECT_EQ(owner("first"), "v1");
    EXPECT_EQ(owner("lower"), "v3");
    EXPECT_EQ(owner("audio"), "a0");
    ASSERT_NE(clip("first"), nullptr);
    ASSERT_NE(clip("lower"), nullptr);
    ASSERT_NE(clip("audio"), nullptr);
    EXPECT_EQ(clip("first")->startFrame, 130);
    EXPECT_EQ(clip("lower")->startFrame, 130);
    EXPECT_EQ(clip("audio")->startFrame, 140);
    EXPECT_EQ(*clip("middle"), middle);
    expectSelection({"first", "lower", "audio"});
    const auto after = json();
    session.undo(); EXPECT_EQ(json(), before);
    session.redo(); EXPECT_EQ(json(), after);
}

TEST_F(MultiSelectionUiTest, AudioLeadShiftsAudioRowsAndKeepsVideoOnItsTrack) {
    click(timeline, point(40));
    click(timeline, point(50, 4), Qt::ControlModifier);
    const auto before = json();
    drag(timeline, point(50, 4), point(70, 5));
    EXPECT_EQ(owner("audio"), "a1");
    EXPECT_EQ(owner("first"), "v0");
    ASSERT_NE(clip("first"), nullptr);
    ASSERT_NE(clip("audio"), nullptr);
    EXPECT_EQ(clip("audio")->startFrame, 60);
    EXPECT_EQ(clip("first")->startFrame, 50);
    const auto after = json();
    session.undo(); EXPECT_EQ(json(), before);
    session.redo(); EXPECT_EQ(json(), after);
}

TEST_F(MultiSelectionUiTest, InvalidRowForAnyGroupMemberRejectsWholeMove) {
    click(timeline, point(40));
    click(timeline, point(40, 2), Qt::ControlModifier);
    const auto before = json();
    int requests = 0;
    QObject::connect(&timeline, &TimelineWidget::editRequested, [&] { ++requests; });
    drag(timeline, point(40), point(140, 2)); // lower would need nonexistent v4.
    EXPECT_EQ(json(), before);
    drag(timeline, point(40), point(140, 4)); // Audio destination for a video lead.
    EXPECT_EQ(json(), before);
    drag(timeline, point(40), {720, 345}); // Below all rows.
    EXPECT_EQ(json(), before);
    expectSelection({"first", "lower"});
    EXPECT_EQ(requests, 0);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(MultiSelectionUiTest, SameSourceVideoGroupCanCreateOneTopTrack) {
    selectPair();
    const auto before = json();
    const auto count = active()->tracks.size();
    drag(timeline, point(40), {400, 40});
    ASSERT_EQ(active()->tracks.size(), count + 1);
    const auto topId = active()->tracks.front().id;
    EXPECT_NE(topId, "v0");
    EXPECT_EQ(owner("first"), topId);
    EXPECT_EQ(owner("second"), topId);
    ASSERT_NE(clip("first"), nullptr);
    ASSERT_NE(clip("second"), nullptr);
    EXPECT_EQ(clip("first")->startFrame, 50);
    EXPECT_EQ(clip("second")->startFrame, 110);
    expectSelection({"first", "second"});
    const auto after = json();
    session.undo(); EXPECT_EQ(json(), before);
    session.redo(); EXPECT_EQ(json(), after);
}

TEST_F(MultiSelectionUiTest, NewTopTrackRejectsMultipleSourceRowsAndMixedTypes) {
    const auto before = json();
    int requests = 0;
    QObject::connect(&timeline, &TimelineWidget::editRequested, [&] { ++requests; });
    click(timeline, point(40));
    click(timeline, point(40, 2), Qt::ControlModifier);
    drag(timeline, point(40), {400, 40});
    EXPECT_EQ(json(), before);
    expectSelection({"first", "lower"});
    click(timeline, point(40)); // Collapse on release.
    click(timeline, point(50, 4), Qt::ControlModifier);
    drag(timeline, point(40), {400, 40});
    EXPECT_EQ(json(), before);
    expectSelection({"first", "audio"});
    EXPECT_EQ(requests, 0);
    EXPECT_FALSE(session.isDirty());
}

TEST_F(MultiSelectionUiTest, DeleteAllCommitsOnceAndOneUndoRestoresCompleteProject) {
    key(timeline, Qt::Key_A, Qt::ControlModifier);
    const auto before = json();
    int requests = 0, changes = 0;
    QObject::connect(&timeline, &TimelineWidget::editRequested, [&] { ++requests; });
    QObject::connect(&session, &EditorSession::timelineChanged, [&](qint64) { ++changes; });
    key(timeline, Qt::Key_Delete);
    for (const auto& track : active()->tracks) EXPECT_TRUE(track.clips.empty());
    expectSelection({});
    EXPECT_EQ(requests, 1);
    EXPECT_EQ(changes, 1);
    const auto after = json();
    key(timeline, Qt::Key_Delete);
    EXPECT_EQ(requests, 1);
    session.undo(); EXPECT_EQ(json(), before);
    session.redo(); EXPECT_EQ(json(), after);
}

TEST_F(MultiSelectionUiTest, SplitRippleAndTrimCannotEditMultipleSelectedClips) {
    selectPair();
    timeline.setCurrentFrame(45);
    const auto before = json();
    int requests = 0;
    QObject::connect(&timeline, &TimelineWidget::editRequested, [&] { ++requests; });
    timeline.splitSelectedClip();
    timeline.rippleDeleteSelectedClip();
    key(timeline, Qt::Key_S);
    EXPECT_EQ(json(), before);
    EXPECT_EQ(requests, 0);
    // Multi-selection edges move the whole group rather than trimming a member.
    drag(timeline, {281, 72}, {321, 72});
    expectSelection({"first", "second"});
    ASSERT_NE(clip("first"), nullptr);
    ASSERT_NE(clip("second"), nullptr);
    EXPECT_EQ(clip("first")->startFrame, 40);
    EXPECT_EQ(clip("second")->startFrame, 100);
    EXPECT_EQ(clip("first")->durationFrames, 30);
    EXPECT_EQ(clip("first")->trimStartFrame, 5);
    EXPECT_EQ(clip("second")->durationFrames, 30);
    EXPECT_EQ(requests, 1);
    session.undo(); EXPECT_EQ(json(), before);
    // Collapsing to one member restores ordinary singleton operations.
    click(timeline, point(40));
    timeline.setCurrentFrame(45);
    timeline.splitSelectedClip();
    EXPECT_NE(json(), before);
    EXPECT_EQ(requests, 2);
    session.undo(); EXPECT_EQ(json(), before);
}

class PendingGradeSelectionUiTest : public MultiSelectionUiTest,
                                    public ::testing::WithParamInterface<bool> {};
TEST_P(PendingGradeSelectionUiTest, GroupEditFlushesGradeAsSeparateUndoStep) {
    selectPair();
    const auto before = json();
    ASSERT_TRUE(session.beginClipEffectsTransaction("first").message.empty());
    const auto effects = heisenberg::JsonValue::array({{
        {"id", "grade"}, {"type", "video.basicGrade"}, {"enabled", true},
        {"params", {{"exposure", {{"value", 1.5}}}}}
    }});
    ASSERT_TRUE(session.updateClipEffectsTransaction(effects).changed());
    const auto graded = json();
    int requests = 0;
    QObject::connect(&timeline, &TimelineWidget::editRequested, &timeline, [&] {
        ++requests;
        EXPECT_TRUE(session.commitClipEffectsTransaction().changed());
    });
    if (GetParam()) key(timeline, Qt::Key_Delete);
    else drag(timeline, point(40), point(60));
    EXPECT_EQ(requests, 1);
    EXPECT_NE(json(), graded);
    if (!GetParam()) {
        ASSERT_NE(clip("first"), nullptr);
        ASSERT_TRUE(clip("first")->effects.has_value());
        EXPECT_EQ(*clip("first")->effects, effects);
        expectSelection({"first", "second"});
    }
    const auto edited = json();
    session.undo(); EXPECT_EQ(json(), graded);
    session.undo(); EXPECT_EQ(json(), before);
    session.redo(); EXPECT_EQ(json(), graded);
    session.redo(); EXPECT_EQ(json(), edited);
}

TEST_F(MultiSelectionUiTest, SavedGroupMoveAndOwnedCompositionSnapshotsSurviveReopenAndUndo) {
    selectPair();
    const auto captured = session.snapshot();
    const auto originalFirst = *clip("first");
    const auto originalSecond = *clip("second");
    drag(timeline, point(40), point(60));
    const auto after = json();
    const auto movedFirst = *clip("first");
    const auto movedSecond = *clip("second");
    const auto movedSnapshot = session.snapshot();
    session.undo();
    heisenberg::CompositionPlan oldPlan, movedPlan;
    std::string error;
    ASSERT_TRUE(heisenberg::CompositionBuilder::build(captured, oldPlan, &error)) << error;
    ASSERT_TRUE(heisenberg::CompositionBuilder::build(movedSnapshot, movedPlan, &error)) << error;
    ASSERT_NE(planClip(oldPlan, "first"), nullptr);
    ASSERT_NE(planClip(oldPlan, "second"), nullptr);
    ASSERT_NE(planClip(movedPlan, "first"), nullptr);
    ASSERT_NE(planClip(movedPlan, "second"), nullptr);
    EXPECT_EQ(planClip(oldPlan, "first")->visualSnapshot, originalFirst);
    EXPECT_EQ(planClip(oldPlan, "second")->visualSnapshot, originalSecond);
    EXPECT_EQ(planClip(movedPlan, "first")->visualSnapshot, movedFirst);
    EXPECT_EQ(planClip(movedPlan, "second")->visualSnapshot, movedSecond);
    session.redo();
    ASSERT_EQ(json(), after);
    QString saveError;
    ASSERT_TRUE(session.savePackage(package, &saveError)) << saveError.toStdString();
    EditorSession reopened;
    ASSERT_TRUE(reopened.loadPackage(package, &saveError)) << saveError.toStdString();
    EXPECT_EQ(heisenberg::ProjectCodec::encode(reopened.editor().project()), json());
    ASSERT_NE(reopened.editor().activeTimeline()->clipById("first"), nullptr);
    EXPECT_EQ(*reopened.editor().activeTimeline()->clipById("first"), movedFirst);
    timeline.setSession(&reopened);
    expectSelection({}); // Selection is transient UI state.
    timeline.setSession(&session);
}

INSTANTIATE_TEST_SUITE_P(CtrlAndShift, ModifierSelectionUiTest,
    ::testing::Values(Qt::ControlModifier, Qt::ShiftModifier));
INSTANTIATE_TEST_SUITE_P(MoveAndDelete, PendingGradeSelectionUiTest, ::testing::Bool());
} // namespace
