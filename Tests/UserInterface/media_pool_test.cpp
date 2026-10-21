#include <Backend/EditorSession.hpp>
#include <Models/ProjectCodec.hpp>
#include <Backend/PlayerController.hpp>
#include <MainWidget/MediaPoolWidget.hpp>
#include <MainWidget/TimelineWidget.hpp>
#include <MainWidget/MainWindow.hpp>
#include <QFileDialog>
#include <QFileInfo>
#include <QTimer>
#include <QDir>

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
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <cmath>

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

TEST(ProjectPackageUiTest, SaveDialogUsesBergDefaultsAndAvoidsDuplicateUppercaseSuffix) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    EditorSession session;
    MainWindow window(&session);
    const auto options = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    for (const QString& filename : {QString("named"), QString("uppercase.BERG")}) {
        const bool saveAs = !session.editor().projectDir().empty();
        QTimer::singleShot(0, &window, [&]() {
            auto* dialog = window.findChild<QFileDialog*>("saveProjectDialog");
            if (!dialog) { ADD_FAILURE() << "Project save dialog did not open"; return; }
            EXPECT_EQ(dialog->defaultSuffix(), QString("berg"));
            EXPECT_EQ(dialog->selectedNameFilter(), QString("Berg Project (*.berg)"));
            EXPECT_EQ(QFileInfo(dialog->selectedFiles().value(0)).fileName(), saveAs ? QString("Copy.berg") : QString("Untitled.berg"));
            dialog->setDirectory(directory.path());
            dialog->selectFile(filename);
            static_cast<QDialog*>(dialog)->accept();
        });
        EXPECT_TRUE(window.saveProjectPackage(saveAs));
        const QString expected = directory.path() + '/' + filename + (filename.endsWith(".BERG") ? "" : ".berg");
        EXPECT_TRUE(QFileInfo::exists(expected + "/project.json"));
        EXPECT_TRUE(QFileInfo::exists(expected + "/media.json"));
        EXPECT_FALSE(QFileInfo::exists(expected + ".berg"));
        EXPECT_EQ(QDir(QString::fromStdString(session.editor().projectDir())).absolutePath(), QDir(expected).absolutePath());
        EditorSession reopened;
        QString error;
        EXPECT_TRUE(reopened.loadPackage(expected, &error)) << error.toStdString();
    }
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, options);
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
    const QString package = directory.path() + "/edit.berg";
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
    const QString package = directory.path() + "/unsupported.berg";
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

TEST(MediaPoolUiTest, AudioTilesHaveDurationNoThumbnailAndSingleClickPreview) {
    EditorSession session;
    MediaPoolWidget pool(&session);
    pool.resize(360, 480);
    pool.show();
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MP3, HEISENBERG_TEST_MP3}).isEmpty());
    ASSERT_EQ(session.editor().manifest().entries().size(), 1u);
    const auto& entry = session.editor().manifest().entries().front();
    EXPECT_EQ(entry.type, heisenberg::ClipType::Audio);
    EXPECT_TRUE(entry.hasAudio.value_or(false));
    EXPECT_GT(entry.duration, 1.0);
    auto* list = pool.findChild<MediaListWidget*>();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 1);
    EXPECT_TRUE(list->item(0)->icon().isNull());
    EXPECT_TRUE(list->item(0)->data(Qt::UserRole + 1).toString().contains("Audio"));
    EXPECT_TRUE(list->item(0)->data(Qt::UserRole + 1).toString().contains(" s"));
    EXPECT_TRUE(list->item(0)->flags().testFlag(Qt::ItemIsDragEnabled));
    int previews = 0;
    QString previewRef;
    QObject::connect(&pool, &MediaPoolWidget::previewRequested, [&](const QString& ref) {
        ++previews; previewRef = ref;
    });
    QCoreApplication::processEvents();
    const QPoint position = list->visualItemRect(list->item(0)).center();
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent click(type, QPointF(position), QPointF(list->viewport()->mapToGlobal(position)),
            Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &click);
    }
    EXPECT_EQ(previews, 1);
    EXPECT_EQ(previewRef.toStdString(), entry.id);
    EXPECT_TRUE(pool.findChild<QPushButton*>("addSelectedMedia")->isEnabled());
    std::unique_ptr<QMimeData> mime(list->model()->mimeData({list->model()->index(0, 0)}));
    EXPECT_TRUE(mime->hasFormat(heisenberg::ui::kMediaMimeType));
}

TEST(MediaPoolUiTest, AudioOnlyDropCreatesAudioTrackAndRejectsWrongLaneAndExplicitTarget) {
    EditorSession session;
    MediaPoolWidget pool(&session);
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MP3, HEISENBERG_TEST_MEDIA}).isEmpty());
    const auto audioRef = QString::fromStdString(session.editor().manifest().entries()[0].id);
    const auto videoRef = QString::fromStdString(session.editor().manifest().entries()[1].id);
    TimelineWidget timeline;
    timeline.setSession(&session);
    timeline.resize(1000, 200);
    timeline.show();
    auto* list = pool.findChild<MediaListWidget*>();
    std::unique_ptr<QMimeData> audioMime(list->model()->mimeData({list->model()->index(0, 0)}));
    const auto before = heisenberg::ProjectCodec::encode(session.editor().project());
    EXPECT_FALSE(drop(timeline, audioMime.get(), QPoint(280, 48)));
    EXPECT_EQ(heisenberg::ProjectCodec::encode(session.editor().project()), before);
    ASSERT_TRUE(drop(timeline, audioMime.get(), QPoint(280, 92)));
    ASSERT_EQ(session.editor().activeTimeline()->tracks.size(), 1u);
    EXPECT_EQ(session.editor().activeTimeline()->tracks[0].type, heisenberg::ClipType::Audio);
    EXPECT_EQ(session.editor().activeTimeline()->tracks[0].clips[0].startFrame, 30);
    const QString audioTrackId = QString::fromStdString(session.editor().activeTimeline()->tracks[0].id);
    const auto audioOnly = heisenberg::ProjectCodec::encode(session.editor().project());
    EXPECT_EQ(session.placeMedia(videoRef, 0, audioTrackId).status, heisenberg::EditStatus::Invalid);
    EXPECT_EQ(session.placeMedia(audioRef, 0, "unknown-track").status, heisenberg::EditStatus::Invalid);
    EXPECT_EQ(heisenberg::ProjectCodec::encode(session.editor().project()), audioOnly);
    auto videoMime = payload(session, videoRef);
    EXPECT_FALSE(drop(timeline, videoMime.get(), QPoint(200, 92)));
    ASSERT_TRUE(drop(timeline, videoMime.get(), QPoint(200, 48)));
    ASSERT_EQ(session.editor().activeTimeline()->tracks.size(), 2u);
    EXPECT_EQ(session.editor().activeTimeline()->tracks[0].type, heisenberg::ClipType::Video);
    EXPECT_EQ(session.editor().activeTimeline()->tracks[1].id, audioTrackId.toStdString());
    const auto mixed = heisenberg::ProjectCodec::encode(session.editor().project());
    ASSERT_TRUE(drop(timeline, audioMime.get(), QPoint(440, 92))); // Existing audio track, frame 70.
    ASSERT_EQ(session.editor().activeTimeline()->tracks.size(), 2u);
    EXPECT_EQ(session.editor().activeTimeline()->tracks[1].id, audioTrackId.toStdString());
    EXPECT_EQ(session.editor().activeTimeline()->tracks[1].clips.back().startFrame, 70);
    session.undo();
    EXPECT_EQ(heisenberg::ProjectCodec::encode(session.editor().project()), mixed);
    session.undo();
    EXPECT_EQ(heisenberg::ProjectCodec::encode(session.editor().project()), audioOnly);
    QTemporaryDir directory;
    QString error;
    ASSERT_TRUE(session.savePackage(directory.path() + "/audio.berg", &error)) << error.toStdString();
    EditorSession reopened;
    ASSERT_TRUE(reopened.loadPackage(directory.path() + "/audio.berg", &error)) << error.toStdString();
    EXPECT_EQ(heisenberg::ProjectCodec::encode(reopened.editor().project()), audioOnly);
}

TEST(MediaPoolUiTest, AudioSourceOpensPausedAuditionsAndReturnsToMixedTimeline) {
    EditorSession session;
    heisenberg::ui::PlayerController player(&session);
    player.setHardwareDecode(false);
    ASSERT_TRUE(session.importFiles({HEISENBERG_TEST_MEDIA, HEISENBERG_TEST_MP3, HEISENBERG_TEST_LONG_AUDIO}).isEmpty());
    const auto video = QString::fromStdString(session.editor().manifest().entries()[0].id);
    const auto mp3 = QString::fromStdString(session.editor().manifest().entries()[1].id);
    const auto audio = QString::fromStdString(session.editor().manifest().entries()[2].id);
    ASSERT_TRUE(session.placeMedia(video, 0).changed());
    ASSERT_TRUE(session.placeMedia(audio, 0).changed());
    ASSERT_TRUE(waitFor([&]() { return player.isSeekable() && player.frameCount() == 90; }));
    for (const auto& ref : {mp3, audio}) {
        player.previewAsset(ref);
        ASSERT_TRUE(waitFor([&]() { return player.previewingSource() && player.isSeekable() &&
            player.currentFile() == QString::fromStdString(session.editor().resolver().asset(ref.toStdString()).path) &&
            player.duration() > 1.2; }));
        EXPECT_FALSE(player.isPlaying());
        ASSERT_TRUE(waitFor([&]() { return std::abs(player.currentTime()) < 0.001; }));
        auto drain = []() {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 100) { QCoreApplication::processEvents(); QThread::msleep(5); }
        };
        const double paused = player.currentTime();
        drain();
        EXPECT_FALSE(player.isPlaying());
        EXPECT_NEAR(player.currentTime(), paused, 0.001);
        player.play();
        ASSERT_TRUE(waitFor([&]() { return player.isPlaying() && player.currentTime() > paused + 0.08; }));
        player.pause();
        ASSERT_TRUE(waitFor([&]() { return !player.isPlaying(); }));
        drain(); // Finish notifications already queued before the acknowledged pause.
        const double auditionPaused = player.currentTime();
        drain();
        EXPECT_FALSE(player.isPlaying());
        EXPECT_NEAR(player.currentTime(), auditionPaused, 0.001);
    }
    player.beginScrub();
    player.scrubToFrame(45);
    player.endScrub(45);
    ASSERT_TRUE(waitFor([&]() { return !player.previewingSource() && player.frameCount() == 90 &&
        std::abs(player.currentTime() - 1.5) < 0.04; }));
    EXPECT_NEAR(player.duration(), 3.0, 0.02);
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
