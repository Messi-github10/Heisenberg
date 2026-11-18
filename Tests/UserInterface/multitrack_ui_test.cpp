#include <Backend/EditorSession.hpp>
#include <MainWidget/TimelineWidget.hpp>
#include <Models/ProjectCodec.hpp>

#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <functional>

namespace {
using heisenberg::ClipType;
using heisenberg::ui::EditorSession;

void mouse(QWidget& widget, QEvent::Type type, QPointF point) {
    QMouseEvent event(type, point, widget.mapToGlobal(point.toPoint()),
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}
void click(QWidget& widget, QPointF point) {
    mouse(widget, QEvent::MouseButtonPress, point);
    mouse(widget, QEvent::MouseButtonRelease, point);
}
void drag(QWidget& widget, QPointF from, QPointF to) {
    mouse(widget, QEvent::MouseButtonPress, from);
    mouse(widget, QEvent::MouseMove, to);
    mouse(widget, QEvent::MouseButtonRelease, to);
}
void key(QWidget& widget, int code) {
    QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}
void wheel(TimelineWidget& widget, QPoint delta, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QWheelEvent event(QPointF(600, 68), widget.mapToGlobal(QPoint(600, 68)), {}, delta,
        Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&widget, &event);
}

class MultiTrackUiTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        package = directory.path() + "/synthetic.berg";
        ASSERT_TRUE(session.savePackage(package));
        auto project = session.editor().project();
        heisenberg::Timeline active;
        active.id = "timeline";
        active.name = "Synthetic multi-track";
        // Interleaved storage exercises grouped visual order and global reorder indices.
        for (int i = 0; i < 6; ++i) {
            heisenberg::Track video;
            video.id = "v" + std::to_string(i);
            video.name = "Video " + std::to_string(i + 1);
            video.displayHeight = i == 0 ? 60.0 : 44.0;
            heisenberg::Clip clip;
            clip.id = "c" + std::to_string(i);
            clip.mediaRef = "video";
            clip.startFrame = 30;
            clip.durationFrames = 30;
            video.clips.push_back(clip);
            active.tracks.push_back(video);
            if (i < 2) {
                heisenberg::Track audio;
                audio.id = "a" + std::to_string(i);
                audio.name = "Audio " + std::to_string(i + 1);
                audio.type = ClipType::Audio;
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
        for (const auto& document : {std::pair{QString("project.json"), heisenberg::ProjectCodec::encode(project)},
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
        timeline.resize(1000, 210);
        timeline.show();
        QCoreApplication::processEvents();
        scroll = timeline.findChild<QScrollBar*>("timelineVerticalScrollBar");
        ASSERT_NE(scroll, nullptr);
    }
    std::string json() const { return heisenberg::ProjectCodec::encode(session.editor().project()); }
    const heisenberg::Track* track(const std::string& id) const { return session.editor().activeTimeline()->trackById(id); }
    const heisenberg::Clip* clip(const std::string& id = "c0") const { return session.editor().activeTimeline()->clipById(id); }
    void context(QPoint point, const char* actionName, std::function<void()> modal = {}) {
        bool found = false;
        QTimer::singleShot(0, &timeline, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) { ADD_FAILURE() << "Track menu missing"; return; }
            auto* action = menu->findChild<QAction*>(actionName);
            if (!action || !action->isEnabled()) { menu->close(); return; }
            found = true;
            if (modal) QTimer::singleShot(0, &timeline, modal);
            menu->setActiveAction(action);
            key(*menu, Qt::Key_Return);
        });
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, timeline.mapToGlobal(point));
        QApplication::sendEvent(&timeline, &event);
        EXPECT_TRUE(found);
    }
    void appearance(double percent, bool accept = true) {
        QTimer::singleShot(0, &timeline, [percent, accept] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) { ADD_FAILURE() << "Appearance dialog missing"; return; }
            auto* opacity = dialog->findChild<QDoubleSpinBox*>("clipOpacity");
            if (!opacity) { dialog->reject(); ADD_FAILURE() << "Opacity field missing"; return; }
            opacity->setValue(percent);
            if (accept) dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            else dialog->reject();
        });
        timeline.editSelectedClipAppearance();
    }
    EditorSession session;
    TimelineWidget timeline;
    QTemporaryDir directory;
    QString package;
    QScrollBar* scroll = nullptr;
};

TEST_F(MultiTrackUiTest, HeadersSelectWithoutMutingAndRememberEachMediaType) {
    const auto before = json();
    click(timeline, {30, 128}); // v1 starts at y110 despite a0 occupying storage index 1.
    EXPECT_EQ(timeline.targetTrackForMedia("video"), "v1");
    EXPECT_EQ(timeline.targetTrackForMedia("audio"), "a0");
    EXPECT_EQ(json(), before);
    key(timeline, Qt::Key_M);
    EXPECT_TRUE(track("v1")->muted);
    EXPECT_FALSE(track("v0")->muted);
    session.undo();
    EXPECT_EQ(json(), before);
    key(timeline, Qt::Key_H);
    EXPECT_TRUE(track("v1")->hidden);
    click(timeline, {320, 128}); // Hidden tracks retain a selectable clip lane.
    EXPECT_EQ(timeline.selectedClipId(), "c1");
    session.undo();
    scroll->setValue(scroll->maximum());
    click(timeline, {30, 142}); // a0 after all six video lanes.
    EXPECT_EQ(timeline.targetTrackForMedia("audio"), "a0");
    EXPECT_EQ(timeline.targetTrackForMedia("video"), "v1");
    key(timeline, Qt::Key_H);
    EXPECT_EQ(json(), before); // H is video only.
    key(timeline, Qt::Key_M);
    EXPECT_TRUE(track("a0")->muted);
}

TEST_F(MultiTrackUiTest, VerticalWheelSharedMappingAndFixedRulerSupportScrolledHits) {
    EXPECT_TRUE(scroll->isVisible());
    EXPECT_EQ(scroll->maximum(), 208); // 20px strip + 368px lanes - 180px viewport.
    EXPECT_LT(static_cast<QWidget&>(timeline).minimumSizeHint().height(), 210);
    wheel(timeline, {0, -120});
    EXPECT_EQ(scroll->value(), 80);
    scroll->setValue(100); // Include the new 20px strip to retain clipped-lane coverage.
    click(timeline, {320, 48}); // v1 top110 minus100, clipped to ruler30.
    EXPECT_EQ(timeline.selectedClipId(), "c1");
    click(timeline, {140, 48}); // v1 button relative y38: name/header selection only.
    EXPECT_FALSE(track("v1")->muted);
    EXPECT_TRUE(timeline.selectedClipId().isEmpty());
    EXPECT_EQ(timeline.targetTrackForMedia("video"), "v1");
    click(timeline, {320, 48}); // Restore clip selection before verifying ruler scrubbing.
    ASSERT_EQ(timeline.selectedClipId(), "c1");
    click(timeline, {320, 15}); // Ruler cannot select a scrolled-under clip.
    EXPECT_EQ(timeline.selectedClipId(), "c1");
    EXPECT_EQ(timeline.currentFrame(), 40);
    scroll->setValue(80);
    click(timeline, {140, 48}); // v1 top30, button relative y18.
    EXPECT_TRUE(track("v1")->muted);
    EXPECT_FALSE(track("v0")->muted);
    timeline.resize(1000, 1000);
    QCoreApplication::processEvents();
    EXPECT_EQ(scroll->value(), 0);
    EXPECT_TRUE(scroll->isHidden());
}

TEST_F(MultiTrackUiTest, ShiftWheelAndHorizontalDeltaPanWithoutVerticalMotion) {
    ASSERT_TRUE(session.moveClip("c5", 300).changed());
    wheel(timeline, {0, -120}, Qt::ShiftModifier);
    EXPECT_EQ(scroll->value(), 0);
    drag(timeline, {240, 68}, {320, 68}); // 80px scroll: c0 frame30 ->50.
    EXPECT_EQ(clip()->startFrame, 50);
    session.undo();
    wheel(timeline, {-120, 0});
    EXPECT_EQ(scroll->value(), 0);
    drag(timeline, {200, 68}, {280, 68}); // Scroll160, grab at frame50.
    EXPECT_EQ(clip()->startFrame, 50);
    session.undo();
    wheel(timeline, {0, 120}, Qt::ControlModifier);
    EXPECT_EQ(scroll->value(), 0);
}

TEST_F(MultiTrackUiTest, SameFrameCrossTrackMoveCommitsOnceAndTrimsStayOnSource) {
    const auto before = json();
    drag(timeline, {320, 68}, {320, 128});
    ASSERT_NE(clip(), nullptr);
    EXPECT_EQ(clip()->startFrame, 30);
    EXPECT_EQ(session.editor().activeTimeline()->trackForClip("c0")->id, "v1");
    EXPECT_EQ(timeline.targetTrackForMedia("video"), "v1");
    const auto after = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), after);
    session.undo();
    drag(timeline, {281, 68}, {321, 128}); // Left trim cannot migrate.
    EXPECT_EQ(json(), before);
    scroll->setValue(scroll->maximum());
    click(timeline, {320, 92}); // v5 is last video lane at bottom scroll.
    EXPECT_EQ(timeline.selectedClipId(), "c5");
    const auto beforeWrongType = json();
    drag(timeline, {320, 92}, {400, 142}); // Audio destination rejects video.
    EXPECT_EQ(json(), beforeWrongType);
}

TEST_F(MultiTrackUiTest, RenameAndReorderUseTrackIdsAndGlobalIndicesWithUndo) {
    const auto before = json();
    context({30, 128}, "trackRename", [] {
        auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        if (!dialog) { ADD_FAILURE() << "Rename dialog missing"; return; }
        dialog->setTextValue("Overlay");
        dialog->accept();
    });
    ASSERT_TRUE(track("v1")->name);
    EXPECT_EQ(*track("v1")->name, "Overlay");
    const auto renamed = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), renamed);
    context({30, 128}, "trackMoveUp");
    EXPECT_EQ(session.editor().activeTimeline()->tracks.front().id, "v1");
    click(timeline, {320, 68});
    EXPECT_EQ(timeline.selectedClipId(), "c1");
    session.undo();
    EXPECT_EQ(json(), renamed);
    context({30, 68}, "trackMoveDown");
    EXPECT_EQ(session.editor().activeTimeline()->tracks.front().id, "v1");
    session.undo();
    EXPECT_EQ(json(), renamed);
}

TEST_F(MultiTrackUiTest, PopulatedTrackDeleteRequiresConfirmationAndOneUndoRestoresClips) {
    const auto before = json();
    context({30, 128}, "trackDelete", [] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) { ADD_FAILURE() << "Delete confirmation missing"; return; }
        box->button(QMessageBox::No)->click();
    });
    EXPECT_EQ(json(), before);
    context({30, 128}, "trackDelete", [] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) { ADD_FAILURE() << "Delete confirmation missing"; return; }
        box->button(QMessageBox::Yes)->click();
    });
    EXPECT_EQ(track("v1"), nullptr);
    EXPECT_EQ(clip("c1"), nullptr);
    EXPECT_EQ(timeline.targetTrackForMedia("video"), "v0");
    session.undo();
    EXPECT_EQ(json(), before);
}

TEST_F(MultiTrackUiTest, AddSlotsRememberNewTracksAndOpacityCancelNoOpAndUndoAreLocal) {
    const auto before = json();
    timeline.addVideoTrack();
    const auto videoTarget = timeline.targetTrackForMedia("video");
    EXPECT_NE(videoTarget, "v0");
    ASSERT_NE(track(videoTarget.toStdString()), nullptr);
    timeline.addAudioTrack();
    EXPECT_NE(timeline.targetTrackForMedia("audio"), "a0");
    EXPECT_EQ(timeline.targetTrackForMedia("video"), videoTarget);
    session.undo();
    session.undo();
    EXPECT_EQ(json(), before);
    click(timeline, {320, 68});
    appearance(25, false);
    EXPECT_EQ(json(), before);
    appearance(100);
    EXPECT_EQ(json(), before);
    appearance(25);
    EXPECT_DOUBLE_EQ(clip()->opacity, .25);
    const auto changed = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), changed);
}

enum class PendingEdit { Move, Trim, Mute, Hidden, Opacity };
class PendingGradeMultiTrackUiTest : public MultiTrackUiTest,
                                    public ::testing::WithParamInterface<PendingEdit> {};
TEST_P(PendingGradeMultiTrackUiTest, FlushReplacesTimelineAndKeepsGradeAsSeparateUndoStep) {
    click(timeline, {320, 68});
    const auto before = json();
    ASSERT_TRUE(session.beginClipEffectsTransaction("c0").message.empty());
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
    switch (GetParam()) {
    case PendingEdit::Move: drag(timeline, {320, 68}, {400, 128}); break;
    case PendingEdit::Trim: drag(timeline, {281, 68}, {321, 68}); break;
    case PendingEdit::Mute: click(timeline, {140, 68}); break;
    case PendingEdit::Hidden: click(timeline, {112, 68}); break;
    case PendingEdit::Opacity: appearance(50); break;
    }
    EXPECT_EQ(requests, 1);
    EXPECT_NE(json(), graded);
    session.undo();
    EXPECT_EQ(json(), graded);
    session.undo();
    EXPECT_EQ(json(), before);
}
INSTANTIATE_TEST_SUITE_P(TimelineMutations, PendingGradeMultiTrackUiTest,
    ::testing::Values(PendingEdit::Move, PendingEdit::Trim, PendingEdit::Mute, PendingEdit::Hidden, PendingEdit::Opacity));
} // namespace
