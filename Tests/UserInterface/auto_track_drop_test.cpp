#include <Backend/EditorSession.hpp>
#include <MainWidget/TimelineWidget.hpp>
#include <Models/ProjectCodec.hpp>

#include <gtest/gtest.h>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QScrollBar>
#include <memory>

namespace {
using heisenberg::ClipType;
using heisenberg::ui::EditorSession;

void mouse(TimelineWidget& widget, QEvent::Type type, QPointF point) {
    QMouseEvent event(type, point, widget.mapToGlobal(point.toPoint()),
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}
void drag(TimelineWidget& widget, QPointF from, QPointF to) {
    mouse(widget, QEvent::MouseButtonPress, from);
    mouse(widget, QEvent::MouseMove, to);
    mouse(widget, QEvent::MouseButtonRelease, to);
}
void escape(TimelineWidget& widget) {
    QKeyEvent event(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}
std::unique_ptr<QMimeData> payload(EditorSession& session, const QString& ref) {
    auto mime = std::make_unique<QMimeData>();
    mime->setData(heisenberg::ui::kMediaMimeType, QJsonDocument(QJsonObject{
        {"session", session.token()}, {"mediaRef", ref}}).toJson());
    return mime;
}
bool preview(TimelineWidget& widget, QMimeData* mime, QPoint point) {
    QDragEnterEvent enter(point, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &enter);
    QDragMoveEvent move(point, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &move);
    return move.isAccepted();
}
bool drop(TimelineWidget& widget, QMimeData* mime, QPoint point) {
    preview(widget, mime, point);
    QDropEvent event(QPointF(point), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
    return event.isAccepted();
}
void leave(TimelineWidget& widget) {
    QDragLeaveEvent event;
    QApplication::sendEvent(&widget, &event);
}

class AutoTrackDropTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA, HEISENBERG_TEST_LONG_AUDIO}).isEmpty());
        for (const auto& entry : session.editor().manifest().entries()) {
            if (entry.type == ClipType::Video) video = QString::fromStdString(entry.id);
            if (entry.type == ClipType::Audio) audio = QString::fromStdString(entry.id);
        }
        ASSERT_FALSE(video.isEmpty());
        ASSERT_FALSE(audio.isEmpty());
        ASSERT_TRUE(session.placeMedia(video, 30).changed());
        clipId = session.editor().activeTimeline()->tracks.front().clips.front().id;
        sourceTrackId = session.editor().activeTimeline()->trackForClip(clipId)->id;
        timeline.setSession(&session);
        timeline.setSnappingEnabled(false);
        timeline.resize(1000, 210);
        timeline.show();
        QCoreApplication::processEvents();
    }
    std::string json() const { return heisenberg::ProjectCodec::encode(session.editor().project()); }
    const heisenberg::Timeline* active() const { return session.editor().activeTimeline(); }
    const heisenberg::Clip* clip() const { return active()->clipById(clipId); }
    EditorSession session;
    TimelineWidget timeline;
    QString video, audio;
    std::string clipId, sourceTrackId;
};

TEST_F(AutoTrackDropTest, MarkedPoolStripDropSupportsOverwriteAndInsertAsOneUndoStep) {
    ASSERT_TRUE(session.setSourceIn(video, .25));
    ASSERT_TRUE(session.setSourceOut(video, .75));
    qint64 trim = 0, duration = 0;
    ASSERT_TRUE(session.sourcePlacementRange(video, trim, duration));
    ASSERT_EQ(trim, 8);
    ASSERT_EQ(duration, 15);
    auto mime = payload(session, video);
    const auto before = json();
    const auto original = *clip();
    for (const bool insert : {false, true}) {
        SCOPED_TRACE(insert ? "insert" : "overwrite");
        timeline.setInsertMode(insert);
        ASSERT_TRUE(drop(timeline, mime.get(), {320, 40})); // frame40, inside the permanent strip.
        ASSERT_EQ(active()->tracks.size(), 2u);
        const auto* owner = active()->trackForClip(timeline.selectedClipId().toStdString());
        ASSERT_NE(owner, nullptr);
        EXPECT_EQ(owner->id, active()->tracks.front().id);
        EXPECT_NE(owner->id, sourceTrackId);
        ASSERT_EQ(owner->clips.size(), 1u);
        EXPECT_EQ(owner->clips.front().startFrame, 40);
        EXPECT_EQ(owner->clips.front().trimStartFrame, trim);
        EXPECT_EQ(owner->clips.front().durationFrames, duration);
        EXPECT_EQ(*clip(), original);
        EXPECT_EQ(timeline.targetTrackForMedia(video).toStdString(), owner->id);
        const auto after = json();
        session.undo();
        EXPECT_EQ(json(), before);
        session.redo();
        EXPECT_EQ(json(), after);
        session.undo();
        EXPECT_EQ(json(), before);
    }
}

TEST_F(AutoTrackDropTest, SameFrameClipMoveCreatesTopTrackAndPreservesPayload) {
    ASSERT_TRUE(session.setClipOpacity(QString::fromStdString(clipId), .35).changed());
    const auto effects = heisenberg::JsonValue::array({{
        {"id", "grade"}, {"type", "video.basicGrade"}, {"enabled", true},
        {"params", {{"exposure", {{"value", 1.25}}}}}
    }});
    ASSERT_TRUE(session.setClipEffects(QString::fromStdString(clipId), effects).changed());
    const auto original = *clip();
    const auto before = json();
    drag(timeline, {320, 68}, {320, 40}); // Ten-frame grab offset remains unchanged.
    ASSERT_NE(clip(), nullptr);
    EXPECT_EQ(*clip(), original);
    ASSERT_EQ(active()->tracks.size(), 2u);
    EXPECT_TRUE(active()->trackById(sourceTrackId)->clips.empty());
    EXPECT_EQ(active()->trackForClip(clipId)->id, active()->tracks.front().id);
    EXPECT_NE(active()->trackForClip(clipId)->id, sourceTrackId);
    EXPECT_EQ(timeline.selectedClipId().toStdString(), clipId);
    EXPECT_EQ(timeline.targetTrackForMedia(video).toStdString(), active()->tracks.front().id);
    const auto after = json();
    session.undo();
    EXPECT_EQ(json(), before);
    session.redo();
    EXPECT_EQ(json(), after);
}

TEST_F(AutoTrackDropTest, PreviewCancelWrongMimeAudioAndTrimNeverEdit) {
    auto mime = payload(session, video);
    auto audioMime = payload(session, audio);
    EditorSession foreignSession;
    auto foreign = payload(foreignSession, video);
    QMimeData wrong;
    wrong.setText("video");
    const auto before = json();
    int requests = 0;
    QObject::connect(&timeline, &TimelineWidget::editRequested, &timeline, [&] { ++requests; });
    const auto baseline = timeline.grab().toImage();
    ASSERT_TRUE(preview(timeline, mime.get(), {320, 40}));
    const auto highlighted = timeline.grab().toImage();
    EXPECT_NE(highlighted.pixelColor(900, 35), baseline.pixelColor(900, 35)); // Full-width strip highlight.
    EXPECT_EQ(highlighted.pixelColor(100, 35), baseline.pixelColor(100, 35)); // Header excluded.
    EXPECT_EQ(highlighted.pixelColor(900, 15), baseline.pixelColor(900, 15)); // Ruler excluded.
    EXPECT_EQ(json(), before);
    leave(timeline);
    EXPECT_EQ(timeline.grab().toImage(), baseline);
    ASSERT_TRUE(preview(timeline, mime.get(), {320, 40}));
    escape(timeline);
    EXPECT_EQ(timeline.grab().toImage(), baseline);
    leave(timeline);
    EXPECT_FALSE(drop(timeline, &wrong, {320, 40}));
    EXPECT_FALSE(drop(timeline, foreign.get(), {320, 40}));
    EXPECT_FALSE(drop(timeline, audioMime.get(), {320, 40}));
    drag(timeline, {281, 68}, {321, 40}); // A trim handle cannot create a track.
    mouse(timeline, QEvent::MouseButtonPress, {320, 68});
    mouse(timeline, QEvent::MouseMove, {320, 40});
    escape(timeline);
    mouse(timeline, QEvent::MouseButtonRelease, {320, 40});
    ASSERT_TRUE(session.placeMedia(audio, 30).changed());
    const auto withAudio = json();
    drag(timeline, {320, 112}, {320, 40});
    EXPECT_EQ(json(), withAudio);
    session.undo();
    EXPECT_EQ(json(), before);
    EXPECT_EQ(requests, 0);
}

TEST_F(AutoTrackDropTest, StripScrollBoundaryAndAbsentVideoPlaceholderUseSharedDestination) {
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(session.addTrack(ClipType::Video).changed());
    auto* scroll = timeline.findChild<QScrollBar*>("timelineVerticalScrollBar");
    ASSERT_NE(scroll, nullptr);
    EXPECT_EQ(scroll->maximum(), 104); // 20 + five44px videos +44px audio -180px viewport.
    auto mime = payload(session, video);
    const auto before = json();
    EXPECT_FALSE(drop(timeline, mime.get(), {159, 40}));
    EXPECT_FALSE(drop(timeline, mime.get(), {320, 29}));
    EXPECT_FALSE(drop(timeline, mime.get(), {scroll->geometry().left(), 40}));
    EXPECT_EQ(json(), before);
    scroll->setValue(19);
    ASSERT_TRUE(preview(timeline, mime.get(), {320, 30})); // Last visible strip pixel.
    leave(timeline);
    const auto visibleTrackId = active()->tracks.front().id;
    scroll->setValue(20);
    ASSERT_TRUE(drop(timeline, mime.get(), {320, 30})); // Strip gone: first existing video lane.
    EXPECT_EQ(active()->tracks.size(), 5u);
    EXPECT_EQ(active()->trackForClip(timeline.selectedClipId().toStdString())->id, visibleTrackId);
    session.undo();
    EXPECT_EQ(json(), before);

    EditorSession empty;
    ASSERT_TRUE(empty.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    const auto emptyRef = QString::fromStdString(empty.editor().manifest().entries().front().id);
    timeline.setSession(&empty);
    auto emptyMime = payload(empty, emptyRef);
    const auto emptyBefore = heisenberg::ProjectCodec::encode(empty.editor().project());
    for (const int y : {40, 50}) { // Permanent strip and absent-video placeholder both work.
        ASSERT_TRUE(drop(timeline, emptyMime.get(), {320, y}));
        ASSERT_EQ(empty.editor().activeTimeline()->tracks.size(), 1u);
        EXPECT_EQ(empty.editor().activeTimeline()->tracks.front().clips.front().startFrame, 40);
        empty.undo();
        EXPECT_EQ(heisenberg::ProjectCodec::encode(empty.editor().project()), emptyBefore);
    }
    timeline.setSession(&session);
}

TEST_F(AutoTrackDropTest, PendingGradeFlushPreservesBothDropAndMoveAsSeparateUndoSteps) {
    for (const bool poolDrop : {true, false}) {
        SCOPED_TRACE(poolDrop ? "pool drop" : "clip move");
        const auto before = json();
        ASSERT_TRUE(session.beginClipEffectsTransaction(QString::fromStdString(clipId)).message.empty());
        const auto effects = heisenberg::JsonValue::array({{
            {"id", "grade"}, {"type", "video.basicGrade"}, {"enabled", true},
            {"params", {{"exposure", {{"value", 1.5}}}}}
        }});
        ASSERT_TRUE(session.updateClipEffectsTransaction(effects).changed());
        const auto graded = json();
        int requests = 0;
        const auto connection = QObject::connect(&timeline, &TimelineWidget::editRequested, &timeline, [&] {
            ++requests;
            EXPECT_TRUE(session.commitClipEffectsTransaction().changed());
        });
        if (poolDrop) {
            auto mime = payload(session, video);
            ASSERT_TRUE(drop(timeline, mime.get(), {320, 40}));
        } else drag(timeline, {320, 68}, {320, 40});
        QObject::disconnect(connection);
        EXPECT_EQ(requests, 1);
        ASSERT_EQ(active()->tracks.size(), 2u);
        ASSERT_TRUE(clip()->effects.has_value());
        EXPECT_EQ(*clip()->effects, effects);
        EXPECT_EQ(timeline.targetTrackForMedia(video).toStdString(), active()->tracks.front().id);
        session.undo();
        EXPECT_EQ(json(), graded);
        session.undo();
        EXPECT_EQ(json(), before);
    }
}
} // namespace
