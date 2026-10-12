#include <Backend/EditorSession.hpp>
#include <Backend/PlayerController.hpp>
#include <MainWidget/MediaPoolWidget.hpp>
#include <MainWidget/TimelineWidget.hpp>

#include <gtest/gtest.h>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <functional>

using heisenberg::ui::EditorSession;

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

bool drop(TimelineWidget& timeline, QMimeData* mime, const QPoint& position) {
    QDragEnterEvent enter(position, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &enter);
    QDropEvent event(QPointF(position), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &event);
    return event.isAccepted();
}

std::unique_ptr<QMimeData> payload(EditorSession& session, const QString& ref) {
    auto mime = std::make_unique<QMimeData>();
    mime->setData(heisenberg::ui::kMediaMimeType, QJsonDocument(QJsonObject{
        {"session", session.token()}, {"mediaRef", ref}}).toJson());
    return mime;
}
}

TEST(MediaPoolUiTest, ImportMetadataDuplicateDetectionSearchAndThumbnail) {
    EditorSession session;
    MediaPoolWidget pool(&session);
    EXPECT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA, HEISENBERG_TEST_MEDIA}).isEmpty());
    ASSERT_EQ(session.editor().manifest().entries().size(), 1u);
    const auto& entry = session.editor().manifest().entries().front();
    EXPECT_DOUBLE_EQ(*entry.sourceFPS, 24.0); // New timeline runs at 30 fps.
    EXPECT_EQ(*entry.sourceWidth, 320);
    EXPECT_EQ(*entry.sourceHeight, 240);
    EXPECT_TRUE(*entry.hasAudio);
    EXPECT_NEAR(entry.duration, 1.0, 0.1);
    auto* list = pool.findChild<MediaListWidget*>();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 1);
    ASSERT_TRUE(waitFor([&]() { return !list->item(0)->icon().isNull(); }));
    auto* search = pool.findChild<QLineEdit*>();
    ASSERT_NE(search, nullptr);
    search->setText("no matching media");
    EXPECT_TRUE(list->item(0)->isHidden());
    search->clear();
    EXPECT_FALSE(list->item(0)->isHidden());
    EXPECT_TRUE(session.isDirty());
}

TEST(MediaPoolUiTest, ActualPoolDragExtendsEmptyTimelineAndRoundTripsPackage) {
    EditorSession session;
    MediaPoolWidget pool(&session);
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    TimelineWidget timeline;
    timeline.setSession(&session);
    timeline.resize(1800, 200);
    timeline.show();
    auto* list = pool.findChild<MediaListWidget*>();
    ASSERT_NE(list, nullptr);
    std::unique_ptr<QMimeData> mime(list->model()->mimeData({list->model()->index(0, 0)}));
    ASSERT_TRUE(mime->hasFormat(heisenberg::ui::kMediaMimeType));
    // 160px header + 300 frames * 4px/frame. Placement must not clamp to old duration.
    ASSERT_TRUE(drop(timeline, mime.get(), QPoint(1360, 48)));
    const auto* track = session.editor().activeTrack();
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->clips.size(), 1u);
    EXPECT_EQ(track->clips[0].startFrame, 300);
    EXPECT_EQ(track->clips[0].durationFrames, 30);
    const std::string beforeSave = heisenberg::ProjectCodec::encode(session.editor().project());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString error;
    const QString package = directory.path() + "/edit.palmier";
    ASSERT_TRUE(session.savePackage(package, &error)) << error.toStdString();
    EXPECT_FALSE(session.isDirty());
    EditorSession reopened;
    ASSERT_TRUE(reopened.loadPackage(package, &error)) << error.toStdString();
    EXPECT_EQ(heisenberg::ProjectCodec::encode(reopened.editor().project()), beforeSave);
    EXPECT_EQ(reopened.editor().manifest(), session.editor().manifest());
}

TEST(MediaPoolUiTest, OverwriteDropIsOneUndoAndRedoStep) {
    EditorSession session;
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    const auto ref = QString::fromStdString(session.editor().manifest().entries().front().id);
    ASSERT_TRUE(session.placeMedia(ref, 0).changed());
    const auto before = heisenberg::ProjectCodec::encode(session.editor().project());
    TimelineWidget timeline;
    timeline.setSession(&session);
    timeline.resize(800, 200);
    timeline.show();
    auto mime = payload(session, ref);
    ASSERT_TRUE(drop(timeline, mime.get(), QPoint(220, 48))); // frame 15
    const auto after = heisenberg::ProjectCodec::encode(session.editor().project());
    EXPECT_NE(after, before);
    ASSERT_EQ(session.editor().activeTrack()->clips.size(), 2u);
    EXPECT_EQ(session.editor().activeTrack()->clips.front().durationFrames, 15);
    session.undo();
    EXPECT_EQ(heisenberg::ProjectCodec::encode(session.editor().project()), before);
    session.redo();
    EXPECT_EQ(heisenberg::ProjectCodec::encode(session.editor().project()), after);
}

TEST(MediaPoolUiTest, RejectsForeignAndUnknownDragWithoutMutatingTimeline) {
    EditorSession session, other;
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    const auto ref = QString::fromStdString(session.editor().manifest().entries().front().id);
    TimelineWidget timeline;
    timeline.setSession(&session);
    timeline.resize(800, 200);
    timeline.show();
    auto foreign = payload(other, ref);
    auto unknown = payload(session, "not-in-manifest");
    const auto before = heisenberg::ProjectCodec::encode(session.editor().project());
    EXPECT_FALSE(drop(timeline, foreign.get(), QPoint(200, 48)));
    EXPECT_FALSE(drop(timeline, unknown.get(), QPoint(200, 48)));
    EXPECT_EQ(heisenberg::ProjectCodec::encode(session.editor().project()), before);
    EXPECT_EQ(session.editor().activeTrack(), nullptr);
}

TEST(MediaPoolUiTest, OfflineAssetsAndUnsupportedTracksCannotBeDraggedOrEdited) {
    EditorSession session;
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    const auto ref = QString::fromStdString(session.editor().manifest().entries().front().id);
    ASSERT_TRUE(session.placeMedia(ref, 0).changed());
    QTemporaryDir directory;
    const QString package = directory.path() + "/unsupported.palmier";
    QString error;
    ASSERT_TRUE(session.savePackage(package, &error)) << error.toStdString();
    auto project = session.editor().project();
    heisenberg::Track extraTrack;
    extraTrack.id = heisenberg::makeUuid();
    project.timelines.front().tracks.push_back(extraTrack);
    auto manifest = session.editor().manifest();
    auto offline = manifest.entries().front();
    offline.id = heisenberg::makeUuid();
    offline.name = "Missing video";
    offline.source.path = directory.path().toStdString() + "/missing.mp4";
    ASSERT_TRUE(manifest.add(offline));
    for (const auto& document : {std::pair{QString("project.json"), heisenberg::ProjectCodec::encode(project)},
                                std::pair{QString("media.json"), manifest.toJson()}}) {
        QFile file(package + '/' + document.first);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(file.write(document.second.data(), static_cast<qint64>(document.second.size())),
                  static_cast<qint64>(document.second.size()));
    }
    ASSERT_TRUE(session.loadPackage(package, &error)) << error.toStdString();
    MediaPoolWidget pool(&session);
    auto* list = pool.findChild<MediaListWidget*>();
    ASSERT_EQ(list->count(), 2);
    EXPECT_FALSE(list->item(1)->flags().testFlag(Qt::ItemIsDragEnabled));
    std::unique_ptr<QMimeData> offlineMime(list->model()->mimeData({list->model()->index(1, 0)}));
    EXPECT_FALSE(offlineMime->hasFormat(heisenberg::ui::kMediaMimeType));
    list->setCurrentRow(1);
    EXPECT_FALSE(pool.findChild<QPushButton*>("addSelectedMedia")->isEnabled());
    TimelineWidget timeline;
    timeline.setSession(&session);
    timeline.resize(800, 200);
    timeline.show();
    auto mime = payload(session, ref);
    EXPECT_FALSE(drop(timeline, mime.get(), QPoint(200, 48)));
    EXPECT_EQ(session.placeMedia(ref, 10).status, heisenberg::EditStatus::Unsupported);
    EXPECT_EQ(session.editor().project(), project);
    EXPECT_FALSE(session.isDirty());
}

TEST(MediaPoolUiTest, SourcePreviewAndScrubReturnToUnsavedTimeline) {
    EditorSession session;
    heisenberg::ui::PlayerController player(&session);
    player.setHardwareDecode(false);
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    const auto ref = QString::fromStdString(session.editor().manifest().entries().front().id);
    ASSERT_TRUE(session.placeMedia(ref, 60).changed());
    ASSERT_TRUE(waitFor([&]() { return player.isSeekable() && player.currentTime() >= 1.99; }));
    EXPECT_EQ(player.frameCount(), 90);
    EXPECT_NEAR(player.duration(), 3.0, 0.01);
    EXPECT_FALSE(player.previewingSource());
    player.previewAsset(ref);
    ASSERT_TRUE(waitFor([&]() { return player.previewingSource() && player.isSeekable()
        && player.duration() < 1.2; }));
    player.beginScrub();
    player.scrubToFrame(75);
    player.endScrub(75); // Release before asynchronous open completes.
    ASSERT_TRUE(waitFor([&]() { return !player.previewingSource() && player.currentTime() >= 2.49; }));
    EXPECT_NEAR(player.currentTime(), 2.5, 0.04);
    EXPECT_EQ(player.frameCount(), 90);
}

TEST(MediaPoolUiTest, FailedImportDoesNotDirtyProjectOrCreateAssets) {
    EditorSession session;
    EXPECT_FALSE(session.importFiles({"C:/does-not-exist/video.mp4"}).isEmpty());
    EXPECT_FALSE(session.isDirty());
    EXPECT_TRUE(session.editor().manifest().entries().empty());
    EXPECT_EQ(session.placeMedia("unknown", 0).status, heisenberg::EditStatus::Invalid);
    EXPECT_FALSE(session.isDirty());
}

TEST(MediaPoolUiTest, EditedTimelineSeekKeepsAudioClockAndReplaysAfterEnd) {
    EditorSession session;
    heisenberg::ui::PlayerController player(&session);
    player.setHardwareDecode(false);
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA}).isEmpty());
    const auto ref = QString::fromStdString(session.editor().manifest().entries().front().id);
    ASSERT_TRUE(session.placeMedia(ref, 60).changed());
    ASSERT_TRUE(waitFor([&]() { return player.isSeekable() && player.currentTime() >= 1.99; }));
    player.seek(2.5);
    ASSERT_TRUE(waitFor([&]() { return player.currentTime() >= 2.49; }));
    QElapsedTimer playbackTime;
    playbackTime.start();
    player.play();
    ASSERT_TRUE(waitFor([&]() { return !player.isPlaying() && player.currentTime() >= 2.99; }));
    // Only half a second remains: seeking must not reset the audio clock to zero.
    EXPECT_LT(playbackTime.elapsed(), 2000);
    player.play();
    ASSERT_TRUE(waitFor([&]() { return player.isPlaying(); }));
    player.pause();
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
