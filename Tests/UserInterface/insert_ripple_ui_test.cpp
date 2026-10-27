#include <Backend/EditorSession.hpp>
#include <MainWidget/ColorGradePanel.hpp>
#include <MainWidget/MainWindow.hpp>
#include <MainWidget/TimelineWidget.hpp>
#include <Models/ProjectCodec.hpp>

#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QTimer>

namespace {
using heisenberg::ui::EditorSession;

QPoint position(qint64 frame, int y = 48) {
    return {160 + static_cast<int>(frame * 4), y};
}

void mouse(TimelineWidget& timeline, QEvent::Type type, QPoint point) {
    QMouseEvent event(type, QPointF(point), QPointF(timeline.mapToGlobal(point)),
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &event);
}

void select(TimelineWidget& timeline, qint64 frame, int y = 48) {
    mouse(timeline, QEvent::MouseButtonPress, position(frame, y));
    mouse(timeline, QEvent::MouseButtonRelease, position(frame, y));
}

void key(QWidget& widget, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
         bool repeat = false) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers, QString(), repeat);
    QApplication::sendEvent(&widget, &event);
}

bool drop(TimelineWidget& timeline, EditorSession& session, const QString& ref,
          qint64 frame, int y = 48) {
    QMimeData mime;
    mime.setData(heisenberg::ui::kMediaMimeType, QJsonDocument(QJsonObject{
        {"session", session.token()}, {"mediaRef", ref}}).toJson());
    QDragEnterEvent enter(position(frame, y), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &enter);
    QDropEvent event(QPointF(position(frame, y)), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &event);
    return event.isAccepted();
}

class InsertRippleUiTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA, HEISENBERG_TEST_LONG_AUDIO}).isEmpty());
        videoRef = QString::fromStdString(session.editor().manifest().entries()[0].id);
        audioRef = QString::fromStdString(session.editor().manifest().entries()[1].id);
        auto result = session.placeMedia(videoRef, 30);
        ASSERT_TRUE(result.changed()) << result.message;
        ASSERT_FALSE(result.affectedIds.empty());
        firstId = QString::fromStdString(result.affectedIds.back());
        result = session.placeMedia(videoRef, 90);
        ASSERT_TRUE(result.changed()) << result.message;
        ASSERT_FALSE(result.affectedIds.empty());
        secondId = QString::fromStdString(result.affectedIds.back());
        ASSERT_TRUE(session.placeMedia(audioRef, 30).changed());
        ASSERT_TRUE(session.placeMedia(audioRef, 150).changed());
        timeline.setSession(&session);
        timeline.resize(1200, 210);
        timeline.show();
        QCoreApplication::processEvents();
    }

    std::string json() const {
        return heisenberg::ProjectCodec::encode(session.editor().project());
    }
    const heisenberg::Clip* clip(const QString& id) const {
        return session.editor().activeTimeline()->clipById(id.toStdString());
    }
    const heisenberg::Track& track(heisenberg::ClipType type) const {
        const auto& tracks = session.editor().activeTimeline()->tracks;
        for (const auto& candidate : tracks) if (candidate.type == type) return candidate;
        ADD_FAILURE() << "Expected track is missing";
        return tracks.front();
    }

    EditorSession session;
    TimelineWidget timeline;
    QString videoRef, audioRef, firstId, secondId;
};

TEST_F(InsertRippleUiTest, DropModeUsesMarkedRangeAtCursorAndSelectsNewClipWithOneUndoStep) {
    EXPECT_FALSE(timeline.insertMode());
    ASSERT_TRUE(session.setSourceIn(videoRef, .2));
    ASSERT_TRUE(session.setSourceOut(videoRef, .7));
    qint64 trim = 0, duration = 0;
    ASSERT_TRUE(session.sourcePlacementRange(videoRef, trim, duration));
    EXPECT_EQ(trim, 6);
    EXPECT_EQ(duration, 15); // The ghost and both commit paths use this resolved range.
    const auto before = json();
    const auto audioBefore = track(heisenberg::ClipType::Audio);
    for (const bool insert : {false, true}) {
        SCOPED_TRACE(insert ? "insert" : "overwrite");
        timeline.setInsertMode(insert);
        EXPECT_EQ(timeline.insertMode(), insert);
        ASSERT_TRUE(drop(timeline, session, videoRef, 45));
        const auto* placed = clip(timeline.selectedClipId());
        ASSERT_NE(placed, nullptr);
        EXPECT_NE(timeline.selectedClipId(), firstId);
        EXPECT_NE(timeline.selectedClipId(), secondId);
        EXPECT_EQ(placed->startFrame, 45);
        EXPECT_EQ(placed->trimStartFrame, trim);
        EXPECT_EQ(placed->durationFrames, duration);
        ASSERT_NE(clip(firstId), nullptr);
        ASSERT_NE(clip(secondId), nullptr);
        EXPECT_EQ(clip(firstId)->durationFrames, 15);
        EXPECT_EQ(clip(secondId)->startFrame, insert ? 105 : 90);
        EXPECT_EQ(track(heisenberg::ClipType::Video).clips.size(), insert ? 4u : 3u);
        EXPECT_EQ(track(heisenberg::ClipType::Audio), audioBefore);
        const auto after = json();
        session.undo();
        EXPECT_EQ(json(), before);
        session.redo();
        EXPECT_EQ(json(), after);
        session.undo();
    }
}

TEST_F(InsertRippleUiTest, UnmarkedRangeResolvesGhostDurationAndInsertDoesNotAppendOrClamp) {
    qint64 trim = -1, duration = -1;
    ASSERT_TRUE(session.sourcePlacementRange(videoRef, trim, duration));
    EXPECT_EQ(trim, 0);
    EXPECT_EQ(duration, 30);
    timeline.setInsertMode(true);
    ASSERT_TRUE(drop(timeline, session, videoRef, 210));
    const auto* placed = clip(timeline.selectedClipId());
    ASSERT_NE(placed, nullptr);
    EXPECT_EQ(placed->startFrame, 210);
    EXPECT_EQ(placed->durationFrames, duration);
    EXPECT_EQ(clip(secondId)->startFrame, 90);
}

TEST_F(InsertRippleUiTest, AudioInsertAffectsOnlyTargetTrackAndWrongLaneIsRejected) {
    timeline.setInsertMode(true);
    ASSERT_TRUE(session.setSourceIn(audioRef, .2));
    ASSERT_TRUE(session.setSourceOut(audioRef, .7));
    const auto before = json();
    EXPECT_FALSE(drop(timeline, session, audioRef, 60, 48));
    EXPECT_EQ(json(), before);
    const auto videoBefore = track(heisenberg::ClipType::Video);
    const auto audioTrackId = track(heisenberg::ClipType::Audio).id;
    const auto laterAudioId = QString::fromStdString(track(heisenberg::ClipType::Audio).clips.back().id);
    ASSERT_TRUE(drop(timeline, session, audioRef, 60, 92));
    ASSERT_NE(clip(laterAudioId), nullptr);
    EXPECT_EQ(clip(laterAudioId)->startFrame, 165);
    EXPECT_EQ(track(heisenberg::ClipType::Audio).id, audioTrackId);
    EXPECT_EQ(track(heisenberg::ClipType::Video), videoBefore);
    EXPECT_EQ(session.editor().activeTimeline()->tracks.size(), 2u);
    session.undo();
    EXPECT_EQ(json(), before);
}

TEST_F(InsertRippleUiTest, SubFrameSourceRangeRejectsBothDropModesBeforeEditRequested) {
    ASSERT_TRUE(session.setSourceIn(videoRef, .201));
    ASSERT_TRUE(session.setSourceOut(videoRef, .202));
    qint64 trim = 0, duration = 0;
    QString error;
    EXPECT_FALSE(session.sourcePlacementRange(videoRef, trim, duration, &error));
    EXPECT_FALSE(error.isEmpty());
    const auto before = json();
    int requests = 0;
    QObject::connect(&timeline, &TimelineWidget::editRequested, &timeline, [&] { ++requests; });
    for (const bool insert : {false, true}) {
        timeline.setInsertMode(insert);
        EXPECT_FALSE(drop(timeline, session, videoRef, 45));
        EXPECT_EQ(json(), before);
    }
    EXPECT_EQ(requests, 0);
}

TEST_F(InsertRippleUiTest, ShiftDeleteCancelsDraftAndRipplesOnlySelectedTrack) {
    const auto before = json();
    const auto audioBefore = track(heisenberg::ClipType::Audio);
    mouse(timeline, QEvent::MouseButtonPress, position(40));
    mouse(timeline, QEvent::MouseMove, position(70));
    key(timeline, Qt::Key_Delete, Qt::ShiftModifier);
    mouse(timeline, QEvent::MouseButtonRelease, position(70));
    EXPECT_EQ(clip(firstId), nullptr);
    ASSERT_NE(clip(secondId), nullptr);
    EXPECT_EQ(clip(secondId)->startFrame, 60);
    EXPECT_EQ(track(heisenberg::ClipType::Audio), audioBefore);
    const auto after = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), after);
}

TEST_F(InsertRippleUiTest, DeleteAndBackspaceKeepGapsAndModifiedOrRepeatedRippleKeysDoNothing) {
    const auto before = json();
    for (const auto modifiers : {Qt::ControlModifier, Qt::AltModifier, Qt::MetaModifier}) {
        select(timeline, 40);
        key(timeline, Qt::Key_Delete, modifiers | Qt::ShiftModifier);
        EXPECT_EQ(json(), before);
    }
    key(timeline, Qt::Key_Delete, Qt::ShiftModifier, true);
    EXPECT_EQ(json(), before);
    for (const auto code : {Qt::Key_Delete, Qt::Key_Backspace}) {
        select(timeline, 40);
        key(timeline, code);
        EXPECT_EQ(clip(firstId), nullptr);
        ASSERT_NE(clip(secondId), nullptr);
        EXPECT_EQ(clip(secondId)->startFrame, 90);
        session.undo();
        EXPECT_EQ(json(), before);
    }
}

TEST_F(InsertRippleUiTest, ContextMenuExposesRippleDeleteAndSearchKeepsShiftDeleteLocal) {
    MainWindow window(&session);
    auto* widget = window.timelineWidget();
    widget->resize(1200, 210);
    select(*widget, 40);
    const auto before = json();
    auto* search = window.findChild<QLineEdit*>("mediaSearch");
    ASSERT_NE(search, nullptr);
    search->setFocus();
    key(*search, Qt::Key_Delete, Qt::ShiftModifier);
    EXPECT_EQ(json(), before);
    bool sawRipple = false;
    QTimer::singleShot(0, &timeline, [&] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            auto* action = menu->findChild<QAction*>("clipRippleDelete");
            sawRipple = action && action->text() == "Ripple Delete" && action->isEnabled();
            menu->close();
        }
    });
    const QPoint point = position(40);
    QContextMenuEvent event(QContextMenuEvent::Mouse, point, timeline.mapToGlobal(point));
    QApplication::sendEvent(&timeline, &event);
    EXPECT_TRUE(sawRipple);
}

TEST_F(InsertRippleUiTest, EditRequestUsesCopiedClipIdAndIgnoresNestedEdit) {
    select(timeline, 40);
    int requests = 0;
    QObject::connect(&timeline, &TimelineWidget::editRequested, &timeline, [&] {
        ++requests;
        select(timeline, 100);
        timeline.rippleDeleteSelectedClip();
    });
    timeline.removeSelectedClip();
    EXPECT_EQ(requests, 1);
    EXPECT_EQ(clip(firstId), nullptr);
    ASSERT_NE(clip(secondId), nullptr);
    EXPECT_EQ(clip(secondId)->startFrame, 90);
}

enum class EditKind { Split, Remove, RippleDelete, OverwriteDrop, InsertDrop };
class PendingGradeTimelineEditTest : public InsertRippleUiTest,
                                    public ::testing::WithParamInterface<EditKind> {};

TEST_P(PendingGradeTimelineEditTest, MainWindowFlushesGradeBeforeEditAndKeepsSeparateUndoSteps) {
    MainWindow window(&session);
    auto* widget = window.timelineWidget();
    widget->resize(1200, 210);
    select(*widget, 40);
    widget->setCurrentFrame(45);
    auto* panel = window.findChild<ColorGradePanel*>("colorGradePanel");
    ASSERT_NE(panel, nullptr);
    ASSERT_EQ(panel->clipId(), firstId);
    auto* exposure = panel->findChild<QDoubleSpinBox*>("colorGrade_exposure");
    ASSERT_NE(exposure, nullptr);
    const auto before = json();
    exposure->setValue(1.5);
    const auto graded = json();
    ASSERT_NE(graded, before);
    const auto pending = session.beginClipEffectsTransaction(firstId);
    EXPECT_EQ(pending.status, heisenberg::EditStatus::Invalid);
    if (pending.changed()) session.cancelClipEffectsTransaction();
    // A missing MainWindow connection should fail assertions without blocking in an error dialog.
    QTimer fallback;
    QObject::connect(&fallback, &QTimer::timeout, &window, [] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
    });
    fallback.start(0);
    bool accepted = true;
    switch (GetParam()) {
    case EditKind::Split: widget->splitSelectedClip(); break;
    case EditKind::Remove: widget->removeSelectedClip(); break;
    case EditKind::RippleDelete: widget->rippleDeleteSelectedClip(); break;
    case EditKind::OverwriteDrop: accepted = drop(*widget, session, videoRef, 45); break;
    case EditKind::InsertDrop:
        widget->setInsertMode(true);
        accepted = drop(*widget, session, videoRef, 45);
        break;
    }
    fallback.stop();
    EXPECT_TRUE(accepted);
    EXPECT_NE(json(), graded);
    session.undo();
    EXPECT_EQ(json(), graded); // First undo removes only the timeline edit.
    session.undo();
    EXPECT_EQ(json(), before); // Second undo removes the separately committed grade.
}

INSTANTIATE_TEST_SUITE_P(TimelineEdits, PendingGradeTimelineEditTest,
    ::testing::Values(EditKind::Split, EditKind::Remove, EditKind::RippleDelete,
                      EditKind::OverwriteDrop, EditKind::InsertDrop));
} // namespace
